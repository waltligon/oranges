/*
 * (C) 2001 Clemson University and The University of Chicago
 *
 * See COPYING in top-level directory.
 */

#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/time.h>
#include <time.h>
#include <stdlib.h>

#include "pvfs2.h"
#include "pvfs2-mgmt.h"

#ifndef PVFS2_VERSION
#define PVFS2_VERSION "Unknown"
#endif

struct options
{
    char* mnt_point;
    int mnt_point_set;
    int debug_mask;
    int debug_mask_set;
};

static struct options* parse_args(int argc, char* argv[],
                                  const PVFS_util_tab *mnt);
static void usage(int argc, char** argv);

int main(int argc, char **argv)
{
    int ret = -1;
    PVFS_fs_id cur_fs;
    const PVFS_util_tab* tab;
    struct options* user_opts = NULL;
    char pvfs_path[PVFS_NAME_MAX] = {0};
    PVFS_sysresp_init resp_init;
    PVFS_credentials creds;

    /* look at pvfstab */
    tab = PVFS_util_parse_pvfstab(NULL);
    if(!tab)
    {
        fprintf(stderr, "Error: failed to parse pvfstab.\n");
        return(-1);
    }

    /* look at command line arguments */
    user_opts = parse_args(argc, argv, tab);
    if(!user_opts)
    {
	fprintf(stderr, "Error: failed to parse command line arguments.\n");
	usage(argc, argv);
	return(-1);
    }

    memset(&resp_init, 0, sizeof(resp_init));
    ret = PVFS_sys_initialize(*tab, GOSSIP_NO_DEBUG, &resp_init);
    if(ret < 0)
    {
	PVFS_perror("PVFS_sys_initialize", ret);
	return(-1);
    }

    /* translate local path into pvfs2 relative path */
    ret = PVFS_util_resolve(user_opts->mnt_point,
        &cur_fs, pvfs_path, PVFS_NAME_MAX);
    if(ret < 0)
    {
	fprintf(stderr, "Error: could not find filesystem for %s in pvfstab\n", 
	    user_opts->mnt_point);
	return(-1);
    }

    creds.uid = getuid();
    creds.gid = getgid();

    ret = PVFS_mgmt_setparam_all(cur_fs, creds, PVFS_SERV_PARAM_GOSSIP_MASK,
	user_opts->debug_mask, NULL);

    PVFS_sys_finalize();

    return(ret);
}


/* parse_args()
 *
 * parses command line arguments
 *
 * returns pointer to options structure on success, NULL on failure
 */
static struct options* parse_args(int argc, char* argv[], const
    PVFS_util_tab *mnt)
{
    /* getopt stuff */
    extern char* optarg;
    extern int optind, opterr, optopt;
    char flags[] = "vm:";
    int one_opt = 0;
    int len = 0;

    struct options* tmp_opts = NULL;
    int ret = -1;

    /* create storage for the command line options */
    tmp_opts = (struct options*)malloc(sizeof(struct options));
    if(!tmp_opts){
	return(NULL);
    }
    memset(tmp_opts, 0, sizeof(struct options));

    /* fill in defaults (except for hostid) */
    tmp_opts->debug_mask = 0;

    /* look at command line arguments */
    while((one_opt = getopt(argc, argv, flags)) != EOF){
	switch(one_opt)
        {
            case('v'):
                printf("%s\n", PVFS2_VERSION);
                exit(0);
	    case('m'):
		len = strlen(optarg)+1;
		tmp_opts->mnt_point = (char*)malloc(len+1);
		if(!tmp_opts->mnt_point)
		{
		    free(tmp_opts);
		    return(NULL);
		}
		memset(tmp_opts->mnt_point, 0, len+1);
		ret = sscanf(optarg, "%s", tmp_opts->mnt_point);
		if(ret < 1){
		    free(tmp_opts);
		    return(NULL);
		}
		/* TODO: dirty hack... fix later.  The remove_dir_prefix()
		 * function expects some trailing segments or at least
		 * a slash off of the mount point
		 */
		strcat(tmp_opts->mnt_point, "/");
		tmp_opts->mnt_point_set = 1;
		break;
	    case('?'):
		usage(argc, argv);
		exit(EXIT_FAILURE);
	}
    }

    if(optind != (argc - 1))
    {
	usage(argc, argv);
	exit(EXIT_FAILURE);
    }

    tmp_opts->debug_mask = PVFS_debug_eventlog_to_mask(argv[argc-1]);
    tmp_opts->debug_mask_set = 1;

    /* typical case of just a single tab entry requires no -m argument */
    if (!tmp_opts->mnt_point_set) {
	if (mnt->mntent_count == 1) {
	    /* see dirty hack above */
	    char *x = malloc(strlen(mnt->mntent_array[0].mnt_dir) + 2);
	    if (!x)
		return 0;
	    strcpy(x, mnt->mntent_array[0].mnt_dir);
	    strcat(x, "/");
	    tmp_opts->mnt_point = x;
	} else
	    return 0;
    }

    return(tmp_opts);
}


static void usage(int argc, char** argv)
{
    int i = 0;
    char *mask = NULL;

    fprintf(stderr, "\n");
    fprintf(stderr, "Usage  : %s [-m fs_mount_point] <mask list>\n",
	argv[0]);
    fprintf(stderr, "Example: %s -m /mnt/pvfs2 \"network,server\"\n",
	argv[0]);
    fprintf(stderr, "Available masks include:\n");

    while((mask = PVFS_debug_get_next_debug_keyword(i++)) != NULL)
    {
        fprintf(stderr,"\t%s  ",mask);
        if ((i % 4) == 0)
            fprintf(stderr,"\n");
    }
    fprintf(stderr, "\n");

    return;
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 noexpandtab
 */

