/*
 * (C) 2014 Clemson University and The University of Chicago
 *
 * See COPYING in top-level directory.
 */

#include <stdio.h>
#include <errno.h>
#include <fts.h>
#include <sys/types.h>
#include <sys/stat.h>
#if __has_include("attr/xattr.h")
    #include <attr/xattr.h>
#else
    #include <sys/xattr.h>
#endif
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include "orange.h"

/* optional parameters, filled in by parse_args() */
struct user_options
{
    char *dist_name;
    char *dist_params;
    int32_t num_dfiles;
    int32_t layout;
    char *server_list;
    int32_t interactive;
    int32_t recursive;
    int32_t verbose;
    int32_t debug;
    int32_t num_files;
    char **filenames;
};

struct layout_table_s 
{
    char *name;
    int value;
};

static int translate_layout(char *layout);
static int parse_args(int argc, char **argv, struct user_options *user_opts_p);
static void usage(int argc, char **argv);

int main(int argc, char **argv)
{
    int ret = EXIT_FAILURE;
    FTS *fs;
    FTSENT *node;
    int flags = 0;
    unsigned char error_seen = 0;
    struct user_options user_opts =
                      {NULL, NULL, 0, 0, NULL, 0, 0, 0, 0, 0, NULL};
    
    /* look at command line arguments */
    ret = parse_args(argc, argv, &user_opts);
    if(ret < 0)
    {
        fprintf(stderr, "Error: failed to parse command line arguments.\n");
        if(user_opts.filenames)
        {
            free(user_opts.filenames);
        }
        return(-1);
    }

    fs = fts_open(user_opts.filenames, FTS_COMFOLLOW | FTS_PHYSICAL , NULL);
    if(fs == NULL)
    {
        perror("fts_open");
        ret = -1;
        goto main_out;
    }
    if (!user_opts.recursive)
    {
        /* remove directory contents from the list of items to set eattr */
        node = fts_children(fs, 0);
        while(node)
        {
            if (node->fts_info == FTS_D)
            {
                fts_set(fs, node, FTS_SKIP);
            }
            node=node->fts_link;
        }
    }
    /* process all attributes */
    while((node = fts_read(fs)) != NULL)
    {
        if (user_opts.debug)
        {
            printf("accpath = %s path = %s level = %d\n",
                   node->fts_accpath, node->fts_path, node->fts_level);
        }
        switch(node->fts_info)
        {
        case FTS_D : /* preorder dir */
        {
            int fd; /* file descriptor for target file */
            if (user_opts.interactive)
            {
                char response = 0, c = 0;
                printf("Set directory hints on %s? (y/n)", node->fts_accpath);
                fflush(stdout);
                response = fgetc(stdin);
                while((c = fgetc(stdin)) != '\n');
                if (response != 'y' && response != 'Y')
                {
                    break;
                }
            }
            if (user_opts.verbose)
            {
                printf("Setting directory hints on %s\n", node->fts_accpath);
            }
            fd = open(node->fts_accpath, O_RDWR | O_EXCL);
            if (fd < 0)
            {
                perror("open");
                break;
            }
            if (user_opts.dist_name)
            {
                ret = fsetxattr(fd,
                               "user.pvfs2.dist_name",
                               user_opts.dist_name,
                               strlen(user_opts.dist_name) + 1,
                               flags);
                if (ret == 0 && user_opts.verbose)
                {
                    fprintf(stderr, "Distribution name set\n");
                }
                else if (ret < 0)
                {
                    perror("dist name");
                    return ret;
                }
            }
            if (user_opts.dist_params)
            {
                ret = fsetxattr(fd,
                               "user.pvfs2.dist_params",
                               user_opts.dist_params,
                               strlen(user_opts.dist_params) + 1,
                               flags);
                if (ret == 0 && user_opts.verbose)
                {
                    fprintf(stderr, "Distribution parameters set\n");
                }
                else if (ret < 0)
                {
                    perror("dist param");
                }
            }
            if (user_opts.layout > PVFS_SYS_LAYOUT_NULL &&
                user_opts.layout <= PVFS_SYS_LAYOUT_MAX)
            {
                char layout_str[10];
                int slen;
                slen = snprintf(layout_str, 10, "%05d", user_opts.layout);
                ret = fsetxattr(fd,
                                "user.pvfs2.layout",
                                layout_str,
                                slen + 1,
                                flags);
                if (ret == 0 && user_opts.verbose)
                {
                    fprintf(stderr, "Layout set to %d\n", user_opts.layout);
                }
                else if (ret < 0)
                {
                    perror("layout");
                    return ret;
                }
            }
            if (user_opts.layout == PVFS_SYS_LAYOUT_LIST &&
                user_opts.server_list)
            {
                PVFS_sys_layout *layout = NULL;
                char *eattr_str = NULL;
                int strsz = 0;

                /* server lists are stored as BMI addresss strings
                 * packed into one field with each server delimited by a NULL 
                 */
                layout = pvfs_layout_fd(fd, user_opts.server_list);
                if (!layout)
                {
                    perror("create_layout");
                    return ret;
                }
                eattr_str = (char *)malloc(PVFS_SYS_LIMIT_LAYOUT);
                ret = pvfs_layout_string(layout,
                                         eattr_str,
                                         PVFS_SYS_LIMIT_LAYOUT);
                if (ret != 0)
                {
                    perror("layout_string");
                    return ret;
                }
                strsz = strlen(eattr_str) + 1;
                ret = fsetxattr(fd,
                                "user.pvfs2.server_list",
                                eattr_str,
                                strsz,
                                flags);
                if (ret == 0 && user_opts.verbose)
                {
                    fprintf(stderr, "Server list set(%d) = |%s|\n",
                            strsz, eattr_str);
                }
                else if (ret < 0)
                {
                    perror("server list");
                    return ret;
                }
                user_opts.num_dfiles = layout->server_list.count;
                pvfs_release_layout(layout);
                free(eattr_str);
            }
            if (user_opts.num_dfiles > 0)
            {
                char num_dfiles_str[10];
                int slen;
                slen = snprintf(num_dfiles_str,
                                10,
                                "%05d",
                                user_opts.num_dfiles);
                ret = fsetxattr(fd,
                                "user.pvfs2.num_dfiles",
                                num_dfiles_str,
                                slen + 1,
                                flags);
                if (ret == 0 && user_opts.verbose)
                {
                    fprintf(stderr, "Number of dfiles set to %d\n",
                            user_opts.num_dfiles);
                }
                else if (ret < 0)
                {
                    perror("num dfiles");
                }
            }
            if (ret < 0)
            {
                if ((errno == ENOENT))
                {
                    break;
                }
                perror("fsetxattr");
                error_seen = 1;
                fprintf(stderr,
                        "fsetxattr failed on path: %s\n",
                        node->fts_accpath);
            }
            close(fd);
            break;
        }
        case FTS_DP : /* postorder dir */
        case FTS_F : /* reg file */
        case FTS_SL : /* sym link */
        case FTS_SLNONE : /* sym link - no target */
            break;
        case FTS_DNR : /* can't read dir */
            /* skip with msg */
            error_seen = 1;
            fprintf(stderr,
                    "Cannot read directory %s - skipping\n",
                    node->fts_accpath);
            break;
        case FTS_DC :   /* cycle dir */
        case FTS_DOT :  /* dot dir */
        case FTS_NS :   /* no stat err */
        case FTS_NSOK : /* no stat ok */
        case FTS_ERR :  /* error */
        default:
            fprintf(stderr,
                    "%s: %s is not a directory, or symbolic link\n",
                    argv[0],
                    node->fts_path);
            usage(argc, argv);
            ret = -1;
            goto main_out;
            break;
        }
    }

main_out:

    fts_close(fs);

    if(error_seen)
    {
        ret = EXIT_FAILURE;
    }
    else
    {
        ret = EXIT_SUCCESS;
    }
    if(user_opts.filenames)
    {
        free(user_opts.filenames);
    }
    return ret;
}

