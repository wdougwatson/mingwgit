/*
 * Builtin "git commit"
 *
 * Copyright (c) 2007 Kristian Høgsberg <krh@redhat.com>
 * Based on git-commit.sh by Junio C Hamano and Linus Torvalds
 */

#include "cache.h"
#include "cache-tree.h"
#include "color.h"
#include "dir.h"
#include "builtin.h"
#include "diff.h"
#include "diffcore.h"
#include "commit.h"
#include "revision.h"
#include "wt-status.h"
#include "run-command.h"
#include "refs.h"
#include "log-tree.h"
#include "strbuf.h"
#include "utf8.h"
#include "parse-options.h"
#include "path-list.h"
#include "unpack-trees.h"

static const char * const builtin_commit_usage[] = {
	"git-commit [options] [--] <filepattern>...",
	NULL
};

static const char * const builtin_status_usage[] = {
	"git-status [options] [--] <filepattern>...",
	NULL
};

static unsigned char head_sha1[20], merge_head_sha1[20];
static char *use_message_buffer;
static const char commit_editmsg[] = "COMMIT_EDITMSG";
static struct lock_file index_lock; /* real index */
static struct lock_file false_lock; /* used only for partial commits */
static enum {
	COMMIT_AS_IS = 1,
	COMMIT_NORMAL,
	COMMIT_PARTIAL,
} commit_style;

static char *logfile, *force_author, *template_file;
static char *edit_message, *use_message;
static int all, edit_flag, also, interactive, only, amend, signoff;
static int quiet, verbose, untracked_files, no_verify, allow_empty;
/*
 * The default commit message cleanup mode will remove the lines
 * beginning with # (shell comments) and leading and trailing
 * whitespaces (empty lines or containing only whitespaces)
 * if editor is used, and only the whitespaces if the message
 * is specified explicitly.
 */
static enum {
	CLEANUP_SPACE,
	CLEANUP_NONE,
	CLEANUP_ALL,
} cleanup_mode;
static char *cleanup_arg;

static int use_editor = 1, initial_commit, in_merge;
const char *only_include_assumed;
struct strbuf message;

static int opt_parse_m(const struct option *opt, const char *arg, int unset)
{
	struct strbuf *buf = opt->value;
	if (unset)
		strbuf_setlen(buf, 0);
	else {
		strbuf_addstr(buf, arg);
		strbuf_addch(buf, '\n');
		strbuf_addch(buf, '\n');
	}
	return 0;
}

static struct option builtin_commit_options[] = {
	OPT__QUIET(&quiet),
	OPT__VERBOSE(&verbose),
	OPT_GROUP("Commit message options"),

	OPT_STRING('F', "file", &logfile, "FILE", "read log from file"),
	OPT_STRING(0, "author", &force_author, "AUTHOR", "override author for commit"),
	OPT_CALLBACK('m', "message", &message, "MESSAGE", "specify commit message", opt_parse_m),
	OPT_STRING('c', "reedit-message", &edit_message, "COMMIT", "reuse and edit message from specified commit "),
	OPT_STRING('C', "reuse-message", &use_message, "COMMIT", "reuse message from specified commit"),
	OPT_BOOLEAN('s', "signoff", &signoff, "add Signed-off-by: header"),
	OPT_STRING('t', "template", &template_file, "FILE", "use specified template file"),
	OPT_BOOLEAN('e', "edit", &edit_flag, "force edit of commit"),

	OPT_GROUP("Commit contents options"),
	OPT_BOOLEAN('a', "all", &all, "commit all changed files"),
	OPT_BOOLEAN('i', "include", &also, "add specified files to index for commit"),
	OPT_BOOLEAN(0, "interactive", &interactive, "interactively add files"),
	OPT_BOOLEAN('o', "only", &only, "commit only specified files"),
	OPT_BOOLEAN('n', "no-verify", &no_verify, "bypass pre-commit hook"),
	OPT_BOOLEAN(0, "amend", &amend, "amend previous commit"),
	OPT_BOOLEAN(0, "untracked-files", &untracked_files, "show all untracked files"),
	OPT_BOOLEAN(0, "allow-empty", &allow_empty, "ok to record an empty change"),
	OPT_STRING(0, "cleanup", &cleanup_arg, "default", "how to strip spaces and #comments from message"),

