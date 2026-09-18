/*
 * Copyright 2004 John M Bell <jmb202@ecs.soton.ac.uk>
 * Copyright 2020 Vincent Sanders <vince@netsurf-browser.org>
 *
 * This file is part of NetSurf, http://www.netsurf-browser.org/
 *
 * NetSurf is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * NetSurf is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * \file
 * Free text search.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "utils/errors.h"
#include "utils/utils.h"
#include "utils/ascii.h"
#include "netsurf/types.h"
#include "desktop/selection.h"

#include "content/content.h"
#include "content/content_protected.h"
#include "content/hlcache.h"
#include "content/textsearch.h"


/**
 * Search match.
 *
 * The sentinel entry is embedded directly in the search context, so
 * creating a search no longer requires a separate heap allocation.
 */
struct list_entry {
	struct list_entry *prev;
	struct list_entry *next;

	unsigned start_idx;
	unsigned end_idx;

	struct box *start_box;
	struct box *end_box;

	struct selection *sel;
};


/**
 * Context for a free text search.
 */
struct textsearch_context {
	/** Content search was performed upon. */
	struct content *c;

	/** Opaque pointer passed to constructor. */
	void *gui_p;

	/** Sentinel/list head. */
	struct list_entry found;

	/** Current selected match. */
	struct list_entry *current;

	/** Query string search results are for. */
	char *string;

	/** Length of cached query string. */
	size_t string_len;

	/** Cached case-sensitivity state. */
	bool prev_case_sens;

	/** Whether the next operation starts a new search. */
	bool newsearch;

	/** Number of matches currently stored. */
	size_t match_count;

	/** Last value passed to search_show_all(). */
	bool showing_all;
};


/**
 * Fast ASCII case folding.
 *
 * MIME/text search input is byte-oriented here. This deliberately only
 * folds ASCII A-Z, matching the behavior required by the existing
 * ASCII search implementation.
 */
static inline unsigned char
textsearch_fold_ascii(unsigned char c)
{
	if (c >= 'A' && c <= 'Z') {
		c = (unsigned char)(c + ('a' - 'A'));
	}

	return c;
}


/**
 * Fast ASCII case-insensitive character comparison.
 */
static inline bool
textsearch_char_equal(unsigned char a,
		      unsigned char b,
		      bool case_sensitive)
{
	if (case_sensitive) {
		return a == b;
	}

	return textsearch_fold_ascii(a) == textsearch_fold_ascii(b);
}


/**
 * Broadcast textsearch message.
 */
static inline void
textsearch_broadcast(struct textsearch_context *textsearch,
		     int type,
		     bool state,
		     const char *string)
{
	union content_msg_data msg_data;

	msg_data.textsearch.type = type;
	msg_data.textsearch.ctx = textsearch->gui_p;
	msg_data.textsearch.state = state;
	msg_data.textsearch.string = string;

	content_broadcast(textsearch->c,
			  CONTENT_MSG_TEXTSEARCH,
			  &msg_data);
}


/**
 * Release the memory used by the list of matches.
 *
 * Selection objects are destroyed after the list links have been
 * cleared. This is important because selection destruction may cause
 * synchronous toolkit callbacks.
 */
static void
free_matches(struct textsearch_context *textsearch)
{
	struct list_entry *cur;

	if (textsearch == NULL) {
		return;
	}

	cur = textsearch->found.next;

	/*
	 * Detach the complete list before destroying selections.
	 */
	textsearch->found.prev = NULL;
	textsearch->found.next = NULL;
	textsearch->current = NULL;
	textsearch->match_count = 0;

	while (cur != NULL) {
		struct list_entry *next = cur->next;

		if (cur->sel != NULL) {
			selection_destroy(cur->sel);
			cur->sel = NULL;
		}

		free(cur);
		cur = next;
	}
}


/**
 * Specifies whether all matches or just the current match should be
 * highlighted.
 */
static void
search_show_all(bool all, struct textsearch_context *context)
{
	struct list_entry *entry;

	/*
	 * Avoid walking every match when the requested state has not
	 * changed. The current match can still change while showing all,
	 * so only use this optimization when all matches are displayed.
	 */
	if (all == context->showing_all && all) {
		return;
	}

	for (entry = context->found.next;
	     entry != NULL;
	     entry = entry->next) {

		bool should_show = all || entry == context->current;

		if (!should_show) {
			if (entry->sel != NULL) {
				selection_destroy(entry->sel);
				entry->sel = NULL;
			}
			continue;
		}

		if (entry->sel == NULL) {
			entry->sel = selection_create(context->c);

			if (entry->sel != NULL) {
				selection_init(entry->sel);

				selection_set_position(
					entry->sel,
					entry->start_idx,
					entry->end_idx);
			}
		}
	}

	context->showing_all = all;
}


