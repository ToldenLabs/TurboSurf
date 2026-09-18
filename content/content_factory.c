/*
 * Copyright 2011 John-Mark Bell <jmb@netsurf-browser.org>
 *
 * This file is part of NetSurf, http://www.netsurf-browser.org/
 *
 * NetSurf is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * NetSurf is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * \file
 * Content factory implementation.
 */

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "utils/http.h"

#include "content/content.h"
#include "content/content_factory.h"
#include "content/content_protected.h"
#include "content/llcache.h"

#define CONTENT_HANDLER_BUCKETS 64

/**
 * Entry in list of content handlers.
 *
 * Entries are stored in a small hash table rather than a single
 * global linked list. MIME types are still compared using
 * libwapcaplet's case-insensitive comparison to preserve behavior.
 */
typedef struct content_handler_entry {
	/** Next entry in this hash bucket. */
	struct content_handler_entry *next;

	/** MIME type handled by handler. */
	lwc_string *mime_type;

	/** Precomputed case-insensitive hash. */
	uint32_t hash;

	/** Length of the MIME type. */
	size_t length;

	/** Content handler object. */
	const content_handler *handler;
} content_handler_entry;

/**
 * Hash table of registered content handlers.
 */
static content_handler_entry *
	content_handlers[CONTENT_HANDLER_BUCKETS];

/**
 * Hash a MIME type.
 *
 * MIME types are ASCII in normal HTTP usage, so ASCII case folding is
 * sufficient for the hash. The final equality check still uses
 * lwc_string_caseless_isequal(), so hash collisions and unusual input
 * remain correct.
 *
 * FNV-1a is deliberately used here because it is tiny and fast.
 */
static inline uint32_t
content_mime_hash(const char *data, size_t length)
{
	uint32_t hash = UINT32_C(2166136261);

	for (size_t i = 0; i < length; i++) {
		unsigned char c = (unsigned char)data[i];

		if (c >= 'A' && c <= 'Z')
			c = (unsigned char)(c + ('a' - 'A'));

		hash ^= c;
		hash *= UINT32_C(16777619);
	}

	return hash;
}

/**
 * Calculate hash bucket for a MIME type.
 */
static inline unsigned int
content_handler_bucket(uint32_t hash)
{
	/*
	 * CONTENT_HANDLER_BUCKETS is a power of two, allowing a cheap
	 * mask instead of modulo.
	 */
	return hash & (CONTENT_HANDLER_BUCKETS - 1);
}

/**
 * Clean up after the content factory.
 */
void content_factory_fini(void)
{
	for (unsigned int bucket = 0;
	     bucket < CONTENT_HANDLER_BUCKETS;
	     bucket++) {

		content_handler_entry *entry = content_handlers[bucket];

		content_handlers[bucket] = NULL;

		while (entry != NULL) {
			content_handler_entry *victim = entry;

			entry = entry->next;

			if (victim->handler->fini != NULL)
				victim->handler->fini();

			lwc_string_unref(victim->mime_type);
			free(victim);
		}
	}
}

/**
 * Register a handler with the content factory.
 *
 * \param mime_type MIME type to handle
 * \param handler Content handler for MIME type
 * \return NSERROR_OK on success, appropriate error otherwise
 *
 * \note Latest registration for a MIME type wins.
 */
