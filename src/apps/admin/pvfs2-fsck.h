/*
 * (C) 2004 Clemson University and The University of Chicago
 *
 * See COPYING in top-level directory.
 */

#ifndef __PVFS2_FSCK_H
#define __PVFS2_FSCK_H

/* utility functions */
static struct options *parse_args(int argc, char* argv[]);
static void usage(int argc, char** argv);
static char *get_type_str(int type);

/* processing functions */
struct handlelist *build_handlelist(PVFS_fs_id cur_fs,
				    PVFS_BMI_addr_t *addr_array,
				    int server_count,
				    PVFS_credentials *creds);

int traverse_directory_tree(PVFS_fs_id cur_fs,
			    struct handlelist *hl,
			    PVFS_BMI_addr_t *addr_array,
			    int server_count,
			    PVFS_credentials *creds,
			    struct options *opts_p);

int descend(PVFS_fs_id cur_fs,
	    struct handlelist *hl,
	    struct handlelist *alt_hl,
	    PVFS_object_ref pref,
	    PVFS_credentials *creds);

int verify_datafiles(PVFS_fs_id cur_fs,
		     struct handlelist *hl,
		     struct handlelist *alt_hl,
		     PVFS_object_ref mf_ref,
		     int df_count,
		     PVFS_credentials *creds);

struct handlelist *find_sub_trees(PVFS_fs_id cur_fs,
				  struct handlelist *hl,
				  PVFS_id_gen_t *addr_array,
				  PVFS_credentials *creds);

struct handlelist *fill_lost_and_found(PVFS_fs_id cur_fs,
				       struct handlelist *hl,
				       PVFS_id_gen_t *addr_array,
				       PVFS_credentials *creds);

void cull_leftovers(PVFS_fs_id cur_fs,
		    struct handlelist *hl,
		    PVFS_id_gen_t *addr_array,
		    PVFS_credentials *creds);

/* handlelist structure, functions */
struct handlelist {
    int server_ct;
    PVFS_handle **list_array;
    int *size_array;
    int *used_array;
};

static struct handlelist *handlelist_initialize(int *handle_counts,
						int server_count);

static void handlelist_add_handle(struct handlelist *hl,
				  PVFS_handle handles,
				  int server_idx);

static void handlelist_add_handles(struct handlelist *hl,
				   PVFS_handle *handles,
				   int handle_count,
				   int server_idx);

static void handlelist_finished_adding_handles(struct handlelist *hl);

static int handlelist_find_handle(struct handlelist *hl,
				  PVFS_handle handle,
				  int *server_idx_p);

static void handlelist_remove_handle(struct handlelist *hl,
				     PVFS_handle handle,
				     int server_idx);

static int handlelist_return_handle(struct handlelist *hl,
				    PVFS_handle *handle_p,
				    int *server_idx_p);

static void handlelist_finalize(struct handlelist **hl);

static void handlelist_print(struct handlelist *hl);

#endif

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