/**
 * Search for a string in a content.
 *
 * \param context The search context.
 * \param string The string to search for.
 * \param string_len Length of search string.
 * \param flags Flags controlling the search.
 */
static nserror
search_text(struct textsearch_context *context,
	    const char *string,
	    size_t string_len,
	    search_flags_t flags)
{
	struct rect bounds;
	union content_msg_data msg_data;

	const bool case_sensitive =
		(flags & SEARCH_FLAG_CASE_SENSITIVE) != 0;

	const bool forwards =
		(flags & SEARCH_FLAG_FORWARDS) != 0;

	const bool showall =
		(flags & SEARCH_FLAG_SHOWALL) != 0;

	nserror res = NSERROR_OK;

	if (context == NULL || context->c == NULL) {
		return NSERROR_OK;
	}

	/*
	 * A changed query or case-sensitivity setting invalidates the
	 * current match list.
	 */
	if (context->newsearch ||
	    context->prev_case_sens != case_sensitive) {

		free(context->string);
		context->string = NULL;
		context->string_len = 0;

		context->current = NULL;

		free_matches(context);

		/*
		 * Keep the stored query available for subsequent navigation.
		 */
		if (string_len != 0) {
			context->string = malloc(string_len + 1);

			if (context->string == NULL) {
				return NSERROR_NOMEM;
			}

			memcpy(context->string, string, string_len);
			context->string[string_len] = '\0';
			context->string_len = string_len;
		}

		context->showing_all = false;

		/* Indicate find operation starting. */
		textsearch_broadcast(context,
				     CONTENT_TEXTSEARCH_FIND,
				     true,
				     NULL);

		/*
		 * Perform the content-specific search.
		 */
		res = context->c->handler->textsearch_find(
			context->c,
			context,
			string,
			(unsigned)string_len,
			case_sensitive);

		/* Indicate find operation finished. */
		textsearch_broadcast(context,
				     CONTENT_TEXTSEARCH_FIND,
				     false,
				     NULL);

		if (res != NSERROR_OK) {
			free_matches(context);
			return res;
		}

		context->prev_case_sens = case_sensitive;

		/*
		 * New search starts at the first match.
		 */
		context->current = context->found.next;
		context->newsearch = false;
	} else if (context->current != NULL) {
		/*
		 * Continue an existing search.
		 */
		if (forwards) {
			if (context->current->next != NULL) {
				context->current = context->current->next;
			}
		} else {
			if (context->current->prev != NULL) {
				context->current = context->current->prev;
			}
		}
	}

	/*
	 * Update match state.
	 */
	textsearch_broadcast(
		context,
		CONTENT_TEXTSEARCH_MATCH,
		context->current != NULL,
		NULL);

	search_show_all(showall, context);

	/*
	 * Update back state.
	 */
	textsearch_broadcast(
		context,
		CONTENT_TEXTSEARCH_BACK,
		context->current != NULL &&
		context->current->prev != NULL,
		NULL);

	/*
	 * Update forward state.
	 */
	textsearch_broadcast(
		context,
		CONTENT_TEXTSEARCH_FORWARD,
		context->current != NULL &&
		context->current->next != NULL,
		NULL);

	if (context->current == NULL) {
		return NSERROR_OK;
	}

	/*
	 * Calculate the bounds of the selected match.
	 */
	res = context->c->handler->textsearch_bounds(
		context->c,
		context->current->start_idx,
		context->current->end_idx,
		context->current->start_box,
		context->current->end_box,
		&bounds);

	if (res == NSERROR_OK) {
		msg_data.scroll.area = true;
		msg_data.scroll.x0 = bounds.x0;
		msg_data.scroll.y0 = bounds.y0;
		msg_data.scroll.x1 = bounds.x1;
		msg_data.scroll.y1 = bounds.y1;

		content_broadcast(
			context->c,
			CONTENT_MSG_SCROLL,
			&msg_data);
	}

	return res;
}


/**
 * Begins/continues the search process.
 *
 * \param context The search context in use.
 * \param flags Flags controlling search direction/etc.
 * \param string String to match.
 */
