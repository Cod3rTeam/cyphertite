/*
 * Copyright (c) 2011, 2012 Conformal Systems LLC <info@conformal.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/stat.h>

#include <inttypes.h>
#include <unistd.h>
#include <limits.h>
#include <stdlib.h>
#include <dirent.h>
#include <libgen.h>
#include <fcntl.h>
#include <errno.h>

#ifdef NEED_LIBCLENS
#include <clens.h>
#endif

#include <assl.h>
#include <clog.h>
#include <exude.h>

#include <ctutil.h>

#include <ct_types.h>
#include <ct_crypto.h>
#include <ct_proto.h>
#include <ct_match.h>
#include <ct_ctfile.h>
#include <ct_lib.h>
#include <ct_ext.h>
#include <ct_internal.h>

ct_op_cb ct_cull_send_shas;
ct_op_cb ct_cull_setup;
ct_op_cb ct_cull_start_shas;
ct_op_cb ct_cull_start_complete;
ct_op_cb ct_cull_send_complete;
ct_op_cb ct_cull_complete;
ct_op_cb ct_cull_collect_ctfiles;
ct_op_cb ct_cull_fetch_all_ctfiles;

/*
 * clean up after a ctfile archive/extract operation by freeing the remotename
 */
void
ctfile_op_cleanup(struct ct_global_state *state, struct ct_op *op)
{
	struct ct_ctfileop_args	*cca = op->op_args;

	e_free(&cca->cca_remotename);
}

struct ctfile_archive_state {
	FILE		*cas_handle;
	struct fnode	*cas_fnode;
	off_t		 cas_size;
	off_t		 cas_offset;
	int		 cas_block_no;
	int		 cas_open_sent;
};