	OPT_END()
};

static void rollback_index_files(void)
{
	switch (commit_style) {
	case COMMIT_AS_IS:
		break; /* nothing to do */
	case COMMIT_NORMAL:
		rollback_lock_file(&index_lock);
		break;
	case COMMIT_PARTIAL:
		rollback_lock_file(&index_lock);
		rollback_lock_file(&false_lock);
		break;
	}
}

static int commit_index_files(void)
{
	int err = 0;

	switch (commit_style) {
	case COMMIT_AS_IS:
		break; /* nothing to do */
	case COMMIT_NORMAL:
		err = commit_lock_file(&index_lock);
		break;
	case COMMIT_PARTIAL:
		err = commit_lock_file(&index_lock);
		rollback_lock_file(&false_lock);
		break;
	}

	return err;
}

/*
 * Take a union of paths in the index and the named tree (typically, "HEAD"),
 * and return the paths that match the given pattern in list.
 */
static int list_paths(struct path_list *list, const char *with_tree,
		      const char *prefix, const char **pattern)
{
	int i;
	char *m;

	for (i = 0; pattern[i]; i++)
		;
	m = xcalloc(1, i);

	if (with_tree)
		overlay_tree_on_cache(with_tree, prefix);

	for (i = 0; i < active_nr; i++) {
		struct cache_entry *ce = active_cache[i];
		if (ce->ce_flags & CE_UPDATE)
			continue;
		if (!pathspec_match(pattern, m, ce->name, 0))
			continue;
		path_list_insert(ce->name, list);
	}

	return report_path_error(m, pattern, prefix ? strlen(prefix) : 0);
}

static void add_remove_files(struct path_list *list)
{
	int i;
	for (i = 0; i < list->nr; i++) {
		struct path_list_item *p = &(list->items[i]);
		if (file_exists(p->path))
			add_file_to_cache(p->path, 0);
		else
			remove_file_from_cache(p->path);
	}
}

static void create_base_index(void)
{
	struct tree *tree;
	struct unpack_trees_options opts;
	struct tree_desc t;

	if (initial_commit) {
		discard_cache();
		return;
	}

	memset(&opts, 0, sizeof(opts));
	opts.head_idx = 1;
	opts.index_only = 1;
	opts.merge = 1;
	opts.src_index = &the_index;
	opts.dst_index = &the_index;

	opts.fn = oneway_merge;
	tree = parse_tree_indirect(head_sha1);
	if (!tree)
		die("failed to unpack HEAD tree object");
	parse_tree(tree);
	init_tree_desc(&t, tree->buffer, tree->size);
	if (unpack_trees(1, &t, &opts))
		exit(128); /* We've already reported the error, finish dying */
}

