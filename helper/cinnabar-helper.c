/* Helper program for git-cinnabar
 *
 * It receives commands on stdin and outputs results on stdout.
 * The following commands are supported:
 * - git2hg <committish>
 *     Returns the contents of the git note containing git->hg metadata
 *     for the given commit in a `cat-file --batch`-like format.
 * - hg2git <hg_sha1>
 *     Returns the sha1 of the git object corresponding to the given
 *     mercurial sha1.
 * - manifest <hg_sha1>
 *     Returns the contents of the mercurial manifest with the given
 *     mercurial sha1, preceded by its length in text form, and followed
 *     by a carriage return.
 * - check-manifest <hg_sha1>
 *     Returns 'ok' when the sha1 of the contents of the mercurial manifest
 *     matches the manifest sha1, otherwise returns 'error'.
 * - cat-file <object>
 *     Returns the contents of the given git object, in a `cat-file
 *     --batch`-like format.
 *  - connect <url>
 *     Connects to the mercurial repository at the given url. The helper then
 *     expects one of the following commands:
 *     - state
 *       This prints out three blocks of data, being the result of the
 *       following commands on the repository: branchmap, heads, bookmarks.
 *     - known <node>+
 *       Calls the "known" command on the repository and returns the
 *       corresponding result.
 *     - listkeys <namespace>
 *     	 Calls the "listkeys" command on the repository and returns the
 *     	 corresponding result.
 *     - getbundle <heads> <common> <bundle2caps>
 *       Calls the "getbundle" command on the repository and streams a
 *       changegroup in result. `heads` and `common` are comma separated
 *       lists of changesets.
 *     - unbundle <head>+
 *       Calls the "unbundle command on the repository.
 *     - pushkey <namespace> <key> <old> <new>
 *     	 Calls the "pushkey" command on the repository and returns the
 *     	 corresponding result.
 *     - lookup <key>
 *       Calls the "lookup" command on the repository and returns the
 *     	 corresponding result.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cache.h"
#include "blob.h"
#include "commit.h"
#include "diff.h"
#include "diffcore.h"
#include "exec_cmd.h"
#include "log-tree.h"
#include "strbuf.h"
#include "string-list.h"
#include "notes.h"
#include "streaming.h"
#include "object.h"
#include "oidset.h"
#include "progress.h"
#include "quote.h"
#include "revision.h"
#include "tree.h"
#include "tree-walk.h"
#include "hg-connect.h"
#include "hg-data.h"
#include "cinnabar-helper.h"
#include "cinnabar-fast-import.h"

#define STRINGIFY(s) _STRINGIFY(s)
#define _STRINGIFY(s) # s

#ifndef HELPER_HASH
#define HELPER_HASH unknown
#endif

#define CMD_VERSION 2400

static const char NULL_NODE[] = "0000000000000000000000000000000000000000";

struct notes_tree git2hg, hg2git, files_meta;

struct oidset hg2git_seen = OIDSET_INIT;

int metadata_flags = 0;
int cinnabar_check = 0;

static int config(const char *name, struct strbuf *result)
{
	struct strbuf key = STRBUF_INIT;
	char *p, *end;
	const char *val;

	strbuf_addstr(&key, "GIT_CINNABAR_");
	strbuf_addstr(&key, name);
	for (p = key.buf + sizeof("git_cinnabar"), end = key.buf + key.len;
	     p < end; p++)
		*p = toupper(*p);
	val = getenv(key.buf);
	if (!val) {
		strbuf_release(&key);
		strbuf_addstr(&key, "cinnabar.");
		strbuf_addstr(&key, name);
		if (git_config_get_value(key.buf, &val))
			return 1;
	}
	strbuf_addstr(result, val);
	strbuf_release(&key);
	return 0;
}

static int cleanup_object_array_entry(struct object_array_entry *entry, void *data)
{
	if (entry->item->type == OBJ_TREE)
		free_tree_buffer((struct tree *)entry->item);
	return 1;
}

static void rev_info_release(struct rev_info *revs)
{
	int i;

	object_array_filter(&revs->pending, cleanup_object_array_entry, NULL);
	object_array_clear(&revs->pending);
	object_array_clear(&revs->boundary_commits);
	for (i = 0; i < revs->cmdline.nr; i++)
		free((void *)revs->cmdline.rev[i].name);
	free(revs->cmdline.rev);
	revs->cmdline.rev = NULL;
}

static void split_command(char *line, const char **command,
			  struct string_list *args)
{
	struct string_list split_line = STRING_LIST_INIT_NODUP;
	string_list_split_in_place(&split_line, line, ' ', 1);
	*command = split_line.items[0].string;
	if (split_line.nr > 1)
		string_list_split_in_place(
			args, split_line.items[1].string, ' ', -1);
	string_list_clear(&split_line, 0);
}

static int can_send_null_buffer = 0;

static void send_buffer(struct strbuf *buf)
{
	if (buf) {
		struct strbuf header = STRBUF_INIT;

		strbuf_addf(&header, "%lu\n", buf->len);
		write_or_die(1, header.buf, header.len);
		strbuf_release(&header);

		write_or_die(1, buf->buf, buf->len);
		write_or_die(1, "\n", 1);
	} else if (can_send_null_buffer) {
		write_or_die(1, "-1\n\n", 4);
	} else {
		write_or_die(1, "0\n\n", 3);
	}
}

/* Send git object info and content to stdout, like cat-file --batch does. */
static void send_object(unsigned const char *sha1)
{
	struct strbuf header = STRBUF_INIT;
	enum object_type type;
	unsigned long sz;
	struct git_istream *st;

	st = open_istream(sha1, &type, &sz, NULL);

	strbuf_addf(&header, "%s %s %lu\n", sha1_to_hex(sha1), typename(type),
	            sz);

	write_or_die(1, header.buf, header.len);

	strbuf_release(&header);

	for (;;) {
		char buf[1024 * 16];
		ssize_t wrote;
		ssize_t readlen = read_istream(st, buf, sizeof(buf));

		if (readlen <= 0)
			break;

		wrote = write_in_full(1, buf, readlen);
		if (wrote < readlen)
			break;

		sz -= wrote;
	}

	if (sz != 0)
		die("Failed to write object");

	write_or_die(1, "\n", 1);

	close_istream(st);
}

static void do_cat_file(struct string_list *args)
{
	unsigned char sha1[20];

	if (args->nr != 1)
		goto not_found;

	if (get_sha1(args->items[0].string, sha1))
		goto not_found;

	send_object(sha1);
	return;

not_found:
	write_or_die(1, NULL_NODE, 40);
	write_or_die(1, "\n", 1);
}

struct ls_tree_context {
	struct strbuf buf;
	struct object_list *list;
	int recursive;
};

