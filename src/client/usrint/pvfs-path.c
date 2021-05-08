/* 
 * (C) 2011 Clemson University and The University of Chicago 
 *
 * See COPYING in top-level directory.
 */

/** \file
 *  \ingroup usrint
 *
 *  PVFS2 user interface routines - routines to manage open files
 */
#define USRINT_SOURCE 1
#include "usrint.h"
#include <sys/syscall.h>
#ifndef SYS_readdir
#define SYS_readdir 89
#endif
//#include "posix-ops.h"
#include "openfile-util.h"
#include "iocommon.h"
//#include "posix-pvfs.h"
#include "pvfs-path.h"
//#ifdef PVFS_AIO_ENABLE
//#include "aiocommon.h"
//#endif

//#if PVFS_UCACHE_ENABLE
//#include "ucache.h"
//#endif

/**
 * Just like pvfs_qualify path, except it also expands
 * symbolic links which requires looking up each
 * segment one at a time so we only do this if we feel
 * we might have a sym link.
 *
 * Values set on a pvfs_path
 * PATH_LOOKEDUP - the path points to an existing fs object
 * PATH_RESOLVED - the path points into a PVFS volume
 * PATH_MNTPOINT - the mount point of the PVFS volume
 * PATH_ERROR - an error occured in the process
 * PATH_EXPANDED - indicates this routine was run successfully
 *
 * Paths can be looked up and not resolved (a non-pvfs object)
 * or resolved and not looked up (a pvfs path but object not found)
 * or neither (not an existing path at all) or both (a pvfs object).
 * mntpoint only valid if resolved.  error contains the errno value.
 *
 * NOTE:  if you make changes to this function after
 * 08/08/2012, remember to make changes to PVFS_qualify_path()
 * in pvfs-qualify-path.c
 */
char *PVFS_expand_path(const char *path, int skip_last_lookup)
{
    int ret = 0;
    int is_a_link = 0;
    int readlinks = 0;
    int n = 0;
    char *opath = NULL;
    char *npath = NULL;
    char *buf = NULL;
    char *obuf = NULL;
    char link_path[PVFS_PATH_MAX + 1];
    PVFS_path_t *Ppath;
    PVFS_object_ref obj_ref;

    if (!path)
    {
        return NULL;
    }
    /* see if this is a PVFS path if not make one */
    Ppath = PVFS_path_from_expanded((char *)path);
    if (!VALID_PATH_MAGIC(Ppath))
    {
        Ppath = PVFS_new_path(path);
    }
    opath = Ppath->orig_path;
    npath = Ppath->expanded_path;

    /* if we need to start over at expand, clear flags first */
    /* OK to expand a qualified path */
    if (PATH_EXPANDED(Ppath))
    {
        return Ppath->expanded_path;
    }

    /* reset things to initial conditions */
    CLEAR_ERROR(Ppath);
    CLEAR_MNTPOINT(Ppath);
    CLEAR_RESOLVED(Ppath);
    CLEAR_LOOKEDUP(Ppath);
    Ppath->pvfs_path = NULL;
    Ppath->filename = NULL;
    Ppath->fs_id = PVFS_FS_ID_NULL;
    Ppath->handle = 0;
    memset(link_path, 0, PVFS_PATH_MAX + 1);
    /* as we expand the path we will be resolving and looking up
     * each segment, each segment is considered qualified as we
     * go so we set the qualified flag
     */
    SET_QUALIFIED(Ppath);
    /* now expand the path */
    memset(npath, 0, PVFS_PATH_MAX + 1);
    if(*opath != '/')
    {
        if (!getcwd(Ppath->expanded_path, PVFS_PATH_MAX - 2))
        {
            return NULL;
        }
        npath += strnlen(npath, PVFS_PATH_MAX);
        /* we need to know if we can already resolve this */
        PVFS_util_resolve_absolute(Ppath->expanded_path);
        if (npath[-1] != '/')
            *npath++ = '/';
    }       
    else
    {
        *npath++ = '/';
        opath++;
    }
    /* Expand each slash-separated pathname component. */
    while (*opath != '\0')
    {
        /* ignore stray '/' */
        if (*opath == '/')
        {
            opath++;
            continue;
        }
        if (*opath == '.' && (opath[1] == '\0' || opath[1] == '/'))
        {
            /* Ignore "." */
            opath++;
            continue;
        }   
        if (*opath == '.' && opath[1] == '.' &&
            (opath[2] == '\0' || opath[2] == '/'))
        {   
            /* Backup for ".." */
            opath += 2;
            while (npath > Ppath->expanded_path + 1 && (--npath)[-1] != '/')
                ; 
            /* clear these, we will regenerate them if needed */
            CLEAR_RESOLVED(Ppath);
            CLEAR_LOOKEDUP(Ppath);
            Ppath->pvfs_path = NULL;
            Ppath->filename = NULL;
            Ppath->fs_id = PVFS_FS_ID_NULL;
            Ppath->handle = 0;
            continue;
        }   
        /* Safely copy the next pathname component. */
        while (*opath != '\0' && *opath != '/')
        {
            if (npath - Ppath->expanded_path > PVFS_PATH_MAX - 2)
            {
                ret = -PVFS_ENAMETOOLONG;
                goto err;
            }
            *npath++ = *opath++;
        }

        /* Protect against infinite loops. */
#define MAX_READLINKS 16
        if (readlinks++ > MAX_READLINKS)
        {
            ret = -PVFS_ELOOP;
            goto err;
        }

        /* See if last pathname component is a symlink. */
        *npath = '\0';
        is_a_link = 0;

        /* this must assume clean path */
        /* this will set RESOLVED and MNTPOINT flags */
        ret = PVFS_util_resolve_absolute(Ppath->expanded_path);
        /* if this is a no-follow situation then we skip */
        /* resolving the last segment, so see if this is */
        /* the last segment */
        if (skip_last_lookup)
        {
            char *p;
            p = opath;
            while(*p)
            {
                if (*p== '/')
                {
                    p += 1;
                    continue;
                }
                if (*p == '.' && *(p+1) == '0')
                {
                    p += 1;
                    continue;
                }
                if (*p == '.' && *(p+1) == '/')
                {
                    p += 2;
                    continue;
                }
                break;
            }
            if (!*p)
            {
                continue; /* returns to main loop */
            }
        }
        if (!ret)
        {
            /* this is for PVFS space lookups */
            PVFS_sys_attr attr;

            /* this must assume clean path */
            ret = iocommon_lookup_absolute(Ppath->expanded_path,
                                           PVFS2_LOOKUP_LINK_NO_FOLLOW,
                                           &obj_ref,
                                           NULL,
                                           0);
            Ppath->rc = ret; /* save this return code */
            if (ret == 0)
            {
                /* get the attributes to see if it is a link */
                Ppath->rc = iocommon_getattr(obj_ref, &attr,
                                             PVFS_ATTR_SYS_TYPE |
                                             PVFS_ATTR_SYS_LNK_TARGET);
                if (attr.objtype == PVFS_TYPE_SYMLINK)
                {
                    /* get the link contents */
                    strncpy(link_path, attr.link_target, PVFS_PATH_MAX);
                    n = strlen(link_path);
                    is_a_link = 1;
                    /* even though we were success looking this up
                     * it is a symbolic link and we are going to expand
                     * it so we don't want to update the fs_id and
                     * handle just leave the last one in place
                     */
                }
                else
                {
                    /* This successfully looked up so we have a valid
                    * object up to the current spot in the list 
                    * If the next lookup works we will overwrite
                    * so we leave the object of the last sucessful
                    * lookup
                    */
                    Ppath->fs_id = obj_ref.fs_id;
                    Ppath->handle = obj_ref.handle;
                    Ppath->filename = npath;
                    SET_LOOKEDUP(Ppath);
                }
            }
            else
            {
                /* Failed to look this up so this is either an
                 * erroneous path or it is a path to an object
                 * we are creating.  
                 */
                *npath++ = '/';
                break; /* out of while (*opath != '\0') */
            }
        }
        else
        {
            /* this is for non PVFS space */
            #ifdef SYS_readlink
            n = syscall(SYS_readlink,
            #else
            n = syscall(SYS_readlinkat, AT_FDCWD,
            #endif
                        Ppath->expanded_path,
                        link_path,
                        PVFS_PATH_MAX);
            if (n < 0)
            {
                
                if (errno != EINVAL)
                {
                    /* an error so we bail out */
                    Ppath->rc = -errno;
                    errno = 0;
                    goto err;
                }
                /* else not a sym link 
                 * check to see if it is a valid object
                 */
                #ifdef SYS_stat
                struct stat sbuf;
                n = syscall(SYS_stat,
                            Ppath->expanded_path,
                            &sbuf);
                #else
                struct statx sbuf;
                n = syscall(SYS_statx, AT_FDCWD
                            Ppath->expanded_path,
                            AT_STATX_SYNC_AS_STAT,
                            STATX_BASIC_STATS,
                            &sbuf);
                #endif

                if (n < 0)
                {
                    /* Failed to look this up so this is either an
                     * erroneous path or it is a path to an object
                     * we are creating.  
                     */
                    *npath++ = '/';
                    break; /* out of while (*opath != '\0') */
                }
                /* mark path up to here as existing  - though not PVFS */
                SET_LOOKEDUP(Ppath);
                /* Ppath->filename = npath; */
                /* cover the error code */
                errno = 0;
            }
            else
            {
                /* is a sym link */
                is_a_link = (n > 0);
                if (is_a_link)
                {
                    /* Note: readlink doesn't add the null byte. */
                    link_path[n] = '\0';
                }
                /* n == 0 is a link with no path ??? */
            }
        }
        /* expand symlink */
        if (is_a_link)
        {
            int m;

            if (*link_path == '/')
            {
                /* Start over for an absolute symlink. */
                npath = Ppath->expanded_path;
                /* since we are starting from the root */
                /* we can nolonger be resolved or looked up */
                CLEAR_MNTPOINT(Ppath);
                CLEAR_RESOLVED(Ppath);
                CLEAR_LOOKEDUP(Ppath);
                Ppath->pvfs_path = NULL;
                Ppath->filename = NULL;
                Ppath->fs_id = PVFS_FS_ID_NULL;
                Ppath->handle = 0;
            }
            else
            {
                /* Otherwise back up over this component. */
                while (*(--npath) != '/')
                    ;
            }

            /* Insert symlink contents into path. */
            m = strlen(opath);
            if (buf)
            {
                /* want to free buf but a previous symlink */
                /* may have set opath pointing into buf */
                /* and we reference opath below so we */
                /* delay the free */
                obuf = buf;
                buf = NULL;
            }
            buf = malloc(m + n + 1);
            if(!buf)
            {
                ret = -PVFS_ENOMEM;
                goto err;
            }
            memcpy(buf, link_path, n);
            memcpy(buf + n, opath, m + 1);
            opath = buf;
            if (obuf)
            {
                free(obuf);
                obuf = NULL;
            }
        }
        *npath++ = '/';
    }
    /* Delete trailing slash but don't whomp a lone slash. */
    if (npath != Ppath->expanded_path + 1 && npath[-1] == '/')
    {
        npath--;
    }
    /* Make sure it's null terminated. */
    *npath = '\0';

    if (buf)
    {
        free(buf);
    }

    SET_EXPANDED(Ppath);
    if (!PATH_RESOLVED(Ppath))
    {
        /* final check to see if the path resolves */
        PVFS_util_resolve_absolute(Ppath->expanded_path);
    }
    if (PATH_RESOLVED(Ppath) && !PATH_LOOKEDUP(Ppath))
    {
        /* final attempt to look up the path */
        ret = iocommon_lookup_absolute(Ppath->expanded_path,
                                       PVFS2_LOOKUP_LINK_NO_FOLLOW,
                                       &obj_ref,
                                       NULL,
                                       0);
        Ppath->rc = ret; /* save this return code */
        if (ret == 0)
        {
            Ppath->fs_id = obj_ref.fs_id;
            Ppath->handle = obj_ref.handle;
            Ppath->filename = npath;
            SET_LOOKEDUP(Ppath);
        }
    }
    /* if this was already a PVFS_path then this is
     * the same path that was passed in
     */
    return Ppath->expanded_path;

err:
    if (Ppath->rc != 0)
    {
        SET_ERROR(Ppath);
    }
    if (obuf)
    {
        free(obuf);
    }
    if (buf)
    {
        free(buf);
    }
    return NULL;
}

/**
 * Determines if a path is part of a PVFS Filesystem 
 *
 * returns 1 if PVFS 0 otherwise
 */

int is_pvfs_path(const char **path, int skip_last_lookup)
{
    int rc = 0;
    PVFS_path_t *Ppath;
    char *newpath = NULL ;
#if PVFS_USRINT_KMOUNT
    int npsize;
    struct stat sbuf;
    struct statfs fsbuf;
#endif
    
    if(pvfs_sys_init())
    {
        return 0; /* assume non-PVFS because we haven't initialized yet */
                  /* see comments in openfile-util.c */
    }

    if (!path || !*path)
    {
        errno = EINVAL;
        return 0; /* let glibc sort out the error */
    }
#if PVFS_USRINT_KMOUNT
    memset(&sbuf, 0, sizeof(sbuf));
    memset(&fsbuf, 0, sizeof(fsbuf));
    npsize = strnlen(path, PVFS_PATH_MAX) + 1;
    newpath = (char *)malloc(npsize);
    if (!newpath)
    {
        return 0; /* let glibc sort out the error */
    }
    strncpy(newpath, path, npsize);
    
    /* first try to stat the path */
    /* this must call standard glibc stat */
    rc = glibc_ops.stat(newpath, &sbuf);
    if (rc < 0)
    {
        int count;
        /* path doesn't exist, try removing last segment */
        for(count = strlen(newpath) - 2; count > 0; count--)
        {
            if(newpath[count] == '/')
            {
                newpath[count] = '\0';
                break;
            }
        }
        /* this must call standard glibc stat */
        rc = glibc_ops.stat(newpath, &sbuf);
        if (rc < 0)
        {
            /* can't find the path must be an error */
            free(newpath);
            return 0; /* let glibc sort out the error */
        }
    }
    /* this must call standard glibc statfs */
    rc = glibc_ops.statfs(newpath, &fsbuf);
    free(newpath);
    if(fsbuf.f_type == PVFS_FS)
    {
        return 1; /* PVFS */
    }
    else
    {
        return 0; /* not PVFS assume the kernel can handle it */
    }
/***************************************************************/
#else /* PVFS_USRINT_KMOUNT */
/***************************************************************/
    /* we might not be able to stat the file direcly
     * so we will use our resolver to look up the path
     * prefix in the mount tab files
     */
    /* this allocates a PVFS_path_s which is freed below */
    Ppath = PVFS_new_path(*path);
    if (!Ppath)
    {
        return 0; /* let glibc sort out the error */
    }
    *path = Ppath->expanded_path;
    /* we are passing in a PVFS_path_t so this won't make another */
    newpath = PVFS_qualify_path(*path);
    /* *path == extended_path this func returns extended_path so */
    /* now newpath == *path */
    rc = PVFS_util_resolve_absolute(newpath);
    if (rc < 0)
    {
        if (rc == -PVFS_ENOENT)
        {
            /* This is our first opportunity to fully expand the path by
             * calling expand_path.  In the ideal. if this succeeds we
             * won't have to do much later, just use the results.
             * Newpath doesn't actually change.
             */
            newpath = PVFS_expand_path(*path, skip_last_lookup);
            if (!newpath)
            {
                return 0; /* an error returned - let glibc deal with it */
            }
            if (!PATH_RESOLVED(Ppath))
            {
                /* if path was not resolved, not PVFS */
                return 0;
            }
            else
            {
                /* path resolved - expanded path is PVFS */
                return 1;
            }
        }
        errno = rc;
        return 0; /* an error returned - let glibc deal with it */
    }
    return 1; /* a PVFS path */
#endif /* PVFS_USRINT_KMOUNT */
}

/**
 * Split a pathname into a directory and a filename.
 * If non-null is passed as the directory or filename,
 * the field will be allocated and filled with the correct value
 *
 * A slash at the end of the path is interpreted as no filename
 * and is an error.  To parse the last dir in a path, remove this
 * trailing slash.  No filename with no directory is OK.
 *
 * WBL - New design we should never have trailing or extra slashes
 * dots or double dots so no special code to deal with them.
 */
int split_pathname( const char *path,
                    char **directory,
                    char **filename)
{
    int i = 0;
    int fnlen = 0;
    int length = 0;

    if (!path || !directory || !filename)
    {
        errno = EINVAL;
        return -1;
    }
    /* Split path into a directory and filename */
    length = strnlen(path, PVFS_PATH_MAX);
    if (length == PVFS_PATH_MAX)
    {
        errno = ENAMETOOLONG;
        return -1;
    }
    i = length - 1;
    for (; i >= 0; i--)
    {
        if (path[i] == '/')
        {
            /* parse the directory */
            *directory = (char *)malloc(i + 1);
            if (!*directory)
            {
                return -1;
            }
            memset(*directory, 0, (i + 1));
            strncpy(*directory, path, i);
            (*directory)[i] = 0;
            break;
        }
    }
    if (i == -1)
    {
        /* found no '/' path is all filename */
        *directory = NULL;
    }
    else if (i == 0)
    {
        /* special case, this is an item in the true root dir */
        /* the true root cannot be a PVFS mount point and we */
        /* would not be here if this hadn't resolved at some */
        /* point so it must be a dir, the PVFS mount point */
        /* easiest thing is put it all in the dir output and */
        /* set the EISDIR so iocommon_open will know it is */
        /* everything */
        free(*directory);
        *directory = (char *)malloc(length + 1);
        if (!*directory)
        {
            return -1;
        }
        memset(*directory, 0, (length + 1));
        strncpy(*directory, path, length);
        (*directory)[length] = 0;
        *filename = NULL;
        errno = EISDIR;
        return -1;
    }
    /* copy the filename */
    i++;
    fnlen = length - i /* - slashes*/;
    if (fnlen == 0)
    {
        filename = NULL;
        if (*directory)
        {
            errno = EISDIR;
        }
        else
        {
            errno = ENOENT;
        }
        return -1;
    }
    *filename = (char *)malloc(fnlen + 1);
    if (!*filename)
    {
        if (*directory)
        {
            free(*directory);
        }
        *directory = NULL;
        *filename = NULL;
        return -1;
    }
    memset(*filename, 0, (fnlen + 1));
    strncpy(*filename, path + i, fnlen);
    (*filename)[fnlen] = 0;
    return 0;
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=4 sts=4 sw=4 expandtab
 */