void
ctfile_archive(struct ct_global_state *state, struct ct_op *op)
{
	char				 tpath[PATH_MAX];
	struct ct_ctfileop_args		*cca = op->op_args;
	struct ctfile_archive_state	*cas = op->op_priv;
	const char			*ctfile = cca->cca_localname;
	const char			*rname = cca->cca_remotename;
	struct ct_trans			*ct_trans;
	struct stat			 sb;
	ssize_t				 rsz, rlen;
	int				 error;

	switch (ct_get_file_state(state)) {
	case CT_S_STARTING:
		cas = e_calloc(1, sizeof(*cas));
		op->op_priv = cas;

		if (cca->cca_tdir) {
			snprintf(tpath, sizeof tpath, "%s/%s",
			    cca->cca_tdir, ctfile);
		} else {
			strlcpy(tpath, ctfile, sizeof(tpath));
		}
		CNDBG(CT_LOG_FILE, "opening ctfile for archive %s", ctfile);
		if ((cas->cas_handle = fopen(tpath, "rb")) == NULL)
			CFATAL("can't open %s for reading", ctfile);
		if (cca->cca_ctfile) {
			struct ctfile_parse_state	 xs_ctx;
			int				 ret;
			if (ctfile_parse_init_f(&xs_ctx, cas->cas_handle, NULL))
				CFATALX("%s is not a valid ctfile, can't "
				    "open", tpath);
			while ((ret = ctfile_parse(&xs_ctx)) != XS_RET_EOF) {
				if (ret == XS_RET_SHA)  {
					if (ctfile_parse_seek(&xs_ctx))
						CFATALX("seek failed");
				} else if (ret == XS_RET_FAIL) {
					CFATALX("%s is not a valid ctfile, EOF"
					    " not found", tpath);
				}

			}
			ctfile_parse_close(&xs_ctx);
			fseek(cas->cas_handle, 0L, SEEK_SET);
		}

		if (fstat(fileno(cas->cas_handle), &sb) == -1)
			CFATAL("can't stat backup file %s", ctfile);
		cas->cas_size = sb.st_size;
		cas->cas_fnode = e_calloc(1, sizeof(*cas->cas_fnode));

		if (rname == NULL) {
			rname = ctfile_cook_name(ctfile);
			cca->cca_remotename = (char *)rname;
		}
		break;
	case CT_S_FINISHED:
		/* We're done here */
		return;
	case CT_S_WAITING_SERVER:
		CNDBG(CT_LOG_FILE, "waiting on remote open");
		return;
	default:
		break;
	}

	CNDBG(CT_LOG_FILE, "entered for block %d", cas->cas_block_no);
	ct_set_file_state(state, CT_S_RUNNING);
loop:
	ct_trans = ct_trans_alloc(state);
	if (ct_trans == NULL) {
		/* system busy, return */
		CNDBG(CT_LOG_TRANS, "ran out of transactions, waiting");
		ct_set_file_state(state, CT_S_WAITING_TRANS);
		return;
	}

	if (cas->cas_open_sent == 0) {
		cas->cas_open_sent = 1;
		ct_xml_file_open(state, ct_trans, rname, MD_O_WRITE, 0);
		/* xml thread will wake us up when it gets the open */
		ct_set_file_state(state, CT_S_WAITING_SERVER);
		return;
	}

	/* Are we done here? */
	if (cas->cas_size == cas->cas_offset) {
		ct_set_file_state(state, CT_S_FINISHED);

		ct_trans->tr_fl_node = NULL;
		ct_trans->tr_state = TR_S_XML_CLOSE;
		ct_trans->tr_eof = 1;
		ct_trans->hdr.c_flags = C_HDR_F_METADATA;
		ct_trans->tr_ctfile_name = rname;
		state->ct_stats->st_bytes_tot += cas->cas_size;
		e_free(&cas);
		op->op_priv = NULL;
		ct_queue_first(state, ct_trans);
		CNDBG(CT_LOG_FILE, "setting eof on trans %" PRIu64,
		    ct_trans->tr_trans_id);
		return;
	}
	/* perform read */
	rsz = cas->cas_size - cas->cas_offset;

	CNDBG(CT_LOG_FILE, "rsz %ld max %d", (long) rsz,
	    state->ct_max_block_size);
	if (rsz > state->ct_max_block_size) {
		rsz = state->ct_max_block_size;
	}

	ct_trans->tr_dataslot = 0;
	rlen = fread(ct_trans->tr_data[0], sizeof(char), rsz, cas->cas_handle);

	CNDBG(CT_LOG_FILE, "read %ld", (long) rlen);

	state->ct_stats->st_bytes_read += rlen;

	ct_trans->tr_fl_node = cas->cas_fnode;
	ct_trans->tr_chsize = ct_trans->tr_size[0] = rlen;
	ct_trans->tr_state = TR_S_READ;
	ct_trans->tr_type = TR_T_WRITE_CHUNK;
	ct_trans->tr_eof = 0;
	ct_trans->hdr.c_flags = C_HDR_F_METADATA;
	ct_trans->hdr.c_flags |= cca->cca_encrypted ? C_HDR_F_ENCRYPTED : 0;
	ct_trans->hdr.c_ex_status = 2; /* we handle new metadata protocol */
	/* Set chunkno for restart and for iv generation */
	ct_trans->tr_ctfile_chunkno = cas->cas_block_no;
	ct_trans->tr_ctfile_name = rname;

	cas->cas_block_no++;

	if (rsz != rlen || (rlen + cas->cas_offset) == cas->cas_size) {
		/* short read, file truncated or EOF */
		CNDBG(CT_LOG_FILE, "DONE");
		error = fstat(fileno(cas->cas_handle), &sb);
		if (error) {
			CWARNX("file stat error %s %d %s",
			    ctfile, errno, strerror(errno));
		} else if (sb.st_size != cas->cas_size) {
			CWARNX("file truncated during backup %s",
			    ctfile);
			/*
			 * may need to perform special nop processing
			 * to pad archive file to right number of chunks
			 */
		}
		/*
		 * we don't set eof here because the next go round
		 * will hit the state done case above
		 */
		cas->cas_offset = cas->cas_size;
		ct_trans->tr_eof = 1;
	} else {
		cas->cas_offset += rlen;
	}
	ct_queue_first(state, ct_trans);
	CNDBG(CT_LOG_FILE, " trans %"PRId64", read size %ld, into %p rlen %ld",
	    ct_trans->tr_trans_id, (long) rsz, ct_trans->tr_data[0],
	    (long) rlen);
	CNDBG(CT_LOG_FILE, "sizes rlen %ld offset %ld size %ld", (long) rlen,
	    (long)cas->cas_offset, (long)cas->cas_size);


	goto loop;
}

void
ct_xml_file_open(struct ct_global_state *state, struct ct_trans *trans,
    const char *file, int mode, uint32_t chunkno)
{
	trans->tr_state = TR_S_XML_OPEN;

	if (ct_create_xml_open(&trans->hdr, (void **)&trans->tr_data[2],
	    file, mode, chunkno) != 0)
		CFATALX("can't create xml open packet");
	trans->tr_dataslot = 2;
	trans->tr_size[2] = trans->hdr.c_size;

	CNDBG(CT_LOG_XML, "open trans %"PRIu64, trans->tr_trans_id);
	ct_queue_first(state, trans);

}