static nserror
content_textsearch_step(struct textsearch_context *textsearch,
			 search_flags_t flags,
			 const char *string)
{
	size_t string_len;
	size_t i;

	if (textsearch == NULL || string == NULL) {
		return NSERROR_BAD_PARAMETER;
	}

	/*
	 * Broadcast recent query string.
	 */
	textsearch_broadcast(
		textsearch,
		CONTENT_TEXTSEARCH_RECENT,
		false,
		string);

	string_len = strlen(string);

	/*
	 * A query containing only '*' and '#' has no meaningful text
	 * anchor. Preserve the original special behavior.
	 */
	for (i = 0; i < string_len; i++) {
		if (string[i] != '#' && string[i] != '*') {
			break;
		}
	}

	if (i < string_len) {
		return search_text(
			textsearch,
			string,
			string_len,
			flags);
	}

	/*
	 * Wildcard-only search: clear existing matches.
	 */
	{
		union content_msg_data msg_data;

		free_matches(textsearch);

		textsearch->showing_all = false;

		textsearch_broadcast(
			textsearch,
			CONTENT_TEXTSEARCH_MATCH,
			true,
			NULL);

		textsearch_broadcast(
			textsearch,
			CONTENT_TEXTSEARCH_BACK,
			false,
			NULL);

		textsearch_broadcast(
			textsearch,
			CONTENT_TEXTSEARCH_FORWARD,
			false,
			NULL);

		msg_data.scroll.area = false;
		msg_data.scroll.x0 = 0;
		msg_data.scroll.y0 = 0;

		content_broadcast(
			textsearch->c,
			CONTENT_MSG_SCROLL,
			&msg_data);
	}

	return NSERROR_OK;
}


/**
 * Terminate a search.
 *
 * \param c Content to clear.
 */
static nserror
content_textsearch__clear(struct content *c)
{
	if (c == NULL) {
		return NSERROR_OK;
	}

	free(c->textsearch.string);
	c->textsearch.string = NULL;

	if (c->textsearch.context != NULL) {
		content_textsearch_destroy(c->textsearch.context);
		c->textsearch.context = NULL;
	}

	return NSERROR_OK;
}


/**
 * Create a search context.
 *
 * \param c Content the search context is connected to.
 * \param gui_data Context pointer passed to provider routines.
 * \param textsearch_out Pointer receiving new context.
 * \return NSERROR_OK on success.
 */
static nserror
content_textsearch_create(struct content *c,
			  void *gui_data,
			  struct textsearch_context **textsearch_out)
{
	struct textsearch_context *context;

	if (c == NULL || textsearch_out == NULL) {
		return NSERROR_BAD_PARAMETER;
	}

	if (c->handler->textsearch_find == NULL ||
	    c->handler->textsearch_bounds == NULL) {
		/*
		 * Content has no free text find handler.
		 */
		return NSERROR_NOT_IMPLEMENTED;
	}

	context = malloc(sizeof(*context));
	if (context == NULL) {
		return NSERROR_NOMEM;
	}

	/*
	 * Initialize the embedded sentinel.
	 *
	 * This replaces the separate heap allocation used by the original
	 * implementation.
	 */
	context->found.prev = NULL;
	context->found.next = NULL;
	context->found.start_idx = 0;
	context->found.end_idx = 0;
	context->found.start_box = NULL;
	context->found.end_box = NULL;
	context->found.sel = NULL;

	context->current = NULL;
	context->string = NULL;
	context->string_len = 0;
	context->prev_case_sens = false;
	context->newsearch = true;
	context->match_count = 0;
	context->showing_all = false;

	context->c = c;
	context->gui_p = gui_data;

	*textsearch_out = context;

	return NSERROR_OK;
}


/**
 * Find a string/pattern in a string.
 *
 * Pattern syntax:
 *
 *   '*' matches zero or more characters.
 *   '#' matches exactly one character.
 *   all other characters match literally.
 *
 * The pattern may match anywhere within the source string.
 *
 * \param string Source string.
 * \param s_len Source length.
 * \param pattern Search pattern.
 * \param p_len Pattern length.
 * \param case_sens Whether matching is case sensitive.
 * \param m_len Receives matched length.
 * \return Pointer to first matching substring, or NULL.
 */
