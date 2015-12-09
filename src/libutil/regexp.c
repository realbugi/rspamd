/*
 * Copyright (c) 2015, Vsevolod Stakhov
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *	 * Redistributions of source code must retain the above copyright
 *	   notice, this list of conditions and the following disclaimer.
 *	 * Redistributions in binary form must reproduce the above copyright
 *	   notice, this list of conditions and the following disclaimer in the
 *	   documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY AUTHOR ''AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "regexp.h"
#include "cryptobox.h"
#include "ref.h"
#include "util.h"
#include "rspamd.h"
#include <pcre.h>

typedef guchar regexp_id_t[rspamd_cryptobox_HASHBYTES];

#define RSPAMD_REGEXP_FLAG_RAW (1 << 1)
#define RSPAMD_REGEXP_FLAG_NOOPT (1 << 2)
#define RSPAMD_REGEXP_FLAG_FULL_MATCH (1 << 3)

struct rspamd_regexp_s {
	gdouble exec_time;
	gchar *pattern;
	pcre *re;
	pcre_extra *extra;
#ifdef HAVE_PCRE_JIT
	pcre_jit_stack *jstack;
	pcre_jit_stack *raw_jstack;
#endif
	pcre *raw_re;
	pcre_extra *raw_extra;
	regexp_id_t id;
	ref_entry_t ref;
	gpointer ud;
	gpointer re_class;
	guint64 cache_id;
	guint max_hits;
	gint flags;
	gint pcre_flags;
	gint ncaptures;
	gint nbackref;
};

struct rspamd_regexp_cache {
	GHashTable *tbl;
};

static struct rspamd_regexp_cache *global_re_cache = NULL;
static gboolean can_jit = FALSE;

static GQuark
rspamd_regexp_quark (void)
{
	return g_quark_from_static_string ("rspamd-regexp");
}

static void
rspamd_regexp_generate_id (const gchar *pattern, const gchar *flags,
		regexp_id_t out)
{
	rspamd_cryptobox_hash_state_t st;

	rspamd_cryptobox_hash_init (&st, NULL, 0);

	if (flags) {
		rspamd_cryptobox_hash_update (&st, flags, strlen (flags));
	}

	rspamd_cryptobox_hash_update (&st, pattern, strlen (pattern));
	rspamd_cryptobox_hash_final (&st, out);
}

static void
rspamd_regexp_dtor (rspamd_regexp_t *re)
{
	if (re) {
		if (re->raw_re && re->raw_re != re->re) {
			pcre_free (re->raw_re);
#ifdef HAVE_PCRE_JIT
			if (re->raw_extra) {
				pcre_free_study (re->raw_extra);
			}
			if (re->raw_jstack) {
				pcre_jit_stack_free (re->raw_jstack);
			}
#else
			pcre_free (re->raw_extra);
#endif
		}
		if (re->re) {
			pcre_free (re->re);
#ifdef HAVE_PCRE_JIT
			if (re->extra) {
				pcre_free_study (re->extra);
			}
			if (re->jstack) {
				pcre_jit_stack_free (re->jstack);
			}
#else
			pcre_free (re->extra);
#endif
		}

		if (re->pattern) {
			g_free (re->pattern);
		}
	}
}

rspamd_regexp_t*
rspamd_regexp_new (const gchar *pattern, const gchar *flags,
		GError **err)
{
	const gchar *start = pattern, *end, *flags_str = NULL, *err_str;
	rspamd_regexp_t *res;
	pcre *r;
	gchar sep = 0, *real_pattern;
	gint regexp_flags = 0, rspamd_flags = 0, err_off, study_flags = 0, ncaptures;
	gboolean strict_flags = FALSE;

	rspamd_regexp_library_init ();

	if (flags == NULL) {
		/* We need to parse pattern and detect flags set */
		if (*start == '/') {
			sep = '/';
		}
		else if (*start == 'm') {
			start ++;
			sep = *start;

			/* Paired braces */
			if (sep == '{') {
				sep = '}';
			}

			rspamd_flags |= RSPAMD_REGEXP_FLAG_FULL_MATCH;
		}
		if (sep == '\0' || g_ascii_isalnum (sep)) {
			/* We have no flags, no separators and just use all line as expr */
			start = pattern;
			end = start + strlen (pattern);
			rspamd_flags &= ~RSPAMD_REGEXP_FLAG_FULL_MATCH;
		}
		else {
			end = strrchr (pattern, sep);

			if (end == NULL || end <= start) {
				g_set_error (err, rspamd_regexp_quark(), EINVAL,
						"pattern is not enclosed with %c: %s",
						sep, pattern);
				return NULL;
			}
			flags_str = end + 1;
			start ++;
		}
	}
	else {
		/* Strictly check all flags */
		strict_flags = TRUE;
		start = pattern;
		end = pattern + strlen (pattern);
		flags_str = flags;
	}

	rspamd_flags |= RSPAMD_REGEXP_FLAG_RAW;

	regexp_flags |= PCRE_NEWLINE_ANYCRLF;
	regexp_flags &= ~PCRE_UTF8;

	if (flags_str != NULL) {
		while (*flags_str) {
			switch (*flags_str) {
			case 'i':
				regexp_flags |= PCRE_CASELESS;
				break;
			case 'm':
				regexp_flags |= PCRE_MULTILINE;
				break;
			case 's':
				regexp_flags |= PCRE_DOTALL;
				break;
			case 'x':
				regexp_flags |= PCRE_EXTENDED;
				break;
			case 'u':
				rspamd_flags &= ~RSPAMD_REGEXP_FLAG_RAW;
				regexp_flags |= PCRE_UTF8;
				break;
			case 'O':
				/* We optimize all regexps by default */
				rspamd_flags |= RSPAMD_REGEXP_FLAG_NOOPT;
				break;
			case 'r':
				rspamd_flags |= RSPAMD_REGEXP_FLAG_RAW;
				regexp_flags &= ~PCRE_UTF8;
				break;
			default:
				if (strict_flags) {
					g_set_error (err, rspamd_regexp_quark(), EINVAL,
							"invalid regexp flag: %c in pattern %s",
							*flags_str, pattern);
					return NULL;
				}
				msg_warn ("invalid flag '%c' in pattern %s", *flags_str, pattern);
				goto fin;
				break;
			}
			flags_str++;
		}
	}