int
ct_xml_file_open_polled(struct ct_global_state *state, const char *file,
    int mode, uint32_t chunkno)
{
#define ASSL_TIMEOUT 20
	struct ct_header	 hdr;
	void			*body;
	size_t			 sz;
	int			 rv = 1;

	CNDBG(CT_LOG_XML, "setting up XML");

	if (ct_create_xml_open(&hdr, &body, file, mode, chunkno) != 0)
		CFATALX("can't create ctfile open packet");

	sz = hdr.c_size;
	/* use previous packet id so it'll fit with the state machine */
	hdr.c_tag = state->ct_packet_id - 1;
	ct_wire_header(&hdr);
	if (ct_assl_io_write_poll(state->ct_assl_ctx, &hdr, sizeof hdr,
	    ASSL_TIMEOUT) != sizeof hdr) {
		CWARNX("could not write header");
		goto done;
	}
	if (ct_assl_io_write_poll(state->ct_assl_ctx, body, sz,
	    ASSL_TIMEOUT) != sz) {
		CWARNX("could not write body");
		goto done;
	}
	e_free(&body);

	/* get server reply */
	if (ct_assl_io_read_poll(state->ct_assl_ctx, &hdr, sizeof hdr,
	    ASSL_TIMEOUT) != sizeof hdr) {
		CWARNX("invalid header size");
		goto done;
	}
	ct_unwire_header(&hdr);

	if (hdr.c_status == C_HDR_S_OK && hdr.c_opcode == C_HDR_O_XML_REPLY)
		rv = 0;

	/* we know the open was ok or bad, just read the body and dump it */
	body = e_calloc(1, hdr.c_size);
	if (ct_assl_io_read_poll(state->ct_assl_ctx, body, hdr.c_size,
	    ASSL_TIMEOUT) != hdr.c_size) {
		rv = 1;
	}
	e_free(&body);

done:
	if (body)
		e_free(&body);
	return (rv);
#undef ASSL_TIMEOUT
}

void
ct_xml_file_close(struct ct_global_state *state)
{
	struct ct_trans			*trans;

	trans = ct_trans_alloc(state);
	if (trans == NULL) {
		/* system busy, return  XXX this would be pretty bad. */
		CNDBG(CT_LOG_TRANS, "ran out of transactions, waiting");
		ct_set_file_state(state, CT_S_WAITING_TRANS);
		return;
	}

	trans->tr_state = TR_S_XML_CLOSING;

	if (ct_create_xml_close(&trans->hdr, (void **)&trans->tr_data[2]) != 0)
		CFATALX("Could not create xml close packet");
	trans->tr_dataslot = 2;
	trans->tr_size[2] = trans->hdr.c_size;

	ct_queue_first(state, trans);
}

struct ctfile_extract_state {
	struct fnode	*ces_fnode;
	int		 ces_block_no;
	int		 ces_open_sent;
	int		 ces_is_open;
};

void
ctfile_extract(struct ct_global_state *state, struct ct_op *op)
{
	struct ct_ctfileop_args		*cca = op->op_args;
	struct ctfile_extract_state	*ces = op->op_priv;
	const char			*ctfile = cca->cca_localname;
	const char			*rname = cca->cca_remotename;
	struct ct_trans			*trans;
	struct ct_header		*hdr;

	switch (ct_get_file_state(state)) {
	case CT_S_STARTING:
		ces = e_calloc(1, sizeof(*ces));
		op->op_priv = ces;

		if (rname == NULL) {
			rname = ctfile_cook_name(ctfile);
			cca->cca_remotename = (char *)rname;
		}
		state->extract_state = ct_file_extract_init(cca->cca_tdir,
		    0, 0, state->ct_verbose, 0);
		break;
	case CT_S_WAITING_SERVER:
		CNDBG(CT_LOG_FILE, "waiting on remote open");
		/* FALLTHROUGH */
	case CT_S_FINISHED:
		return;
	default:
		break;
	}
	ct_set_file_state(state, CT_S_RUNNING);

again:
	trans = ct_trans_alloc(state);
	if (trans == NULL) {
		/* system busy, return */
		CNDBG(CT_LOG_TRANS, "ran out of transactions, waiting");
		ct_set_file_state(state, CT_S_WAITING_TRANS);
		return;
	}
	if (ces->ces_open_sent == 0) {
		ct_xml_file_open(state, trans, rname, MD_O_READ, 0);
		ces->ces_open_sent = 1;
		/* xml thread will wake us up when it gets the open */
		ct_set_file_state(state, CT_S_WAITING_SERVER);
		return;
	} else if (ces->ces_is_open == 0) {
		ces->ces_is_open = 1;
		ces->ces_fnode = e_calloc(1, sizeof(*ces->ces_fnode));
		ces->ces_fnode->fl_type = C_TY_REG;
		ces->ces_fnode->fl_parent_dir =
		    ct_file_extract_get_rootdir(state->extract_state);
		ces->ces_fnode->fl_name = e_strdup(ctfile);
		ces->ces_fnode->fl_sname = e_strdup(ctfile);
		ces->ces_fnode->fl_mode = S_IRUSR | S_IWUSR;
		ces->ces_fnode->fl_uid = getuid();
		ces->ces_fnode->fl_gid = getgid();
		ces->ces_fnode->fl_atime = time(NULL);
		ces->ces_fnode->fl_mtime = time(NULL);

		trans = ct_trans_realloc_local(state, trans);
		trans->tr_fl_node = ces->ces_fnode;
		trans->tr_state = TR_S_EX_FILE_START;
		trans->hdr.c_flags |= C_HDR_F_METADATA;
		ct_queue_first(state, trans);
		goto again;
	}

	trans->tr_fl_node = ces->ces_fnode;
	trans->tr_state = TR_S_EX_SHA;
	trans->tr_type = TR_T_READ_CHUNK;
	trans->tr_eof = 0;
	trans->tr_ctfile_chunkno = ces->ces_block_no++;
	trans->tr_ctfile_name = rname;

	hdr = &trans->hdr;
	hdr->c_ex_status = 2;
	hdr->c_flags |= C_HDR_F_METADATA;

	if (ct_create_iv_ctfile(trans->tr_ctfile_chunkno, trans->tr_iv,
	    sizeof(trans->tr_iv)))
		CFATALX("can't create iv for ctfile block no %d",
		    trans->tr_ctfile_chunkno);
	ct_queue_first(state, trans);
}

