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
 * Bayesian classifier
 */
#include "classifiers.h"
#include "rspamd.h"
#include "filter.h"
#include "cfg_file.h"
#include "stat_internal.h"
#include "math.h"

#define msg_err_bayes(...) rspamd_default_log_function (G_LOG_LEVEL_CRITICAL, \
        "bayes", task->task_pool->tag.uid, \
        G_STRFUNC, \
        __VA_ARGS__)
#define msg_warn_bayes(...)   rspamd_default_log_function (G_LOG_LEVEL_WARNING, \
        "bayes", task->task_pool->tag.uid, \
        G_STRFUNC, \
        __VA_ARGS__)
#define msg_info_bayes(...)   rspamd_default_log_function (G_LOG_LEVEL_INFO, \
        "bayes", task->task_pool->tag.uid, \
        G_STRFUNC, \
        __VA_ARGS__)
#define msg_debug_bayes(...)  rspamd_default_log_function (G_LOG_LEVEL_DEBUG, \
        "bayes", task->task_pool->tag.uid, \
        G_STRFUNC, \
        __VA_ARGS__)


static inline GQuark
bayes_error_quark (void)
{
	return g_quark_from_static_string ("bayes-error");
}

/**
 * Returns probability of chisquare > value with specified number of freedom
 * degrees
 * @param value value to test
 * @param freedom_deg number of degrees of freedom
 * @return
 */
static gdouble
inv_chi_square (struct rspamd_task *task, gdouble value, gint freedom_deg)
{
	double prob, sum, m;
	gint i;

	errno = 0;
	m = -value;
	prob = exp (value);

	if (errno == ERANGE) {
		msg_err_bayes ("exp overflow");
		return 0;
	}

	sum = prob;

	for (i = 1; i < freedom_deg; i++) {
		prob *= m / (gdouble)i;
		msg_debug_bayes ("prob: %.6f", prob);
		sum += prob;
	}

	return MIN (1.0, sum);
}

struct bayes_task_closure {
	double ham_prob;
	double spam_prob;
	guint64 processed_tokens;
	guint64 total_hits;
	struct rspamd_task *task;
};

/*
 * Mathematically we use pow(complexity, complexity), where complexity is the
 * window index
 */
static const double feature_weight[] = { 0, 1, 4, 27, 256, 3125, 46656, 823543 };

#define PROB_COMBINE(prob, cnt, weight, assumed) (((weight) * (assumed) + (cnt) * (prob)) / ((weight) + (cnt)))
/*
 * In this callback we calculate local probabilities for tokens
 */
static void
bayes_classify_token (struct rspamd_classifier *ctx,
		rspamd_token_t *tok, struct bayes_task_closure *cl)
{
	guint i;
	gint id;
	guint64 spam_count = 0, ham_count = 0, total_count = 0;
	struct rspamd_statfile *st;
	struct rspamd_task *task;
	double spam_prob, spam_freq, ham_freq, bayes_spam_prob, bayes_ham_prob,
		ham_prob, fw, w, norm_sum, norm_sub, val;

	task = cl->task;

	for (i = 0; i < ctx->statfiles_ids->len; i++) {
		id = g_array_index (ctx->statfiles_ids, gint, i);
		st = g_ptr_array_index (ctx->ctx->statfiles, id);
		g_assert (st != NULL);
		val = tok->values[id];

		if (val > 0) {
			if (st->stcf->is_spam) {
				spam_count += val;
			}
			else {
				ham_count += val;
			}

			total_count += val;
			cl->total_hits += val;
		}
	}

	/* Probability for this token */
	if (total_count > 0) {
		spam_freq = ((double)spam_count / MAX (1., (double) ctx->spam_learns));
		ham_freq = ((double)ham_count / MAX (1., (double)ctx->ham_learns));
		spam_prob = spam_freq / (spam_freq + ham_freq);
		ham_prob = ham_freq / (spam_freq + ham_freq);
		fw = feature_weight[tok->window_idx % G_N_ELEMENTS (feature_weight)];
		norm_sum = (spam_freq + ham_freq) * (spam_freq + ham_freq);
		norm_sub = (spam_freq - ham_freq) * (spam_freq - ham_freq);

		w = (norm_sub) / (norm_sum) *
				(fw * total_count) / (4.0 * (1.0 + fw * total_count));
		bayes_spam_prob = PROB_COMBINE (spam_prob, total_count, w, 0.5);
		norm_sub = (ham_freq - spam_freq) * (ham_freq - spam_freq);
		w = (norm_sub) / (norm_sum) *
				(fw * total_count) / (4.0 * (1.0 + fw * total_count));
		bayes_ham_prob = PROB_COMBINE (ham_prob, total_count, w, 0.5);

		cl->spam_prob += log2 (bayes_spam_prob);
		cl->ham_prob += log2 (bayes_ham_prob);
		cl->processed_tokens ++;

		msg_debug_bayes ("token: weight: %f, total_count: %L, "
				"spam_count: %L, ham_count: %L,"
				"spam_prob: %.3f, ham_prob: %.3f, "
				"bayes_spam_prob: %.3f, bayes_ham_prob: %.3f, "
				"current spam prob: %.3f, current ham prob: %.3f",
				fw, total_count, spam_count, ham_count,
				spam_prob, ham_prob,
				bayes_spam_prob, bayes_ham_prob,
				cl->spam_prob, cl->ham_prob);
	}
}



gboolean
bayes_init (rspamd_mempool_t *pool, struct rspamd_classifier *cl)
{
	cl->cfg->flags |= RSPAMD_FLAG_CLASSIFIER_INTEGER;

	return TRUE;
}

gboolean
bayes_classify (struct rspamd_classifier * ctx,
		GPtrArray *tokens,
		struct rspamd_task *task)
{
	double final_prob, h, s, *pprob;
	char *sumbuf;
	struct rspamd_statfile *st = NULL;
	struct bayes_task_closure cl;
	rspamd_token_t *tok;
	guint i;
	gint id;
	GList *cur;

	g_assert (ctx != NULL);
	g_assert (tokens != NULL);

	memset (&cl, 0, sizeof (cl));
	cl.task = task;

	/* Check min learns */
	if (ctx->cfg->min_learns > 0) {
		if (ctx->ham_learns < ctx->cfg->min_learns) {
			msg_info_task ("skip classification as ham class has not enough "
					"learns: %ul, %ud required",
					ctx->ham_learns, ctx->cfg->min_learns);

			return TRUE;
		}
		if (ctx->spam_learns < ctx->cfg->min_learns) {
			msg_info_task ("skip classification as spam class has not enough "
					"learns: %ul, %ud required",
					ctx->spam_learns, ctx->cfg->min_learns);

			return TRUE;
		}
	}

	for (i = 0; i < tokens->len; i ++) {
		tok = g_ptr_array_index (tokens, i);

		bayes_classify_token (ctx, tok, &cl);
	}

	h = 1 - inv_chi_square (task, cl.spam_prob, cl.processed_tokens);
	s = 1 - inv_chi_square (task, cl.ham_prob, cl.processed_tokens);

	if (isfinite (s) && isfinite (h)) {
		final_prob = (s + 1.0 - h) / 2.;
		msg_debug_bayes (
				"<%s> got ham prob %.2f -> %.2f and spam prob %.2f -> %.2f,"
						" %L tokens processed of %ud total tokens",
				task->message_id,
				cl.ham_prob,
				h,
				cl.spam_prob,
				s,
				cl.processed_tokens,
				tokens->len);
	}
	else {
		/*
		 * We have some overflow, hence we need to check which class
		 * is NaN
		 */
		if (isfinite (h)) {
			final_prob = 1.0;
			msg_debug_bayes ("<%s> spam class is overflowed, as we have no"
					" ham samples", task->message_id);
		}
		else if (isfinite (s)) {
			final_prob = 0.0;
			msg_debug_bayes ("<%s> ham class is overflowed, as we have no"
					" spam samples", task->message_id);
		}
		else {
			final_prob = 0.5;
			msg_warn_bayes ("<%s> spam and ham classes are both overflowed",
					task->message_id);
		}
	}

	pprob = rspamd_mempool_alloc (task->task_pool, sizeof (*pprob));
	*pprob = final_prob;
	rspamd_mempool_set_variable (task->task_pool, "bayes_prob", pprob, NULL);

	if (cl.processed_tokens > 0 && fabs (final_prob - 0.5) > 0.05) {

		sumbuf = rspamd_mempool_alloc (task->task_pool, 32);

		/* Now we can have exactly one HAM and exactly one SPAM statfiles per classifier */
		for (i = 0; i < ctx->statfiles_ids->len; i++) {
			id = g_array_index (ctx->statfiles_ids, gint, i);
			st = g_ptr_array_index (ctx->ctx->statfiles, id);

			if (final_prob > 0.5 && st->stcf->is_spam) {
				break;
			}
			else if (final_prob < 0.5 && !st->stcf->is_spam) {
				break;
			}
		}

		/* Correctly scale HAM */
		if (final_prob < 0.5) {
			final_prob = 1.0 - final_prob;
		}

		/*
		 * Bayes p is from 0.5 to 1.0, but confidence is from 0 to 1, so
		 * we need to rescale it to display correctly
		 */
		rspamd_snprintf (sumbuf, 32, "%.2f%%", (final_prob - 0.5) * 200.);
		final_prob = rspamd_normalize_probability (final_prob, 0.5);
		g_assert (st != NULL);
		cur = g_list_prepend (NULL, sumbuf);
		rspamd_task_insert_result (task,
				st->stcf->symbol,
				final_prob,
				cur);
	}

	return TRUE;
}

gboolean
bayes_learn_spam (struct rspamd_classifier * ctx,
		GPtrArray *tokens,
		struct rspamd_task *task,
		gboolean is_spam,
		gboolean unlearn,
		GError **err)
{
	guint i, j;
	gint id;
	struct rspamd_statfile *st;
	rspamd_token_t *tok;
	gboolean incrementing;

	g_assert (ctx != NULL);
	g_assert (tokens != NULL);

	incrementing = ctx->cfg->flags & RSPAMD_FLAG_CLASSIFIER_INCREMENTING_BACKEND;

	for (i = 0; i < tokens->len; i++) {
		tok = g_ptr_array_index (tokens, i);

		for (j = 0; j < ctx->statfiles_ids->len; j++) {
			id = g_array_index (ctx->statfiles_ids, gint, j);
			st = g_ptr_array_index (ctx->ctx->statfiles, id);
			g_assert (st != NULL);

			if (!!st->stcf->is_spam == !!is_spam) {
				if (incrementing) {
					tok->values[id] = 1;
				}
				else {
					tok->values[id]++;
				}
			}
			else if (tok->values[id] > 0 && unlearn) {
				/* Unlearning */
				if (incrementing) {
					tok->values[id] = -1;
				}
				else {
					tok->values[id]--;
				}
			}
			else if (incrementing) {
				tok->values[id] = 0;
			}
		}
	}

	return TRUE;
}