static char *prepare_index(int argc, const char **argv, const char *prefix)
{
	int fd;
	struct path_list partial;
	const char **pathspec = NULL;

	if (interactive) {
		interactive_add(argc, argv, prefix);
		commit_style = COMMIT_AS_IS;
		return get_index_file();
	}

	if (read_cache() < 0)
		die("index file corrupt");

	if (*argv)
		pathspec = get_pathspec(prefix, argv);

	/*
	 * Non partial, non as-is commit.
	 *
	 * (1) get the real index;
	 * (2) update the_index as necessary;
	 * (3) write the_index out to the real index (still locked);
	 * (4) return the name of the locked index file.
	 *
	 * The caller should run hooks on the locked real index, and
	 * (A) if all goes well, commit the real index;
	 * (B) on failure, rollback the real index.
	 */
	if (all || (also && pathspec && *pathspec)) {
		int fd = hold_locked_index(&index_lock, 1);
		add_files_to_cache(0, also ? prefix : NULL, pathspec);
		refresh_cache(REFRESH_QUIET);
		if (write_cache(fd, active_cache, active_nr) ||
		    close_lock_file(&index_lock))
			die("unable to write new_index file");
		commit_style = COMMIT_NORMAL;
		return index_lock.filename;
	}

	/*
	 * As-is commit.
	 *
	 * (1) return the name of the real index file.
	 *
	 * The caller should run hooks on the real index, and run
	 * hooks on the real index, and create commit from the_index.
	 * We still need to refresh the index here.
	 */
	if (!pathspec || !*pathspec) {
		fd = hold_locked_index(&index_lock, 1);
		refresh_cache(REFRESH_QUIET);
		if (write_cache(fd, active_cache, active_nr) ||
		    commit_locked_index(&index_lock))
			die("unable to write new_index file");
		commit_style = COMMIT_AS_IS;
		return get_index_file();
	}

	/*
	 * A partial commit.
	 *
	 * (0) find the set of affected paths;
	 * (1) get lock on the real index file;
	 * (2) update the_index with the given paths;
	 * (3) write the_index out to the real index (still locked);
	 * (4) get lock on the false index file;
	 * (5) reset the_index from HEAD;
	 * (6) update the_index the same way as (2);
	 * (7) write the_index out to the false index file;
	 * (8) return the name of the false index file (still locked);
	 *
	 * The caller should run hooks on the locked false index, and
	 * create commit from it.  Then
	 * (A) if all goes well, commit the real index;
	 * (B) on failure, rollback the real index;
	 * In either case, rollback the false index.
	 */
	commit_style = COMMIT_PARTIAL;

	if (file_exists(git_path("MERGE_HEAD")))
		die("cannot do a partial commit during a merge.");

	memset(&partial, 0, sizeof(partial));
	partial.strdup_paths = 1;
	if (list_paths(&partial, initial_commit ? NULL : "HEAD", prefix, pathspec))
		exit(1);

	discard_cache();
	if (read_cache() < 0)
		die("cannot read the index");

	fd = hold_locked_index(&index_lock, 1);
	add_remove_files(&partial);
	refresh_cache(REFRESH_QUIET);
	if (write_cache(fd, active_cache, active_nr) ||
	    close_lock_file(&index_lock))
		die("unable to write new_index file");

	fd = hold_lock_file_for_update(&false_lock,
				       git_path("next-index-%d", getpid()), 1);

	create_base_index();
	add_remove_files(&partial);
	refresh_cache(REFRESH_QUIET);

	if (write_cache(fd, active_cache, active_nr) ||
	    close_lock_file(&false_lock))
		die("unable to write temporary index file");

	discard_cache();
	read_cache_from(false_lock.filename);

	return false_lock.filename;
}

static int run_status(FILE *fp, const char *index_file, const char *prefix, int nowarn)
{
	struct wt_status s;

	wt_status_prepare(&s);
	if (wt_status_relative_paths)
		s.prefix = prefix;

	if (amend) {
		s.amend = 1;
		s.reference = "HEAD^1";
	}
	s.verbose = verbose;
	s.untracked = untracked_files;
	s.index_file = index_file;
	s.fp = fp;
	s.nowarn = nowarn;

	wt_status_print(&s);

	return s.commitable;
}

static int run_hook(const char *index_file, const char *name, ...)
{
	struct child_process hook;
	const char *argv[10], *env[2];
	char index[PATH_MAX];
	va_list args;
	int i;

	va_start(args, name);
	argv[0] = git_path("hooks/%s", name);
	i = 0;
	do {
		if (++i >= ARRAY_SIZE(argv))
			die ("run_hook(): too many arguments");
		argv[i] = va_arg(args, const char *);
	} while (argv[i]);
	va_end(args);

	snprintf(index, sizeof(index), "GIT_INDEX_FILE=%s", index_file);
	env[0] = index;
	env[1] = NULL;

	if (access(argv[0], X_OK) < 0)
		return 0;

	memset(&hook, 0, sizeof(hook));
	hook.argv = argv;
	hook.no_stdin = 1;
	hook.stdout_to_stderr = 1;
	hook.env = env;

	return run_command(&hook);
}

static int is_a_merge(const unsigned char *sha1)
{
	struct commit *commit = lookup_commit(sha1);
	if (!commit || parse_commit(commit))
		die("could not parse HEAD commit");
	return !!(commit->parents && commit->parents->next);
}

static const char sign_off_header[] = "Signed-off-by: ";

static int prepare_to_commit(const char *index_file, const char *prefix)
{
	struct stat statbuf;
	int commitable, saved_color_setting;
	struct strbuf sb;
	char *buffer;
	FILE *fp;
	const char *hook_arg1 = NULL;
	const char *hook_arg2 = NULL;

	if (!no_verify && run_hook(index_file, "pre-commit", NULL))
		return 0;

	strbuf_init(&sb, 0);
	if (message.len) {
		strbuf_addbuf(&sb, &message);
		hook_arg1 = "message";
	} else if (logfile && !strcmp(logfile, "-")) {
		if (isatty(0))
			fprintf(stderr, "(reading log message from standard input)\n");
		if (strbuf_read(&sb, 0, 0) < 0)
			die("could not read log from standard input");
		hook_arg1 = "message";
	} else if (logfile) {
		if (strbuf_read_file(&sb, logfile, 0) < 0)
			die("could not read log file '%s': %s",
			    logfile, strerror(errno));
		hook_arg1 = "message";
	} else if (use_message) {
		buffer = strstr(use_message_buffer, "\n\n");
		if (!buffer || buffer[2] == '\0')
			die("commit has empty message");
		strbuf_add(&sb, buffer + 2, strlen(buffer + 2));
		hook_arg1 = "commit";
		hook_arg2 = use_message;
	} else if (!stat(git_path("MERGE_MSG"), &statbuf)) {
		if (strbuf_read_file(&sb, git_path("MERGE_MSG"), 0) < 0)
			die("could not read MERGE_MSG: %s", strerror(errno));
		hook_arg1 = "merge";
	} else if (!stat(git_path("SQUASH_MSG"), &statbuf)) {
		if (strbuf_read_file(&sb, git_path("SQUASH_MSG"), 0) < 0)
			die("could not read SQUASH_MSG: %s", strerror(errno));
		hook_arg1 = "squash";
	} else if (template_file && !stat(template_file, &statbuf)) {
		if (strbuf_read_file(&sb, template_file, 0) < 0)
			die("could not read %s: %s",
			    template_file, strerror(errno));
		hook_arg1 = "template";
	}

	/*
	 * This final case does not modify the template message,
	 * it just sets the argument to the prepare-commit-msg hook.
	 */
	else if (in_merge)
		hook_arg1 = "merge";

	fp = fopen(git_path(commit_editmsg), "w");
	if (fp == NULL)
		die("could not open %s", git_path(commit_editmsg));

	if (cleanup_mode != CLEANUP_NONE)
		stripspace(&sb, 0);

	if (signoff) {
		struct strbuf sob;
		int i;

		strbuf_init(&sob, 0);
		strbuf_addstr(&sob, sign_off_header);
		strbuf_addstr(&sob, fmt_name(getenv("GIT_COMMITTER_NAME"),
					     getenv("GIT_COMMITTER_EMAIL")));
		strbuf_addch(&sob, '\n');
		for (i = sb.len - 1; i > 0 && sb.buf[i - 1] != '\n'; i--)
			; /* do nothing */
		if (prefixcmp(sb.buf + i, sob.buf)) {
			if (prefixcmp(sb.buf + i, sign_off_header))
				strbuf_addch(&sb, '\n');
			strbuf_addbuf(&sb, &sob);
		}
		strbuf_release(&sob);
	}

	if (fwrite(sb.buf, 1, sb.len, fp) < sb.len)
		die("could not write commit template: %s", strerror(errno));

	strbuf_release(&sb);

	if (use_editor) {
		if (in_merge)
			fprintf(fp,
				"#\n"
				"# It looks like you may be committing a MERGE.\n"
				"# If this is not correct, please remove the file\n"
				"#	%s\n"
				"# and try again.\n"
				"#\n",
				git_path("MERGE_HEAD"));

		fprintf(fp,
			"\n"
			"# Please enter the commit message for your changes.\n"
			"# (Comment lines starting with '#' will ");
		if (cleanup_mode == CLEANUP_ALL)
			fprintf(fp, "not be included)\n");
		else /* CLEANUP_SPACE, that is. */
			fprintf(fp, "be kept.\n"
				"# You can remove them yourself if you want to)\n");
		if (only_include_assumed)
			fprintf(fp, "# %s\n", only_include_assumed);

		saved_color_setting = wt_status_use_color;
		wt_status_use_color = 0;
		commitable = run_status(fp, index_file, prefix, 1);
		wt_status_use_color = saved_color_setting;
	} else {
		struct rev_info rev;
		unsigned char sha1[20];
		const char *parent = "HEAD";

		if (!active_nr && read_cache() < 0)
			die("Cannot read index");

		if (amend)
			parent = "HEAD^1";

		if (get_sha1(parent, sha1))
			commitable = !!active_nr;
		else {
			init_revisions(&rev, "");
			rev.abbrev = 0;
			setup_revisions(0, NULL, &rev, parent);
			DIFF_OPT_SET(&rev.diffopt, QUIET);
			DIFF_OPT_SET(&rev.diffopt, EXIT_WITH_STATUS);
			run_diff_index(&rev, 1 /* cached */);

			commitable = !!DIFF_OPT_TST(&rev.diffopt, HAS_CHANGES);
		}
	}

	fclose(fp);

	if (!commitable && !in_merge && !allow_empty &&
	    !(amend && is_a_merge(head_sha1))) {
		run_status(stdout, index_file, prefix, 0);
		unlink(commit_editmsg);
		return 0;
	}

	/*
	 * Re-read the index as pre-commit hook could have updated it,
	 * and write it out as a tree.  We must do this before we invoke
	 * the editor and after we invoke run_status above.
	 */
	discard_cache();
	read_cache_from(index_file);
	if (!active_cache_tree)
		active_cache_tree = cache_tree();
	if (cache_tree_update(active_cache_tree,
			      active_cache, active_nr, 0, 0) < 0) {
		error("Error building trees");
		return 0;
	}

	if (run_hook(index_file, "prepare-commit-msg",
		     git_path(commit_editmsg), hook_arg1, hook_arg2, NULL))
		return 0;

	if (use_editor) {
		char index[PATH_MAX];
		const char *env[2] = { index, NULL };
		snprintf(index, sizeof(index), "GIT_INDEX_FILE=%s", index_file);
		launch_editor(git_path(commit_editmsg), NULL, env);
	}

	if (!no_verify &&
	    run_hook(index_file, "commit-msg", git_path(commit_editmsg), NULL)) {
		return 0;
	}

	return 1;
}

