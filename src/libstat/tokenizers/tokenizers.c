/*-
 * Copyright 2016 Vsevolod Stakhov
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
/*
 * Common tokenization functions
 */

#include "rspamd.h"
#include "tokenizers.h"
#include "stat_internal.h"
#include "contrib/mumhash/mum.h"
#include "libmime/lang_detection.h"
#include "libstemmer.h"

#include <unicode/utf8.h>
#include <unicode/uchar.h>
#include <unicode/uiter.h>
#include <unicode/ubrk.h>
#include <unicode/ucnv.h>
#if U_ICU_VERSION_MAJOR_NUM >= 44
#include <unicode/unorm2.h>
#endif

#include <math.h>

typedef gboolean (*token_get_function) (rspamd_stat_token_t * buf, gchar const **pos,
		rspamd_stat_token_t * token,
		GList **exceptions, gsize *rl, gboolean check_signature);

const gchar t_delimiters[255] = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 1,
	1, 0, 0, 1, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 1, 0, 1, 1, 1, 1, 1, 0,
	1, 1, 1, 1, 1, 1, 1, 1, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 1, 1,
	1, 1, 1, 1, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 1, 1, 1, 1, 1, 1, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 1, 1, 1, 1, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0
};

/* Get next word from specified f_str_t buf */
static gboolean
rspamd_tokenizer_get_word_raw (rspamd_stat_token_t * buf,
		gchar const **cur, rspamd_stat_token_t * token,
		GList **exceptions, gsize *rl, gboolean unused)
{
	gsize remain, pos;
	const gchar *p;
	struct rspamd_process_exception *ex = NULL;

	if (buf == NULL) {
		return FALSE;
	}

	g_assert (cur != NULL);

	if (exceptions != NULL && *exceptions != NULL) {
		ex = (*exceptions)->data;
	}

	if (token->original.begin == NULL || *cur == NULL) {
		if (ex != NULL) {
			if (ex->pos == 0) {
				token->original.begin = buf->original.begin + ex->len;
				token->original.len = ex->len;
				token->flags = RSPAMD_STAT_TOKEN_FLAG_EXCEPTION;
			}
			else {
				token->original.begin = buf->original.begin;
				token->original.len = 0;
			}
		}
		else {
			token->original.begin = buf->original.begin;
			token->original.len = 0;
		}
		*cur = token->original.begin;
	}

	token->original.len = 0;

	pos = *cur - buf->original.begin;
	if (pos >= buf->original.len) {
		return FALSE;
	}

	remain = buf->original.len - pos;
	p = *cur;

	/* Skip non delimiters symbols */
	do {
		if (ex != NULL && ex->pos == pos) {
			/* Go to the next exception */
			*exceptions = g_list_next (*exceptions);
			*cur = p + ex->len;
			return TRUE;
		}
		pos++;
		p++;
		remain--;
	} while (remain > 0 && t_delimiters[(guchar)*p]);

	token->original.begin = p;

	while (remain > 0 && !t_delimiters[(guchar)*p]) {
		if (ex != NULL && ex->pos == pos) {
			*exceptions = g_list_next (*exceptions);
			*cur = p + ex->len;
			return TRUE;
		}
		token->original.len++;
		pos++;
		remain--;
		p++;
	}

	if (remain == 0) {
		return FALSE;
	}

	if (rl) {
		*rl = token->original.len;
	}

	token->flags = RSPAMD_STAT_TOKEN_FLAG_TEXT;

	*cur = p;

	return TRUE;
}

static inline gboolean
rspamd_tokenize_check_limit (gboolean decay,
							 guint word_decay,
							 guint nwords,
							 guint64 *hv,
							 guint64 *prob,
							 const rspamd_stat_token_t *token,
							 gssize remain,
							 gssize total)
{
	static const gdouble avg_word_len = 6.0;

	if (!decay) {
		if (token->original.len >= sizeof (guint64)) {
#ifdef _MUM_UNALIGNED_ACCESS
			*hv = mum_hash_step (*hv, *(guint64 *)token->original.begin);
#else
			guint64 tmp;
			memcpy (&tmp, token->original.begin, sizeof (tmp));
			*hv = mum_hash_step (*hv, tmp);
#endif
		}

		/* Check for decay */
		if (word_decay > 0 && nwords > word_decay && remain < (gssize)total) {
			/* Start decay */
			gdouble decay_prob;

			*hv = mum_hash_finish (*hv);

			/* We assume that word is 6 symbols length in average */
			decay_prob = (gdouble)word_decay / ((total - (remain)) / avg_word_len) * 10;
			decay_prob = floor (decay_prob) / 10.0;

			if (decay_prob >= 1.0) {
				*prob = G_MAXUINT64;
			}
			else {
				*prob = decay_prob * G_MAXUINT64;
			}

			return TRUE;
		}
	}
	else {
		/* Decaying probability */
		/* LCG64 x[n] = a x[n - 1] + b mod 2^64 */
		*hv = (*hv) * 2862933555777941757ULL + 3037000493ULL;

		if (*hv > *prob) {
			return TRUE;
		}
	}

	return FALSE;
}

static inline gboolean
rspamd_utf_word_valid (const gchar *text, const gchar *end,
		gint32 start, gint32 finish)
{
	const gchar *st = text + start, *fin = text + finish;
	UChar32 c;

	if (st >= end || fin > end || st >= fin) {
		return FALSE;
	}

	U8_NEXT (text, start, finish, c);

	if (u_isalnum (c)) {
		return TRUE;
	}

	return FALSE;
}
#define SHIFT_EX do { \
    cur = g_list_next (cur); \
    if (cur) { \
        ex = (struct rspamd_process_exception *) cur->data; \
    } \
    else { \
        ex = NULL; \
    } \
} while(0)

GArray *
rspamd_tokenize_text (const gchar *text, gsize len,
					  const UText *utxt,
					  enum rspamd_tokenize_type how,
					  struct rspamd_config *cfg,
					  GList *exceptions,
					  guint64 *hash)
{
	rspamd_stat_token_t token, buf;
	const gchar *pos = NULL;
	gsize l = 0;
	GArray *res;
	GList *cur = exceptions;
	guint min_len = 0, max_len = 0, word_decay = 0, initial_size = 128;
	guint64 hv = 0;
	gboolean decay = FALSE;
	guint64 prob = 0;
	static UBreakIterator* bi = NULL;

	if (text == NULL) {
		return NULL;
	}

	buf.original.begin = text;
	buf.original.len = len;
	buf.flags = 0;
	token.original.begin = NULL;
	token.original.len = 0;
	token.flags = 0;

	if (cfg != NULL) {
		min_len = cfg->min_word_len;
		max_len = cfg->max_word_len;
		word_decay = cfg->words_decay;
		initial_size = word_decay * 2;
	}

	res = g_array_sized_new (FALSE, FALSE, sizeof (rspamd_stat_token_t),
			initial_size);

	if (G_UNLIKELY (how == RSPAMD_TOKENIZE_RAW || utxt == NULL)) {
		while (rspamd_tokenizer_get_word_raw (&buf, &pos, &token, &cur, &l, FALSE)) {
			if (l == 0 || (min_len > 0 && l < min_len) ||
				(max_len > 0 && l > max_len)) {
				token.original.begin = pos;
				continue;
			}

			if (token.original.len > 0 &&
				rspamd_tokenize_check_limit (decay, word_decay, res->len,
					&hv, &prob, &token, pos - text, len)) {
				if (!decay) {
					decay = TRUE;
				}
				else {
					token.original.begin = pos;
					continue;
				}
			}

			g_array_append_val (res, token);
			token.original.begin = pos;
		}
	}
	else {
		/* UTF8 boundaries */
		UErrorCode uc_err = U_ZERO_ERROR;
		int32_t last, p;
		struct rspamd_process_exception *ex = NULL;

		if (bi == NULL) {
			bi = ubrk_open (UBRK_WORD, NULL, NULL, 0, &uc_err);

			g_assert (U_SUCCESS (uc_err));
		}

		ubrk_setUText (bi, (UText*)utxt, &uc_err);
		last = ubrk_first (bi);
		p = last;

		if (cur) {
			ex = (struct rspamd_process_exception *)cur->data;
		}

		while (p != UBRK_DONE) {
start_over:
			token.original.len = 0;

			if (p > last) {
				if (ex && cur) {
					/* Check exception */
					if (ex->pos >= last && ex->pos <= p) {
						/* We have an exception within boundary */
						/* First, start to drain exceptions from the start */
						while (cur && ex->pos <= last) {
							/* We have an exception at the beginning, skip those */
							last += ex->len;

							if (ex->type == RSPAMD_EXCEPTION_URL) {
								token.original.begin = "!!EX!!";
								token.original.len = sizeof ("!!EX!!") - 1;
								token.flags = RSPAMD_STAT_TOKEN_FLAG_EXCEPTION;

								g_array_append_val (res, token);
								token.flags = 0;
							}

							if (last > p) {
								/* Exception spread over the boundaries */
								while (last > p && p != UBRK_DONE) {
									p = ubrk_next (bi);
								}

								/* We need to reset our scan with new p and last */
								SHIFT_EX;
								goto start_over;
							}

							SHIFT_EX;
						}

						/* Now, we can have an exception within boundary again */
						if (cur && ex->pos >= last && ex->pos <= p) {
							/* Append the first part */
							if (rspamd_utf_word_valid (text, text + len, last,
									ex->pos)) {
								token.original.begin = text + last;
								token.original.len = ex->pos - last;
								token.flags = 0;
								g_array_append_val (res, token);
							}

							/* Process the current exception */
							last += ex->len + (ex->pos - last);

							if (ex->type == RSPAMD_EXCEPTION_URL) {
								token.original.begin = "!!EX!!";
								token.original.len = sizeof ("!!EX!!") - 1;
								token.flags = RSPAMD_STAT_TOKEN_FLAG_EXCEPTION;

								g_array_append_val (res, token);
							}

							if (last > p) {
								/* Exception spread over the boundaries */
								while (last > p && p != UBRK_DONE) {
									p = ubrk_next (bi);
								}
								/* We need to reset our scan with new p and last */
								SHIFT_EX;
								goto start_over;
							}

							SHIFT_EX;
						}
						else if (p > last) {
							if (rspamd_utf_word_valid (text, text + len, last, p)) {
								token.original.begin = text + last;
								token.original.len = p - last;
								token.flags = RSPAMD_STAT_TOKEN_FLAG_TEXT |
											  RSPAMD_STAT_TOKEN_FLAG_UTF;
							}
						}
					}
					else if (ex->pos < last) {
						/* Forward exceptions list */
						while (cur && ex->pos <= last) {
							/* We have an exception at the beginning, skip those */
							SHIFT_EX;
						}

						if (rspamd_utf_word_valid (text, text + len, last, p)) {
							token.original.begin = text + last;
							token.original.len = p - last;
							token.flags = RSPAMD_STAT_TOKEN_FLAG_TEXT |
										  RSPAMD_STAT_TOKEN_FLAG_UTF;
						}
					}
					else {
						/* No exceptions within boundary */
						if (rspamd_utf_word_valid (text, text + len, last, p)) {
							token.original.begin = text + last;
							token.original.len = p - last;
							token.flags = RSPAMD_STAT_TOKEN_FLAG_TEXT |
										  RSPAMD_STAT_TOKEN_FLAG_UTF;
						}
					}
				}
				else {
					if (rspamd_utf_word_valid (text, text + len, last, p)) {
						token.original.begin = text + last;
						token.original.len = p - last;
						token.flags = RSPAMD_STAT_TOKEN_FLAG_TEXT |
									  RSPAMD_STAT_TOKEN_FLAG_UTF;
					}
				}

				if (token.original.len > 0 &&
					rspamd_tokenize_check_limit (decay, word_decay, res->len,
						&hv, &prob, &token, p, len)) {
					if (!decay) {
						decay = TRUE;
					} else {
						token.original.len = 0;
					}
				}
			}

			if (token.original.len > 0) {
				g_array_append_val (res, token);
			}

			last = p;
			p = ubrk_next (bi);
		}
	}

	if (!decay) {
		hv = mum_hash_finish (hv);
	}

	if (hash) {
		*hash = hv;
	}

	return res;
}

#undef SHIFT_EX

GArray *
rspamd_tokenize_subject (struct rspamd_task *task)
{
	UText utxt = UTEXT_INITIALIZER;
	UErrorCode uc_err = U_ZERO_ERROR;
	gsize slen;
	gboolean valid_utf = TRUE;
	GArray *words = NULL;
	guint i = 0;
	gint32 uc;
	rspamd_stat_token_t *tok;

	if (task->subject) {
		const gchar *p = task->subject;

		slen = strlen (task->subject);

		while (i < slen) {
			U8_NEXT (p, i, slen, uc);

			if (((gint32) uc) < 0) {
				valid_utf = FALSE;
				break;
			}
#if U_ICU_VERSION_MAJOR_NUM < 50
			if (u_isalpha (uc)) {
				gint32 sc = ublock_getCode (uc);

				if (sc == UBLOCK_THAI) {
					valid_utf = FALSE;
					msg_info_task ("enable workaround for Thai characters for old libicu");
					break;
				}
			}
#endif
		}

		if (valid_utf) {
			utext_openUTF8 (&utxt,
					task->subject,
					slen,
					&uc_err);

			words = rspamd_tokenize_text (task->subject, slen,
					&utxt, RSPAMD_TOKENIZE_UTF,
					task->cfg, NULL, NULL);

			utext_close (&utxt);
		}
		else {
			words = rspamd_tokenize_text (task->subject, slen,
					NULL, RSPAMD_TOKENIZE_RAW,
					task->cfg, NULL, NULL);
		}
	}

	if (words != NULL) {

		for (i = 0; i < words->len; i++) {
			tok = &g_array_index (words, rspamd_stat_token_t, i);
			tok->flags |= RSPAMD_STAT_TOKEN_FLAG_SUBJECT;
		}
	}

	return words;
}