fin:

	real_pattern = g_malloc (end - start + 1);
	rspamd_strlcpy (real_pattern, start, end - start + 1);

	r = pcre_compile (real_pattern, regexp_flags, &err_str, &err_off, NULL);

	if (r == NULL) {
		g_set_error (err, rspamd_regexp_quark(), EINVAL,
			"invalid regexp pattern: '%s': %s at position %d",
			pattern, err_str, err_off);
		g_free (real_pattern);

		return NULL;
	}

	/* Now allocate the target structure */
	res = g_slice_alloc0 (sizeof (*res));
	REF_INIT_RETAIN (res, rspamd_regexp_dtor);
	res->flags = rspamd_flags;
	res->pattern = real_pattern;
	res->cache_id = RSPAMD_INVALID_ID;
	res->pcre_flags = regexp_flags;
	res->max_hits = 0;

	if (rspamd_flags & RSPAMD_REGEXP_FLAG_RAW) {
		res->raw_re = r;
	}
	else {
		res->re = r;
		res->raw_re = pcre_compile (pattern, regexp_flags & ~PCRE_UTF8,
				&err_str, &err_off, NULL);

		if (res->raw_re == NULL) {
			msg_warn ("invalid raw regexp pattern: '%s': %s at position %d",
					pattern, err_str, err_off);
		}
	}

#ifdef HAVE_PCRE_JIT
	study_flags |= PCRE_STUDY_JIT_COMPILE;