/*
 * Find out if the message starting at position 'start' in the strbuf
 * contains only whitespace and Signed-off-by lines.
 */
static int message_is_empty(struct strbuf *sb, int start)
{
	struct strbuf tmpl;
	const char *nl;
	int eol, i;

	if (cleanup_mode == CLEANUP_NONE && sb->len)
		return 0;

	/* See if the template is just a prefix of the message. */
	strbuf_init(&tmpl, 0);
	if (template_file && strbuf_read_file(&tmpl, template_file, 0) > 0) {
		stripspace(&tmpl, cleanup_mode == CLEANUP_ALL);
		if (start + tmpl.len <= sb->len &&
		    memcmp(tmpl.buf, sb->buf + start, tmpl.len) == 0)
			start += tmpl.len;
	}
	strbuf_release(&tmpl);

	/* Check if the rest is just whitespace and Signed-of-by's. */
	for (i = start; i < sb->len; i++) {
		nl = memchr(sb->buf + i, '\n', sb->len - i);
		if (nl)
			eol = nl - sb->buf;
		else
			eol = sb->len;

		if (strlen(sign_off_header) <= eol - i &&
		    !prefixcmp(sb->buf + i, sign_off_header)) {
			i = eol;
			continue;
		}
		while (i < eol)
			if (!isspace(sb->buf[i++]))
				return 0;
	}

	return 1;
}