void
rspamd_normalize_words (GArray *words, rspamd_mempool_t *pool)
{
	rspamd_stat_token_t *tok;
	guint i;
	UErrorCode uc_err = U_ZERO_ERROR;
	guint clen, dlen;
	gint r;
	UConverter *utf8_converter;
#if U_ICU_VERSION_MAJOR_NUM >= 44
	const UNormalizer2 *norm = rspamd_get_unicode_normalizer ();
	gint32 end;
	UChar *src = NULL, *dest = NULL;
#endif

	utf8_converter = rspamd_get_utf8_converter ();

	for (i = 0; i < words->len; i++) {
		tok = &g_array_index (words, rspamd_stat_token_t, i);

		if (tok->flags & RSPAMD_STAT_TOKEN_FLAG_UTF) {
			UChar *unicode;
			gchar *utf8;
			gsize ulen;

			uc_err = U_ZERO_ERROR;
			ulen = tok->original.len;
			unicode = rspamd_mempool_alloc (pool, sizeof (UChar) * (ulen + 1));
			ulen = ucnv_toUChars (utf8_converter,
					unicode,
					tok->original.len + 1,
					tok->original.begin,
					tok->original.len,
					&uc_err);


			if (!U_SUCCESS (uc_err)) {
				tok->flags |= RSPAMD_STAT_TOKEN_FLAG_BROKEN_UNICODE;
				tok->unicode.begin = NULL;
				tok->unicode.len = 0;
				tok->normalized.begin = NULL;
				tok->normalized.len = 0;
			}
			else {
				/* Perform normalization if available and needed */
#if U_ICU_VERSION_MAJOR_NUM >= 44
				/* We can now check if we need to decompose */
				end = unorm2_spanQuickCheckYes (norm, src, ulen, &uc_err);

				if (!U_SUCCESS (uc_err)) {
					tok->unicode.begin = unicode;
					tok->unicode.len = ulen;
					tok->normalized.begin = NULL;
					tok->normalized.len = 0;
					tok->flags |= RSPAMD_STAT_TOKEN_FLAG_BROKEN_UNICODE;
				}
				else {
					if (end == ulen) {
						/* Already normalised */
						tok->unicode.begin = unicode;
						tok->unicode.len = ulen;
						tok->normalized.begin = tok->original.begin;
						tok->normalized.len = tok->original.len;
					}
					else {
						/* Perform normalization */

						dest = rspamd_mempool_alloc (pool, ulen * sizeof (UChar));
						/* First part */
						memcpy (dest, src, end * sizeof (*dest));
						/* Second part */
						ulen = unorm2_normalizeSecondAndAppend (norm, dest, end,
								ulen,
								src + end, ulen - end, &uc_err);

						if (!U_SUCCESS (uc_err)) {
							if (uc_err != U_BUFFER_OVERFLOW_ERROR) {
								msg_warn_pool_check ("cannot normalise text '%*s': %s",
										(gint)tok->original.len, tok->original.begin,
										u_errorName (uc_err));
								tok->unicode.begin = unicode;
								tok->unicode.len = ulen;
								tok->normalized.begin = NULL;
								tok->normalized.len = 0;
								tok->flags |= RSPAMD_STAT_TOKEN_FLAG_BROKEN_UNICODE;
							}
						}
						else {
							/* Copy normalised back */
							tok->unicode.begin = dest;
							tok->unicode.len = ulen;
							tok->flags |= RSPAMD_STAT_TOKEN_FLAG_NORMALISED;

							/* Convert utf8 to produce normalized part */
							clen = ucnv_getMaxCharSize (utf8_converter);
							dlen = UCNV_GET_MAX_BYTES_FOR_STRING (ulen, clen);

							utf8 = rspamd_mempool_alloc (pool,
									sizeof (*utf8) * dlen + 1);
							r = ucnv_fromUChars (utf8_converter,
									utf8,
									dlen,
									dest,
									ulen,
									&uc_err);
							utf8[r] = '\0';

							tok->normalized.begin = utf8;
							tok->normalized.len = r;
						}
					}
				}
#else
				/* Legacy libicu path */
				tok->unicode.begin = unicode;
				tok->unicode.len = ulen;
				tok->normalized.begin = tok->original.begin;
				tok->normalized.len = tok->original.len;
#endif
			}
		}
	}
}