#endif

	if (!(rspamd_flags & RSPAMD_REGEXP_FLAG_NOOPT)) {
		/* Optimize regexp */
		if (res->re) {
			res->extra = pcre_study (res->re, study_flags, &err_str);
			if (res->extra != NULL) {
#ifdef HAVE_PCRE_JIT
				gint jit, n;

				if (can_jit) {
					jit = 0;
					n = pcre_fullinfo (res->re, res->extra,
							PCRE_INFO_JIT, &jit);

					if (n != 0 || jit != 1) {
						msg_debug ("jit compilation of %s is not supported", pattern);
						res->jstack = NULL;
					}
					else {
						res->jstack = pcre_jit_stack_alloc (32 * 1024, 512 * 1024);
						pcre_assign_jit_stack (res->extra, NULL, res->jstack);
					}
				}
#endif
			}
			else {
				msg_warn ("cannot optimize regexp pattern: '%s': %s",
						pattern, err_str);
			}
		}

		if (res->raw_re) {
			if (res->raw_re != res->re) {
				res->raw_extra = pcre_study (res->raw_re, study_flags, &err_str);
				if (res->raw_extra != NULL) {
#ifdef HAVE_PCRE_JIT
					gint jit, n;

					if (can_jit) {
						jit = 0;
						n = pcre_fullinfo (res->raw_re, res->raw_extra,
								PCRE_INFO_JIT, &jit);

						if (n != 0 || jit != 1) {
							msg_debug ("jit compilation of %s is not supported",
									pattern);
							res->raw_jstack = NULL;
						}
						else {
							res->raw_jstack = pcre_jit_stack_alloc (32 * 1024,
									512 * 1024);
							pcre_assign_jit_stack (res->raw_extra, NULL,
									res->raw_jstack);
						}
					}
#endif
				}
				else {
					msg_warn ("cannot optimize raw regexp pattern: '%s': %s",
							pattern, err_str);
				}
			}
			else {
#ifdef HAVE_PCRE_JIT
				/* Just alias pointers */
				res->raw_extra = res->extra;
				res->raw_jstack = res->jstack;
#endif
			}
		}
	}

	rspamd_regexp_generate_id (pattern, flags, res->id);

	/* Check number of captures */
	if (pcre_fullinfo (res->raw_re, res->extra, PCRE_INFO_CAPTURECOUNT,
			&ncaptures) == 0) {
		res->ncaptures = ncaptures;
	}

	/* Check number of backrefs */
	if (pcre_fullinfo (res->raw_re, res->extra, PCRE_INFO_BACKREFMAX,
			&ncaptures) == 0) {
		res->nbackref = ncaptures;
	}

	return res;
}

gboolean
rspamd_regexp_search (rspamd_regexp_t *re, const gchar *text, gsize len,
		const gchar **start, const gchar **end, gboolean raw,
		GArray *captures)
{
	pcre *r;
	pcre_extra *ext;
#if defined(HAVE_PCRE_JIT) && defined(HAVE_PCRE_JIT_FAST)
	pcre_jit_stack *st = NULL;
#endif
	const gchar *mt;
	gsize remain = 0;
	gint rc, match_flags = 0, *ovec, ncaptures, i;

	g_assert (re != NULL);
	g_assert (text != NULL);

	if (len == 0) {
		len = strlen (text);
	}

	if (end != NULL && *end != NULL) {
		/* Incremental search */
		mt = (*end);

		if ((gint)len > (mt - text)) {
			remain = len - (mt - text);
		}
	}
	else {
		mt = text;
		remain = len;
	}

	if (remain == 0) {
		return FALSE;
	}

	match_flags = PCRE_NEWLINE_ANYCRLF;

	if ((re->flags & RSPAMD_REGEXP_FLAG_RAW) || raw) {
		r = re->raw_re;
		ext = re->raw_extra;
#if defined(HAVE_PCRE_JIT) && defined(HAVE_PCRE_JIT_FAST)
		st = re->raw_jstack;
#endif
	}
	else {
		r = re->re;
		ext = re->extra;
#if defined(HAVE_PCRE_JIT) && defined(HAVE_PCRE_JIT_FAST)
		if (g_utf8_validate (mt, remain, NULL)) {
			st = re->jstack;
		}
#endif
	}

	g_assert (r != NULL);
	ncaptures = (re->ncaptures + 1) * 3;
	ovec = g_alloca (sizeof (gint) * ncaptures);

	if (!(re->flags & RSPAMD_REGEXP_FLAG_NOOPT)) {
#ifdef HAVE_PCRE_JIT
# ifdef HAVE_PCRE_JIT_FAST
		/* XXX: flags seems to be broken with jit fast path */
		g_assert (remain > 0);
		g_assert (mt != NULL);

		if (st != NULL) {
			rc = pcre_jit_exec (r, ext, mt, remain, 0, 0, ovec,
					ncaptures, st);
		}
		else {
			rc = pcre_exec (r, ext, mt, remain, 0, match_flags, ovec,
					ncaptures);
		}
# else
		rc = pcre_exec (r, ext, mt, remain, 0, match_flags, ovec,
				ncaptures);
#endif
#else
		rc = pcre_exec (r, ext, mt, remain, 0, match_flags, ovec,
				ncaptures);
#endif
	}
	else {
		rc = pcre_exec (r, ext, mt, remain, 0, match_flags, ovec,
				ncaptures);
	}

	if (rc >= 0) {
		if (start) {
			*start = mt + ovec[0];
		}
		if (end) {
			*end = mt + ovec[1];
		}

		if (captures != NULL && rc > 1) {
			struct rspamd_re_capture *elt;

			g_assert (g_array_get_element_size (captures) ==
					sizeof (struct rspamd_re_capture));
			g_array_set_size (captures, rc);

			for (i = 0; i < rc; i ++) {
				elt = &g_array_index (captures, struct rspamd_re_capture, i);
				elt->p = mt + ovec[i * 2];
				elt->len = (mt + ovec[i * 2 + 1]) - elt->p;

			}
		}

		if (re->flags & RSPAMD_REGEXP_FLAG_FULL_MATCH) {
			/* We also ensure that the match is full */
			if (ovec[0] != 0 || (guint)ovec[1] < len) {
				return FALSE;
			}
		}

		return TRUE;
	}

	return FALSE;
}

const char*
rspamd_regexp_get_pattern (rspamd_regexp_t *re)
{
	g_assert (re != NULL);

	return re->pattern;
}

guint
rspamd_regexp_get_flags (rspamd_regexp_t *re)
{
	g_assert (re != NULL);

	return re->flags;
}

guint
rspamd_regexp_get_pcre_flags (rspamd_regexp_t *re)
{
	g_assert (re != NULL);

	return re->pcre_flags;
}

gint
rspamd_regexp_get_nbackrefs (rspamd_regexp_t *re)
{
	g_assert (re != NULL);

	return re->nbackref;
}

gint
rspamd_regexp_get_ncaptures (rspamd_regexp_t *re)
{
	g_assert (re != NULL);

	return re->ncaptures;
}

guint
rspamd_regexp_get_maxhits (rspamd_regexp_t *re)
{
	g_assert (re != NULL);

	return re->max_hits;
}

guint
rspamd_regexp_set_maxhits (rspamd_regexp_t *re, guint new_maxhits)
{
	guint old_hits;

	g_assert (re != NULL);
	old_hits = re->max_hits;
	re->max_hits = new_maxhits;

	return old_hits;
}

guint64
rspamd_regexp_get_cache_id (rspamd_regexp_t *re)
{
	g_assert (re != NULL);

	return re->cache_id;
}

guint64
rspamd_regexp_set_cache_id (rspamd_regexp_t *re, guint64 id)
{
	guint64 old;

	g_assert (re != NULL);
	old = re->cache_id;
	re->cache_id = id;

	return old;
}

gboolean
rspamd_regexp_match (rspamd_regexp_t *re, const gchar *text, gsize len,
		gboolean raw)
{
	const gchar *start = NULL, *end = NULL;

	g_assert (re != NULL);
	g_assert (text != NULL);

	if (rspamd_regexp_search (re, text, len, &start, &end, raw, NULL)) {
		if (start == text && end == text + len) {
			return TRUE;
		}
	}

	return FALSE;
}

void
rspamd_regexp_unref (rspamd_regexp_t *re)
{
	REF_RELEASE (re);
}

rspamd_regexp_t*
rspamd_regexp_ref (rspamd_regexp_t *re)
{
	g_assert (re != NULL);

	REF_RETAIN (re);

	return re;
}

void
rspamd_regexp_set_ud (rspamd_regexp_t *re, gpointer ud)
{
	g_assert (re != NULL);

	re->ud = ud;
}

gpointer
rspamd_regexp_get_ud (rspamd_regexp_t *re)
{
	g_assert (re != NULL);

	return re->ud;
}

gboolean
rspamd_regexp_equal (gconstpointer a, gconstpointer b)
{
	const guchar *ia = a, *ib = b;

	return (memcmp (ia, ib, sizeof (regexp_id_t)) == 0);
}

guint32
rspamd_regexp_hash (gconstpointer a)
{
	const guchar *ia = a;
	guint32 res;

	memcpy (&res, ia, sizeof (res));

	return res;
}

gboolean
rspamd_regexp_cmp (gconstpointer a, gconstpointer b)
{
	const guchar *ia = a, *ib = b;

	return memcmp (ia, ib, sizeof (regexp_id_t));
}