static int translate_layout(char *layout)
{
    int i = 0;
    int ret = 0;
    static struct layout_table_s layout_table[] = {
        {"none", 1},
        {"1", 1},
        {"round_robin", 2},
        {"2", 2},
        {"random", 3},
        {"3", 3},
        {"list", 4},
        {"4", 4},
        {"local", 5},
        {"5", 5}
    };

    for(i = 0; i < sizeof(layout_table)/sizeof(struct layout_table_s); i++)
    {
        ret = strcmp(layout_table[i].name, layout);
        if(ret == 0)
        {
            return layout_table[i].value;
        }
    }
    return 0;
    fprintf(stderr, "Laoyout unrecognized\n");
}

/* parse_args()
 *
 * parses command line arguments
 *
 * Returns 0 on success and exits with EXIT_FAILURE on failure.
 */
static int parse_args(int argc, char **argv, struct user_options *user_opts_p)
{
    int one_opt = 0;
    char flags[] = "vVdirfhD:p:n:l:L:";
    int index = 1; /* stand in for optind */

    static struct option lopt[] = {
        {"dist_name", 1, NULL, 'D'},
        {"dist_params", 1, NULL, 'p'},
        {"num_datafiles", 1, NULL, 'n'},
        {"layout", 1, NULL, 'l'},
        {"server_list", 1, NULL, 'L'},
        {"recursive", 0, NULL, 'r'},
        {"interactive", 0, NULL, 'i'},
        {"help", 0, NULL, 'h'},
        {"Version", 0, NULL, 'V'},
        {"verbose", 0, NULL, 'v'},
        {"debug", 0, NULL, 'd'},
        {NULL, 0, NULL, 0}
    };

    opterr = 0;

    while((one_opt = getopt_long(argc, argv, flags, lopt, NULL)) != -1)
    {
        index++;
        switch(one_opt)
        {
            case('V'):
                printf("%s\n", PVFS2_VERSION);
                break;
            case('v'):
                user_opts_p->verbose = 1;
                break;
            case('d'):
                user_opts_p->debug = 1;
                break;
            case('i'):
                user_opts_p->interactive = 1;
                break;
            case('r'):
                user_opts_p->recursive = 1;
                break;
            case('D'):
                if (!optarg)
                {
                    fprintf(stderr, "bad argument\n");
                    break;
                }
                user_opts_p->dist_name = strdup(optarg);
                index++;
                break;
            case('s'):
                if (!optarg)
                {
                    fprintf(stderr, "bad argument\n");
                    break;
                }
                user_opts_p->dist_params = strdup(optarg);
                index++;
                break;
            case('n'):
                if (!optarg)
                {
                    fprintf(stderr, "bad argument\n");
                    break;
                }
                user_opts_p->num_dfiles = atoi(optarg);
                index++;
                break;
            case('l'):
                if (!optarg)
                {
                    fprintf(stderr, "bad argument\n");
                    break;
                }
                user_opts_p->layout = translate_layout(optarg);
                index++;
                break;
            case('L'):
                if (!optarg)
                {
                    fprintf(stderr, "bad argument\n");
                    break;
                }
                user_opts_p->server_list = strdup(optarg);
                index++;
                break;
            case('h'):
            default:
                usage(argc, argv);
                exit(EXIT_FAILURE);
                break;
        }
    }

    /* optind is broken, use index */
    if (index < argc)
    {
        /* there are arguments - file names - to process */
        int i = 0;
        user_opts_p->num_files = argc - index;
        user_opts_p->filenames = malloc((argc - index + 1) * sizeof(char *));
        while (index < argc)
        {
            user_opts_p->filenames[i] = argv[index];
            index++;
            i++;
        }
        user_opts_p->filenames[i] = NULL;
    }
    else
    {
        usage(argc, argv);
        exit(EXIT_FAILURE);
    }
    return 0;
}


static void usage(int argc, char **argv)
{
    fprintf(stderr, "Usage: %s [-hVvdirDpnlL] pvfs2 dir name[s]\n", argv[0]);
    fprintf(stderr, "\t-h, --help - print this message\n");
    fprintf(stderr, "\t-V, --Version - print Version info\n");
    fprintf(stderr, "\t-v, --verbose - print informative messages\n");
    fprintf(stderr, "\t-d, --debug - print debugging info\n");
    fprintf(stderr, "\t-i, --interactive - ask before setting each item\n");
    fprintf(stderr, "\t-r, --recursive - set subdirectories\n");
    fprintf(stderr, "\t-D, --dist_name - string name of distribution\n");
    fprintf(stderr, "\t-p, --dist_params - string list K:V+K:V ...\n");
    fprintf(stderr, "\t-n, --num_datafiles - int number of dfiles\n");
    fprintf(stderr, "\t-l, --layout - int(string) layout number(name)\n");
    fprintf(stderr, "\t-L, --server_list - string list of server numbers \n");
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */

