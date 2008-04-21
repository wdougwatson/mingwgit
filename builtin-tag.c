/*
 * Builtin "git tag"
 *
 * Copyright (c) 2007 Kristian Høgsberg <krh@redhat.com>,
 *                    Carlos Rica <jasampler@gmail.com>
 * Based on git-tag.sh and mktag.c by Linus Torvalds.
 */

#include "cache.h"
#include "builtin.h"
#include "refs.h"
#include "tag.h"
#include "run-command.h"
#include "parse-options.h"

static const char * const git_tag_usage[] = {
	"git-tag [-a|-s|-u <key-id>] [-f] [-m <msg>|-F <file>] <tagname> [<head>]",
	"git-tag -d <tagname>...",
	"git-tag -l [-n[<num>]] [<pattern>]",
	"git-tag -v <tagname>...",
	NULL
};

static char signingkey[1000];

void launch_editor(const char *path, struct strbuf *buffer, const char *const *env)
{
	const char *editor, *terminal;

	editor = getenv("GIT_EDITOR");
	if (!editor && editor_program)
		editor = editor_program;
	if (!editor)
		editor = getenv("VISUAL");
	if (!editor)
		editor = getenv("EDITOR");

	terminal = getenv("TERM");
	if (!editor && (!terminal || !strcmp(terminal, "dumb"))) {
		fprintf(stderr,
		"Terminal is dumb but no VISUAL nor EDITOR defined.\n"
		"Please supply the message using either -m or -F option.\n");
		exit(1);
	}

	if (!editor)
		editor = "vi";

	if (strcmp(editor, ":")) {
		size_t len = strlen(editor);
		int i = 0;
		const char *args[6];
		struct strbuf arg0;

		strbuf_init(&arg0, 0);
		if (strcspn(editor, "$ \t'") != len) {
			/* there are specials */
			strbuf_addf(&arg0, "%s \"$@\"", editor);
			args[i++] = "sh";
			args[i++] = "-c";
			args[i++] = arg0.buf;
		}
		args[i++] = editor;
		args[i++] = path;
		args[i] = NULL;

		if (run_command_v_opt_cd_env(args, 0, NULL, env))
			die("There was a problem with the editor %s.", editor);
		strbuf_release(&arg0);
	}

	if (!buffer)
		return;
	if (strbuf_read_file(buffer, path, 0) < 0)
		die("could not read message file '%s': %s",
		    path, strerror(errno));
}

struct tag_filter {
	const char *pattern;
	int lines;
};

#define PGP_SIGNATURE "-----BEGIN PGP SIGNATURE-----"

static int show_reference(const char *refname, const unsigned char *sha1,
			  int flag, void *cb_data)
{
	struct tag_filter *filter = cb_data;

	if (!fnmatch(filter->pattern, refname, 0)) {
		int i;
		unsigned long size;
		enum object_type type;
		char *buf, *sp, *eol;
		size_t len;

		if (!filter->lines) {
			printf("%s\n", refname);
			return 0;
		}
		printf("%-15s ", refname);

		buf = read_sha1_file(sha1, &type, &size);
		if (!buf || !size)
			return 0;

		/* skip header */
		sp = strstr(buf, "\n\n");
		if (!sp) {
			free(buf);
			return 0;
		}
		/* only take up to "lines" lines, and strip the signature */
		for (i = 0, sp += 2;
				i < filter->lines && sp < buf + size &&
				prefixcmp(sp, PGP_SIGNATURE "\n");
				i++) {
			if (i)
				printf("\n    ");
			eol = memchr(sp, '\n', size - (sp - buf));
			len = eol ? eol - sp : size - (sp - buf);
			fwrite(sp, len, 1, stdout);
			if (!eol)
				break;
			sp = eol + 1;
		}
		putchar('\n');
		free(buf);
	}

	return 0;
}

static int list_tags(const char *pattern, int lines)
{
	struct tag_filter filter;

	if (pattern == NULL)
		pattern = "*";

	filter.pattern = pattern;
	filter.lines = lines;

	for_each_tag_ref(show_reference, (void *) &filter);

	return 0;
}

typedef int (*each_tag_name_fn)(const char *name, const char *ref,
				const unsigned char *sha1);

static int for_each_tag_name(const char **argv, each_tag_name_fn fn)
{
	const char **p;
	char ref[PATH_MAX];
	int had_error = 0;
	unsigned char sha1[20];

	for (p = argv; *p; p++) {
		if (snprintf(ref, sizeof(ref), "refs/tags/%s", *p)
					>= sizeof(ref)) {
			error("tag name too long: %.*s...", 50, *p);
			had_error = 1;
			continue;
		}
		if (!resolve_ref(ref, sha1, 1, NULL)) {
			error("tag '%s' not found.", *p);
			had_error = 1;
			continue;
		}
		if (fn(*p, ref, sha1))
			had_error = 1;
	}
	return had_error;
}

static int delete_tag(const char *name, const char *ref,
				const unsigned char *sha1)
{
	if (delete_ref(ref, sha1))
		return 1;
	printf("Deleted tag '%s'\n", name);
	return 0;
}

static int verify_tag(const char *name, const char *ref,
				const unsigned char *sha1)
{
	const char *argv_verify_tag[] = {"git-verify-tag",
					"-v", "SHA1_HEX", NULL};
	argv_verify_tag[2] = sha1_to_hex(sha1);

	if (run_command_v_opt(argv_verify_tag, 0))
		return error("could not verify the tag '%s'", name);
	return 0;
}

static int do_sign(struct strbuf *buffer)
{
	struct child_process gpg;
	const char *args[4];
	char *bracket;
	int len;

	if (!*signingkey) {
		if (strlcpy(signingkey, git_committer_info(IDENT_ERROR_ON_NO_NAME),
				sizeof(signingkey)) > sizeof(signingkey) - 1)
			return error("committer info too long.");
		bracket = strchr(signingkey, '>');
		if (bracket)
			bracket[1] = '\0';
	}

	/* When the username signingkey is bad, program could be terminated
	 * because gpg exits without reading and then write gets SIGPIPE. */
	signal(SIGPIPE, SIG_IGN);

	memset(&gpg, 0, sizeof(gpg));
	gpg.argv = args;
	gpg.in = -1;
	gpg.out = -1;
	args[0] = "gpg";
	args[1] = "-bsau";
	args[2] = signingkey;
	args[3] = NULL;

	if (start_command(&gpg))
		return error("could not run gpg.");

	if (write_in_full(gpg.in, buffer->buf, buffer->len) != buffer->len) {
		close(gpg.in);
		close(gpg.out);
		finish_command(&gpg);
		return error("gpg did not accept the tag data");
	}
	close(gpg.in);
	len = strbuf_read(buffer, gpg.out, 1024);
	close(gpg.out);

	if (finish_command(&gpg) || !len || len < 0)
		return error("gpg failed to sign the tag");

	return 0;
}

static const char tag_template[] =
	"\n"
	"#\n"
	"# Write a tag message\n"
	"#\n";

static void set_signingkey(const char *value)
{
	if (strlcpy(signingkey, value, sizeof(signingkey)) >= sizeof(signingkey))
		die("signing key value too long (%.10s...)", value);
}

static int git_tag_config(const char *var, const char *value)
{
	if (!strcmp(var, "user.signingkey")) {
		if (!value)
			return config_error_nonbool(value);
		set_signingkey(value);
		return 0;
	}

	return git_default_config(var, value);
}

static void write_tag_body(int fd, const unsigned char *sha1)
{
	unsigned long size;
	enum object_type type;
	char *buf, *sp, *eob;
	size_t len;

	buf = read_sha1_file(sha1, &type, &size);
	if (!buf)
		return;
	/* skip header */
	sp = strstr(buf, "\n\n");

	if (!sp || !size || type != OBJ_TAG) {
		free(buf);
		return;
	}
	sp += 2; /* skip the 2 LFs */
	eob = strstr(sp, "\n" PGP_SIGNATURE "\n");
	if (eob)
		len = eob - sp;
	else
		len = buf + size - sp;
	write_or_die(fd, sp, len);

	free(buf);
}

static void create_tag(const unsigned char *object, const char *tag,
		       struct strbuf *buf, int message, int sign,
		       unsigned char *prev, unsigned char *result)
{
	enum object_type type;
	char header_buf[1024];
	int header_len;

	type = sha1_object_info(object, NULL);
	if (type <= OBJ_NONE)
	    die("bad object type.");

	header_len = snprintf(header_buf, sizeof(header_buf),
			  "object %s\n"
			  "type %s\n"
			  "tag %s\n"
			  "tagger %s\n\n",
			  sha1_to_hex(object),
			  typename(type),
			  tag,
			  git_committer_info(IDENT_ERROR_ON_NO_NAME));

	if (header_len > sizeof(header_buf) - 1)
		die("tag header too big.");

	if (!message) {
		char *path;
		int fd;

		/* write the template message before editing: */
		path = xstrdup(git_path("TAG_EDITMSG"));
		fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
		if (fd < 0)
			die("could not create file '%s': %s",
						path, strerror(errno));

		if (!is_null_sha1(prev))
			write_tag_body(fd, prev);
		else
			write_or_die(fd, tag_template, strlen(tag_template));
		close(fd);

		launch_editor(path, buf, NULL);

		unlink(path);
		free(path);
	}

	stripspace(buf, 1);

	if (!message && !buf->len)
		die("no tag message?");

	strbuf_insert(buf, 0, header_buf, header_len);

	if (sign && do_sign(buf) < 0)
		die("unable to sign the tag");
	if (write_sha1_file(buf->buf, buf->len, tag_type, result) < 0)
		die("unable to write tag file");
}