const char *
content_textsearch_find_pattern(const char *string,
				int s_len,
				const char *pattern,
				int p_len,
				bool case_sens,
				unsigned int *m_len)
{
	/*
	 * Keep the search entirely iterative.
	 *
	 * The state array is retained from the original implementation's
	 * bounded backtracking approach, but the actual character matching
	 * uses cheaper ASCII folding.
	 */
	struct {
		const char *ss;
		const char *s;
		const char *p;
		bool first;
	} context[16];

	const char *ep;
	const char *es;
	const char *p;
	const char *ss;
	const char *s;

	bool first = true;
	int top = 0;

	if (string == NULL ||
	    pattern == NULL ||
	    m_len == NULL ||
	    s_len < 0 ||
	    p_len < 0) {
		return NULL;
	}

	if (p_len == 0) {
		*m_len = 1;
		return string;
	}

	ep = pattern + p_len;
	es = string + s_len;

	/*
	 * Virtual '*' allows the pattern to match anywhere in the source.
	 */
	p = pattern - 1;
	ss = string;
	s = string;

	while (p < ep) {
		bool matches;

		/*
		 * Wildcard/star handling.
		 */
		if (p < pattern || *p == '*') {
			char ch;

			/*
			 * Collapse consecutive stars.
			 */
			do {
				p++;
			} while (p < ep && *p == '*');

			/*
			 * Pattern consists entirely of stars.
			 */
			if (p >= ep) {
				break;
			}

			ch = *p;

			/*
			 * Find the next possible starting character.
			 *
			 * '#' matches anything, so no scan is necessary.
			 */
			if (ch != '#') {
				unsigned char target =
					(unsigned char)ch;

				if (!case_sens) {
					target =
						textsearch_fold_ascii(target);
				}

				while (s < es) {
					unsigned char source =
						(unsigned char)*s;

					if (case_sens) {
						if (source == target) {
							break;
						}
					} else if (textsearch_fold_ascii(source) ==
						   target) {
						break;
					}

					s++;
				}
			}

			if (s < es) {
				/*
				 * Save the state required to retry the star
				 * against the next source character.
				 */
				if (top < (int)NOF_ELEMENTS(context)) {
					context[top].ss = ss;
					context[top].s = s + 1;
					context[top].p = p - 1;
					context[top].first = first;
					top++;
				}

				if (first) {
					ss = s;
					first = false;
				}

				matches = true;
			} else {
				matches = false;
			}
		} else if (s < es) {
			/*
			 * Normal character / single-character wildcard.
			 */
			unsigned char ch =
				(unsigned char)*p;

			if (ch == '#') {
				matches = true;
			} else {
				matches = textsearch_char_equal(
					(unsigned char)*s,
					ch,
					case_sens);
			}

			if (matches && first) {
				ss = s;
				first = false;
			}
		} else {
			matches = false;
		}

		if (matches) {
			p++;
			s++;
			continue;
		}

		/*
		 * Match failed. Resume the most recent wildcard state.
		 */
		if (--top < 0) {
			return NULL;
		}

		ss = context[top].ss;
		s = context[top].s;
		p = context[top].p;
		first = context[top].first;
	}

	/*
	 * Pattern successfully consumed.
	 *
	 * Preserve the historical behavior of reporting at least one
	 * character for a successful match.
	 */
	*m_len = (unsigned int)max(s - ss, 1);

	return ss;
}


/**
 * Add a search match.
 *
 * \param context Search context.
 * \param start_idx Start position.
 * \param end_idx End position.
 * \param start_box Starting content box.
 * \param end_box Ending content box.
 */
nserror
content_textsearch_add_match(struct textsearch_context *context,
			     unsigned start_idx,
			     unsigned end_idx,
			     struct box *start_box,
			     struct box *end_box)
{
	struct list_entry *entry;
	struct list_entry *tail;

	if (context == NULL) {
		return NSERROR_BAD_PARAMETER;
	}

	entry = malloc(sizeof(*entry));
	if (entry == NULL) {
		return NSERROR_NOMEM;
	}

	entry->start_idx = start_idx;
	entry->end_idx = end_idx;
	entry->start_box = start_box;
	entry->end_box = end_box;
	entry->sel = NULL;
	entry->next = NULL;

	/*
	 * The sentinel's prev pointer is always the tail.
	 */
	tail = context->found.prev;

	entry->prev = tail;

	if (tail == NULL) {
		context->found.next = entry;
	} else {
		tail->next = entry;
	}

	context->found.prev = entry;
	context->match_count++;

