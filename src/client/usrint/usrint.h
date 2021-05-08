/* 
 * (C) 2011 Clemson University and The University of Chicago 
 *
 * See COPYING in top-level directory.
 */

/** \file
 *  \ingroup usrint
 *
 *  PVFS2 user interface routines
 */

#include "pvfs2-config.h"

#ifndef USRINT_H
#define USRINT_H 1

#ifndef _GNU_SOURCE
# define _GNU_SOURCE 1
#endif

#ifndef _ATFILE_SOURCE
# define _ATFILE_SOURCE 1
#endif

#ifndef _LARGEFILE_SOURCE
# define _LARGEFILE_SOURCE 1
#endif

#ifndef _LARGEFILE64_SOURCE
# define _LARGEFILE64_SOURCE 1
#endif

#ifndef _REENTRANT
# define _REENTRANT 1
#endif

#ifndef _THREAD_SAFE
# define _THREAD_SAFE 1
#endif

/*
 * This seems to control redirect of 32-bit IO to 64-bit IO
 * We want to avoid this in our source
 */
#ifdef USRINT_SOURCE

#ifdef _FORTIFY_SOURCE
# undef _FORTIFY_SOURCE
#endif
# define _FORTIFY_SOURCE 0

# ifdef _FILE_OFFSET_BITS
#  undef _FILE_OFFSET_BITS 
# endif

#else

# ifndef _FILE_OFFSET_BITS
#  define _FILE_OFFSET_BITS 64
# endif

# ifdef __OPTIMIZE__
#  undef __OPTIMIZE__
# endif

# define __NO_INLINE__ 1

#endif /* USRINT_SOURCE */

/*
 * this defines __USE_LARGEFILE, __USE_LARGEFILE64, and
 * __USE_FILE_OFFSET64 which control many of the other includes
 */
#ifdef HAVE_FEATURES_H
# include <features.h>
#endif

/*
 * force this stuff off if the source requests
 * the stuff controlling inlining and def'ing of
 * functions in stdio is really mixed up and varies from
 * one generation of the headers to another.
 * I hate to whack all inlining and related stuff
 * but it seems the only reliable way to turn it off.
 * USRINT code can get this or not with the var below
 * I question that header files should be doing
 * optimization in the first place.  WBL
 */
#ifdef USRINT_SOURCE

# ifdef __USE_FILE_OFFSET64
#  undef __USE_FILE_OFFSET64
# endif

/* This seems to reappear on some systems, so whack it again */
# ifdef __OPTIMIZE__
#  undef __OPTIMIZE__
# endif

# ifdef __REDIRECT
#  undef __REDIRECT
# endif

# ifdef __USE_EXTERN_INLINES
#  undef __USE_EXTERN_INLINES
# endif

# ifdef __USE_FORTIFY_LEVEL
#  undef __USE_FORTIFY_LEVEL
# endif
# define __USE_FORTIFY_LEVEL 0

#endif /* USRINT SOURCE */

#if 0
#ifdef _IO_MTSAFE_IO
#pragma message "MTSAFE is defined"
#else
#pragma message "MTSAFE is NOT defined"
#endif
#endif

/* locking - should activate glibc pthreads locks - if not already on */
/* need a config option here to turn off thread safety - otherwise
 * assume it should be on.  This only controls locking on stdio layer.
 * Presumably much of it could be turned off for efficiency sake - maybe
 * a runtime option rather than library build time option.
 */
#if 0

# ifdef _IO_MTSAFE_IO
#  undef _IO_MTSAFE_IO
# endif

# define _IO_MTSAFE_IO 1

#endif /* 0 */

#include "pvfs2-internal.h"
#include "gossip.h"

#include <errno.h>
#include <fcntl.h>
//#define _GLIBCPP_USE_WCHAR_T 1
//#include <libio.h>
//#undef _GLIBCPP_USE_WCHAR_T
#include <stdio.h>
#ifndef _IO_FILE
#include <bits/types/__FILE.h>
#define _IO_FILE __FILE

//copied from the depracated code
#define _IO_MAGIC_MASK 0xFFFF0000
#define _IO_MAGIC 0xFBAD0000
#define _IO_NO_WRITES 8
#define _IO_NO_READS 4
#define _IO_USER_BUF 1
#define _IO_CURRENTLY_PUTTING 0x800
#define _IO_DELETE_DONT_CLOSE 0x40
struct _IO_marker {
  struct _IO_marker *_next;
  struct _IO_FILE *_sbuf;
  int _pos;
};

//source: http://man7.org/linux/man-pages/man3/setbuf.3.html
//these have the same value as before depracation of libio.h
#define _IO_UNBUFFERED _IONBF
#define _IO_LINE_BUF _IOLBF
#endif

#include <utime.h>

#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif

#ifdef HAVE_STDLIB_H
# include <stdlib.h>
#endif