struct msg_arg {
	int given;
	struct strbuf buf;
};

static int parse_msg_arg(const struct option *opt, const char *arg, int unset)
{
	struct msg_arg *msg = opt->value;

	if (!arg)
		return -1;
	if (msg->buf.len)
		strbuf_addstr(&(msg->buf), "\n\n");
	strbuf_addstr(&(msg->buf), arg);
	msg->given = 1;
	return 0;
}

int cmd_tag(int argc, const char **argv, const char *prefix)
{
	struct strbuf buf;
	unsigned char object[20], prev[20];
	char ref[PATH_MAX];
	const char *object_ref, *tag;
	struct ref_lock *lock;

	int annotate = 0, sign = 0, force = 0, lines = 0,
		list = 0, delete = 0, verify = 0;
	char *msgfile = NULL, *keyid = NULL;
	struct msg_arg msg = { 0, STRBUF_INIT };
	struct option options[] = {
		OPT_BOOLEAN('l', NULL, &list, "list tag names"),
		{ OPTION_INTEGER, 'n', NULL, &lines, NULL,
				"print n lines of each tag message",
				PARSE_OPT_OPTARG, NULL, 1 },
		OPT_BOOLEAN('d', NULL, &delete, "delete tags"),
		OPT_BOOLEAN('v', NULL, &verify, "verify tags"),

		OPT_GROUP("Tag creation options"),
		OPT_BOOLEAN('a', NULL, &annotate,
					"annotated tag, needs a message"),
		OPT_CALLBACK('m', NULL, &msg, "msg",
			     "message for the tag", parse_msg_arg),
		OPT_STRING('F', NULL, &msgfile, "file", "message in a file"),
		OPT_BOOLEAN('s', NULL, &sign, "annotated and GPG-signed tag"),
		OPT_STRING('u', NULL, &keyid, "key-id",
					"use another key to sign the tag"),
		OPT_BOOLEAN('f', NULL, &force, "replace the tag if exists"),
		OPT_END()
	};

	git_config(git_tag_config);

	argc = parse_options(argc, argv, options, git_tag_usage, 0);

	if (keyid) {
		sign = 1;
		set_signingkey(keyid);
	}
	if (sign)
		annotate = 1;

	if (list)
		return list_tags(argv[0], lines);
	if (delete)
		return for_each_tag_name(argv, delete_tag);
	if (verify)
		return for_each_tag_name(argv, verify_tag);

	strbuf_init(&buf, 0);
	if (msg.given || msgfile) {
		if (msg.given && msgfile)
			die("only one -F or -m option is allowed.");
		annotate = 1;
		if (msg.given)
			strbuf_addbuf(&buf, &(msg.buf));
		else {
			if (!strcmp(msgfile, "-")) {
				if (strbuf_read(&buf, 0, 1024) < 0)
					die("cannot read %s", msgfile);
			} else {
				if (strbuf_read_file(&buf, msgfile, 1024) < 0)
					die("could not open or read '%s': %s",
						msgfile, strerror(errno));
			}
		}
	}

	if (argc == 0) {
		if (annotate)
			usage_with_options(git_tag_usage, options);
		return list_tags(NULL, lines);
	}
	tag = argv[0];

	object_ref = argc == 2 ? argv[1] : "HEAD";
	if (argc > 2)
		die("too many params");

	if (get_sha1(object_ref, object))
		die("Failed to resolve '%s' as a valid ref.", object_ref);

	if (snprintf(ref, sizeof(ref), "refs/tags/%s", tag) > sizeof(ref) - 1)
		die("tag name too long: %.*s...", 50, tag);
	if (check_ref_format(ref))
		die("'%s' is not a valid tag name.", tag);

	if (!resolve_ref(ref, prev, 1, NULL))
		hashclr(prev);
	else if (!force)
		die("tag '%s' already exists", tag);

	if (annotate)
		create_tag(object, tag, &buf, msg.given || msgfile,
			   sign, prev, object);

	lock = lock_any_ref_for_update(ref, prev, 0);
	if (!lock)
		die("%s: cannot lock the ref", ref);
	if (write_ref_sha1(lock, object, NULL) < 0)
		die("%s: cannot update the ref", ref);

	strbuf_release(&buf);
	return 0;
}