static int fill_ls_tree(const unsigned char *sha1, struct strbuf *base,
			const char *pathname, unsigned mode, int stage,
			void *context)
{
	struct ls_tree_context *ctx = (struct ls_tree_context *) context;
	struct strbuf *buf = &ctx->buf;
	const char *type = blob_type;

	if (S_ISGITLINK(mode)) {
		type = commit_type;
	} else if (S_ISDIR(mode)) {
		object_list_insert((struct object *)lookup_tree(sha1),
		                   &ctx->list);
		if (ctx->recursive)
			return READ_TREE_RECURSIVE;
		type = tree_type;
	}

	strbuf_addf(buf, "%06o %s %s\t", mode, type, sha1_to_hex(sha1));
	strbuf_addbuf(buf, base);
	strbuf_addstr(buf, pathname);
	strbuf_addch(buf, '\0');
	return 0;
}

static void do_ls_tree(struct string_list *args)
{
	unsigned char sha1[20];
	struct tree *tree = NULL;
	struct ls_tree_context ctx = { STRBUF_INIT, NULL, 0 };
	struct pathspec match_all;

	if (args->nr == 2) {
		if (strcmp(args->items[1].string, "-r"))
			goto not_found;
		ctx.recursive = 1;
	} else if (args->nr != 1)
		goto not_found;

	if (get_sha1(args->items[0].string, sha1))
		goto not_found;

	tree = parse_tree_indirect(sha1);
	if (!tree)
		goto not_found;

	memset(&match_all, 0, sizeof(match_all));
	read_tree_recursive(tree, "", 0, 0, &match_all, fill_ls_tree, &ctx);
	send_buffer(&ctx.buf);
	strbuf_release(&ctx.buf);

	while (ctx.list) {
		struct object *obj = ctx.list->item;
		struct object_list *elem = ctx.list;
		ctx.list = elem->next;
		free(elem);
		free_tree_buffer((struct tree *)obj);
	}
	return;
not_found:
	write_or_die(1, "0\n\n", 3);
}

static const char **string_list_to_argv(struct string_list *args)
{
	const char **argv = malloc(sizeof(char *) * (args->nr + 2));
	int i;

	argv[0] = "";
	for (i = 0; i < args->nr; i++) {
		argv[i + 1] = args->items[i].string;
	}
	argv[args->nr + 1] = NULL;

	return argv;
}

static void do_rev_list(struct string_list *args)
{
	struct rev_info revs;
	struct commit *commit;
	struct strbuf buf = STRBUF_INIT;
	const char **argv = string_list_to_argv(args);

	init_revisions(&revs, NULL);
	// Note: we do a pass through, but don't make much effort to actually
	// support all the options properly.
	setup_revisions(args->nr + 1, argv, &revs, NULL);
	free(argv);

	if (prepare_revision_walk(&revs))
		die("revision walk setup failed");

	while ((commit = get_revision(&revs)) != NULL) {
		struct commit_list *parent;
		if (commit->object.flags & BOUNDARY)
			strbuf_addch(&buf, '-');
		strbuf_addstr(&buf, oid_to_hex(&commit->object.oid));
		strbuf_addch(&buf, ' ');
		strbuf_addstr(&buf, oid_to_hex(&commit->tree->object.oid));
		parent = commit->parents;
		while (parent) {
			strbuf_addch(&buf, ' ');
			strbuf_addstr(&buf, oid_to_hex(
				&parent->item->object.oid));
			parent = parent->next;
		}
		strbuf_addch(&buf, '\n');
	}

	// More extensive than reset_revision_walk(). Otherwise --boundary
	// and pathspecs don't work properly.
	clear_object_flags(ALL_REV_FLAGS);
	send_buffer(&buf);
	strbuf_release(&buf);
	rev_info_release(&revs);
}

static void strbuf_diff_tree(struct diff_queue_struct *q,
                             struct diff_options *opt, void *data)
{
	struct strbuf *buf = (struct strbuf *) data;
	int i;

	for (i = 0; i < q->nr; i++) {
		struct diff_filepair *p = q->queue[i];
		if (p->status == 0)
			die("internal diff status error");
		if (p->status == DIFF_STATUS_UNKNOWN)
			continue;
		strbuf_addf(buf, "%06o %06o %s %s %c",
		            p->one->mode,
		            p->two->mode,
		            oid_to_hex(&p->one->oid),
		            oid_to_hex(&p->two->oid),
		            p->status);
		if (p->score)
			strbuf_addf(buf, "%03d",
			            (int)(p->score * 100 / MAX_SCORE));
		strbuf_addch(buf, '\t');
		if (p->status == DIFF_STATUS_COPIED ||
		    p->status == DIFF_STATUS_RENAMED) {
			strbuf_addstr(buf, p->one->path);
			strbuf_addch(buf, '\0');
			strbuf_addstr(buf, p->two->path);
		} else {
			strbuf_addstr(buf, p->one->mode ? p->one->path
			                                : p->two->path);
		}
		strbuf_addch(buf, '\0');
	}
}

static void do_diff_tree(struct string_list *args)
{
	struct rev_info revs;
	struct strbuf buf = STRBUF_INIT;
	const char **argv = string_list_to_argv(args);

	init_revisions(&revs, NULL);
	revs.diff = 1;
	// Note: we do a pass through, but don't make much effort to actually
	// support all the options properly.
	setup_revisions(args->nr + 1, argv, &revs, NULL);
	revs.diffopt.output_format = DIFF_FORMAT_CALLBACK;
	revs.diffopt.format_callback = strbuf_diff_tree;
	revs.diffopt.format_callback_data = &buf;
	DIFF_OPT_SET(&revs.diffopt, RECURSIVE);
	free(argv);

	if (revs.pending.nr != 2)
		die("diff-tree needs two revs");

	diff_tree_sha1(revs.pending.objects[0].item->oid.hash,
	               revs.pending.objects[1].item->oid.hash,
	               "", &revs.diffopt);
	log_tree_diff_flush(&revs);
	send_buffer(&buf);
	strbuf_release(&buf);
	rev_info_release(&revs);
}

static void do_get_note(struct notes_tree *t, struct string_list *args)
{
	unsigned char sha1[20];
	const unsigned char *note;

	if (args->nr != 1)
		goto not_found;

	ensure_notes(t);

	if (get_sha1_committish(args->items[0].string, sha1))
		goto not_found;

	note = get_note(t, lookup_replace_object(sha1));
	if (!note)
		goto not_found;

	send_object(note);
	return;

not_found:
	write_or_die(1, NULL_NODE, 40);
	write_or_die(1, "\n", 1);
}