void
rspamd_stem_words (GArray *words, rspamd_mempool_t *pool,
				   const gchar *language,
				   struct rspamd_lang_detector *d)
{
	static GHashTable *stemmers = NULL;
	struct sb_stemmer *stem = NULL;
	guint i;
	rspamd_stat_token_t *tok;
	gchar *dest;
	gsize dlen;

	if (!stemmers) {
		stemmers = g_hash_table_new (rspamd_strcase_hash,
				rspamd_strcase_equal);
	}

	if (language && language[0] != '\0') {
		stem = g_hash_table_lookup (stemmers, language);

		if (stem == NULL) {

			stem = sb_stemmer_new (language, "UTF_8");

			if (stem == NULL) {
				msg_debug_pool (
						"<%s> cannot create lemmatizer for %s language",
						language);
				g_hash_table_insert (stemmers, g_strdup (language),
						GINT_TO_POINTER (-1));
			}
			else {
				g_hash_table_insert (stemmers, g_strdup (language),
						stem);
			}
		}
		else if (stem == GINT_TO_POINTER (-1)) {
			/* Negative cache */
			stem = NULL;
		}
	}
	for (i = 0; i < words->len; i++) {
		tok = &g_array_index (words, rspamd_stat_token_t, i);

		if (tok->flags & RSPAMD_STAT_TOKEN_FLAG_UTF) {
			if (stem) {
				const gchar *stemmed;

				stemmed = sb_stemmer_stem (stem,
						tok->normalized.begin, tok->normalized.len);

				dlen = strlen (stemmed);

				if (dlen > 0) {
					dest = rspamd_mempool_alloc (pool, dlen);
					memcpy (dest, stemmed, dlen);
					rspamd_str_lc_utf8 (dest, dlen);
					tok->stemmed.len = dlen;
					tok->stemmed.begin = dest;
					tok->flags |= RSPAMD_STAT_TOKEN_FLAG_STEMMED;
				}
				else {
					/* Fallback */
					dest = rspamd_mempool_alloc (pool, tok->normalized.len);
					memcpy (dest, tok->normalized.begin, tok->normalized.len);
					rspamd_str_lc_utf8 (dest, tok->normalized.len);
					tok->stemmed.len = tok->normalized.len;
					tok->stemmed.begin = dest;
				}
			}
			else {
				/* No stemmer, utf8 lowercase */
				dest = rspamd_mempool_alloc (pool, tok->normalized.len);
				memcpy (dest, tok->normalized.begin, tok->normalized.len);
				rspamd_str_lc_utf8 (dest, tok->normalized.len);
				tok->stemmed.len = tok->normalized.len;
				tok->stemmed.begin = dest;
			}

			if (tok->stemmed.len > 0 && rspamd_language_detector_is_stop_word (d,
					tok->stemmed.begin, tok->stemmed.len)) {
				tok->flags |= RSPAMD_STAT_TOKEN_FLAG_STOP_WORD;
			}
		}
		else {
			if (tok->flags & RSPAMD_STAT_TOKEN_FLAG_TEXT) {
				/* Raw text, lowercase */
				dest = rspamd_mempool_alloc (pool, tok->original.len);
				memcpy (dest, tok->original.begin, tok->original.len);
				rspamd_str_lc (dest, tok->original.len);
				tok->stemmed.len = tok->original.len;
				tok->stemmed.begin = dest;
			}
		}
	}
}