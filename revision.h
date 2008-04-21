#ifndef REVISION_H
#define REVISION_H

#define SEEN		(1u<<0)
#define UNINTERESTING   (1u<<1)
#define TREESAME	(1u<<2)
#define SHOWN		(1u<<3)
#define TMP_MARK	(1u<<4) /* for isolated cases; clean after use */
#define BOUNDARY	(1u<<5)
#define CHILD_SHOWN	(1u<<6)
#define ADDED		(1u<<7)	/* Parents already parsed and added? */
#define SYMMETRIC_LEFT	(1u<<8)
#define TOPOSORT	(1u<<9)	/* In the active toposort list.. */

struct rev_info;
struct log_info;

struct rev_info {
	/* Starting list */
	struct commit_list *commits;
	struct object_array pending;

	/* Parents of shown commits */
	struct object_array boundary_commits;

	/* Basic information */
	const char *prefix;
	void *prune_data;
	unsigned int early_output;

	/* Traversal flags */
	unsigned int	dense:1,
			prune:1,
			no_merges:1,
			no_walk:1,
			show_all:1,
			remove_empty_trees:1,
			simplify_history:1,
			lifo:1,
			topo_order:1,
			tag_objects:1,
			tree_objects:1,
			blob_objects:1,
			edge_hint:1,
			limited:1,
			unpacked:1, /* see also ignore_packed below */
			boundary:2,
			left_right:1,
			parents:1,
			reverse:1,
			cherry_pick:1,
			first_parent_only:1;

	/* Diff flags */
	unsigned int	diff:1,
			full_diff:1,
			show_root_diff:1,
			no_commit_id:1,
			verbose_header:1,
			ignore_merges:1,
			combine_merges:1,
			dense_combined_merges:1,
			always_show_header:1;

	/* Format info */
	unsigned int	shown_one:1,
			abbrev_commit:1,
			use_terminator:1;
	enum date_mode date_mode;

	const char **ignore_packed; /* pretend objects in these are unpacked */
	int num_ignore_packed;

	unsigned int	abbrev;
	enum cmit_fmt	commit_format;
	struct log_info *loginfo;
	int		nr, total;
	const char	*mime_boundary;
	char		*message_id;
	const char	*ref_message_id;
	const char	*add_signoff;
	const char	*extra_headers;
	const char	*log_reencode;
	const char	*subject_prefix;
	int		no_inline;
	int		show_log_size;

	/* Filter by commit log message */
	struct grep_opt	*grep_filter;

	/* special limits */
	int skip_count;
	int max_count;
	unsigned long max_age;
	unsigned long min_age;

	/* diff info for patches and for paths limiting */
	struct diff_options diffopt;
	struct diff_options pruning;

	struct reflog_walk_info *reflog_info;
};

#define REV_TREE_SAME		0
#define REV_TREE_NEW		1
#define REV_TREE_DIFFERENT	2

/* revision.c */
typedef void (*show_early_output_fn_t)(struct rev_info *, struct commit_list *);
volatile show_early_output_fn_t show_early_output;

extern void init_revisions(struct rev_info *revs, const char *prefix);
extern int setup_revisions(int argc, const char **argv, struct rev_info *revs, const char *def);
extern int handle_revision_arg(const char *arg, struct rev_info *revs,int flags,int cant_be_filename);

extern int prepare_revision_walk(struct rev_info *revs);
extern struct commit *get_revision(struct rev_info *revs);

extern void mark_parents_uninteresting(struct commit *commit);
extern void mark_tree_uninteresting(struct tree *tree);

struct name_path {
	struct name_path *up;
	int elem_len;
	const char *elem;
};

extern void add_object(struct object *obj,
		       struct object_array *p,
		       struct name_path *path,
		       const char *name);

extern void add_pending_object(struct rev_info *revs, struct object *obj, const char *name);

extern void add_head_to_pending(struct rev_info *);

enum commit_action {
	commit_ignore,
	commit_show,
	commit_error
};

extern enum commit_action simplify_commit(struct rev_info *revs, struct commit *commit);

#endif