static size_t get_abbrev_sha1_hex(const char *hex, unsigned char *sha1)
{
	const char *hex_start = hex;
	unsigned char *end = sha1 + 20;
	while (sha1 < end) {
		unsigned int val;
		if (!hex[0])
			val = 0xff;
		else if (!hex[1])
			val = (hexval(hex[0]) << 4) | 0xf;
		else
			val = (hexval(hex[0]) << 4) | hexval(hex[1]);
		if (val & ~0xff)
			return 0;
		*sha1++ = val;
		if (!hex[0] || !hex[1])
			break;
		hex += 2;
	}
	while (sha1 < end) {
		*sha1++ = 0xff;
	}
	return hex - hex_start + !!hex[0];
}

int abbrev_sha1_cmp(const unsigned char *ref_sha1,
		    const unsigned char *abbrev_sha1, size_t len)
{
        int i;

        for (i = 0; i < len / 2; i++, ref_sha1++, abbrev_sha1++) {
                if (*ref_sha1 != *abbrev_sha1)
                        return *ref_sha1 - *abbrev_sha1;
        }

	if (len % 2) {
		unsigned char ref_bits = *ref_sha1 & 0xf0;
		unsigned char abbrev_bits = *abbrev_sha1 & 0xf0;
		if (ref_bits != abbrev_bits)
			return ref_bits - abbrev_bits;
	}

        return 0;

}

/* Definitions from git's notes.c. See there for more details */
struct int_node {
	void *a[16];
};

struct leaf_node {
	unsigned char key_sha1[20];
	unsigned char val_sha1[20];
};

#define PTR_TYPE_NULL     0
#define PTR_TYPE_INTERNAL 1
#define PTR_TYPE_NOTE     2
#define PTR_TYPE_SUBTREE  3

#define GET_PTR_TYPE(ptr)       ((uintptr_t) (ptr) & 3)
#define CLR_PTR_TYPE(ptr)       ((void *) ((uintptr_t) (ptr) & ~3))

#define GET_NIBBLE(n, sha1) (((sha1[(n) >> 1]) >> ((~(n) & 0x01) << 2)) & 0x0f)

/* This function assumes the note tree has been populated for the given key,
 * which means get_note must have been called before */
static struct leaf_node *note_tree_abbrev_find(struct notes_tree *t,
		struct int_node *tree, unsigned char n,
		const unsigned char *key_sha1, size_t len)
{
	unsigned char i, j;
	void *p;

	if (n > len) {
		for (i = 17, j = 0; j < 16; j++) {
			if (tree->a[j])
				i = (i < 17) ? 16 : j;
		}
		if (i >= 16)
			return NULL;
	} else {
		i = GET_NIBBLE(n, key_sha1);
	}

	p = tree->a[i];

	switch (GET_PTR_TYPE(p)) {
	case PTR_TYPE_INTERNAL:
		tree = CLR_PTR_TYPE(p);
		return note_tree_abbrev_find(t, tree, ++n, key_sha1, len);
	case PTR_TYPE_SUBTREE:
		return NULL;
	default:
		{
			struct leaf_node *node = CLR_PTR_TYPE(p);
			if (node && !abbrev_sha1_cmp(node->key_sha1, key_sha1,
			                             len))
				return node;
			return NULL;
		}
	}
}

const unsigned char *get_abbrev_note(struct notes_tree *t,
		const unsigned char *object_sha1, size_t len)
{
	struct leaf_node *found;

	if (!t)
		t = &default_notes_tree;
	assert(t->initialized);
	found = note_tree_abbrev_find(t, t->root, 0, object_sha1, len);
	return found ? found->val_sha1 : NULL;
}


static const unsigned char *resolve_hg2git(const unsigned char *sha1,
                                           size_t len)
{
	const unsigned char *note;

	ensure_notes(&hg2git);

	note = get_note(&hg2git, sha1);
	if (len == 40)
		return note;

	return get_abbrev_note(&hg2git, sha1, len);
}

static void do_hg2git(struct string_list *args)
{
	unsigned char sha1[20];
	const unsigned char *note;
	size_t sha1_len;

	if (args->nr != 1)
		goto not_found;

	sha1_len =  get_abbrev_sha1_hex(args->items[0].string, sha1);
	if (!sha1_len)
		goto not_found;

	note = resolve_hg2git(sha1, sha1_len);
	if (note) {
		write_or_die(1, sha1_to_hex(note), 40);
		write_or_die(1, "\n", 1);
		return;
	}

not_found:
	write_or_die(1, NULL_NODE, 40);
	write_or_die(1, "\n", 1);
}

/* Return the mercurial manifest character corresponding to the given
 * git file mode. */
static const char *hgattr(unsigned int mode)
{
	if (S_ISLNK(mode))
		return "l";
	if (S_ISREG(mode)) {
		if ((mode & 0755) == 0755)
			return "x";
		else if ((mode & 0644) == 0644)
			return "";
	}
	die("Unsupported mode %06o", mode);
}

/* The git storage for a mercurial manifest is a commit with two directories
 * at its root:
 * - a git directory, matching the git tree in the git commit corresponding to
 *   the mercurial changeset using the manifest.
 * - a hg directory, containing the same file paths, but where all pointed
 *   objects are commits (mode 160000 in the git tree) whose sha1 is actually
 *   the mercurial sha1 for the corresponding mercurial file.
 * Reconstructing the mercurial manifest requires file paths, mercurial sha1
 * for each file, and the corresponding attribute ("l" for symlinks, "x" for
 * executables"). The hg directory alone is not enough for that, because it
 * lacks the attribute information. So, both directories are recursed in
 * parallel to generate the original manifest data.
 */
struct manifest_tree {
	unsigned char git[20];
	unsigned char hg[20];
};

static void track_tree(struct tree *tree, struct object_list **tree_list)
{
	if (tree_list) {
		object_list_insert(&tree->object, tree_list);
		tree->object.flags |= SEEN;
	}
}

/* Fills a manifest_tree with the tree sha1s for the git/ and hg/
 * subdirectories of the given (git) manifest tree. */
static int get_manifest_tree(struct tree *tree,
                             struct manifest_tree *result,
                             struct object_list **tree_list)
{
	struct tree_desc desc;
	struct name_entry entry;

	track_tree(tree, tree_list);

	/* If the tree is empty, return an empty tree for both git
	 * and hg. */
	if (!tree->size) {
		hashcpy(result->git, tree->object.oid.hash);
		hashcpy(result->hg, tree->object.oid.hash);
		return 0;
	}

	init_tree_desc(&desc, tree->buffer, tree->size);
	/* The first entry in the manifest tree is the git subtree. */
	if (!tree_entry(&desc, &entry))
		goto not_found;
	if (strcmp(entry.path, "git"))
		goto not_found;
	hashcpy(result->git, entry.oid->hash);