#ifdef HAVE_STDARG_H
# include <assert.h>
#endif

#ifdef HAVE_STDARG_H
# include <libgen.h>
#endif

#ifdef HAVE_STDARG_H
# include <dirent.h>
#endif

#ifdef HAVE_STDARG_H
# include <string.h>
#endif

#ifdef HAVE_STDARG_H
# include <stdarg.h>
#endif

#ifdef HAVE_MEMORY_H
# include <memory.h>
#endif

#include <limits.h>

#ifdef HAVE_SYS_TYPES_H
# include <sys/types.h>
#endif

#include <sys/select.h>

#ifdef HAVE_SYS_SOCKET_H
# include <sys/socket.h>
#endif

#include <sys/resource.h>

#ifdef HAVE_SYS_SENDFILE_H
# include <sys/sendfile.h>
#endif

/* #include <sys/statvfs.h> */ /* struct statfs on OS X */
#ifdef HAVE_SYS_VFS_H
# include <sys/vfs.h> /* struct statfs on Linux */
#endif

#ifdef HAVE_SYS_STAT_H
# include <sys/stat.h>
#endif

#ifdef HAVE_SYS_STATVFS_H
# include <sys/statvfs.h>
#endif

#ifdef HAVE_SYS_UIO_H
# include <sys/uio.h>
#endif

#ifdef PVFS_HAVE_ACL_INCLUDES
# include <sys/acl.h>
# include <acl/libacl.h>
#endif

#ifdef HAVE_SYS_MMAN_H
# include <sys/mman.h>
#endif

#ifdef HAVE_SYS_TIME_H
# include <sys/time.h>
#endif

#ifdef HAVE_LINUX_TYPES_H
# include <linux/types.h>
#endif

#ifdef HAVE_SELINUX_H
# include <selinux/selinux.h>
#else
  typedef void *security_context_t;
#endif

#ifdef HAVE_ATTR_XATTR_H
# include <attr/xattr.h>
#else
# ifdef HAVE_SYS_ATTR_H
#  include <sys/xattr.h>
# else
#  define XATTR_CREATE 0x1
#  define XATTR_REPLACE 0x2
extern int setxattr(const char *path, const char *name,
                    const void *value, size_t size, int flags);
extern int lsetxattr(const char *path, const char *name,
                     const void *value, size_t size, int flags);
extern int fsetxattr(int fd, const char *name,
                     const void *value, size_t size, int flags);
extern ssize_t getxattr(const char *path, const char *name,
                    void *value, size_t size);
extern ssize_t lgetxattr(const char *path, const char *name,
                     void *value, size_t size);
extern ssize_t fgetxattr(int fd, const char *name, void *value, size_t size);
extern ssize_t atomicxattr(const char *path, const char *name,
                           void *value, size_t valsize, void *response,
                           size_t respsize, int flags, int opcode);
extern ssize_t latomicxattr(const char *path, const char *name,
                            void *value, size_t valsize, void *response,
                            size_t respsize, int flags, int opcode);
extern ssize_t fatomicxattr(int fd, const char *name,
                            void *value, size_t valsize, void *response,
                            size_t respsize, int flags, int opcode);
extern ssize_t listxattr(const char *path, char *list, size_t size);
extern ssize_t llistxattr(const char *path, char *list, size_t size);
extern ssize_t flistxattr(int fd, char *list, size_t size);
extern int removexattr(const char *path, const char *name);
extern int lremovexattr(const char *path, const char *name);
extern int fremovexattr(int fd, const char *name);
# endif /* HAVE_SYS_ATTR_H */
#endif /* HAVE_ATTR_XATTR_H */

/* #include <linux/dirent.h> diff source need diff versions */
#include <time.h>
#include <dlfcn.h>

//#include <mpi.h>

/* PVFS specific includes */
#include "pvfs2.h"
#include "pvfs2-hint.h"
#include "pvfs2-debug.h"
#include "pvfs2-types.h"
#include "pvfs2-req-proto.h"
#include "gen-locks.h"
#include "env-vars.h"

/* Just in case this is not defined - sizeof blocks reported in stat */
#ifndef S_BLKSIZE
# define S_BLKSIZE 512
#endif

/* magic numbers for PVFS filesystem */
#define PVFS_FS 537068840
#define LINUX_FS 61267

#define PVFS_FD_SUCCESS 0
#define PVFS_FD_FAILURE -1

/* Defines GNU's O_NOFOLLOW flag to be false if its not set */ 
#ifndef O_NOFOLLOW
# define O_NOFOLLOW 0
#endif

/* Define AT_FDCWD and related flags on older systems */
#ifndef AT_FDCWD
# define AT_FDCWD		-100	/* Special value used to indicate
					   the *at functions should use the
					   current working directory. */
#endif