void
ct_complete_metadata(struct ct_global_state *state, struct ct_trans *trans)
{
	int			slot, done = 0, release_fnode = 0;

	switch(trans->tr_state) {
	case TR_S_EX_FILE_START:
		/* XXX can we recover from this? */
		if (ct_file_extract_open(state->extract_state,
		    trans->tr_fl_node) != 0)
			CFATALX("unable to open file %s",
			    trans->tr_fl_node->fl_name);
		break;
	case TR_S_EX_READ:
	case TR_S_EX_DECRYPTED:
	case TR_S_EX_UNCOMPRESSED:
		if (trans->hdr.c_status == C_HDR_S_OK) {
			slot = trans->tr_dataslot;
			CNDBG(CT_LOG_FILE, "writing packet sz %d",
			    trans->tr_size[slot]);
			ct_file_extract_write(state->extract_state,
			    trans->tr_fl_node, trans->tr_data[slot],
			    trans->tr_size[slot]);
		} else {
			ct_set_file_state(state, CT_S_FINISHED);
		}
		break;

	case TR_S_DONE:
		/* More operations to be done? */
		if (ct_op_complete(state))
			done = 1;

		/* Clean up reconnect name, shared between all trans */
		if (trans->tr_ctfile_name != NULL)
			e_free(&trans->tr_ctfile_name);

		if (!done)
			return;
		ct_shutdown(state);
		break;
	case TR_S_WMD_READY:
		if (trans->tr_eof != 0)
			release_fnode = 1;
	case TR_S_XML_OPEN:
	case TR_S_XML_CLOSING:
	case TR_S_XML_CLOSED:
	case TR_S_XML_OPENED:
	case TR_S_READ:
		break;
	case TR_S_EX_FILE_END:
		ct_file_extract_close(state->extract_state,
		    trans->tr_fl_node);
		ct_file_extract_cleanup(state->extract_state);
		state->extract_state = NULL;
		release_fnode = 1;
		/* FALLTHROUGH */
	case TR_S_XML_CLOSE:
		CNDBG(CT_LOG_FILE, "eof reached, closing file");
		ct_xml_file_close(state);
		break;

	case TR_S_XML_CULL_REPLIED:
		ct_wakeup_file(state->event_state);
		break;
	default:
		CFATALX("unexpected tr state in %s %d", __func__,
		    trans->tr_state);
	}

	if (release_fnode != 0)
		ct_free_fnode(trans->tr_fl_node);
}

void
ctfile_list_start(struct ct_global_state *state, struct ct_op *op)
{
	struct ct_trans			*trans;

	ct_set_file_state(state, CT_S_FINISHED);

	trans = ct_trans_alloc(state);

	trans->tr_state = TR_S_XML_LIST;

	if (ct_create_xml_list(&trans->hdr, (void **)&trans->tr_data[2]) != 0)
		CFATALX("Could not create xml list packet");
	trans->tr_dataslot = 2;
	trans->tr_size[2] = trans->hdr.c_size;

	ct_queue_first(state, trans);
}

void
ctfile_list_complete(struct ctfile_list *files,  int matchmode, char **flist,
    char **excludelist, struct ctfile_list_tree *results)
{
	struct ct_match		*match, *ex_match = NULL;
	struct ctfile_list_file	*file;

	if (SIMPLEQ_EMPTY(files))
		return;

	match = ct_match_compile(matchmode, flist);
	if (excludelist)
		ex_match = ct_match_compile(matchmode, excludelist);
	while ((file = SIMPLEQ_FIRST(files)) != NULL) {
		SIMPLEQ_REMOVE_HEAD(files, mlf_link);
		if (ct_match(match, file->mlf_name) == 0 && (ex_match == NULL ||
		    ct_match(ex_match, file->mlf_name) == 1)) {
			RB_INSERT(ctfile_list_tree, results, file);
		} else {
			e_free(&file);
		}
	}
	if (ex_match != NULL)
		ct_match_unwind(ex_match);
	ct_match_unwind(match);
}

void
ctfile_delete(struct ct_global_state *state, struct ct_op *op)
{
	const char			*rname = op->op_args;
	struct ct_trans			*trans;

	trans = ct_trans_alloc(state);
	trans->tr_state = TR_S_XML_DELETE;

	rname = ctfile_cook_name(rname);

	if (ct_create_xml_delete(&trans->hdr, (void **)&trans->tr_data[2],
	    rname) != 0)
		CFATALX("Could not create xml delete packet for %s", rname);
	trans->tr_dataslot = 2;
	trans->tr_size[2] = trans->hdr.c_size;

	e_free(&rname);

	ct_queue_first(state, trans);
}

void
ct_handle_xml_reply(struct ct_global_state *state, struct ct_trans *trans,
    struct ct_header *hdr, void *vbody)
{
	char *filename;

	switch (trans->tr_state) {
	case TR_S_XML_OPEN:
		if (ct_parse_xml_open_reply(hdr, vbody, &filename) != 0)
			CFATALX("XXX");
		if (filename == NULL)
			CFATALX("couldn't open remote file");
		CNDBG(CT_LOG_FILE, "%s opened\n",
		    filename);
		e_free(&filename);
		ct_set_file_state(state, CT_S_RUNNING);
		ct_wakeup_file(state->event_state);
		trans->tr_state = TR_S_XML_OPENED;
		break;
	case TR_S_XML_CLOSING:
		if (ct_parse_xml_close_reply(hdr, vbody) != 0)
			CFATALX("XXX");
		trans->tr_state = TR_S_DONE;
		break;
	case TR_S_XML_LIST:
		if (ct_parse_xml_list_reply(hdr, vbody,
		    &state->ctfile_list_files) != 0)
			CFATALX("XXX");
		trans->tr_state = TR_S_DONE;
		break;
	case TR_S_XML_DELETE:
		if (ct_parse_xml_delete_reply(hdr, vbody, &filename) != 0)
			CFATALX("XXX");
		if (filename == NULL)
			printf("specified archive does not exist\n");
		else
			printf("%s deleted\n", filename);
		trans->tr_state = TR_S_DONE;
		break;
	case TR_S_XML_CULL_SEND:
		/* XXX this is for both complete and setup */
		if (ct_parse_xml_cull_setup_reply(hdr, vbody))
			CFATALX("XXX");
		trans->tr_state = TR_S_DONE;
		break;
	case TR_S_XML_CULL_SHA_SEND:
		if (ct_parse_xml_cull_shas_reply(hdr, vbody))
			CFATALX("XXX");
		if (trans->tr_eof == 1)
			trans->tr_state = TR_S_DONE;
		else
			trans->tr_state = TR_S_XML_CULL_REPLIED;
		break;
	case TR_S_XML_CULL_COMPLETE_SEND:
		if (ct_parse_xml_cull_complete_reply(hdr, vbody))
			CFATALX("XXX");
		trans->tr_state = TR_S_DONE;
		break;
	default:
#if defined (CT_EXT_XML_REPLY_HANDLER)
		CT_EXT_XML_REPLY_HANDLER(trans, hdr, vbody);
#endif
		CABORTX("unexpected transaction state %d", trans->tr_state);
	}

	ct_queue_transfer(state, trans);
	ct_body_free(state, vbody, hdr);
	ct_header_free(state, hdr);
}

#if 0
/*
 * Delete all metadata files that were found by the preceding list operation.
 */
void
ctfile_trigger_delete(struct ct_op *op)
{
	struct ctfile_list_tree	 results;
	struct ctfile_list_file	*file = NULL;

	RB_INIT(&results);
	ctfile_list_complete(op->op_matchmode, op->op_filelist,
	    op->op_excludelist, &results);
	while((file = RB_ROOT(&results)) != NULL) {
		CNDBG(CT_LOG_CRYPTO, "deleting remote crypto secrets file %s",
		    file->mlf_name);
		ct_add_operation_after(op, ctfile_delete, NULL, NULL,
		    e_strdup(file->mlf_name), NULL, NULL, NULL, 0, 0);
		RB_REMOVE(ctfile_list_tree, &results, file);
		e_free(&file);
	}
}
#endif

/*
 * Verify that the ctfile name is kosher for remote mode.
 * - Encode the name (with a fake prefix) to make sure it fits.
 * - To help with interoperability, scan for a few special characters
 *   and punt if we find those.
 */
int
ctfile_verify_name(char *ctfile)
{
	const char	*set = CT_CTFILE_REJECTCHRS;
	char		 b[CT_CTFILE_MAXLEN], b64[CT_CTFILE_MAXLEN];
	size_t		 span, ctfilelen;
	int		 sz;

	if (ctfile == NULL)
		return 1;

	sz = snprintf(b, sizeof(b), "YYYYMMDD-HHMMSS-%s", ctfile);
	if (sz == -1 || sz >= sizeof(b))
		return 1;

	/* Make sure it fits. */
	sz = ct_base64_encode(CT_B64_M_ENCODE, (uint8_t *)b, strlen(b),
	    (uint8_t *)b64, sizeof(b64));
	if (sz != 0)
		return 1;

	ctfilelen = strlen(ctfile);
	span = strcspn(ctfile, set);
	return !(span == ctfilelen);
}

/*
 * Data structures to hold cull data
 *
 * Should this be stored in memory or build a temporary DB
 * to hold it due to the number of shas involved?
 */

struct ct_sha_lookup ct_sha_rb_head = RB_INITIALIZER(&ct_sha_rb_head);
uint64_t shacnt;
uint64_t sha_payload_sz;

int ct_cmp_sha(struct sha_entry *, struct sha_entry *);

RB_PROTOTYPE(ct_sha_lookup, sha_entry, s_rb, ct_cmp_sha);
RB_GENERATE(ct_sha_lookup, sha_entry, s_rb, ct_cmp_sha);

int
ct_cmp_sha(struct sha_entry *d1, struct sha_entry *d2)
{
	return bcmp(d1->sha, d2->sha, sizeof (d1->sha));
}

void
ct_cull_sha_insert(const uint8_t *sha)
{
	//char			shat[SHA_DIGEST_STRING_LENGTH];
	struct sha_entry	*node, *oldnode;

	node = e_malloc(sizeof(*node));
	bcopy (sha, node->sha, sizeof(node->sha));

	//ct_sha1_encode((uint8_t *)sha, shat);
	//printf("adding sha %s\n", shat);

	oldnode = RB_INSERT(ct_sha_lookup, &ct_sha_rb_head, node);
	if (oldnode != NULL) {
		/* already present, throw away copy */
		e_free(&node);
	} else
		shacnt++;

}

void
ct_cull_kick(struct ct_global_state *state)
{

	CNDBG(CT_LOG_TRANS, "add_op cull_setup");
	CNDBG(CT_LOG_SHA, "shacnt %" PRIu64 , shacnt);

	ct_add_operation(state, ctfile_list_start,
	    ct_cull_fetch_all_ctfiles, NULL);
	ct_add_operation(state, ct_cull_collect_ctfiles, NULL,  NULL);
	ct_add_operation(state, ct_cull_setup, NULL, NULL);
	ct_add_operation(state, ct_cull_send_shas, NULL, NULL);
	ct_add_operation(state, ct_cull_send_complete, ct_cull_complete, NULL);
}

int
ct_cull_add_shafile(const char *file, const char *cachedir)
{
	struct ctfile_parse_state	xs_ctx;
	char				*ct_next_filename;
	char				*ct_filename_free = NULL;
	char				*cachename;
	int				ret;

	CNDBG(CT_LOG_TRANS, "processing [%s]", file);

	/*
	 * XXX - should we keep a list of added files,
	 * since we do files based on the list and 'referenced' files?
	 * rather than operating on files multiple times?
	 * might be useful for marking files at 'do not delete'
	 * (depended on by other MD archives.
	 */

next_file:
	ct_next_filename = NULL;

	/* filename may be absolute, or in cache dir */
	if (ct_absolute_path(file)) {
		cachename = e_strdup(file);
	} else {
		e_asprintf(&cachename, "%s%s", cachedir, file);
	}

	ret = ctfile_parse_init(&xs_ctx, cachename, cachedir);
	e_free(&cachename);
	CNDBG(CT_LOG_CTFILE, "opening [%s]", file);

	if (ret)
		CFATALX("failed to open %s", file);

	if (ct_filename_free) {
		e_free(&ct_filename_free);
	}

	if (xs_ctx.xs_gh.cmg_prevlvl_filename) {
		CNDBG(CT_LOG_CTFILE, "previous backup file %s\n",
		    xs_ctx.xs_gh.cmg_prevlvl_filename);
		ct_next_filename = e_strdup(xs_ctx.xs_gh.cmg_prevlvl_filename);
		ct_filename_free = ct_next_filename;
	}

	do {
		ret = ctfile_parse(&xs_ctx);
		switch (ret) {
		case XS_RET_FILE:
			/* nothing to do, ct_populate_fnode2 is optional now */
			break;
		case XS_RET_FILE_END:
			/* nothing to do */
			break;
		case XS_RET_SHA:
			if (xs_ctx.xs_gh.cmg_flags & CT_MD_CRYPTO)
				ct_cull_sha_insert(xs_ctx.xs_csha);
			else
				ct_cull_sha_insert(xs_ctx.xs_sha);
			break;
		case XS_RET_EOF:
			break;
		case XS_RET_FAIL:
			;
		}

	} while (ret != XS_RET_EOF && ret != XS_RET_FAIL);

	ctfile_parse_close(&xs_ctx);

	if (ret != XS_RET_EOF) {
		CWARNX("end of archive not hit");
	} else {
		if (ct_next_filename) {
			file = ct_next_filename;
			goto next_file;
		}
	}
	return (0);
}

void
ct_cull_complete(struct ct_global_state *state, struct ct_op *op)
{
	CNDBG(CT_LOG_SHA, "shacnt %" PRIu64 " shapayload %" PRIu64, shacnt,
	    sha_payload_sz);
}

uint64_t cull_uuid; /* set up with random number in ct_cull_setup() */
/* tune this */
int sha_per_packet = 1000;

void
ct_cull_setup(struct ct_global_state *state, struct ct_op *op)
{
	struct ct_trans			*trans;

	arc4random_buf(&cull_uuid, sizeof(cull_uuid));

	CNDBG(CT_LOG_TRANS, "cull_setup");
	ct_set_file_state(state, CT_S_RUNNING);

	trans = ct_trans_alloc(state);

	if (trans == NULL) {
		ct_set_file_state(state, CT_S_WAITING_TRANS);
		return;
	}

	if (ct_create_xml_cull_setup(&trans->hdr, (void **)&trans->tr_data[2],
	    cull_uuid, CT_CULL_PRECIOUS) != 0)
		CFATALX("Could not create xml cull setup packet");
	trans->tr_dataslot = 2;
	trans->tr_size[2] = trans->hdr.c_size;
	trans->tr_state = TR_S_XML_CULL_SEND;

	ct_queue_first(state, trans);
}

int sent_complete;
void
ct_cull_send_complete(struct ct_global_state *state, struct ct_op *op)
{
	struct ct_trans			*trans;

	if (sent_complete) {
		return;
	}

	CNDBG(CT_LOG_TRANS, "send cull_complete");
	trans = ct_trans_alloc(state);

	if (trans == NULL) {
		ct_set_file_state(state, CT_S_WAITING_TRANS);
		return;
	}
	sent_complete = 1;

	if (ct_create_xml_cull_complete(&trans->hdr,
	    (void **)&trans->tr_data[2], cull_uuid, CT_CULL_PROCESS) != 0)
		CFATALX("Could not create xml cull setup packet");
	trans->tr_dataslot = 2;
	trans->tr_size[2] = trans->hdr.c_size;
	trans->tr_state = TR_S_XML_CULL_COMPLETE_SEND;
	ct_set_file_state(state, CT_S_FINISHED);

	ct_queue_first(state, trans);
}


void
ct_cull_send_shas(struct ct_global_state *state, struct ct_op *op)
{
	struct ct_trans			*trans;
	int				 sha_add;

	CNDBG(CT_LOG_TRANS, "cull_send_shas");
	if (shacnt == 0 || RB_EMPTY(&ct_sha_rb_head)) {
		ct_set_file_state(state, CT_S_FINISHED);
		return;
	}
	ct_set_file_state(state, CT_S_RUNNING);

	trans = ct_trans_alloc(state);

	if (trans == NULL) {
		ct_set_file_state(state, CT_S_WAITING_TRANS);
		return;
	}

	trans->tr_state = TR_S_XML_CULL_SHA_SEND;
	if (ct_create_xml_cull_shas(&trans->hdr, (void **)&trans->tr_data[2],
	    cull_uuid, &ct_sha_rb_head, sha_per_packet, &sha_add) != 0)
		CFATALX("can't create cull shas packet");
	shacnt -= sha_add;
	trans->tr_dataslot = 2;
	trans->tr_size[2] = trans->hdr.c_size;

	CNDBG(CT_LOG_SHA, "sending shas [%s]", (char *)trans->tr_data[2]);
	CNDBG(CT_LOG_SHA, "sending shas len %lu",
	    (unsigned long)trans->hdr.c_size);
	sha_payload_sz += trans->hdr.c_size;

	if (shacnt == 0 || RB_EMPTY(&ct_sha_rb_head)) {
		ct_set_file_state(state, CT_S_FINISHED);
		trans->tr_eof = 1;
		CNDBG(CT_LOG_SHA, "shacnt %" PRIu64, shacnt);
	}
	ct_queue_first(state, trans);
}

/*
 * Code to get all metadata files on the server.
 * to be used for cull.
 */
struct ctfile_list_tree ct_cull_all_ctfiles =
    RB_INITIALIZER(&ct_cull_all_ctfiles);
char		*all_ctfiles_pattern[] = {
			"^[[:digit:]]{8}-[[:digit:]]{6}-.*",
			NULL,
		 };