	/* The second entry in the manifest tree is the hg subtree. */
	if (!tree_entry(&desc, &entry))
		goto not_found;
	if (strcmp(entry.path, "hg"))
		goto not_found;
	hashcpy(result->hg, entry.oid->hash);

	/* There shouldn't be any other entry. */
	if (tree_entry(&desc, &entry))
		goto not_found;

	return 0;

not_found:
	return -1;
}

struct manifest_tree_state {
	struct tree *tree_git, *tree_hg;
	struct tree_desc desc_git, desc_hg;
};

static int manifest_tree_state_init(const struct manifest_tree *tree,
                                    struct manifest_tree_state *result,
                                    struct object_list **tree_list)
{
	result->tree_git = parse_tree_indirect(tree->git);
	if (!result->tree_git)
		return -1;
	track_tree(result->tree_git, tree_list);

	result->tree_hg = parse_tree_indirect(tree->hg);
	if (!result->tree_hg)
		return -1;
	track_tree(result->tree_hg, tree_list);

	init_tree_desc(&result->desc_git, result->tree_git->buffer,
	               result->tree_git->size);
	init_tree_desc(&result->desc_hg, result->tree_hg->buffer,
	               result->tree_hg->size);
	return 0;
}

struct manifest_entry {
	const unsigned char *sha1;
	const unsigned char *other_sha1;
	const char *path;
	unsigned int mode;
};

/* Like tree_entry, returns true for success. */
static int manifest_tree_entry(struct manifest_tree_state *state,
                               struct manifest_entry *result)
{
	struct name_entry entry_git, entry_hg;
	int has_git_entry = tree_entry(&state->desc_git, &entry_git);
	int has_hg_entry = tree_entry(&state->desc_hg, &entry_hg);
	if (has_git_entry != has_hg_entry)
		goto corrupted;
	if (!has_git_entry) {
		result->path = NULL;
		return 0;
	}

	result->sha1 = entry_hg.oid->hash;
	result->path = entry_hg.path;
	result->mode = entry_git.mode;
	if (strcmp(entry_hg.path, entry_git.path))
		goto corrupted;
	if (S_ISDIR(entry_git.mode)) {
		if (entry_git.mode != entry_hg.mode)
			goto corrupted;
	}
	result->other_sha1 = entry_git.oid->hash;
	return 1;
corrupted:
	die("Corrupted metadata");
}

static void recurse_manifest(const struct manifest_tree *tree,
                             struct strbuf *manifest, char *base,
                             struct object_list **tree_list)
{
	struct manifest_tree_state state;
	struct manifest_entry entry;
	size_t base_len = strlen(base);

	if (manifest_tree_state_init(tree, &state, tree_list))
		goto corrupted;

	while (manifest_tree_entry(&state, &entry)) {
		if (S_ISDIR(entry.mode)) {
			struct strbuf dir = STRBUF_INIT;
			struct manifest_tree subtree;
			if (base_len)
				strbuf_add(&dir, base, base_len);
			strbuf_addstr(&dir, entry.path);
			strbuf_addch(&dir, '/');
			hashcpy(subtree.git, entry.other_sha1);
			hashcpy(subtree.hg, entry.sha1);
			recurse_manifest(&subtree, manifest, dir.buf, tree_list);
			strbuf_release(&dir);
			continue;
		}
		strbuf_addf(manifest, "%s%s%c%s%s\n", base, entry.path,
		            '\0', sha1_to_hex(entry.sha1), hgattr(entry.mode));
	}

	return;
corrupted:
	die("Corrupted metadata");

}

struct strslice {
	size_t len;
	const char *buf;
};

/* Return whether two entries have matching sha1s and modes */
static int manifest_entry_equal(const struct manifest_entry *e1,
                                const struct manifest_entry *e2)
{
	if (e1->mode != e2->mode)
		return 0;
	if (hashcmp(e1->sha1, e2->sha1))
		return 0;
	if (!S_ISDIR(e1->mode))
		return 1;
	/* For trees, both sha1 need to match */
	return hashcmp(e1->other_sha1, e2->other_sha1) == 0;
}

/* Return whether base + name matches path */
static int path_match(const char *base, size_t base_len,
                      const char *name, size_t name_len, const char *path)
{
	return memcmp(base, path, base_len) == 0 &&
	       memcmp(name, path + base_len, name_len) == 0 &&
	       (path[base_len + name_len] == '\0' ||
	        path[base_len + name_len] == '/');
}

static void recurse_manifest2(const struct manifest_tree *ref_tree,
                              struct strslice *ref_manifest,
                              const struct manifest_tree *tree,
                              struct strbuf *manifest, char *base,
                              struct object_list **tree_list)
{
	struct manifest_tree_state ref, cur;
	struct manifest_entry ref_entry, cur_entry;
	struct manifest_tree ref_subtree, cur_subtree;
	const char *next = ref_manifest->buf;
	struct strbuf dir = STRBUF_INIT;
	size_t base_len = strlen(base);
	size_t ref_entry_len = 0;
	int cmp = 0;

	if (manifest_tree_state_init(ref_tree, &ref, tree_list))
		goto corrupted;

	if (manifest_tree_state_init(tree, &cur, tree_list))
		goto corrupted;

	for (;;) {
		if (cmp >= 0)
			manifest_tree_entry(&cur, &cur_entry);
		if (cmp <= 0) {
			manifest_tree_entry(&ref, &ref_entry);
			assert(ref_manifest->buf + ref_manifest->len >= next);
			ref_manifest->len -= next - ref_manifest->buf;
			ref_manifest->buf = next;
			ref_entry_len = ref_entry.path ?
				strlen(ref_entry.path) : 0;
			assert(!ref_entry.path ||
			       path_match(base, base_len, ref_entry.path,
			                  ref_entry_len, next));
		}
		if (!ref_entry.path) {
			if (!cur_entry.path)
				break;
			cmp = 1;
		} else if (!cur_entry.path) {
			cmp = -1;
		} else {
			cmp = name_compare(
				ref_entry.path, ref_entry_len,
				cur_entry.path, strlen(cur_entry.path));
		}
		if (cmp <= 0) {
			const char *tail = next + ref_manifest->len;
			size_t len = base_len + ref_entry_len + 41;
			do {
				next = memchr(next + len, '\n', tail - next)
				       + 1;
			} while (S_ISDIR(ref_entry.mode) &&
			         (tail - next > len) &&
			         path_match(base, base_len, ref_entry.path,
			                    ref_entry_len, next));
		}
		/* File/directory was removed, nothing to do */
		if (cmp < 0)
			continue;
		/* File/directory didn't change, copy from the reference
		 * manifest. */
		if (cmp == 0 && manifest_entry_equal(&ref_entry, &cur_entry)) {
			strbuf_add(manifest, ref_manifest->buf,
			           next - ref_manifest->buf);
			continue;
		}
		if (!S_ISDIR(cur_entry.mode)) {
			strbuf_addf(manifest, "%s%s%c%s%s\n", base,
			            cur_entry.path, '\0',
			            sha1_to_hex(cur_entry.sha1),
			            hgattr(cur_entry.mode));
			continue;
		}

		if (base_len)
			strbuf_add(&dir, base, base_len);
		strbuf_addstr(&dir, cur_entry.path);
		strbuf_addch(&dir, '/');
		hashcpy(cur_subtree.git, cur_entry.other_sha1);
		hashcpy(cur_subtree.hg, cur_entry.sha1);
		if (cmp == 0 && S_ISDIR(ref_entry.mode)) {
			hashcpy(ref_subtree.git, ref_entry.other_sha1);
			hashcpy(ref_subtree.hg, ref_entry.sha1);
			recurse_manifest2(&ref_subtree, ref_manifest,
				          &cur_subtree, manifest, dir.buf,
			                  tree_list);
		} else
			recurse_manifest(&cur_subtree, manifest, dir.buf,
			                 tree_list);
		strbuf_release(&dir);
	}