#ifndef AT_SYMLINK_NOFOLLOW
# define AT_SYMLINK_NOFOLLOW	0x100	/* Do not follow symbolic links.  */
#endif

#ifndef AT_REMOVDIR
# define AT_REMOVEDIR		0x200	/* Remove directory instead of
					   unlinking file.  */
#endif

#ifndef AT_SYMLINK_FOLLOW
# define AT_SYMLINK_FOLLOW	0x400	/* Follow symbolic links.  */
#endif

#ifndef AT_EACCESS
# define AT_EACCESS		0x200	/* Test access permitted for
					   effective IDs, not real IDs.  */
#endif

#define true   1 
#define false  0 
#define O_HINTS     02000000  /* PVFS hints are present */
#define O_NOTPVFS   04000000  /* Turn off sym links from PVFS to non-PVFS */

/* constants for this library */
/* size of stdio default buffer - starting at 1Meg */
#define PVFS_BUFSIZE (1024*1024)
#define PVFS_SHM_MAGIC (0xfffefdfcfbfa9081)
#define PVFS_NOFILE_MAX (1024)
#define PATH_TABLE_SIZE (1024*1024)
#define FD_TABLE_SIZE (64)
#define PVFS_SHMOBJ (PVFS_NOFILE_MAX -1)

/* extra function prototypes */

extern int posix_readdir(unsigned int fd, struct dirent *dirp,
                         unsigned int count);

extern int fseek64(FILE *stream, const off64_t offset, int whence);

extern off64_t ftell64(FILE *stream);

extern int pvfs_convert_iovec(const struct iovec *vector,
                              int count,
                              PVFS_Request *req,
                              void **buf);

#if !defined(__linux__) || !defined(__GLIBC__) || \
    !(__GLIBC__ >= 3 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 9))
extern int dup3(int oldfd, int newfd, int flags);
#endif

/* MPI functions */ 
//int MPI_File_open(MPI_Comm comm, char *filename,
//                    int amode, MPI_Info info, MPI_File *mpi_fh); 
//int MPI_File_write(MPI_File mpi_fh, void *buf,
//                    int count, MPI_Datatype datatype, MPI_Status *status); 

/* Macros */

/* debugging */
#ifdef gossip_debug
# undef gossip_debug
#endif

#ifdef GOSSIP_USRINT_DEBUG
# undef GOSSIP_USRINT_DEBUG
#endif

/* locally control gossip_debug calls */
#define USRINT_DEBUG 0

#if USRINT_DEBUG
# define GOSSIP_USRINT_DEBUG stderr
# define gossip_debug fprintf
#else
# define gossip_debug(__m, __f, ...)
#endif /* USRINT_DEBUG */

#define PVFS_STDIO_DEBUG 0

/* USRINT Configuration Defines - Defaults */
/* These should be defined in pvfs2-config.h */

#ifndef PVFS_USRINT_BUILD
# define PVFS_USRINT_BUILD 1
#endif

#ifndef PVFS_USRINT_CWD
# define PVFS_USRINT_CWD 1
#endif

#ifndef PVFS_USRINT_KMOUNT
# define PVFS_USRINT_KMOUNT 0
#endif

#ifndef PVFS_UCACHE_ENABLE
# define PVFS_UCACHE_ENABLE 1
#endif

/* force redef to be the default - mostly for debugging */
#if 1
# ifdef PVFS_STDIO_REDEFSTREAM
#  undef PVFS_STDIO_REDEFSTREAM
# endif
#endif

#ifndef PVFS_STDIO_REDEFSTREAM
# define PVFS_STDIO_REDEFSTREAM 1
#endif

/* FD sets */
#if 0
# ifdef FD_SET
#  undef FD_SET
# endif
# define FD_SET(d,fdset)                 \
do {                                    \
    pvfs_descriptor *pd;                \
    pd = pvfs_find_descriptor(d);       \
    if (pd)                             \
    {                                   \
        __FD_SET(pd->true_fd,(fdset));  \
    }                                   \
} while(0)

# ifdef FD_CLR
#  undef FD_CLR
# endif
# define FD_CLR(d,fdset)                 \
do {                                    \
    pvfs_descriptor *pd;                \
    pd = pvfs_find_descriptor(d);       \
    if (pd)                             \
    {                                   \
        __FD_CLR(pd->true_fd,(fdset));  \
    }                                   \
} while(0)

# ifdef FD_ISSET
#  undef FD_ISSET
# endif
# define FD_ISSET(d,fdset)               \
do {                                    \
    pvfs_descriptor *pd;                \
    pd = pvfs_find_descriptor(d);       \
    if (pd)                             \
    {                                   \
        __FD_ISSET(pd->true_fd,(fdset));\
    }                                   \
} while(0)
#endif /* 0 */

#endif /* USRINT_H */

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */

