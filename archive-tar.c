/*
 * Copyright (c) 2005, 2006 Rene Scharfe
 */
#include "cache.h"
#include "commit.h"
#include "tar.h"
#include "builtin.h"
#include "archive.h"

#define RECORDSIZE	(512)
#define BLOCKSIZE	(RECORDSIZE * 20)

static char block[BLOCKSIZE];
static unsigned long offset;

static time_t archive_time;
static int tar_umask = 002;
static int verbose;
static const struct commit *commit;
static size_t base_len;

/* writes out the whole block, but only if it is full */
static void write_if_needed(void)
{
	if (offset == BLOCKSIZE) {
		write_or_die(1, block, BLOCKSIZE);
		offset = 0;
	}
}

/*
 * queues up writes, so that all our write(2) calls write exactly one
 * full block; pads writes to RECORDSIZE
 */
static void write_blocked(const void *data, unsigned long size)
{
	const char *buf = data;
	unsigned long tail;

	if (offset) {
		unsigned long chunk = BLOCKSIZE - offset;
		if (size < chunk)
			chunk = size;
		memcpy(block + offset, buf, chunk);
		size -= chunk;
		offset += chunk;
		buf += chunk;
		write_if_needed();
	}
	while (size >= BLOCKSIZE) {
		write_or_die(1, buf, BLOCKSIZE);
		size -= BLOCKSIZE;
		buf += BLOCKSIZE;
	}
	if (size) {
		memcpy(block + offset, buf, size);
		offset += size;
	}
	tail = offset % RECORDSIZE;
	if (tail)  {
		memset(block + offset, 0, RECORDSIZE - tail);
		offset += RECORDSIZE - tail;
	}
	write_if_needed();
}

/*
 * The end of tar archives is marked by 2*512 nul bytes and after that
 * follows the rest of the block (if any).
 */
static void write_trailer(void)
{
	int tail = BLOCKSIZE - offset;
	memset(block + offset, 0, tail);
	write_or_die(1, block, BLOCKSIZE);
	if (tail < 2 * RECORDSIZE) {
		memset(block, 0, offset);
		write_or_die(1, block, BLOCKSIZE);
	}
}

/*
 * pax extended header records have the format "%u %s=%s\n".  %u contains
 * the size of the whole string (including the %u), the first %s is the
 * keyword, the second one is the value.  This function constructs such a
 * string and appends it to a struct strbuf.
 */
static void strbuf_append_ext_header(struct strbuf *sb, const char *keyword,
                                     const char *value, unsigned int valuelen)
{
	int len, tmp;

	/* "%u %s=%s\n" */
	len = 1 + 1 + strlen(keyword) + 1 + valuelen + 1;
	for (tmp = len; tmp > 9; tmp /= 10)
		len++;

	strbuf_grow(sb, len);
	strbuf_addf(sb, "%u %s=", len, keyword);
	strbuf_add(sb, value, valuelen);
	strbuf_addch(sb, '\n');
}

static unsigned int ustar_header_chksum(const struct ustar_header *header)
{
	char *p = (char *)header;
	unsigned int chksum = 0;
	while (p < header->chksum)
		chksum += *p++;
	chksum += sizeof(header->chksum) * ' ';
	p += sizeof(header->chksum);
	while (p < (char *)header + sizeof(struct ustar_header))
		chksum += *p++;
	return chksum;
}

static int get_path_prefix(const struct strbuf *path, int maxlen)
{
	int i = path->len;
	if (i > maxlen)
		i = maxlen;
	do {
		i--;
	} while (i > 0 && path->buf[i] != '/');
	return i;
}

static void write_entry(const unsigned char *sha1, struct strbuf *path,
                        unsigned int mode, void *buffer, unsigned long size)
{
	struct ustar_header header;
	struct strbuf ext_header;

	memset(&header, 0, sizeof(header));
	strbuf_init(&ext_header, 0);

	if (!sha1) {
		*header.typeflag = TYPEFLAG_GLOBAL_HEADER;
		mode = 0100666;
		strcpy(header.name, "pax_global_header");
	} else if (!path) {
		*header.typeflag = TYPEFLAG_EXT_HEADER;
		mode = 0100666;
		sprintf(header.name, "%s.paxheader", sha1_to_hex(sha1));
	} else {
		if (verbose)
			fprintf(stderr, "%.*s\n", (int)path->len, path->buf);
		if (S_ISDIR(mode) || S_ISGITLINK(mode)) {
			*header.typeflag = TYPEFLAG_DIR;
			mode = (mode | 0777) & ~tar_umask;
		} else if (S_ISLNK(mode)) {
			*header.typeflag = TYPEFLAG_LNK;
			mode |= 0777;
		} else if (S_ISREG(mode)) {
			*header.typeflag = TYPEFLAG_REG;
			mode = (mode | ((mode & 0100) ? 0777 : 0666)) & ~tar_umask;
		} else {
			error("unsupported file mode: 0%o (SHA1: %s)",
			      mode, sha1_to_hex(sha1));
			return;
		}
		if (path->len > sizeof(header.name)) {
			int plen = get_path_prefix(path, sizeof(header.prefix));
			int rest = path->len - plen - 1;
			if (plen > 0 && rest <= sizeof(header.name)) {
				memcpy(header.prefix, path->buf, plen);
				memcpy(header.name, path->buf + plen + 1, rest);
			} else {
				sprintf(header.name, "%s.data",
				        sha1_to_hex(sha1));
				strbuf_append_ext_header(&ext_header, "path",
				                         path->buf, path->len);
			}
		} else
			memcpy(header.name, path->buf, path->len);
	}

	if (S_ISLNK(mode) && buffer) {
		if (size > sizeof(header.linkname)) {
			sprintf(header.linkname, "see %s.paxheader",
			        sha1_to_hex(sha1));
			strbuf_append_ext_header(&ext_header, "linkpath",
			                         buffer, size);
		} else
			memcpy(header.linkname, buffer, size);
	}

	sprintf(header.mode, "%07o", mode & 07777);
	sprintf(header.size, "%011lo", S_ISREG(mode) ? size : 0);
	sprintf(header.mtime, "%011lo", archive_time);

	sprintf(header.uid, "%07o", 0);
	sprintf(header.gid, "%07o", 0);
	strlcpy(header.uname, "root", sizeof(header.uname));
	strlcpy(header.gname, "root", sizeof(header.gname));
	sprintf(header.devmajor, "%07o", 0);
	sprintf(header.devminor, "%07o", 0);

	memcpy(header.magic, "ustar", 6);
	memcpy(header.version, "00", 2);

	sprintf(header.chksum, "%07o", ustar_header_chksum(&header));

	if (ext_header.len > 0) {
		write_entry(sha1, NULL, 0, ext_header.buf, ext_header.len);
	}
	strbuf_release(&ext_header);
	write_blocked(&header, sizeof(header));
	if (S_ISREG(mode) && buffer && size > 0)
		write_blocked(buffer, size);
}

static void write_global_extended_header(const unsigned char *sha1)
{
	struct strbuf ext_header;

	strbuf_init(&ext_header, 0);
	strbuf_append_ext_header(&ext_header, "comment", sha1_to_hex(sha1), 40);
	write_entry(NULL, NULL, 0, ext_header.buf, ext_header.len);
	strbuf_release(&ext_header);
}

static int git_tar_config(const char *var, const char *value)
{
	if (!strcmp(var, "tar.umask")) {
		if (value && !strcmp(value, "user")) {
			tar_umask = umask(0);
			umask(tar_umask);
		} else {
			tar_umask = git_config_int(var, value);
		}
		return 0;
	}
	return git_default_config(var, value);
}

static int write_tar_entry(const unsigned char *sha1,
                           const char *base, int baselen,
                           const char *filename, unsigned mode, int stage)
{
	static struct strbuf path = STRBUF_INIT;
	void *buffer;
	enum object_type type;
	unsigned long size;

	strbuf_reset(&path);
	strbuf_grow(&path, PATH_MAX);
	strbuf_add(&path, base, baselen);
	strbuf_addstr(&path, filename);
	if (S_ISDIR(mode) || S_ISGITLINK(mode)) {
		strbuf_addch(&path, '/');
		buffer = NULL;
		size = 0;
	} else {
		buffer = sha1_file_to_archive(path.buf + base_len, sha1, mode,
				&type, &size, commit);
		if (!buffer)
			die("cannot read %s", sha1_to_hex(sha1));
	}

	write_entry(sha1, &path, mode, buffer, size);
	free(buffer);

	return READ_TREE_RECURSIVE;
}

int write_tar_archive(struct archiver_args *args)
{
	int plen = args->base ? strlen(args->base) : 0;

	git_config(git_tar_config);

	archive_time = args->time;
	verbose = args->verbose;
	commit = args->commit;
	base_len = args->base ? strlen(args->base) : 0;

	if (args->commit_sha1)
		write_global_extended_header(args->commit_sha1);

	if (args->base && plen > 0 && args->base[plen - 1] == '/') {
		char *base = xstrdup(args->base);
		int baselen = strlen(base);

		while (baselen > 0 && base[baselen - 1] == '/')
			base[--baselen] = '\0';
		write_tar_entry(args->tree->object.sha1, "", 0, base, 040777, 0);
		free(base);
	}
	read_tree_recursive(args->tree, args->base, plen, 0,
			    args->pathspec, write_tar_entry);
	write_trailer();

	return 0;
}