	return;
corrupted:
	die("Corrupted metadata");
}

struct manifest {
	struct manifest_tree tree;
	struct strbuf content;
	struct object_list *tree_list;
};

#define MANIFEST_INIT { { { 0, }, { 0, } }, STRBUF_INIT, NULL }

/* For repositories with a lot of files, generating a manifest is a slow
 * operation.
 * In most cases, there are way less changes between changesets than there
 * are files in the repository, so it is much faster to generate a manifest
 * from a previously generated manifest, by applying the differences between
 * the corresponding trees.
 * Therefore, we always keep the last generated manifest.
 */
static struct manifest generated_manifest = MANIFEST_INIT;

/* The returned strbuf must not be released and/or freed. */
static struct strbuf *generate_manifest(const unsigned char *git_sha1)
{
	struct manifest_tree manifest_tree;
	struct strbuf content = STRBUF_INIT;
	struct object_list *tree_list = NULL;
	struct tree *tree = NULL;

	/* We keep a list of all the trees we've seen while generating the
	 * previous manifest. Each tree is marked as SEEN at that time.
	 * Then, on the next manifest generation, we unmark them as SEEN,
	 * and the generation that follows will re-mark them if they are
	 * re-used. Trees that are not marked SEEN are subsequently freed.
	 */
	struct object_list *previous_list = generated_manifest.tree_list;
	while (previous_list) {
		previous_list->item->flags &= ~SEEN;
		previous_list = previous_list->next;
	}

	tree = parse_tree_indirect(git_sha1);
	if (!tree)
		goto not_found;

	if (get_manifest_tree(tree, &manifest_tree, &tree_list))
		goto not_found;

	if (generated_manifest.content.len) {
		struct strslice gm = {
			generated_manifest.content.len,
			generated_manifest.content.buf
		};
		strbuf_grow(&content, generated_manifest.content.len);
		recurse_manifest2(&generated_manifest.tree, &gm,
		                  &manifest_tree, &content, "", &tree_list);
	} else {
		recurse_manifest(&manifest_tree, &content, "", &tree_list);
	}

	hashcpy(generated_manifest.tree.git, manifest_tree.git);
	hashcpy(generated_manifest.tree.hg, manifest_tree.hg);
	strbuf_swap(&content, &generated_manifest.content);
	strbuf_release(&content);

	previous_list = generated_manifest.tree_list;
	generated_manifest.tree_list = tree_list;

	while (previous_list) {
		struct object *obj = previous_list->item;
		struct object_list *elem = previous_list;
		previous_list = elem->next;
		free(elem);
		if (!obj->flags & SEEN)
			free_tree_buffer((struct tree *)obj);
	}
	return &generated_manifest.content;

not_found:
	return NULL;
}

static void do_manifest(struct string_list *args)
{
	unsigned char sha1[20];
	const unsigned char *manifest_sha1;
	struct strbuf *manifest = NULL;
	size_t sha1_len;

	if (args->nr != 1)
		goto not_found;

	if (!strncmp(args->items[0].string, "git:", 4)) {
		if (get_sha1_hex(args->items[0].string + 4, sha1))
			goto not_found;
		manifest_sha1 = sha1;
	} else {
		sha1_len = get_abbrev_sha1_hex(args->items[0].string, sha1);
		if (!sha1_len)
			goto not_found;

		manifest_sha1 = resolve_hg2git(sha1, sha1_len);
		if (!manifest_sha1)
			goto not_found;
	}

	manifest = generate_manifest(manifest_sha1);
	if (!manifest)
		goto not_found;

	send_buffer(manifest);
	return;

not_found:
	write_or_die(1, "0\n\n", 3);
}

static void get_manifest_sha1(const struct commit *commit, unsigned char *sha1)
{
	const char *msg;
	const char *hex_sha1;

	msg = get_commit_buffer(commit, NULL);

	hex_sha1 = strstr(msg, "\n\n") + 2;

	if (get_sha1_hex(hex_sha1, sha1))
		hashclr(sha1);

	unuse_commit_buffer(commit, msg);
}

static void hg_sha1(struct strbuf *data, const unsigned char *parent1,
                    const unsigned char *parent2, unsigned char *result)
{
	git_SHA_CTX ctx;

	if (!parent1)
		parent1 = null_sha1;
	if (!parent2)
		parent2 = null_sha1;

	git_SHA1_Init(&ctx);

	if (hashcmp(parent1, parent2) < 0) {
		git_SHA1_Update(&ctx, parent1, 20);
		git_SHA1_Update(&ctx, parent2, 20);
	} else {
		git_SHA1_Update(&ctx, parent2, 20);
		git_SHA1_Update(&ctx, parent1, 20);
	}

	git_SHA1_Update(&ctx, data->buf, data->len);

	git_SHA1_Final(result, &ctx);
}

static void do_check_manifest(struct string_list *args)
{
	unsigned char sha1[20], parent1[20], parent2[20], result[20];
	const unsigned char *manifest_sha1;
	const struct commit *manifest_commit;
	struct strbuf *manifest = NULL;

	if (args->nr != 1)
		goto error;

	if (!strncmp(args->items[0].string, "git:", 4)) {
		if (get_sha1_hex(args->items[0].string + 4, sha1))
			goto error;
		manifest_sha1 = sha1;
	} else {
		if (get_sha1_hex(args->items[0].string, sha1))
			goto error;

		manifest_sha1 = resolve_hg2git(sha1, 40);
		if (!manifest_sha1)
			goto error;
	}

	manifest = generate_manifest(manifest_sha1);
	if (!manifest)
		goto error;

	manifest_commit = lookup_commit(manifest_sha1);
	if (!manifest_commit)
		goto error;

	if (manifest_commit->parents) {
		get_manifest_sha1(manifest_commit->parents->item, parent1);
		if (manifest_commit->parents->next) {
			get_manifest_sha1(manifest_commit->parents->next->item,
			                  parent2);
		} else
			hashclr(parent2);
	} else {
		hashclr(parent1);
		hashclr(parent2);
	}

	hg_sha1(manifest, parent1, parent2, result);

	if (manifest_sha1 == sha1)
		get_manifest_sha1(manifest_commit, sha1);

	if (hashcmp(result, sha1) == 0) {
		write_or_die(1, "ok\n", 3);
		return;
	}

error:
	write_or_die(1, "error\n", 6);
}

static void do_check_file(struct string_list *args)
{
	struct hg_file file;
	unsigned char sha1[20], parent1[20], parent2[20], result[20];

	hg_file_init(&file);

	if (args->nr < 1 || args->nr > 3)
		goto error;

	if (get_sha1_hex(args->items[0].string, sha1))
		goto error;

	if (args->nr > 1) {
		if (get_sha1_hex(args->items[1].string, parent1))
			goto error;
	} else
		hashclr(parent1);

	if (args->nr > 2) {
		if (get_sha1_hex(args->items[2].string, parent2))
			goto error;
	} else
		hashclr(parent2);

	hg_file_load(&file, sha1);

	/* We do the quick and dirty thing here, for now.
	 * See details in cinnabar.githg.FileFindParents._set_parents_fallback
	 */
	hg_sha1(&file.file, parent1, parent2, result);
	if (hashcmp(sha1, result) == 0)
		goto ok;

	hg_sha1(&file.file, parent1, NULL, result);
	if (hashcmp(sha1, result) == 0)
		goto ok;

	hg_sha1(&file.file, parent2, NULL, result);
	if (hashcmp(sha1, result) == 0)
		goto ok;

	hg_sha1(&file.file, parent1, parent1, result);
	if (hashcmp(sha1, result) == 0)
		goto ok;

	hg_sha1(&file.file, NULL, NULL, result);
	if (hashcmp(sha1, result))
		goto error;

ok:
	write_or_die(1, "ok\n", 3);
	hg_file_release(&file);
	return;

error:
	write_or_die(1, "error\n", 6);
	hg_file_release(&file);
}

static void do_version(struct string_list *args)
{
	long int version;

	if (args->nr != 1)
		exit(1);

	version = strtol(args->items[0].string, NULL, 10);
	if (version < 100)
		version *= 100;

	if (!version || version < 400 || version > CMD_VERSION)
		exit(128);

	can_send_null_buffer = (version >= 1700);

	write_or_die(1, STRINGIFY(HELPER_HASH) "\n",
	             sizeof(STRINGIFY(HELPER_HASH)));
}

static void string_list_as_oid_array(struct string_list *list,
				     struct oid_array *array)
{
	struct string_list_item *item;
	for_each_string_list_item(item, list) {
		struct object_id oid;
		if (!get_oid_hex(item->string, &oid))
			oid_array_append(array, &oid);
	}
}

static void do_known(struct hg_connection *conn, struct string_list *args)
{
	struct strbuf result = STRBUF_INIT;
	struct oid_array nodes = OID_ARRAY_INIT;
	string_list_as_oid_array(args, &nodes);
	hg_known(conn, &result, &nodes);
	send_buffer(&result);
	oid_array_clear(&nodes);
	strbuf_release(&result);
}

static void do_listkeys(struct hg_connection *conn, struct string_list *args)
{
	struct strbuf result = STRBUF_INIT;
	if (args->nr != 1)
		exit(1);

	hg_listkeys(conn, &result, args->items[0].string);
	send_buffer(&result);
	strbuf_release(&result);
}

static void arg_as_oid_array(char *nodes, struct oid_array *array)
{
	struct string_list list = STRING_LIST_INIT_NODUP;
	string_list_split_in_place(&list, nodes, ',', -1);
	string_list_as_oid_array(&list, array);
	string_list_clear(&list, 0);
}

static void do_getbundle(struct hg_connection *conn, struct string_list *args)
{
	struct oid_array heads = OID_ARRAY_INIT;
	struct oid_array common = OID_ARRAY_INIT;
	const char *bundle2caps = NULL;

	if (args->nr > 3)
		exit(1);

	if (args->nr > 0)
		arg_as_oid_array(args->items[0].string, &heads);
	if (args->nr > 1)
		arg_as_oid_array(args->items[1].string, &common);
	if (args->nr > 2)
		bundle2caps = args->items[2].string;

	hg_getbundle(conn, stdout, &heads, &common, bundle2caps);

	oid_array_clear(&common);
	oid_array_clear(&heads);
}

static void do_unbundle(struct hg_connection *conn, struct string_list *args)
{
	struct strbuf result = STRBUF_INIT;
	struct oid_array heads = OID_ARRAY_INIT;
	if (args->nr < 1)
		exit(1);
	if (args->nr != 1 || strcmp(args->items[0].string, "force"))
		string_list_as_oid_array(args, &heads);
	hg_unbundle(conn, &result, stdin, &heads);
	send_buffer(&result);
	oid_array_clear(&heads);
	strbuf_release(&result);
}

static void do_pushkey(struct hg_connection *conn, struct string_list *args)
{
	struct strbuf result = STRBUF_INIT;

	if (args->nr != 4)
		exit(1);

	hg_pushkey(conn, &result, args->items[0].string, args->items[1].string,
		   args->items[2].string, args->items[3].string);
	send_buffer(&result);
	strbuf_release(&result);
}

static void do_capable(struct hg_connection *conn, struct string_list *args)
{
	struct strbuf result = STRBUF_INIT;
	const char *result_str;

	if (args->nr != 1)
		exit(1);

	result_str = hg_get_capability(conn, args->items[0].string);
	if (result_str) {
		strbuf_addstr(&result, result_str);
		send_buffer(&result);
	} else {
		send_buffer(NULL);
	}
	strbuf_release(&result);
}

static void do_state(struct hg_connection *conn, struct string_list *args)
{
	struct strbuf branchmap = STRBUF_INIT;
	struct strbuf heads = STRBUF_INIT;
	struct strbuf bookmarks = STRBUF_INIT;

	if (args->nr != 0)
		exit(1);

	hg_get_repo_state(conn, &branchmap, &heads, &bookmarks);
	send_buffer(&branchmap);
	send_buffer(&heads);
	send_buffer(&bookmarks);
	strbuf_release(&branchmap);
	strbuf_release(&heads);
	strbuf_release(&bookmarks);
}