static void determine_author_info(struct strbuf *sb)
{
	char *name, *email, *date;

	name = getenv("GIT_AUTHOR_NAME");
	email = getenv("GIT_AUTHOR_EMAIL");
	date = getenv("GIT_AUTHOR_DATE");

	if (use_message) {
		const char *a, *lb, *rb, *eol;

		a = strstr(use_message_buffer, "\nauthor ");
		if (!a)
			die("invalid commit: %s", use_message);

		lb = strstr(a + 8, " <");
		rb = strstr(a + 8, "> ");
		eol = strchr(a + 8, '\n');
		if (!lb || !rb || !eol)
			die("invalid commit: %s", use_message);

		name = xstrndup(a + 8, lb - (a + 8));
		email = xstrndup(lb + 2, rb - (lb + 2));
		date = xstrndup(rb + 2, eol - (rb + 2));
	}

	if (force_author) {
		const char *lb = strstr(force_author, " <");
		const char *rb = strchr(force_author, '>');

		if (!lb || !rb)
			die("malformed --author parameter");
		name = xstrndup(force_author, lb - force_author);
		email = xstrndup(lb + 2, rb - (lb + 2));
	}

	strbuf_addf(sb, "author %s\n", fmt_ident(name, email, date, IDENT_ERROR_ON_NO_NAME));
}

static int parse_and_validate_options(int argc, const char *argv[],
				      const char * const usage[])
{
	int f = 0;

	argc = parse_options(argc, argv, builtin_commit_options, usage, 0);

	if (logfile || message.len || use_message)
		use_editor = 0;
	if (edit_flag)
		use_editor = 1;
	if (!use_editor)
		setenv("GIT_EDITOR", ":", 1);

	if (get_sha1("HEAD", head_sha1))
		initial_commit = 1;

	if (!get_sha1("MERGE_HEAD", merge_head_sha1))
		in_merge = 1;

	/* Sanity check options */
	if (amend && initial_commit)
		die("You have nothing to amend.");
	if (amend && in_merge)
		die("You are in the middle of a merge -- cannot amend.");

	if (use_message)
		f++;
	if (edit_message)
		f++;
	if (logfile)
		f++;
	if (f > 1)
		die("Only one of -c/-C/-F can be used.");
	if (message.len && f > 0)
		die("Option -m cannot be combined with -c/-C/-F.");
	if (edit_message)
		use_message = edit_message;
	if (amend && !use_message)
		use_message = "HEAD";
	if (use_message) {
		unsigned char sha1[20];
		static char utf8[] = "UTF-8";
		const char *out_enc;
		char *enc, *end;
		struct commit *commit;

		if (get_sha1(use_message, sha1))
			die("could not lookup commit %s", use_message);
		commit = lookup_commit_reference(sha1);
		if (!commit || parse_commit(commit))
			die("could not parse commit %s", use_message);

		enc = strstr(commit->buffer, "\nencoding");
		if (enc) {
			end = strchr(enc + 10, '\n');
			enc = xstrndup(enc + 10, end - (enc + 10));
		} else {
			enc = utf8;
		}
		out_enc = git_commit_encoding ? git_commit_encoding : utf8;

		if (strcmp(out_enc, enc))
			use_message_buffer =
				reencode_string(commit->buffer, out_enc, enc);

		/*
		 * If we failed to reencode the buffer, just copy it
		 * byte for byte so the user can try to fix it up.
		 * This also handles the case where input and output
		 * encodings are identical.
		 */
		if (use_message_buffer == NULL)
			use_message_buffer = xstrdup(commit->buffer);
		if (enc != utf8)
			free(enc);
	}

	if (!!also + !!only + !!all + !!interactive > 1)
		die("Only one of --include/--only/--all/--interactive can be used.");
	if (argc == 0 && (also || (only && !amend)))
		die("No paths with --include/--only does not make sense.");
	if (argc == 0 && only && amend)
		only_include_assumed = "Clever... amending the last one with dirty index.";
	if (argc > 0 && !also && !only)
		only_include_assumed = "Explicit paths specified without -i nor -o; assuming --only paths...";
	if (!cleanup_arg || !strcmp(cleanup_arg, "default"))
		cleanup_mode = use_editor ? CLEANUP_ALL : CLEANUP_SPACE;
	else if (!strcmp(cleanup_arg, "verbatim"))
		cleanup_mode = CLEANUP_NONE;
	else if (!strcmp(cleanup_arg, "whitespace"))
		cleanup_mode = CLEANUP_SPACE;
	else if (!strcmp(cleanup_arg, "strip"))
		cleanup_mode = CLEANUP_ALL;
	else
		die("Invalid cleanup mode %s", cleanup_arg);

	if (all && argc > 0)
		die("Paths with -a does not make sense.");
	else if (interactive && argc > 0)
		die("Paths with --interactive does not make sense.");

	return argc;
}