struct rspamd_regexp_cache*
rspamd_regexp_cache_new (void)
{
	struct rspamd_regexp_cache *ncache;

	ncache = g_slice_alloc (sizeof (*ncache));
	ncache->tbl = g_hash_table_new_full (rspamd_regexp_hash, rspamd_regexp_equal,
			NULL, (GDestroyNotify)rspamd_regexp_unref);

	return ncache;
}


rspamd_regexp_t*
rspamd_regexp_cache_query (struct rspamd_regexp_cache* cache,
		const gchar *pattern,
		const gchar *flags)
{
	rspamd_regexp_t *res = NULL;
	regexp_id_t id;

	if (cache == NULL) {
		rspamd_regexp_library_init ();
		cache = global_re_cache;
	}

	g_assert (cache != NULL);
	rspamd_regexp_generate_id (pattern, flags, id);

	res = g_hash_table_lookup (cache->tbl, id);

	return res;
}


rspamd_regexp_t*
rspamd_regexp_cache_create (struct rspamd_regexp_cache *cache,
		const gchar *pattern,
		const gchar *flags, GError **err)
{
	rspamd_regexp_t *res;

	if (cache == NULL) {
		rspamd_regexp_library_init ();
		cache = global_re_cache;
	}

	g_assert (cache != NULL);
	res = rspamd_regexp_cache_query (cache, pattern, flags);

	if (res != NULL) {
		return res;
	}

	res = rspamd_regexp_new (pattern, flags, err);

	if (res) {
		REF_RETAIN (res);
		g_hash_table_insert (cache->tbl, res->id, res);
	}

	return res;
}

void rspamd_regexp_cache_insert (struct rspamd_regexp_cache* cache,
		const gchar *pattern,
		const gchar *flags, rspamd_regexp_t *re)
{
	g_assert (re != NULL);
	g_assert (pattern != NULL);

	if (cache == NULL) {
		rspamd_regexp_library_init ();
		cache = global_re_cache;
	}

	g_assert (cache != NULL);
	/* Generate custom id */
	rspamd_regexp_generate_id (pattern, flags, re->id);

	REF_RETAIN (re);
	g_hash_table_insert (cache->tbl, re->id, re);
}

gboolean
rspamd_regexp_cache_remove (struct rspamd_regexp_cache *cache,
		rspamd_regexp_t *re)
{
	if (cache == NULL) {
		cache = global_re_cache;
	}

	g_assert (cache != NULL);
	g_assert (re != NULL);

	return g_hash_table_remove (cache->tbl, re->id);
}

void
rspamd_regexp_cache_destroy (struct rspamd_regexp_cache *cache)
{
	if (cache != NULL) {
		g_hash_table_destroy (cache->tbl);
	}
}

void
rspamd_regexp_library_init (void)
{
	if (global_re_cache == NULL) {
		global_re_cache = rspamd_regexp_cache_new ();
#ifdef HAVE_PCRE_JIT
		gint jit, rc;
		const gchar *str;


		rc = pcre_config (PCRE_CONFIG_JIT, &jit);

		if (rc == 0 && jit == 1) {
#ifdef PCRE_CONFIG_JITTARGET
			pcre_config (PCRE_CONFIG_JITTARGET, &str);
			msg_info ("pcre is compiled with JIT for %s", str);
#else
			msg_info ("pcre is compiled with JIT for unknown target");
#endif

			can_jit = TRUE;
		}
		else {
			msg_info ("pcre is compiled without JIT support, so many optimisations"
					" are impossible");
		}
#else
		msg_info ("pcre is too old and has no JIT support, so many optimisations"
				" are impossible");
#endif
	}
}

void
rspamd_regexp_library_finalize (void)
{
	if (global_re_cache != NULL) {
		rspamd_regexp_cache_destroy (global_re_cache);
	}
}

gpointer
rspamd_regexp_get_id (rspamd_regexp_t *re)
{
	g_assert (re != NULL);

	return re->id;
}

gpointer
rspamd_regexp_get_class (rspamd_regexp_t *re)
{
	g_assert (re != NULL);

	return re->re_class;
}

gpointer
rspamd_regexp_set_class (rspamd_regexp_t *re, gpointer re_class)
{
	gpointer old_class;

	g_assert (re != NULL);

	old_class = re->re_class;
	re->re_class = re_class;

	return old_class;
}