nserror content_factory_register_handler(const char *mime_type,
		const content_handler *handler)
{
	lwc_string *imime_type;
	lwc_error lerror;
	content_handler_entry *entry;
	content_handler_entry **link;
	const char *data;
	size_t length;
	uint32_t hash;
	unsigned int bucket;
	bool match;

	assert(mime_type != NULL);
	assert(handler != NULL);

	length = strlen(mime_type);

	lerror = lwc_intern_string(mime_type, length, &imime_type);
	if (lerror != lwc_error_ok)
		return NSERROR_NOMEM;

	/*
	 * Calculate the hash once and retain it with the entry.
	 * This avoids recalculating it during every lookup.
	 */
	data = lwc_string_data(imime_type);
	hash = content_mime_hash(data, length);
	bucket = content_handler_bucket(hash);

	/*
	 * Search only the relevant bucket.
	 *
	 * Hash and length comparisons are intentionally performed before
	 * the more expensive case-insensitive lwc comparison.
	 */
	for (entry = content_handlers[bucket];
	     entry != NULL;
	     entry = entry->next) {

		if (entry->hash != hash ||
		    entry->length != length)
			continue;

		if (lwc_string_caseless_isequal(
				imime_type,
				entry->mime_type,
				&match) != lwc_error_ok ||
		    !match)
			continue;

		/*
		 * Existing handler: replace it.
		 */
		lwc_string_unref(imime_type);

		entry->handler = handler;

		return NSERROR_OK;
	}

	/*
	 * New handler.
	 */
	entry = malloc(sizeof(*entry));
	if (entry == NULL) {
		lwc_string_unref(imime_type);
		return NSERROR_NOMEM;
	}

	entry->mime_type = imime_type;
	entry->handler = handler;
	entry->hash = hash;
	entry->length = length;

	/*
	 * Insert at the head of the bucket.
	 */
	link = &content_handlers[bucket];

	entry->next = *link;
	*link = entry;

	return NSERROR_OK;
}

/**
 * Find a handler for a MIME type.
 *
 * \param mime_type MIME type to search for
 * \return Associated handler, or NULL if none
 */
static inline const content_handler *
content_lookup(lwc_string *mime_type)
{
	content_handler_entry *entry;
	const char *data;
	size_t length;
	uint32_t hash;
	unsigned int bucket;
	bool match;

	assert(mime_type != NULL);

	data = lwc_string_data(mime_type);
	length = lwc_string_length(mime_type);

	hash = content_mime_hash(data, length);
	bucket = content_handler_bucket(hash);

	for (entry = content_handlers[bucket];
	     entry != NULL;
	     entry = entry->next) {

		/*
		 * Cheap rejection first.
		 */
		if (entry->hash != hash ||
		    entry->length != length)
			continue;

		/*
		 * Only perform the full case-insensitive comparison
		 * after hash and length have matched.
		 */
		if (lwc_string_caseless_isequal(
				mime_type,
				entry->mime_type,
				&match) != lwc_error_ok)
			continue;

		if (match)
			return entry->handler;
	}

	return NULL;
}

/**
 * Compute the generic content type for a MIME type.
 *
 * \param mime_type MIME type to consider
 * \return Generic content type
 */
content_type
content_factory_type_from_mime_type(lwc_string *mime_type)
{
	const content_handler *handler;

	handler = content_lookup(mime_type);

	if (handler == NULL)
		return CONTENT_NONE;

	return handler->type();
}

/**
 * Create a content object.
 *
 * \param llcache Underlying source data handle
 * \param fallback_charset Character set to fall back to if none specified
 * \param quirks Quirkiness of containing document
 * \param effective_type Effective MIME type of content
 * \return Pointer to content object, or NULL on failure
 */
struct content *
content_factory_create_content(llcache_handle *llcache,
		const char *fallback_charset,
		bool quirks,
		lwc_string *effective_type)
{
	struct content *c = NULL;
	const char *content_type_header;
	const content_handler *handler;
	http_content_type *ct = NULL;
	nserror error;

	/*
	 * Fast handler lookup.
	 */
	handler = content_lookup(effective_type);
	if (handler == NULL)
		return NULL;

	assert(handler->create != NULL);

	/*
	 * Obtain the declared Content-Type header.
	 */
	content_type_header =
		llcache_handle_get_header(llcache, "Content-Type");

	if (content_type_header != NULL) {
		/*
		 * Failure is intentionally ignored, matching the original
		 * behavior.
		 */
		(void)http_parse_content_type(content_type_header, &ct);
	}

	/*
	 * Create the content object.
	 */
	error = handler->create(
		handler,
		effective_type,
		ct != NULL ? ct->parameters : NULL,
		llcache,
		fallback_charset,
		quirks,
		&c);

	/*
	 * The parsed header is no longer required after create().
	 */
	if (ct != NULL)
		http_content_type_destroy(ct);

	if (error != NSERROR_OK)
		return NULL;

	return c;
}