int cmd_status(int argc, const char **argv, const char *prefix)
{
	const char *index_file;
	int commitable;

	git_config(git_status_config);

	if (wt_status_use_color == -1)
		wt_status_use_color = git_use_color_default;

	argc = parse_and_validate_options(argc, argv, builtin_status_usage);

	index_file = prepare_index(argc, argv, prefix);

	commitable = run_status(stdout, index_file, prefix, 0);

	rollback_index_files();

	return commitable ? 0 : 1;
}

static void print_summary(const char *prefix, const unsigned char *sha1)
{
	struct rev_info rev;
	struct commit *commit;

	commit = lookup_commit(sha1);
	if (!commit)
		die("couldn't look up newly created commit");
	if (!commit || parse_commit(commit))
		die("could not parse newly created commit");

	init_revisions(&rev, prefix);
	setup_revisions(0, NULL, &rev, NULL);

	rev.abbrev = 0;
	rev.diff = 1;
	rev.diffopt.output_format =
		DIFF_FORMAT_SHORTSTAT | DIFF_FORMAT_SUMMARY;

	rev.verbose_header = 1;
	rev.show_root_diff = 1;
	get_commit_format("format:%h: %s", &rev);
	rev.always_show_header = 0;
	rev.diffopt.detect_rename = 1;
	rev.diffopt.rename_limit = 100;
	rev.diffopt.break_opt = 0;
	diff_setup_done(&rev.diffopt);

	printf("Created %scommit ", initial_commit ? "initial " : "");

	if (!log_tree_commit(&rev, commit)) {
		struct strbuf buf = STRBUF_INIT;
		format_commit_message(commit, "%h: %s", &buf);
		printf("%s\n", buf.buf);
		strbuf_release(&buf);
	}
}

int git_commit_config(const char *k, const char *v)
{
	if (!strcmp(k, "commit.template")) {
		if (!v)
			return config_error_nonbool(v);
		template_file = xstrdup(v);
		return 0;
	}

	return git_status_config(k, v);
}

static const char commit_utf8_warn[] =
"Warning: commit message does not conform to UTF-8.\n"
"You may want to amend it after fixing the message, or set the config\n"
"variable i18n.commitencoding to the encoding your project uses.\n";

static void add_parent(struct strbuf *sb, const unsigned char *sha1)
{
	struct object *obj = parse_object(sha1);
	const char *parent = sha1_to_hex(sha1);
	if (!obj)
		die("Unable to find commit parent %s", parent);
	if (obj->type != OBJ_COMMIT)
		die("Parent %s isn't a proper commit", parent);
	strbuf_addf(sb, "parent %s\n", parent);
}