static void do_lookup(struct hg_connection *conn, struct string_list *args)
{
	struct strbuf result = STRBUF_INIT;
	if (args->nr != 1)
		exit(1);

	hg_lookup(conn, &result, args->items[0].string);
	send_buffer(&result);
	strbuf_release(&result);
}

static void do_clonebundles(struct hg_connection *conn, struct string_list *args)
{
	struct strbuf result = STRBUF_INIT;
	if (args->nr != 0)
		exit(1);

	hg_clonebundles(conn, &result);
	send_buffer(&result);
	strbuf_release(&result);
}

static void connected_loop(struct hg_connection *conn)
{
	struct strbuf buf = STRBUF_INIT;

	while (strbuf_getline(&buf, stdin) != EOF) {
		struct string_list args = STRING_LIST_INIT_NODUP;
		const char *command;
		split_command(buf.buf, &command, &args);

		if (!*command) {
			string_list_clear(&args, 0);
			break;
		}
		if (!strcmp("known", command))
			do_known(conn, &args);
		else if (!strcmp("listkeys", command))
			do_listkeys(conn, &args);
		else if (!strcmp("getbundle", command))
			do_getbundle(conn, &args);
		else if (!strcmp("unbundle", command))
			do_unbundle(conn, &args);
		else if (!strcmp("pushkey", command))
			do_pushkey(conn, &args);
		else if (!strcmp("capable", command))
			do_capable(conn, &args);
		else if (!strcmp("state", command))
			do_state(conn, &args);
		else if (!strcmp("lookup", command))
			do_lookup(conn, &args);
		else if (!strcmp("clonebundles", command))
			do_clonebundles(conn, &args);
		else
			die("Unknown command: \"%s\"", command);

		string_list_clear(&args, 0);
	}

	strbuf_release(&buf);
}

static void do_connect(struct string_list *args)
{
	const char *url;
	struct hg_connection *conn;

	if (args->nr != 1)
		return;

	url = args->items[0].string;

	conn = hg_connect(url, 0);

	// hg_connect either dies in case of connection failure,
	// or returns NULL, in which case it has sent out a stream
	// to stdout.
	if (conn) {
		write_or_die(1, "ok\n", 3);
		connected_loop(conn);

		hg_finish_connect(conn);
	}
}

static int add_each_head(const struct object_id *oid, void *data)
{
	struct strbuf *buf = (struct strbuf *)data;

	strbuf_addstr(buf, oid_to_hex(oid));
	strbuf_addch(buf, '\n');
	return 0;
}

static void do_heads(struct string_list *args)
{
        struct oid_array *heads = NULL;
        struct strbuf heads_buf = STRBUF_INIT;

        if (args->nr != 1)
                die("heads needs 1 argument");

        if (!strcmp(args->items[0].string, "manifests")) {
                heads = &manifest_heads;
        } else
                die("Unknown kind: %s", args->items[0].string);

	ensure_heads(heads);
	oid_array_for_each_unique(heads, add_each_head, &heads_buf);
	send_buffer(&heads_buf);
	strbuf_release(&heads_buf);
}

static void do_reset_heads(struct string_list *args)
{
        struct oid_array *heads = NULL;

        if (args->nr != 1)
                die("reset-heads needs 1 argument");

        if (!strcmp(args->items[0].string, "manifests")) {
                heads = &manifest_heads;
        } else
                die("Unknown kind: %s", args->items[0].string);

	ensure_heads(heads);
	oid_array_clear(heads);
	// We don't want subsequent ensure_heads to refill the array,
	// so mark it as sorted, which means it's initialized.
	heads->sorted = 1;
}

struct track_upgrade {
	struct oidset set;
	struct progress *progress;
};

static void upgrade_files(const struct manifest_tree *tree,
                          struct track_upgrade *track)
{
	struct manifest_tree_state state;
	struct manifest_entry entry;

	state.tree_hg = lookup_tree(tree->hg);
	if (!state.tree_hg)
		goto corrupted;

	if (state.tree_hg->object.flags & SEEN)
		goto cleanup;

	if (manifest_tree_state_init(tree, &state, NULL))
		goto corrupted;

	while (manifest_tree_entry(&state, &entry)) {
		struct object_id oid;
		if (S_ISDIR(entry.mode)) {
			struct manifest_tree subtree;
			hashcpy(subtree.git, entry.other_sha1);
			hashcpy(subtree.hg, entry.sha1);
			upgrade_files(&subtree, track);
			continue;
		}

		hashcpy(oid.hash, entry.sha1);
		if (oidset_insert(&track->set, &oid))
			continue;

		const unsigned char *note = get_note(&hg2git, entry.sha1);
		if (!note && !is_empty_hg_file(entry.sha1))
			goto corrupted;
		if (note && hashcmp(note, entry.other_sha1)) {
			struct hg_file file;
			struct strbuf buf = STRBUF_INIT;
			unsigned long len;
			enum object_type t;
			char *content;
			content = read_sha1_file_extended(note, &t, &len, 0);
			strbuf_attach(&buf, content, len, len);
			hg_file_init(&file);
			hg_file_from_memory(&file, entry.sha1, &buf);
			remove_note(&hg2git, entry.sha1);
			hg_file_store(&file, NULL);
			hg_file_release(&file);
			note = get_note(&hg2git, entry.sha1);
			if (hashcmp(note, entry.other_sha1))
				goto corrupted;
		}
		display_progress(track->progress, track->set.map.size);
	}

	free_tree_buffer(state.tree_git);
cleanup:
	state.tree_hg->object.flags |= SEEN;
	free_tree_buffer(state.tree_hg);
	return;
corrupted:
	die("Corrupted metadata");

}

static int revs_add_each_head(const struct object_id *oid, void *data)
{
	struct rev_info *revs = (struct rev_info *)data;

	add_pending_sha1(revs, oid_to_hex(oid), oid->hash, 0);

	return 0;
}

static void do_upgrade(struct string_list *args)
{
	struct rev_info revs;
	struct commit *commit;

        if (args->nr != 0)
                die("upgrade takes no arguments");

	ensure_notes(&hg2git);

	init_revisions(&revs, NULL);

	ensure_heads(&manifest_heads);
	oid_array_for_each_unique(&manifest_heads, revs_add_each_head, &revs);

	if (prepare_revision_walk(&revs))
		die("revision walk setup failed");

	struct track_upgrade track = { OIDSET_INIT, NULL };
	track.progress = start_progress("Upgrading files metadata", 0);
	while ((commit = get_revision(&revs)) != NULL) {
		if (parse_tree(commit->tree))
			die("Corrupt mercurial metadata");
		struct manifest_tree manifest_tree;
		if (get_manifest_tree(commit->tree, &manifest_tree, NULL))
			die("Corrupt mercurial metadata");
		upgrade_files(&manifest_tree, &track);
		free_tree_buffer(commit->tree);
	}
        stop_progress(&track.progress);

	oidset_clear(&track.set);
	write_or_die(1, "ok\n", 3);
	rev_info_release(&revs);
}

// 12th bit is only used by builtin/blame.c, so it should be safe to use.
#define FSCK_SEEN (1 << 12)

static void do_seen(struct string_list *args)
{
	struct object_id oid;
	int seen = 0;

	if (args->nr != 2)
		die("seen takes two argument");

	if (get_oid_hex(args->items[1].string, &oid))
		die("Invalid sha1");

	if (!strcmp(args->items[0].string, "hg2git"))
		seen = oidset_insert(&hg2git_seen, &oid);
	else if (!strcmp(args->items[0].string, "git2hg")) {
		struct commit *c = lookup_commit(oid.hash);
		if (!c)
			die("Unknown commit");
		seen = c->object.flags & FSCK_SEEN;
		c->object.flags |= FSCK_SEEN;
	}

	if (seen)
		write_or_die(1, "yes\n", 4);
	else
		write_or_die(1, "no\n", 3);
}

struct dangling_data {
	struct notes_tree *notes;
	struct strbuf *buf;
	int exclude_blobs;
};

static int dangling_note(const unsigned char *object_sha1,
                         const unsigned char *note_sha1, char *note_path,
                         void *cb_data)
{
	struct dangling_data *data = (struct dangling_data *)cb_data;
	struct object_id oid;
	int is_dangling = 0;

	hashcpy(oid.hash, object_sha1);
	if (data->notes == &hg2git) {
		if (!data->exclude_blobs ||
		    (sha1_object_info(note_sha1, NULL) != OBJ_BLOB))
			is_dangling = !oidset_contains(&hg2git_seen, &oid);
	} else if (data->notes == &git2hg) {
		struct commit *c = lookup_commit(oid.hash);
		is_dangling = !c || !(c->object.flags & FSCK_SEEN);
	}

	if (is_dangling) {
		strbuf_add(data->buf, oid_to_hex(&oid), 40);
		strbuf_addch(data->buf, '\n');
	}

	return 0;
}

static void do_dangling(struct string_list *args)
{
	struct strbuf buf = STRBUF_INIT;
	struct dangling_data data = { NULL, &buf, 0 };

        if (args->nr != 1)
                die("dangling takes one argument");

	if (!strcmp(args->items[0].string, "hg2git-no-blobs")) {
		data.notes = &hg2git;
		data.exclude_blobs = 1;
	} else if (!strcmp(args->items[0].string, "hg2git")) {
		data.notes = &hg2git;
	} else if (!strcmp(args->items[0].string, "git2hg")) {
		data.notes = &git2hg;
	} else {
		die("Unknown argument");
	}

	ensure_notes(data.notes);
	for_each_note(data.notes, 0, dangling_note, &data);

	send_buffer(&buf);
	strbuf_release(&buf);
}

static void init_config()
{
	struct strbuf conf = STRBUF_INIT;
	if (!config("check", &conf)) {
		struct strbuf **check = strbuf_split(&conf, ',');
		struct strbuf **c;
		for (c = check; *c; c++)
			if (!strcmp((*c)->buf, "true") ||
			    !strcmp((*c)->buf, "all") ||
			    !strcmp((*c)->buf, "helper"))
				cinnabar_check |= CHECK_HELPER;
		strbuf_list_free(check);
	}
	strbuf_release(&conf);
}

static void init_flags()
{
	struct commit *c;
	const char *body;
	struct strbuf **flags, **f;

	c = lookup_commit_reference_by_name(METADATA_REF);
	if (!c)
		return;
	body = strstr(get_commit_buffer(c, NULL), "\n\n") + 2;
	flags = strbuf_split_str(body, ' ', -1);
	for (f = flags; *f; f++) {
		strbuf_trim(*f);
		if (!strcmp("files-meta", (*f)->buf))
			metadata_flags |= FILES_META;
	}
	strbuf_list_free(flags);
}

int cmd_main(int argc, const char *argv[])
{
	int initialized = 0;
	struct strbuf buf = STRBUF_INIT;

	git_extract_argv0_path(argv[0]);
	git_config(git_default_config, NULL);
	ignore_case = 0;
	save_commit_buffer = 0;
	warn_on_object_refname_ambiguity = 0;

	while (strbuf_getline(&buf, stdin) != EOF) {
		struct string_list args = STRING_LIST_INIT_NODUP;
		const char *command;
		split_command(buf.buf, &command, &args);
		if (!strcmp("version", command)) {
			do_version(&args);
			string_list_clear(&args, 0);
			continue;
		} else if (!strcmp("connect", command)) {
			do_connect(&args);
			string_list_clear(&args, 0);
			break;
		}
		if (!initialized) {
			setup_git_directory();
			git_config(git_diff_basic_config, NULL);
			ignore_case = 0;
			init_config();
			init_flags();
			initialized = 1;
		}
		if (!strcmp("git2hg", command))
			do_get_note(&git2hg, &args);
		else if (!strcmp("file-meta", command))
			do_get_note(&files_meta, &args);
		else if (!strcmp("hg2git", command))
			do_hg2git(&args);
		else if (!strcmp("manifest", command))
			do_manifest(&args);
		else if (!strcmp("check-manifest", command))
			do_check_manifest(&args);
		else if (!strcmp("check-file", command))
			do_check_file(&args);
		else if (!strcmp("cat-file", command))
			do_cat_file(&args);
		else if (!strcmp("ls-tree", command))
			do_ls_tree(&args);
		else if (!strcmp("rev-list", command))
			do_rev_list(&args);
		else if (!strcmp("diff-tree", command))
			do_diff_tree(&args);
		else if (!strcmp("heads", command))
			do_heads(&args);
		else if (!strcmp("reset-heads", command))
			do_reset_heads(&args);
		else if (!strcmp("upgrade", command))
			do_upgrade(&args);
		else if (!strcmp("seen", command))
			do_seen(&args);
		else if (!strcmp("dangling", command))
			do_dangling(&args);
		else if (!maybe_handle_command(command, &args))
			die("Unknown command: \"%s\"", command);

		string_list_clear(&args, 0);
	}

	strbuf_release(&buf);

	if (git2hg.initialized)
		free_notes(&git2hg);

	if (hg2git.initialized)
		free_notes(&hg2git);

	if (files_meta.initialized)
		free_notes(&files_meta);

	oidset_clear(&hg2git_seen);

	return 0;
}