ct_op_cb	ct_cull_extract_cleanup;
void
ct_cull_fetch_all_ctfiles(struct ct_global_state *state, struct ct_op *op)
{
	struct ct_ctfileop_args	*cca;
	struct ctfile_list_tree	 results;
	struct ctfile_list_file	*file;
	char			*cachename;

	RB_INIT(&results);
	ctfile_list_complete(&state->ctfile_list_files, CT_MATCH_REGEX,
	    all_ctfiles_pattern, NULL, &results);
	while ((file = RB_ROOT(&results)) != NULL) {
		RB_REMOVE(ctfile_list_tree, &results, file);
		CNDBG(CT_LOG_CTFILE, "looking for file %s ", file->mlf_name);
		if (!ctfile_in_cache(file->mlf_name,
		    state->ct_config->ct_ctfile_cachedir)) {
			cachename = ctfile_get_cachename(file->mlf_name,
			    state->ct_config->ct_ctfile_cachedir);
			CNDBG(CT_LOG_CTFILE, "getting %s to %s", file->mlf_name,
			    cachename);
			cca = e_calloc(1, sizeof(*cca));
			cca->cca_localname =  e_strdup(file->mlf_name);
			cca->cca_remotename = cca->cca_localname;
			cca->cca_tdir = state->ct_config->ct_ctfile_cachedir;
			cca->cca_ctfile = 1;
			ct_add_operation_after(state, op, ctfile_extract,
			    ct_cull_extract_cleanup, cca);
		} else {
			CNDBG(CT_LOG_CTFILE, "already got %s", file->mlf_name);
		}
		RB_INSERT(ctfile_list_tree, &ct_cull_all_ctfiles, file);
	}
}

void
ct_cull_extract_cleanup(struct ct_global_state *state, struct ct_op *op)
{
	struct ct_ctfileop_args *cca = op->op_args;

	e_free(&cca->cca_localname);
	e_free(&cca);
}

ct_op_cb	ct_cull_delete_cleanup;
void
ct_cull_collect_ctfiles(struct ct_global_state *state, struct ct_op *op)
{
	struct ctfile_list_file	*file, *prevfile, filesearch;
	char			*prev_filename;
	int			timelen;
	char			buf[TIMEDATA_LEN];
	time_t			now;
	int			keep_files = 0;

	if (state->ct_config->ct_ctfile_keep_days == 0)
		CFATALX("cull: ctfile_cull_keep_days must be specified in "
		    "config");

	now = time(NULL);
	now -= (24 * 60 * 60 * state->ct_config->ct_ctfile_keep_days);
	if (strftime(buf, TIMEDATA_LEN, "%Y%m%d-%H%M%S",
	    localtime(&now)) == 0)
		CFATALX("can't format time");

	timelen = strlen(buf);

	RB_FOREACH(file, ctfile_list_tree, &ct_cull_all_ctfiles) {
		if (strncmp (file->mlf_name, buf, timelen) < 0) {
			file->mlf_keep = 0;
		} else {
			file->mlf_keep = 1;
			keep_files++;
		}
	}

	if (keep_files == 0)
		CFATALX("All ctfiles are old and would be deleted, aborting.");

	RB_FOREACH(file, ctfile_list_tree, &ct_cull_all_ctfiles) {
		if (file->mlf_keep == 0)
			continue;

		prev_filename = ctfile_get_previous(file->mlf_name);
prev_ct_file:
		if (prev_filename != NULL) {
			CINFO("prev filename %s", prev_filename);
			strncpy(filesearch.mlf_name, prev_filename,
			    sizeof(filesearch.mlf_name));
			prevfile = RB_FIND(ctfile_list_tree,
			    &ct_cull_all_ctfiles, &filesearch);
			if (prevfile == NULL) {
				CWARNX("file not found in ctfilelist [%s]",
				    prev_filename);
			} else {
				if (prevfile->mlf_keep == 0)
					CINFO("Warning, old ctfile %s still "
					    "referenced by newer backups, "
					    "keeping", prev_filename);
				prevfile->mlf_keep++;
				e_free(&prev_filename);

				prev_filename = ctfile_get_previous(
				    prevfile->mlf_name);
				goto prev_ct_file;
			}
			e_free(&prev_filename);
		}
	}
	RB_FOREACH(file, ctfile_list_tree, &ct_cull_all_ctfiles) {
		if (file->mlf_keep == 0) {
			CNDBG(CT_LOG_CTFILE, "adding %s to delete list",
			    file->mlf_name);
			ct_add_operation(state, ctfile_delete,
			    ct_cull_delete_cleanup, e_strdup(file->mlf_name));
		} else {
			CNDBG(CT_LOG_CTFILE, "adding %s to keep list",
			    file->mlf_name);
			ct_cull_add_shafile(file->mlf_name,
			    state->ct_config->ct_ctfile_cachedir);
		}
	}

	/* cleanup */
	while((file = RB_ROOT(&ct_cull_all_ctfiles)) != NULL) {
		RB_REMOVE(ctfile_list_tree, &ct_cull_all_ctfiles, file);
		e_free(&file);
		/* XXX - name  */
	}
	ct_op_complete(state);
}

void
ct_cull_delete_cleanup(struct ct_global_state *state, struct ct_op *op)
{
	char	*ctfile = op->op_args;

	e_free(&ctfile);
}