int cmd_commit(int argc, const char **argv, const char *prefix)
{
	int header_len;
	struct strbuf sb;
	const char *index_file, *reflog_msg;
	char *nl, *p;
	unsigned char commit_sha1[20];
	struct ref_lock *ref_lock;

	git_config(git_commit_config);

	argc = parse_and_validate_options(argc, argv, builtin_commit_usage);

	index_file = prepare_index(argc, argv, prefix);

	/* Set up everything for writing the commit object.  This includes
	   running hooks, writing the trees, and interacting with the user.  */
	if (!prepare_to_commit(index_file, prefix)) {
		rollback_index_files();
		return 1;
	}

	/*
	 * The commit object
	 */
	strbuf_init(&sb, 0);
	strbuf_addf(&sb, "tree %s\n",
		    sha1_to_hex(active_cache_tree->sha1));

	/* Determine parents */
	if (initial_commit) {
		reflog_msg = "commit (initial)";
	} else if (amend) {
		struct commit_list *c;
		struct commit *commit;

		reflog_msg = "commit (amend)";
		commit = lookup_commit(head_sha1);
		if (!commit || parse_commit(commit))
			die("could not parse HEAD commit");

		for (c = commit->parents; c; c = c->next)
			add_parent(&sb, c->item->object.sha1);
	} else if (in_merge) {
		struct strbuf m;
		FILE *fp;

		reflog_msg = "commit (merge)";
		add_parent(&sb, head_sha1);
		strbuf_init(&m, 0);
		fp = fopen(git_path("MERGE_HEAD"), "r");
		if (fp == NULL)
			die("could not open %s for reading: %s",
			    git_path("MERGE_HEAD"), strerror(errno));
		while (strbuf_getline(&m, fp, '\n') != EOF) {
			unsigned char sha1[20];
			if (get_sha1_hex(m.buf, sha1) < 0)
				die("Corrupt MERGE_HEAD file (%s)", m.buf);
			add_parent(&sb, sha1);
		}
		fclose(fp);
		strbuf_release(&m);
	} else {
		reflog_msg = "commit";
		strbuf_addf(&sb, "parent %s\n", sha1_to_hex(head_sha1));
	}

	determine_author_info(&sb);
	strbuf_addf(&sb, "committer %s\n", git_committer_info(IDENT_ERROR_ON_NO_NAME));
	if (!is_encoding_utf8(git_commit_encoding))
		strbuf_addf(&sb, "encoding %s\n", git_commit_encoding);
	strbuf_addch(&sb, '\n');

	/* Finally, get the commit message */
	header_len = sb.len;
	if (strbuf_read_file(&sb, git_path(commit_editmsg), 0) < 0) {
		rollback_index_files();
		die("could not read commit message");
	}

	/* Truncate the message just before the diff, if any. */
	p = strstr(sb.buf, "\ndiff --git a/");
	if (p != NULL)
		strbuf_setlen(&sb, p - sb.buf + 1);

	if (cleanup_mode != CLEANUP_NONE)
		stripspace(&sb, cleanup_mode == CLEANUP_ALL);
	if (sb.len < header_len || message_is_empty(&sb, header_len)) {
		rollback_index_files();
		die("no commit message?  aborting commit.");
	}
	strbuf_addch(&sb, '\0');
	if (is_encoding_utf8(git_commit_encoding) && !is_utf8(sb.buf))
		fprintf(stderr, commit_utf8_warn);

	if (write_sha1_file(sb.buf, sb.len - 1, commit_type, commit_sha1)) {
		rollback_index_files();
		die("failed to write commit object");
	}

	ref_lock = lock_any_ref_for_update("HEAD",
					   initial_commit ? NULL : head_sha1,
					   0);

	nl = strchr(sb.buf + header_len, '\n');
	if (nl)
		strbuf_setlen(&sb, nl + 1 - sb.buf);
	else
		strbuf_addch(&sb, '\n');
	strbuf_remove(&sb, 0, header_len);
	strbuf_insert(&sb, 0, reflog_msg, strlen(reflog_msg));
	strbuf_insert(&sb, strlen(reflog_msg), ": ", 2);

	if (!ref_lock) {
		rollback_index_files();
		die("cannot lock HEAD ref");
	}
	if (write_ref_sha1(ref_lock, commit_sha1, sb.buf) < 0) {
		rollback_index_files();
		die("cannot update HEAD ref");
	}

	unlink(git_path("MERGE_HEAD"));
	unlink(git_path("MERGE_MSG"));
	unlink(git_path("SQUASH_MSG"));

	if (commit_index_files())
		die ("Repository has been updated, but unable to write\n"
		     "new_index file. Check that disk is not full or quota is\n"
		     "not exceeded, and then \"git reset HEAD\" to recover.");

	rerere();
	run_hook(get_index_file(), "post-commit", NULL);
	if (!quiet)
		print_summary(prefix, commit_sha1);

	return 0;
}