	return NSERROR_OK;
}


/**
 * Determine whether a range is currently highlighted.
 *
 * \param textsearch Search context.
 * \param start_offset Start offset.
 * \param end_offset End offset.
 * \param start_idx Receives highlighted start index.
 * \param end_idx Receives highlighted end index.
 * \return true if highlighted.
 */
bool
content_textsearch_ishighlighted(struct textsearch_context *textsearch,
				  unsigned start_offset,
				  unsigned end_offset,
				  unsigned *start_idx,
				  unsigned *end_idx)
{
	struct list_entry *entry;

	if (textsearch == NULL) {
		return false;
	}

	for (entry = textsearch->found.next;
	     entry != NULL;
	     entry = entry->next) {

		struct selection *sel = entry->sel;

		if (sel != NULL &&
		    selection_highlighted(
			sel,
			start_offset,
			end_offset,
			start_idx,
			end_idx)) {
			return true;
		}
	}

	return false;
}


/**
 * Destroy a text search context.
 */
nserror
content_textsearch_destroy(struct textsearch_context *textsearch)
{
	if (textsearch == NULL) {
		return NSERROR_OK;
	}

	if (textsearch->string != NULL) {
		/*
		 * Broadcast recent query string before freeing it.
		 */
		textsearch_broadcast(
			textsearch,
			CONTENT_TEXTSEARCH_RECENT,
			false,
			textsearch->string);

		free(textsearch->string);
		textsearch->string = NULL;
		textsearch->string_len = 0;
	}

	/*
	 * Restore navigation controls.
	 */
	textsearch_broadcast(
		textsearch,
		CONTENT_TEXTSEARCH_BACK,
		true,
		NULL);

	textsearch_broadcast(
		textsearch,
		CONTENT_TEXTSEARCH_FORWARD,
		true,
		NULL);

	free_matches(textsearch);

	free(textsearch);

	return NSERROR_OK;
}


/**
 * Perform a text search.
 *
 * \param h Content cache handle.
 * \param context GUI/application context.
 * \param flags Search flags.
 * \param string Search string, or NULL to clear.
 */
nserror
content_textsearch(struct hlcache_handle *h,
		   void *context,
		   search_flags_t flags,
		   const char *string)
{
	struct content *c;
	nserror res;

	if (h == NULL) {
		return NSERROR_BAD_PARAMETER;
	}

	c = hlcache_handle_get_content(h);

	if (c == NULL) {
		return NSERROR_BAD_PARAMETER;
	}

	/*
	 * Continuing an identical search is the common navigation case.
	 */
	if (string != NULL &&
	    c->textsearch.string != NULL &&
	    c->textsearch.context != NULL &&
	    strcmp(string, c->textsearch.string) == 0) {

		return content_textsearch_step(
			c->textsearch.context,
			flags,
			string);
	}

	if (string != NULL) {
		size_t string_len = strlen(string);
		char *new_string = NULL;

		/*
		 * Allocate the new query before destroying the existing search.
		 * This prevents losing the current search if allocation fails.
		 */
		if (string_len != 0) {
			new_string = malloc(string_len + 1);

			if (new_string == NULL) {
				return NSERROR_NOMEM;
			}

			memcpy(new_string, string, string_len);
			new_string[string_len] = '\0';
		} else {
			new_string = strdup("");
			if (new_string == NULL) {
				return NSERROR_NOMEM;
			}
		}

		free(c->textsearch.string);
		c->textsearch.string = new_string;

		if (c->textsearch.context != NULL) {
			content_textsearch_destroy(
				c->textsearch.context);

			c->textsearch.context = NULL;
		}

		res = content_textsearch_create(
			c,
			context,
			&c->textsearch.context);

		if (res != NSERROR_OK) {
			return res;
		}

		/*
		 * Perform the actual search and propagate errors.
		 */
		return content_textsearch_step(
			c->textsearch.context,
			flags,
			string);
	}

	/*
	 * NULL query clears the search.
	 */
	return content_textsearch__clear(c);
}


/**
 * Clear text search state.
 */
nserror
content_textsearch_clear(struct hlcache_handle *h)
{
	struct content *c;

	if (h == NULL) {
		return NSERROR_BAD_PARAMETER;
	}

	c = hlcache_handle_get_content(h);

	if (c == NULL) {
		return NSERROR_BAD_PARAMETER;
	}

	return content_textsearch__clear(c);
}
