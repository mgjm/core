/* Copyright (C) 2002 Timo Sirainen */

#include "lib.h"
#include "istream.h"
#include "str.h"
#include "message-address.h"
#include "message-date.h"
#include "message-body-search.h"
#include "message-header-search.h"
#include "message-parser.h"
#include "imap-date.h"
#include "index-storage.h"
#include "index-mail.h"
#include "mail-search.h"

#include <stdlib.h>
#include <ctype.h>

#define TXT_UNKNOWN_CHARSET "[BADCHARSET] Unknown charset"
#define TXT_INVALID_SEARCH_KEY "Invalid search key"

struct index_search_context {
        struct mail_search_context mail_ctx;
	struct index_transaction_context *trans;
	struct index_mailbox *ibox;
	char *charset;
	struct mail_search_arg *args;

	uint32_t seq1, seq2;
	struct index_mail imail;
	struct mail *mail;

	pool_t hdr_pool;
	const char *error;

	int failed;
};

struct search_header_context {
        struct index_search_context *index_context;
	struct mail_search_arg *args;

        struct message_header_line *hdr;

	unsigned int custom_header:1;
	unsigned int threading:1;
};

struct search_body_context {
        struct index_search_context *index_ctx;
	struct istream *input;
	const struct message_part *part;
};

static int seqset_contains(struct mail_search_seqset *set, uint32_t seq)
{
	while (set != NULL) {
		if (seq >= set->seq1 && seq <= set->seq2)
			return TRUE;
		set = set->next;
	}

	return FALSE;
}

static uoff_t str_to_uoff_t(const char *str)
{
	uoff_t num;

	num = 0;
	while (*str != '\0') {
		if (*str < '0' || *str > '9')
			return 0;

		num = num*10 + (*str - '0');
		str++;
	}

	return num;
}

static int search_keyword(struct mail_index *index,
			  const struct mail_index_record *rec,
			  const char *value)
{
	const char **keywords;
	int i;

	for (i = 0; i < INDEX_KEYWORDS_BYTE_COUNT; i++) {
		if (rec->keywords[i] != 0)
			break;
	}

	if (i == INDEX_KEYWORDS_BYTE_COUNT)
		return FALSE; /* no keywords set */

	/*FIXME:keywords = mail_keywords_list_get(index->keywords);
	for (i = 0; i < MAIL_KEYWORDS_COUNT; i++) {
		if (keywords[i] != NULL &&
		    strcasecmp(keywords[i], value) == 0) {
			return rec->msg_flags &
				(1 << (MAIL_KEYWORD_1_BIT+i));
		}
	}*/

	return FALSE;
}

/* Returns >0 = matched, 0 = not matched, -1 = unknown */
static int search_arg_match_index(struct index_mailbox *ibox,
				  struct index_mail *imail,
				  enum mail_search_arg_type type,
				  const char *value)
{
	const struct mail_index_record *rec = imail->data.rec;
	const struct mail_full_flags *full_flags;

	switch (type) {
	case SEARCH_ALL:
		return 1;

	/* flags */
	case SEARCH_ANSWERED:
		return rec->flags & MAIL_ANSWERED;
	case SEARCH_DELETED:
		return rec->flags & MAIL_DELETED;
	case SEARCH_DRAFT:
		return rec->flags & MAIL_DRAFT;
	case SEARCH_FLAGGED:
		return rec->flags & MAIL_FLAGGED;
	case SEARCH_SEEN:
		return rec->flags & MAIL_SEEN;
	case SEARCH_RECENT:
		full_flags = imail->mail.get_flags(&imail->mail);
		return full_flags->flags & MAIL_RECENT;
	case SEARCH_KEYWORD:
		return search_keyword(ibox->index, rec, value);

	default:
		return -1;
	}
}

static void search_index_arg(struct mail_search_arg *arg, void *context)
{
	struct index_search_context *ctx = context;
	int found;

	if (arg->type == SEARCH_SEQSET) {
		found = seqset_contains(arg->value.seqset, ctx->mail->seq);
		ARG_SET_RESULT(arg, found);
		return;
	}

	if (ctx->imail.data.rec == NULL) {
		/* expunged message */
		ARG_SET_RESULT(arg, 0);
		return;
	}

	switch (search_arg_match_index(ctx->ibox, &ctx->imail,
				       arg->type, arg->value.str)) {
	case -1:
		/* unknown */
		break;
	case 0:
		ARG_SET_RESULT(arg, 0);
		break;
	default:
		ARG_SET_RESULT(arg, 1);
		break;
	}
}

/* Returns >0 = matched, 0 = not matched, -1 = unknown */
static int search_arg_match_cached(struct index_search_context *ctx,
				   enum mail_search_arg_type type,
				   const char *value)
{
	time_t date, search_time;
	uoff_t virtual_size, search_size;
	int timezone_offset;

	switch (type) {
	/* internal dates */
	case SEARCH_BEFORE:
	case SEARCH_ON:
	case SEARCH_SINCE:
		date = ctx->mail->get_received_date(ctx->mail);
		if (date == (time_t)-1)
			return -1;

		if (!imap_parse_date(value, &search_time))
			return 0;

		switch (type) {
		case SEARCH_BEFORE:
			return date < search_time;
		case SEARCH_ON:
			return date >= search_time &&
				date < search_time + 3600*24;
		case SEARCH_SINCE:
			return date >= search_time;
		default:
			/* unreachable */
			break;
		}

	/* sent dates */
	case SEARCH_SENTBEFORE:
	case SEARCH_SENTON:
	case SEARCH_SENTSINCE:
		/* NOTE: RFC-3501 specifies that timezone is ignored
		   in searches. date is returned as UTC, so change it. */
		date = ctx->mail->get_date(ctx->mail, &timezone_offset);
		if (date == (time_t)-1)
			return -1;
		date += timezone_offset * 60;

		if (!imap_parse_date(value, &search_time))
			return 0;

		switch (type) {
		case SEARCH_SENTBEFORE:
			return date < search_time;
		case SEARCH_SENTON:
			return date >= search_time &&
				date < search_time + 3600*24;
		case SEARCH_SENTSINCE:
			return date >= search_time;
		default:
			/* unreachable */
			break;
		}

	/* sizes */
	case SEARCH_SMALLER:
	case SEARCH_LARGER:
		virtual_size = ctx->mail->get_size(ctx->mail);
		if (virtual_size == (uoff_t)-1)
			return -1;

		search_size = str_to_uoff_t(value);
		if (type == SEARCH_SMALLER)
			return virtual_size < search_size;
		else
			return virtual_size > search_size;

	default:
		return -1;
	}
}

static void search_cached_arg(struct mail_search_arg *arg, void *context)
{
	struct index_search_context *ctx = context;

	switch (search_arg_match_cached(ctx, arg->type,
					arg->value.str)) {
	case -1:
		/* unknown */
		break;
	case 0:
		ARG_SET_RESULT(arg, 0);
		break;
	default:
		ARG_SET_RESULT(arg, 1);
		break;
	}
}

static int search_sent(enum mail_search_arg_type type, const char *search_value,
		       const unsigned char *sent_value, size_t sent_value_len)
{
	time_t search_time, sent_time;
	int timezone_offset;

	if (sent_value == NULL)
		return 0;

	if (!imap_parse_date(search_value, &search_time))
		return 0;

	/* NOTE: RFC-3501 specifies that timezone is ignored
	   in searches. sent_time is returned as UTC, so change it. */
	if (!message_date_parse(sent_value, sent_value_len,
				&sent_time, &timezone_offset))
		return 0;
	sent_time += timezone_offset * 60;

	switch (type) {
	case SEARCH_SENTBEFORE:
		return sent_time < search_time;
	case SEARCH_SENTON:
		return sent_time >= search_time &&
			sent_time < search_time + 3600*24;
	case SEARCH_SENTSINCE:
		return sent_time >= search_time;
	default:
                i_unreached();
	}
}

static struct header_search_context *
search_header_context(struct index_search_context *ctx,
		      struct mail_search_arg *arg)
{
	int unknown_charset;

	if (arg->context != NULL) {
                message_header_search_reset(arg->context);
		return arg->context;
	}

	if (ctx->hdr_pool == NULL) {
		ctx->hdr_pool =
			pool_alloconly_create("message_header_search", 8192);
	}

	arg->context = message_header_search_init(ctx->hdr_pool, arg->value.str,
						  ctx->charset,
						  &unknown_charset);
	if (arg->context == NULL) {
		ctx->error = unknown_charset ?
			TXT_UNKNOWN_CHARSET : TXT_INVALID_SEARCH_KEY;
	}

	return arg->context;
}

static void search_header_arg(struct mail_search_arg *arg, void *context)
{
	struct search_header_context *ctx = context;
        struct header_search_context *hdr_search_ctx;
	int ret;

	/* first check that the field name matches to argument. */
	switch (arg->type) {
	case SEARCH_SENTBEFORE:
	case SEARCH_SENTON:
	case SEARCH_SENTSINCE:
		/* date is handled differently than others */
		if (strcasecmp(ctx->hdr->name, "Date") == 0) {
			if (ctx->hdr->continues) {
				ctx->hdr->use_full_value = TRUE;
				return;
			}
			ret = search_sent(arg->type, arg->value.str,
					  ctx->hdr->full_value,
					  ctx->hdr->full_value_len);
			ARG_SET_RESULT(arg, ret);
		}
		return;

	case SEARCH_HEADER:
	case SEARCH_HEADER_ADDRESS:
		ctx->custom_header = TRUE;

		if (strcasecmp(ctx->hdr->name, arg->hdr_field_name) != 0)
			return;
	case SEARCH_TEXT:
		/* TEXT goes through all headers */
		ctx->custom_header = TRUE;
		break;
	default:
		return;
	}

	if (arg->value.str[0] == '\0') {
		/* we're just testing existence of the field. always matches. */
		ret = 1;
	} else {
		if (ctx->hdr->continues) {
			ctx->hdr->use_full_value = TRUE;
			return;
		}

		t_push();

		hdr_search_ctx = search_header_context(ctx->index_context, arg);
		if (hdr_search_ctx == NULL)
			ret = 0;
		else if (arg->type == SEARCH_HEADER_ADDRESS) {
			/* we have to match against normalized address */
			struct message_address *addr;
			string_t *str;

			addr = message_address_parse(pool_datastack_create(),
						     ctx->hdr->full_value,
						     ctx->hdr->full_value_len,
						     0);
			str = t_str_new(ctx->hdr->value_len);
			message_address_write(str, addr);
			ret = message_header_search(str_data(str), str_len(str),
						    hdr_search_ctx) ? 1 : 0;
		} else {
			ret = message_header_search(ctx->hdr->full_value,
						    ctx->hdr->full_value_len,
						    hdr_search_ctx) ? 1 : 0;
		}
		t_pop();
	}

	if (ret == 1 ||
	    (arg->type != SEARCH_TEXT && arg->type != SEARCH_HEADER)) {
		/* set only when we definitely know if it's a match */
		ARG_SET_RESULT(arg, ret);
	}
}

static void search_header_unmatch(struct mail_search_arg *arg,
				  void *context __attr_unused__)
{
	switch (arg->type) {
	case SEARCH_SENTBEFORE:
	case SEARCH_SENTON:
	case SEARCH_SENTSINCE:
		if (arg->not) {
			/* date header not found, so we match only for
			   NOT searches */
			ARG_SET_RESULT(arg, 0);
		}
		break;
	case SEARCH_HEADER:
	case SEARCH_HEADER_ADDRESS:
		ARG_SET_RESULT(arg, 0);
		break;
	default:
		break;
	}
}

static void search_header(struct message_part *part,
                          struct message_header_line *hdr, void *context)
{
	struct search_header_context *ctx = context;

	if (hdr == NULL) {
		/* end of headers, mark all unknown SEARCH_HEADERs unmatched */
		mail_search_args_foreach(ctx->args, search_header_unmatch, ctx);
		return;
	}

	if (hdr->eoh)
		return;

	index_mail_parse_header(part, hdr, &ctx->index_context->imail);

	if (ctx->custom_header || strcasecmp(hdr->name, "Date") == 0) {
		ctx->hdr = hdr;

		ctx->custom_header = FALSE;
		mail_search_args_foreach(ctx->args, search_header_arg, ctx);
	}
}

static void search_body(struct mail_search_arg *arg, void *context)
{
	struct search_body_context *ctx = context;
	int ret, unknown_charset;

	if (ctx->index_ctx->error != NULL)
		return;

	if (arg->type == SEARCH_TEXT || arg->type == SEARCH_BODY) {
		i_stream_seek(ctx->input, 0);
		ret = message_body_search(arg->value.str,
					  ctx->index_ctx->charset,
					  &unknown_charset, ctx->input,
					  ctx->part, arg->type == SEARCH_TEXT);

		if (ret < 0) {
			ctx->index_ctx->error = unknown_charset ?
				TXT_UNKNOWN_CHARSET : TXT_INVALID_SEARCH_KEY;
		}

		ARG_SET_RESULT(arg, ret > 0);
	}
}

static int search_arg_match_text(struct mail_search_arg *args,
				 struct index_search_context *ctx)
{
	struct istream *input;
	const char *const *headers;
	int have_headers, have_body;

	/* first check what we need to use */
	headers = mail_search_args_analyze(args, &have_headers, &have_body);
	if (!have_headers && !have_body)
		return TRUE;

	if (have_headers) {
		struct search_header_context hdr_ctx;

		if (have_body)
			headers = NULL;

		input = headers == NULL ?
			ctx->mail->get_stream(ctx->mail, NULL, NULL) :
			ctx->mail->get_headers(ctx->mail, headers);
		if (input == NULL)
			return FALSE;

		memset(&hdr_ctx, 0, sizeof(hdr_ctx));
		hdr_ctx.index_context = ctx;
		hdr_ctx.custom_header = TRUE;
		hdr_ctx.args = args;

		index_mail_parse_header_init(&ctx->imail, headers);
		message_parse_header(NULL, input, NULL,
				     search_header, &hdr_ctx);
	} else {
		struct message_size hdr_size;

		input = ctx->mail->get_stream(ctx->mail, &hdr_size, NULL);
		if (input == NULL)
			return FALSE;

		i_stream_seek(input, hdr_size.physical_size);
	}

	if (have_body) {
		struct search_body_context body_ctx;

		memset(&body_ctx, 0, sizeof(body_ctx));
		body_ctx.index_ctx = ctx;
		body_ctx.input = input;
		body_ctx.part = ctx->mail->get_parts(ctx->mail);

		mail_search_args_foreach(args, search_body, &body_ctx);
	}
	return TRUE;
}

static int search_msgset_fix(struct index_mailbox *ibox,
                             const struct mail_index_header *hdr,
			     struct mail_search_seqset *set,
			     uint32_t *seq1_r, uint32_t *seq2_r)
{
	for (; set != NULL; set = set->next) {
		if (set->seq1 == (uint32_t)-1)
			set->seq1 = hdr->messages_count;
		if (set->seq2 == (uint32_t)-1)
			set->seq2 = hdr->messages_count;

		if (set->seq1 == 0 || set->seq2 == 0 ||
		    set->seq1 > hdr->messages_count ||
		    set->seq2 > hdr->messages_count) {
			mail_storage_set_syntax_error(ibox->box.storage,
						      "Invalid messageset");
			return -1;
		}

		if (*seq1_r > set->seq1 || *seq1_r == 0)
			*seq1_r = set->seq1;
		if (*seq2_r < set->seq2)
			*seq2_r = set->seq2;
	}
	return 0;
}

static int search_parse_msgset_args(struct index_mailbox *ibox,
				    const struct mail_index_header *hdr,
				    struct mail_search_arg *args,
				    uint32_t *seq1_r, uint32_t *seq2_r)
{
	*seq1_r = *seq2_r = 0;

	for (; args != NULL; args = args->next) {
		if (args->type == SEARCH_SUB) {
			if (search_parse_msgset_args(ibox, hdr,
						     args->value.subargs,
						     seq1_r, seq2_r) < 0)
				return -1;
		} else if (args->type == SEARCH_OR) {
			/* FIXME: in cases like "SEEN OR 5 7" we shouldn't
			   limit the range, but in cases like "1 OR 5 7" we
			   should expand the range. A bit tricky, we'll
			   just go through everything now to make it work
			   right. */
			*seq1_r = 1;
			*seq2_r = hdr->messages_count;

                        /* We still have to fix potential seqsets though */
			if (search_parse_msgset_args(ibox, hdr,
						     args->value.subargs,
						     seq1_r, seq2_r) < 0)
				return -1;
		} else if (args->type == SEARCH_SEQSET) {
			if (search_msgset_fix(ibox, hdr, args->value.seqset,
					      seq1_r, seq2_r) < 0)
				return -1;
		} else if (args->type == SEARCH_ALL) {
			/* go through everything. don't stop, have to fix
			   seqsets. */
			*seq1_r = 1;
			*seq2_r = hdr->messages_count;
		}
	}
	return 0;
}

static int search_limit_lowwater(struct index_mailbox *ibox,
				 uint32_t uid_lowwater, uint32_t *first_seq)
{
	uint32_t seq1, seq2;

	if (uid_lowwater == 0)
		return 0;

	if (mail_index_lookup_uid_range(ibox->view, uid_lowwater, (uint32_t)-1,
					&seq1, &seq2) < 0) {
		mail_storage_set_index_error(ibox);
		return -1;
	}

	if (*first_seq < seq1)
		*first_seq = seq1;
	return 0;
}

static int search_limit_by_flags(struct index_mailbox *ibox,
                                 const struct mail_index_header *hdr,
				 struct mail_search_arg *args,
				 uint32_t *seq1, uint32_t *seq2)
{
	for (; args != NULL; args = args->next) {
		if (args->type == SEARCH_SEEN) {
			/* SEEN with 0 seen? */
			if (!args->not && hdr->seen_messages_count == 0)
				return 0;

			if (hdr->seen_messages_count == hdr->messages_count) {
				/* UNSEEN with all seen? */
				if (args->not)
					return 0;

				/* SEEN with all seen */
				args->match_always = TRUE;
			} else if (args->not) {
				/* UNSEEN with lowwater limiting */
				if (search_limit_lowwater(ibox,
                                		hdr->first_unseen_uid_lowwater,
						seq1) < 0)
					return -1;
			}
		}

		if (args->type == SEARCH_DELETED) {
			/* DELETED with 0 deleted? */
			if (!args->not && hdr->deleted_messages_count == 0)
				return 0;

			if (hdr->deleted_messages_count ==
			    hdr->messages_count) {
				/* UNDELETED with all deleted? */
				if (args->not)
					return 0;

				/* DELETED with all deleted */
				args->match_always = TRUE;
			} else if (!args->not) {
				/* DELETED with lowwater limiting */
				if (search_limit_lowwater(ibox,
                                		hdr->first_deleted_uid_lowwater,
						seq1) < 0)
					return -1;
			}
		}

		/*FIXME:if (args->type == SEARCH_RECENT) {
			uid = ibox->index->first_recent_uid;
			if (!args->not && *first_uid < uid)
				*first_uid = ibox->index->first_recent_uid;
			else if (args->not && *last_uid >= uid)
				*last_uid = uid-1;
		}*/
	}

	return *seq1 <= *seq2;
}

static int search_get_seqset(struct index_search_context *ctx,
			     struct mail_search_arg *args)
{
        const struct mail_index_header *hdr;

	if (mail_index_get_header(ctx->ibox->view, &hdr) < 0) {
		mail_storage_set_index_error(ctx->ibox);
		return -1;
	}

	if (search_parse_msgset_args(ctx->ibox, hdr, args,
				     &ctx->seq1, &ctx->seq2) < 0)
		return -1;

	if (ctx->seq1 == 0) {
		ctx->seq1 = 1;
		ctx->seq2 = hdr->messages_count;
	}

	i_assert(ctx->seq1 <= ctx->seq2);

	/* UNSEEN and DELETED in root search level may limit the range */
	if (search_limit_by_flags(ctx->ibox, hdr, args,
				  &ctx->seq1, &ctx->seq2) < 0)
		return -1;
	return 0;
}

int index_storage_search_get_sorting(struct mailbox *box __attr_unused__,
				     enum mail_sort_type *sort_program)
{
	/* currently we don't support sorting */
	*sort_program = MAIL_SORT_END;
	return 0;
}

struct mail_search_context *
index_storage_search_init(struct mailbox_transaction_context *_t,
			  const char *charset, struct mail_search_arg *args,
			  const enum mail_sort_type *sort_program,
			  enum mail_fetch_field wanted_fields,
			  const char *const wanted_headers[])
{
	struct index_transaction_context *t =
		(struct index_transaction_context *)_t;
	struct index_search_context *ctx;

	if (sort_program != NULL && *sort_program != MAIL_SORT_END) {
		i_fatal("BUG: index_storage_search_init(): "
			 "invalid sort_program");
	}

	/*FIXME:if (!index_storage_sync_and_lock(ibox, TRUE, TRUE, MAIL_LOCK_SHARED))
		return NULL;*/

	ctx = i_new(struct index_search_context, 1);
	ctx->mail_ctx.box = &t->ibox->box;
	ctx->trans = t;
	ctx->ibox = t->ibox;
	ctx->charset = i_strdup(charset);
	ctx->args = args;

	ctx->mail = &ctx->imail.mail;
	index_mail_init(t, &ctx->imail, wanted_fields, wanted_headers);

	mail_search_args_reset(ctx->args, TRUE);

	if (search_get_seqset(ctx, args) < 0) {
		ctx->failed = TRUE;
		ctx->seq1 = 1;
		ctx->seq2 = 0;
	}
	return &ctx->mail_ctx;
}

int index_storage_search_deinit(struct mail_search_context *_ctx)
{
        struct index_search_context *ctx = (struct index_search_context *)_ctx;
	int ret;

	ret = ctx->failed || ctx->error != NULL ? -1 : 0;

	if (ctx->imail.pool != NULL)
		index_mail_deinit(&ctx->imail);

	if (ctx->error != NULL) {
		mail_storage_set_error(ctx->ibox->box.storage,
				       "%s", ctx->error);
	}

	if (ctx->hdr_pool != NULL)
		pool_unref(ctx->hdr_pool);

	i_free(ctx);
	return ret;
}

static int search_match_next(struct index_search_context *ctx)
{
        struct mail_search_arg *arg;
	int ret;

	/* check the index matches first */
	mail_search_args_reset(ctx->args, FALSE);
	ret = mail_search_args_foreach(ctx->args, search_index_arg, ctx);
	if (ret >= 0)
		return ret > 0;

	if (ctx->imail.data.rec == NULL) {
		/* expunged message, no way to check if the rest would have
		   matched */
		return FALSE;
	}

	/* next search only from cached arguments */
	ret = mail_search_args_foreach(ctx->args, search_cached_arg, ctx);
	if (ret >= 0)
		return ret > 0;

	/* open the mail file and check the rest */
	if (!search_arg_match_text(ctx->args, ctx))
		return FALSE;

	for (arg = ctx->args; arg != NULL; arg = arg->next) {
		if (arg->result != 1)
			return FALSE;
	}

	return TRUE;
}

struct mail *index_storage_search_next(struct mail_search_context *_ctx)
{
        struct index_search_context *ctx = (struct index_search_context *)_ctx;
	const struct mail_index_record *rec;
	int ret;

	ret = 0;
	while (ctx->seq1 <= ctx->seq2) {
		if (mail_index_lookup(ctx->ibox->view, ctx->seq1, &rec) < 0) {
			ctx->failed = TRUE;
			mail_storage_set_index_error(ctx->ibox);
			return NULL;
		}

		ctx->imail.data.rec = rec;
		ctx->mail->seq = ctx->seq1++;
		ctx->mail->uid = rec == NULL ? 0 : rec->uid;

		ret = index_mail_next(&ctx->imail, rec, ctx->mail->seq, TRUE);
		if (ret < 0)
			break;

		t_push();
		ret = search_match_next(ctx);
		t_pop();

		if (ctx->error != NULL)
			ret = -1;
		if (ret != 0)
			break;
	}

	if (ret <= 0) {
		/* error or last record */
		return NULL;
	}

	return ctx->mail;
}
