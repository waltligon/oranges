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
/* This prevents headers from defining sys calls as macros */
#define USRINT_SOURCE 1

#include "usrint.h"
#include <sys/syscall.h>
#include <paths.h>
#include <signal.h>
#ifndef SYS_readdir
#define SYS_readdir 89
#endif
#include "quicklist.h"
#include "posix-ops.h"
#include "openfile-util.h"
#include "iocommon.h"
#include "posix-pvfs.h"
#include "pvfs-path.h"
#ifdef PVFS_AIO_ENABLE
#include "aiocommon.h"
#endif

#if PVFS_UCACHE_ENABLE
#include "ucache.h"
#endif

/* Debugging macros for initialization when reegular IO is questionable
 */
#define PVFS_INIT_DEBUG 0
#define BUFSZ 255

#define init_printerr(f)                                 \
    do {                                                 \
        char str[BUFSZ];                                 \
        memset(str, 0, BUFSZ);                           \
        glibc_ops.snprintf(str, BUFSZ-1, f);             \
        glibc_ops.write(2, str, BUFSZ);                  \
    } while (0)

#define init_printerr1(f,v)                              \
    do {                                                 \
        char str[BUFSZ];                                 \
        memset(str, 0, BUFSZ);                           \
        glibc_ops.snprintf(str, BUFSZ-1, f, (v));        \
        glibc_ops.write(2, str, BUFSZ);                  \
    } while (0)

#define init_printerr2(f,v1,v2)                          \
    do {                                                 \
        char str[BUFSZ];                                 \
        memset(str, 0, BUFSZ);                           \
        glibc_ops.snprintf(str, BUFSZ-1, f, (v1), (v2)); \
        glibc_ops.write(2, str, BUFSZ);                  \
    } while (0)

#define init_perror(s) init_printerr2("PVFS init error: %s: %s\n", \
                                      s,                           \
                                      strerror(errno));

#if PVFS_INIT_DEBUG
# define init_debug(f) init_printerr(f)
# define init_debug1(f,v) init_printerr1(f, v)
# define init_debug2(f,v1,v2) init_printerr2(f, v1, v2)
#else
# define init_debug(f)
# define init_debug1(f,v)
# define init_debug2(f,v1,v2)
#endif

static struct glibc_redirect_s
{
    int (*stat)(int ver, const char *path, struct stat *buf);
    int (*stat64)(int ver, const char *path, struct stat64 *buf);
    int (*fstat)(int ver, int fd, struct stat *buf);
    int (*fstat64)(int ver, int fd, struct stat64 *buf);
    int (*fstatat)(int ver, int fd, const char *path, struct stat *buf, int flag);
    int (*fstatat64)(int ver, int fd, const char *path, struct stat64 *buf, int flag);
    int (*lstat)(int ver, const char *path, struct stat *buf);
    int (*lstat64)(int ver, const char *path, struct stat64 *buf);
    int (*mknod)(int ver, const char *path, mode_t mode, dev_t dev);
    int (*mknodat)(int ver, int dirfd, const char *path, mode_t mode, dev_t dev);
} glibc_redirect;

/* this is for managing space in the shared memory area - first pass we
 * allocated enough of everything we can use a trivial algorithm but we
 * may choose to go to a more sophisticated version later
 */

typedef struct pvfs_shmcontrol_s
{
    int64_t shm_magic;
    void *shmctrl;            /* pointer to this struct for reference */
    gen_mutex_t shmctrl_lock; /* these control the shm copy during a fork */
    gen_cond_t shmctrl_cond;
    uint32_t shmctrl_copy;    /* indicates a copy is going on during a fork */
    uint32_t shmctrl_shared;  /* number of descendent processes sharing this */
    char shmctrl_cwd[PVFS_PATH_MAX];  /* the current working dir */
    pvfs_descriptor **descriptor_table;
    uint32_t descriptor_table_count;  /* number of active descriptors */
    uint32_t descriptor_table_size;   /* total number of descriptors */
    gen_mutex_t descriptor_table_lock;
    pvfs_descriptor *descriptor_pool;
    uint32_t descriptor_pool_size;
    pvfs_descriptor_status *status_pool;
    uint32_t status_pool_size;
    char *path_table;
    uint32_t path_table_size;
    int *fd_table;
    uint32_t fd_table_size;
    uint32_t fd_table_count;
} pvfs_shmcontrol_t;

static pvfs_shmcontrol_t *shmctrl = NULL;
static char shmobjpath[50];
static int shmobj = -1;
static int shmsize = 0; /* size of shm control area pointed to by shmctrl */

/* This is for managing a list of the above structs.  This struct is
 * referred to as a PDL for brevity, and the term may also refer to the
 * pvfs_shmcontrol_t it points to when convenient.  THe items on this
 * list represent the shm areas of ancestor processes that we are
 * currently sharing an open file descriptor with (really a status
 * struct) and we keep a count of how many of these are still active.
 * When that count becomes zero we can disconntect from the shm area and
 * remove it from the list.  Before doing so we should check if ANY
 * other process is still shareing with it and unlink it as well if they
 * are not.
 */

typedef struct pvfs_desc_list_s
{
    char *name;             /* global name of the PDL shm object */
    pvfs_shmcontrol_t *ptr; /* pointer to the PDL object */
    int64_t offset;         /* difference between currant and prev mapping */
    int fd;                 /* fd PDL object is opened on */
    int size;               /* size of PDL object */
    int shares;             /* number of shares this process uses from PDL */
    struct qlist_head link;
} pvfs_desc_list_t;

static QLIST_HEAD(desc_list); /* list of ancestor descriptor tables */

/* convenience macro for adjusting pointers */
#define P2L(p,t) (t *)(((char *)(p)) + parent_offset)
#define PDLOFF(p,d) (((char *)(p)) + (d)->offset)

#define PREALLOC 3
static pvfs_descriptor **descriptor_table = NULL; /* convenience pointer */

/* THese are used to index descriptors, statuses and dpaths we have
 * allocated in the shm area.
 */
typedef struct index_rec_s
{
    struct qlist_head link;
    int first;
    int size; 
} index_rec_t;

static QLIST_HEAD(desc_index);  /* the descriptor pool */
static QLIST_HEAD(stat_index);  /* the descriptor status pool */
static QLIST_HEAD(path_index);  /* pool for dpaths */

static char rstate[256];  /* used for random number generation */

/* static functions */
static pvfs_descriptor *get_descriptor(void);
static void put_descriptor(pvfs_descriptor *pd);
static pvfs_descriptor_status *get_status(void);
static void put_status(pvfs_descriptor_status *pds);
static int pvfs_desc_alloc(struct qlist_head *pool);
static void pvfs_desc_free(int desc, struct qlist_head *pool);
static void path_index_return(char *path);
static char *path_index_find(int size);

/* initialization stuff */
static void rebuild_descriptor_table(pvfs_desc_list_t *pdl);
static void parent_fork_begin(void);    /* fork parent handler */
static void parent_fork_end(void);      /* fork parent handler */
static void add_descriptor_area_list(char *name,
                                     int fd,
                                     int size,
                                     int64_t offset,
                                     pvfs_shmcontrol_t *new);
static void close_descriptor_area_list(pvfs_desc_list_t *pdl);
static pvfs_desc_list_t *find_descriptor_area_list(
                                 pvfs_descriptor_status *stat);
static pvfs_desc_list_t *first_descriptor_area_list(void);
static void init_descriptor_area(void); /* fork child handler */
static void init_descriptor_area_internal(void); /* create new PDL */
/* actual initialization functions */
static int init_usrint_internal(void)
                    GCC_CONSTRUCTOR(INIT_PRIORITY_PVFSLIB);
static void cleanup_usrint_internal(void)
                    GCC_DESTRUCTOR(CLEANUP_PRIORITY_PVFSLIB);
/* static int pvfs_sys_init_elf(void) GCC_UNUSED; */
static int pvfs_lib_init_flag = 0;      /* initialization done */

posix_ops glibc_ops;

/* wrapper so we can call getpwd for initialization
 */
static int my_glibc_getcwd(char *buf, unsigned long size)
{
    return syscall(SYS_getcwd, buf, size);
}

/* wrappers for some glibc calls that that are actually redirects to
 * other calls - get confused in the tables so a function is defined
 * here
 */

static int my_glibc_stat(const char *path, struct stat *buf)
{
    int rc = glibc_redirect.stat(_STAT_VER, path, buf);
    return rc;
}

static int my_glibc_stat64(const char *path, struct stat64 *buf)
{
    int rc = glibc_redirect.stat64(_STAT_VER, path, buf);
    return rc;
}

static int my_glibc_fstat(int fd, struct stat *buf)
{
    return glibc_redirect.fstat(_STAT_VER, fd, buf);
}

static int my_glibc_fstat64(int fd, struct stat64 *buf)
{
    return glibc_redirect.fstat64(_STAT_VER, fd, buf);
}

static int my_glibc_fstatat(int fd,
                            const char *path,
                            struct stat *buf,
                            int flag)
{
    return glibc_redirect.fstatat(_STAT_VER, fd, path, buf, flag);
}

static int my_glibc_fstatat64(int fd,
                              const char *path,
                              struct stat64 *buf,
                              int flag)
{
    return glibc_redirect.fstatat64(_STAT_VER, fd, path, buf, flag);
}

static int my_glibc_lstat(const char *path, struct stat *buf)
{
    return glibc_redirect.lstat(_STAT_VER, path, buf);
}

static int my_glibc_lstat64(const char *path, struct stat64 *buf)
{
    return glibc_redirect.lstat64(_STAT_VER, path, buf);
}

static int my_glibc_mknod(const char *path, mode_t mode, dev_t dev)
{
    return glibc_redirect.mknod(_MKNOD_VER, path, mode, dev);
}

static int my_glibc_mknodat(int dirfd, const char *path, mode_t mode, dev_t dev)
{
    return glibc_redirect.mknodat(_MKNOD_VER, dirfd, path, mode, dev);
}

static int my_glibc_getdents(u_int fd, struct dirent *dirp, u_int count)
{
    #ifdef SYS_getdents
    return syscall(SYS_getdents, fd, dirp, count);
    #else
    return syscall(SYS_getdents64, fd, dirp, count);
    #endif
}

static int my_glibc_getdents64(u_int fd, struct dirent64 *dirp, u_int count)
{
    return syscall(SYS_getdents64, fd, dirp, count);
}

static int my_glibc_fadvise64(int fd, off64_t offset, off64_t len, int advice)
{
#ifdef __arm__
    return syscall(__NR_arm_fadvise64_64, fd, advice, offset, len);
#else
    return syscall(SYS_fadvise64, fd, offset, len, advice);
#endif
}

static int my_glibc_fadvise(int fd, off_t offset, off_t len, int advice)
{
    return my_glibc_fadvise64(fd, (off64_t)offset, (off64_t)len, advice);
}

static int my_glibc_readdir(u_int fd, struct dirent *dirp, u_int count)
{
    return syscall(SYS_readdir, fd, dirp, count);
}

/* This function creates a table of pointers to glibc functions for all
 * of the io system calls so they can be called when needed
 */
void load_glibc(void)
{ 
    void *libc_handle;
    libc_handle = dlopen("libc.so.6", RTLD_LAZY|RTLD_GLOBAL);
    if (!libc_handle)
    {
        /* stderr should be set up before this */
        //init_debug("Failed to open libc.so\n");
        libc_handle = RTLD_NEXT;
    }
    memset((void *)&glibc_ops, 0, sizeof(glibc_ops));
    /* first set */
    glibc_ops.snprintf = dlsym(libc_handle, "snprintf");
    glibc_ops.open = dlsym(libc_handle, "open");
    glibc_ops.open64 = dlsym(libc_handle, "open64");
    glibc_ops.openat = dlsym(libc_handle, "openat");
    glibc_ops.openat64 = dlsym(libc_handle, "openat64");
    glibc_ops.creat = dlsym(libc_handle, "creat");
    glibc_ops.creat64 = dlsym(libc_handle, "creat64");
    glibc_ops.unlink = dlsym(libc_handle, "unlink");
    glibc_ops.unlinkat = dlsym(libc_handle, "unlinkat");
    glibc_ops.rename = dlsym(libc_handle, "rename");
    glibc_ops.renameat = dlsym(libc_handle, "renameat");
    glibc_ops.read = dlsym(libc_handle, "read");
    glibc_ops.pread = dlsym(libc_handle, "pread");
    glibc_ops.readv = dlsym(libc_handle, "readv");
    glibc_ops.pread64 = dlsym(libc_handle, "pread64");
    glibc_ops.write = dlsym(libc_handle, "write");
    glibc_ops.pwrite = dlsym(libc_handle, "pwrite");
    glibc_ops.writev = dlsym(libc_handle, "writev");
    glibc_ops.pwrite64 = dlsym(libc_handle, "pwrite64");
    glibc_ops.lseek = dlsym(libc_handle, "lseek");
    glibc_ops.lseek64 = dlsym(libc_handle, "lseek64");
    glibc_ops.perror = dlsym(libc_handle, "perror");
    glibc_ops.truncate = dlsym(libc_handle, "truncate");
    glibc_ops.truncate64 = dlsym(libc_handle, "truncate64");
    glibc_ops.ftruncate = dlsym(libc_handle, "ftruncate");
    glibc_ops.ftruncate64 = dlsym(libc_handle, "ftruncate64");
    glibc_ops.fallocate = dlsym(libc_handle, "posix_fallocate");
    glibc_ops.close = dlsym(libc_handle, "close");
    /* stats */
    glibc_ops.stat = my_glibc_stat;
    glibc_redirect.stat = dlsym(libc_handle, "__xstat");
    glibc_ops.stat64 = my_glibc_stat64;
    glibc_redirect.stat64 = dlsym(libc_handle, "__xstat64");
    glibc_ops.fstat = my_glibc_fstat;
    glibc_redirect.fstat = dlsym(libc_handle, "__fxstat");
    glibc_ops.fstat64 = my_glibc_fstat64;
    glibc_redirect.fstat64 = dlsym(libc_handle, "__fxstat64");
    glibc_ops.fstatat = my_glibc_fstatat;
    glibc_redirect.fstatat = dlsym(libc_handle, "__fxstatat");
    glibc_ops.fstatat64 = my_glibc_fstatat64;
    glibc_redirect.fstatat64 = dlsym(libc_handle, "__fxstatat64");
    glibc_ops.lstat = my_glibc_lstat;
    glibc_redirect.lstat = dlsym(libc_handle, "__lxstat");
    glibc_ops.lstat64 = my_glibc_lstat64;
    glibc_redirect.lstat64 = dlsym(libc_handle, "__lxstat64");
    /* times dups chowns mks */
    glibc_ops.futimesat = dlsym(libc_handle, "futimesat");
    glibc_ops.utimes = dlsym(libc_handle, "utimes");
    glibc_ops.utime = dlsym(libc_handle, "utime");
    glibc_ops.futimes = dlsym(libc_handle, "futimes");
    glibc_ops.dup = dlsym(libc_handle, "dup");
    glibc_ops.dup2 = dlsym(libc_handle, "dup2");
    glibc_ops.dup3 = dlsym(libc_handle, "dup3");
    glibc_ops.chown = dlsym(libc_handle, "chown");
    glibc_ops.fchown = dlsym(libc_handle, "fchown");
    glibc_ops.fchownat = dlsym(libc_handle, "fchownat");
    glibc_ops.lchown = dlsym(libc_handle, "lchown");
    glibc_ops.chmod = dlsym(libc_handle, "chmod");
    glibc_ops.fchmod = dlsym(libc_handle, "fchmod");
    glibc_ops.fchmodat = dlsym(libc_handle, "fchmodat");
    glibc_ops.mkdir = dlsym(libc_handle, "mkdir");
    glibc_ops.mkdirat = dlsym(libc_handle, "mkdirat");
    glibc_ops.rmdir = dlsym(libc_handle, "rmdir");
    glibc_ops.readlink = dlsym(libc_handle, "readlink");
    glibc_ops.readlinkat = dlsym(libc_handle, "readlinkat");
    glibc_ops.symlink = dlsym(libc_handle, "symlink");
    glibc_ops.symlinkat = dlsym(libc_handle, "symlinkat");
    glibc_ops.link = dlsym(libc_handle, "link");
    glibc_ops.linkat = dlsym(libc_handle, "linkat");
    /* readdirs and misc */
    glibc_ops.readdir = my_glibc_readdir;
    glibc_ops.getdents = my_glibc_getdents;
    glibc_ops.getdents64 = my_glibc_getdents64;
    glibc_ops.access = dlsym(libc_handle, "access");
    glibc_ops.faccessat = dlsym(libc_handle, "faccessat");
    glibc_ops.flock = dlsym(libc_handle, "flock");
    glibc_ops.fcntl = dlsym(libc_handle, "fcntl");
    glibc_ops.sync = dlsym(libc_handle, "sync");
    glibc_ops.fsync = dlsym(libc_handle, "fsync");
    glibc_ops.fdatasync = dlsym(libc_handle, "fdatasync");
    glibc_ops.fadvise = my_glibc_fadvise;
    glibc_ops.fadvise64 = my_glibc_fadvise64;
    glibc_ops.statfs = dlsym(libc_handle, "statfs");
    glibc_ops.statfs64 = dlsym(libc_handle, "statfs64");
    glibc_ops.fstatfs = dlsym(libc_handle, "fstatfs");
    glibc_ops.fstatfs64 = dlsym(libc_handle, "fstatfs64");
    glibc_ops.statvfs = dlsym(libc_handle, "statvfs");
    glibc_ops.fstatvfs = dlsym(libc_handle, "fstatvfs");
    glibc_ops.mknod = my_glibc_mknod;
    glibc_redirect.mknod = dlsym(libc_handle, "__xmknod");
    glibc_ops.mknodat = my_glibc_mknodat;
    glibc_redirect.mknodat = dlsym(libc_handle, "__xmknodat");
    glibc_ops.sendfile = dlsym(libc_handle, "sendfile");
    glibc_ops.sendfile64 = dlsym(libc_handle, "sendfile64");
#ifdef HAVE_ATTR_XATTR_H
    glibc_ops.setxattr = dlsym(libc_handle, "setxattr");
    glibc_ops.lsetxattr = dlsym(libc_handle, "lsetxattr");
    glibc_ops.fsetxattr = dlsym(libc_handle, "fsetxattr");
    glibc_ops.getxattr = dlsym(libc_handle, "getxattr");
    glibc_ops.lgetxattr = dlsym(libc_handle, "lgetxattr");
    glibc_ops.fgetxattr = dlsym(libc_handle, "fgetxattr");
    glibc_ops.listxattr = dlsym(libc_handle, "listxattr");
    glibc_ops.llistxattr = dlsym(libc_handle, "llistxattr");
    glibc_ops.flistxattr = dlsym(libc_handle, "flistxattr");
    glibc_ops.removexattr = dlsym(libc_handle, "removexattr");
    glibc_ops.lremovexattr = dlsym(libc_handle, "lremovexattr");
    glibc_ops.fremovexattr = dlsym(libc_handle, "fremovexattr");
#endif
    glibc_ops.socket = dlsym(libc_handle, "socket");
    glibc_ops.accept = dlsym(libc_handle, "accept");
    glibc_ops.bind = dlsym(libc_handle, "bind");
    glibc_ops.connect = dlsym(libc_handle, "connect");
    glibc_ops.getpeername = dlsym(libc_handle, "getpeername");
    glibc_ops.getsockname = dlsym(libc_handle, "getsockname");
    glibc_ops.getsockopt = dlsym(libc_handle, "getsockopt");
    glibc_ops.setsockopt = dlsym(libc_handle, "setsockopt");
    glibc_ops.ioctl = dlsym(libc_handle, "ioctl");
    glibc_ops.listen = dlsym(libc_handle, "listen");
    glibc_ops.recv = dlsym(libc_handle, "recv");
    glibc_ops.recvfrom = dlsym(libc_handle, "recvfrom");
    glibc_ops.recvmsg = dlsym(libc_handle, "recvmsg");
    glibc_ops.send = dlsym(libc_handle, "send");
    glibc_ops.sendto = dlsym(libc_handle, "sendto");
    glibc_ops.sendmsg = dlsym(libc_handle, "sendmsg");
    glibc_ops.shutdown = dlsym(libc_handle, "shutdown");
    glibc_ops.socketpair = dlsym(libc_handle, "socketpair");
    glibc_ops.pipe = dlsym(libc_handle, "pipe");
    glibc_ops.umask = dlsym(libc_handle, "umask");
    glibc_ops.getumask = dlsym(libc_handle, "getumask");
    glibc_ops.getdtablesize = dlsym(libc_handle, "getdtablesize");
    glibc_ops.mmap = dlsym(libc_handle, "mmap");
    glibc_ops.munmap = dlsym(libc_handle, "munmap");
    glibc_ops.msync = dlsym(libc_handle, "msync");
#if 0
    /* might need these some day */
    glibc_ops.acl_delete_def_file = dlsym(libc_handle, "acl_delete_def_file");
    glibc_ops.acl_get_fd = dlsym(libc_handle, "acl_get_fd");
    glibc_ops.acl_get_file = dlsym(libc_handle, "acl_get_file");
    glibc_ops.acl_set_fd = dlsym(libc_handle, "acl_set_fd");
    glibc_ops.acl_set_file = dlsym(libc_handle, "acl_set_file");
#endif
    glibc_ops.getfscreatecon = dlsym(libc_handle, "getfscreatecon");
    glibc_ops.getfilecon = dlsym(libc_handle, "getfilecon");
    glibc_ops.lgetfilecon = dlsym(libc_handle, "lgetfilecon");
    glibc_ops.fgetfilecon = dlsym(libc_handle, "fgetfilecon");
    glibc_ops.setfscreatecon = dlsym(libc_handle, "setfscreatecon");
    glibc_ops.setfilecon = dlsym(libc_handle, "setfilecon");
    glibc_ops.lsetfilecon = dlsym(libc_handle, "lsetfilecon");
    glibc_ops.fsetfilecon = dlsym(libc_handle, "fsetfilecon");

    /* PVFS does not implement socket ops */
    pvfs_ops.socket = dlsym(libc_handle, "socket");
    pvfs_ops.accept = dlsym(libc_handle, "accept");
    pvfs_ops.bind = dlsym(libc_handle, "bind");
    pvfs_ops.connect = dlsym(libc_handle, "connect");
    pvfs_ops.getpeername = dlsym(libc_handle, "getpeername");
    pvfs_ops.getsockname = dlsym(libc_handle, "getsockname");
    pvfs_ops.getsockopt = dlsym(libc_handle, "getsockopt");
    pvfs_ops.setsockopt = dlsym(libc_handle, "setsockopt");
    pvfs_ops.ioctl = dlsym(libc_handle, "ioctl");
    pvfs_ops.listen = dlsym(libc_handle, "listen");
    pvfs_ops.recv = dlsym(libc_handle, "recv");
    pvfs_ops.recvfrom = dlsym(libc_handle, "recvfrom");
    pvfs_ops.recvmsg = dlsym(libc_handle, "recvmsg");
    pvfs_ops.send = dlsym(libc_handle, "send");
    pvfs_ops.sendto = dlsym(libc_handle, "sendto");
    pvfs_ops.sendmsg = dlsym(libc_handle, "sendmsg");
    pvfs_ops.shutdown = dlsym(libc_handle, "shutdown");
    pvfs_ops.socketpair = dlsym(libc_handle, "socketpair");
    pvfs_ops.pipe = dlsym(libc_handle, "pipe");

    /* should have been previously opened */
    /* this decrements the reference count */
    if (libc_handle != RTLD_NEXT)
    {
        dlclose(libc_handle);
    }
}

/* These routines manage free lists for descriptors,
     * descriptor_libc_handle,
 * and dpath strings all of which make up the parts of a pvfs file
 * descriptor.  These are kept in a shared memory segment and thus we
 * can't malloc and free them.
 */
static void pvfs_desc_free(int desc, struct qlist_head *pool)
{
    index_rec_t *seg = NULL;
    index_rec_t *prev = NULL;
    index_rec_t *newseg = NULL;
    int merged = 0;
    int at_end = 0;

    /* FIXME: this should depend on which pool we are looking at */
    if (desc < 0 || desc >= shmctrl->descriptor_pool_size)
    {
        gossip_lerr(" returned descriptor not in table range\n");
    }
    /* check for empty list */
    if (qlist_empty(pool))
    {
        /* free list is empty */
        newseg = (index_rec_t *)malloc(sizeof(index_rec_t));
        newseg->first = desc;
        newseg->size = 1;
        qlist_add_tail(&newseg->link, pool);
    }
    /* find the segment to insert the path before */
    qlist_for_each_entry (seg, pool, link)
    {
        if (seg->first > desc)
        {
            break;
        }
    }
    if (&(seg->link) == pool)
    {
        /* we got to the end of the list */
        at_end = 1;
    }
    /* if the segment is not the first on the list */
    if (seg->link.prev != pool)
    {
        prev = qlist_entry(seg->link.prev, index_rec_t, link);
    }
    /* if path is adjacent to the segment merge */
    if (!at_end && (desc == (seg->first - 1)))
    {
        seg->first -= 1;
        seg->size += 1;
        merged = 1;
    }
    /* segment not the first and path is adjacent to previous merge */
    if (prev && ((prev->first + prev->size) == desc))
    {
        /* if not already merged with segment merge with previous */
        if (!merged)
        {
            prev->size += 1;
        }
        /* otherwise a 3 way merge */
        else
        {
            prev->size += seg->size;
            qlist_del(&seg->link);
            free(seg);
        }
        merged = 1;
    }
    /* if no merging then insert the path */
    if (!merged)
    {
        newseg = (index_rec_t *)malloc(sizeof(index_rec_t));
        newseg->first = desc;
        newseg->size = 1;
        /* strictly speaking if at_end, &seg->link == pool */
        if (at_end)
        {
            qlist_add_tail(&newseg->link, pool);
        }
        else
        {
            qlist_add_tail(&newseg->link, &seg->link);
        }
    }
}

static int pvfs_desc_alloc(struct qlist_head *pool)
{
    int rval;
    index_rec_t *seg;
    if (qlist_empty(pool))
    {
        /* free list is empty */
        gossip_lerr("pvfs_desc_alloc sees empty free list\n");
        return -1;
    }
    seg = qlist_entry(pool->next, index_rec_t, link);
    if (seg->size > 1)
    {
        /* split the block */
        rval = seg->first;
        seg->first += 1;
        seg->size -= 1;
        return rval;
    }
    else
    {
        /* remove the block */
        qlist_del(&seg->link);
        rval = seg->first;
        free(seg);
        return rval;
    }
}

static void path_index_return(char *path)
{
    index_rec_t *seg = NULL;
    index_rec_t *prev = NULL;
    index_rec_t *newseg = NULL;
    int merged = 0;
    int size = 0;
    int offset;

    if (path < shmctrl->path_table ||
        path > shmctrl->path_table + shmctrl->path_table_size)
    {
        gossip_lerr(" returned pointer not in path table range\n");
    }

    size = strlen(path) + 1;
    memset(path, 0, size);
    offset = path - shmctrl->path_table;
    /* check for empty list */
    if (path_index.next == &path_index)
    {
        /* free list is empty */
        gossip_lerr("path_index_find sees empty free list\n");
    }
    /* find the segment to insert the path before */
    qlist_for_each_entry (seg, &path_index, link)
    {
        if (seg->first > offset)
        {
            break;
        }
    }
    if (&seg->link == &path_index)
    {
        /* we got to the end of the list */
    }
    /* if the segment is not the first on the list */
    if (seg->link.prev != &path_index)
    {
        prev = qlist_entry(seg->link.prev, index_rec_t, link);
    }
    /* if path is adjacent to the segment merge */
    if (offset + size == seg->first)
    {
        seg->first = offset;
        seg->size += size;
        merged = 1;
    }
    /* segment not the first and path is adjacent to previous merge */
    if (prev && prev->first + prev->size == offset)
    {
        /* if not already merged with segment merge with previous */
        if (!merged)
        {
            prev->size += size;
        }
        /* otherwise a 3 way merge */
        else
        {
            prev->size += seg->size;
            qlist_del(&seg->link);
            free(seg);
        }
        merged = 1;
    }
    /* if no merging then insert the path */
    if (!merged)
    {
        newseg = (index_rec_t *)malloc(sizeof(index_rec_t));
        newseg->first = offset;
        newseg->size = size;
        qlist_add_tail(&newseg->link, &seg->link);
    }
}

static char *path_index_find(int size)
{
    char *rval;
    index_rec_t *seg;
    if (path_index.next == &path_index)
    {
        /* free list is empty */
        gossip_lerr("path_index_find sees empty free list\n");
        return NULL;
    }
    qlist_for_each_entry (seg, &path_index, link)
    {
        if (seg->size > size)
        {
            /* split the block */
            rval = seg->first + shmctrl->path_table;
            seg->first += size;
            seg->size -= size;
            return rval;
        }
        else if (seg->size == size)
        {
            /* remove the block */
            qlist_del(&seg->link);
            rval = seg->first + shmctrl->path_table;
            free(seg);
            return rval;
        }
    }
    /* exited the loop without finding a block */
    errno = ENOMEM;
    return NULL;
}

char *pvfs_dpath_insert(const char *path)
{
    int len = 0;
    char *ixpath;
    /* count bytes in path */
    len = strnlen(path, PVFS_PATH_MAX);
    /* add space for terminator and ref ptr */
    len += 1;
    /* find space in the index */
    ixpath = path_index_find(len);
    /* copy path */
    memcpy(ixpath, path, len);
    return ixpath;
}

void pvfs_dpath_remove(char *path)
{
    path_index_return(path);
}

/*
 * runs on exit to do any cleanup
 */
static void cleanup_usrint_internal(void)
{
    struct qlist_head *qh, *qh_next;

    init_debug("Running cleanup\n");
    /* cache cleanup */
#if PVFS_UCACHE_ENABLE
    /* put code here to flush and/or close files */
#endif
    /* shut down PVFS */
    PVFS_sys_finalize();
    /* run down the list and close all shm areas */
    qlist_for_each_safe(qh, qh_next, &desc_list)
    {
        close_descriptor_area_list(qlist_entry(qh, pvfs_desc_list_t, link));
    }
    /* unlink our area - remains until all others have unmapped */
    if (shmctrl)
    {
        glibc_ops.munmap(shmctrl, shmsize);
    }
    if (shmobj >= 0)
    {
        glibc_ops.close(shmobj);
    }
    if (strnlen(shmobjpath, sizeof(shmobjpath)))
    {
        glibc_ops.unlink(shmobjpath);
    }
    /* clear globals */
    shmobj = -1;
    shmctrl = NULL;
    descriptor_table = NULL;
    memset(shmobjpath, 0, sizeof(shmobjpath));
}

#if PVFS_UCACHE_ENABLE
/*
 * access function to see if cache is currently enabled
 * only used by code ouside of this module
 */
int pvfs_ucache_enabled(void)
{
    return ucache_enabled;
}
#endif

/*
 * On GCC we can make init_usrint_internal() a constructor so it runs
 * before main.  In this case, if there is a attempt to use one of our
 * system calls we can assume it is for a non-pvfs file (this can be
 * checked with pvfs_sys_init() which just checks the flag.  If we do
 * not have constructors its hard to tell so we just do the init and
 * hope for the best.
 *
 * If we later decide to put shared libraries on PVFS this will break
 * this assumption.  The idea here is to limit the checking over and
 * over again if we have initialized since we can't rely on the user to
 * do it and there are so many entry points into the library.  Obviously
 * some day someone will want to put shared libes on PVFS and we'll have
 * to work that out.
 *
 * Addendum - as of 4/1/2013 the GCC constructor mechanism does not
 * guarantee that a module is initialized before another module can call
 * a function in the first module.  
 */

#if 0
/* This function inserts a call during module loading on an ELF system
 * It is fragile and not portable but may give better results than the
 * GCC constructor/destructor mechanism which calls constructors some
 * time after loading modules and before main.  In particular, the GCC
 * construct does not guarantee that a function in the module will not
 * be called by another module before initialization.
 */
static int pvfs_sys_init_elf(void)
{
    __asm__ (".section .init \n call pvfs_sys_init_elf \n .section .text\n");
    pvfs_sys_init_internal();
    return 1;
} 
#endif

/* This is the function called by various routines to be SURE
 * initialization has happened.  This is usually done via the PVFS_INIT
 * macro which can be used to compile these calls out.  The idea is that
 * calls prior to initialization should not call PVFS path's so - but
 * this is not always the case.  Currently this is forced to run
 * initialization because we cannot control when our constructor runs
 * relative to the libc (and possibly others) constructor.
 */
int pvfs_sys_init(void)
{
    if (!pvfs_lib_init_flag)
    {
        return init_usrint_internal();
    }
    return 0;
}

/* 
 * This is the initialization routine - sets up the usrlib as
 * well as initiating the pvfslib including BMI, etc.
 */
static int init_usrint_internal(void)
{
    /* global var pvfs_lib_init_flag: initialization done */
    static int pvfs_initializing_flag = 0;    /* initialization in progress */
    int errno_in = 0;
    index_rec_t *ixseg = NULL;
    int rc = 0;
    int i;

    /* The recursive mutex */
    static gen_mutex_t rec_mutex = GEN_RECURSIVE_MUTEX_INITIALIZER_NP; 

    /* only one initialize happens */
    if(pvfs_lib_init_flag)
    {
        return 0;
    }

    /* preserve errno */
    errno_in = errno;

    /* make sure no double inits */
    gen_mutex_lock(&rec_mutex);
    if(pvfs_lib_init_flag || pvfs_initializing_flag)
    {
        gen_mutex_unlock(&rec_mutex);
        /* make sure errors in here don't carry out */
        errno = errno_in;
        return 1;
    }

    /* set this to prevent pvfs_sys_init from running recursively (indirect) */
    pvfs_initializing_flag = 1;

    /**************************************/
    /*       BEGIN INITIALIZATION         */
    /**************************************/

#if PVFS_USER_ENV_VARS_ENABLED
    env_vars_struct_initialize(&env_vars);
#endif /* USER_ENVIRONMENT_VARIABLES */

    /* this allows system calls to be run */
    /* init_glibc_malloc(); */
    load_glibc();

    init_debug("Beginning pvfs_sys_init_doit\n");

    PINT_initrand();

    /* if this fails not much we can do about it */
    /* atexit(cleanup_usrint_internal); */

    /* we assume if we are running this code this program was
     * just exec'd and the parent may or may not have been PVFS enabled
     *
     * first check for existing shm area - it should be on the magic fd
     * PVFS_SHMOBJ, otherwise we assume the paretn was not PVFS enabled
     * and we will have to create a new one from scratch.
     */

    if (glibc_ops.fcntl(PVFS_SHMOBJ, F_GETFL) >= 0)
    {
        struct stat sbuf;
        int *fd_table;

        /* we found something on the magic fd */
        glibc_ops.fstat(PVFS_SHMOBJ, &sbuf);
        shmsize = sbuf.st_size;

        /* map shm area */
        shmctrl = (pvfs_shmcontrol_t *)glibc_ops.mmap(NULL,
                                                     shmsize,
                                                     PROT_READ | PROT_WRITE,
                                                     MAP_SHARED,
                                                     PVFS_SHMOBJ,
                                                     0);
        if (shmctrl == ((void *)-1))
        {
            glibc_ops.perror("failed to map parent descriptor table");
            exit(-1);
        }
        /* managed to map the object now look for a magic number */
        if (shmctrl->shm_magic == PVFS_SHM_MAGIC)
        {
            /* we found it! */
            shmobj = PVFS_SHMOBJ;
            /* reconstruct the object name */
            memset(shmobjpath, 0, sizeof(shmobjpath));
            snprintf(shmobjpath,
                     50,
                     _PATH_DEV "shm/pvfs-%06d-%06d",
                     (int)getuid(),
                     (int)getpid());
        }
        else
        {
            /* something else is on the magic fd, will is bad news */
            glibc_ops.munmap(shmctrl, shmsize);
            glibc_ops.perror("something occupies the PVFS fd");
            exit(-1);
        }

        /* pointers in the parent not relocated yet */
        fd_table = (int *)((char *)shmctrl->fd_table +
                        ((char *)shmctrl - (char *)shmctrl->shmctrl));

        /* now load any shared PDLs that might be open */
        for (i = 0; i < shmctrl->fd_table_count; i++)
        {
            int psize;
            int fd;
            pvfs_shmcontrol_t *objptr;
            fd = fd_table[i];
            if (glibc_ops.fcntl(fd, F_GETFL) >= 0)
            {
                struct stat sbuf;
                glibc_ops.fstat(fd, &sbuf);
                psize = sbuf.st_size;

                /* map shm area */
                objptr = (pvfs_shmcontrol_t *)glibc_ops.mmap(NULL,
                                                             psize,
                                                             PROT_READ |
                                                                   PROT_WRITE,
                                                             MAP_SHARED,
                                                             fd,
                                                             0);
                if (objptr == ((void *)-1))
                {
                    glibc_ops.perror("failed to map ancestor descriptor table");
                    continue;
                }
                /* managed to map the object now look for a magic number */
                if (objptr->shm_magic == PVFS_SHM_MAGIC)
                {
                    add_descriptor_area_list(NULL,
                                             fd,
                                             psize,
                                             (char *)objptr -
                                                     (char *)objptr->shmctrl,
                                             objptr);
                }
                else
                {
                    glibc_ops.perror("found non descriptor table");
                    glibc_ops.munmap(objptr, psize);
                    continue;
                }
            }
            else
            {
                glibc_ops.perror("non-open object found");
                continue;
            }
        }
        
        /* add our new PDL to the list */
        add_descriptor_area_list(shmobjpath,
                                 shmobj,
                                 shmsize,
                                 (char *)shmctrl - (char *)shmctrl->shmctrl,
                                 shmctrl);

        /* and adjust pointers */
        rebuild_descriptor_table(first_descriptor_area_list());

        /* should be ready to go */
    }
    else
    {
        int flags;
        struct stat sbuf;
        int plen;
        char buf[PVFS_PATH_MAX];

        /* shmobj < 0 so we did not find the existing object */
        /* We assume this process is the first to run PVFS */
        /* so set up a new shared memory descriptor area and initialize */
        init_descriptor_area_internal();

        /* add new PDL to the list */
        add_descriptor_area_list(shmobjpath, shmobj, shmsize, 0L, shmctrl);

        /* set up the CWD */
        memset(buf, 0, PVFS_PATH_MAX);
        rc = my_glibc_getcwd(buf, PVFS_PATH_MAX);
        if (rc < 0)
        {
            /* error */
        }
        plen = strnlen(buf, PVFS_PATH_MAX);
        pvfs_put_cwd(buf, plen);

        /* on exec the free lists, if they existed, were wiped.  If we found
         * the old shm area then we will rebuild the indices in
         * rebuild_descriptor_table, if we are starting clean we will need a
         * set of clean indices.
         */

        /* set up descriptor index free list */
        ixseg = (index_rec_t *)malloc(sizeof(index_rec_t));
        if (!ixseg)
        {
            glibc_ops.perror("failed to malloc desc index pool");
            exit(-1);
        }
        ixseg->first = 0;
        ixseg->size = shmctrl->descriptor_pool_size;
        qlist_add(&ixseg->link, &desc_index);
    
        /* set up desc status index free list */
        ixseg = (index_rec_t *)malloc(sizeof(index_rec_t));
        if (!ixseg)
        {
            glibc_ops.perror("failed to malloc stat index pool");
            exit(-1);
        }
        ixseg->first = 0;
        ixseg->size = shmctrl->status_pool_size;
        qlist_add(&ixseg->link, &stat_index);

        /* set up path index free list */
        ixseg = (index_rec_t *)malloc(sizeof(index_rec_t));
        if (!ixseg)
        {
            glibc_ops.perror("failed to malloc path index pool");
            exit(-1);
        }
        ixseg->first = 0;
        ixseg->size = shmctrl->path_table_size;
        qlist_add(&ixseg->link, &path_index);

        /* init default descriptors if they are in fact open */
        flags = glibc_ops.fcntl(STDIN_FILENO, F_GETFL);
        if (flags != -1)
        {
            pvfs_alloc_descriptor(&glibc_ops, STDIN_FILENO, NULL, 0);
            descriptor_table[STDIN_FILENO]->s->flags = flags;
            glibc_ops.fstat(STDIN_FILENO, &sbuf);
            descriptor_table[STDIN_FILENO]->s->mode = sbuf.st_mode;
            gen_mutex_unlock(&descriptor_table[STDIN_FILENO]->s->lock);
            gen_mutex_unlock(&descriptor_table[STDIN_FILENO]->lock);
        }

        flags = glibc_ops.fcntl(STDOUT_FILENO, F_GETFL);
        if (flags != -1)
        {
            pvfs_alloc_descriptor(&glibc_ops, STDOUT_FILENO, NULL, 0);
            descriptor_table[STDOUT_FILENO]->s->flags = flags;
            glibc_ops.fstat(STDOUT_FILENO, &sbuf);
            descriptor_table[STDOUT_FILENO]->s->mode = sbuf.st_mode;
            gen_mutex_unlock(&descriptor_table[STDOUT_FILENO]->s->lock);
            gen_mutex_unlock(&descriptor_table[STDOUT_FILENO]->lock);
        }

        flags = glibc_ops.fcntl(STDERR_FILENO, F_GETFL);
        if (flags != -1)
        {
            pvfs_alloc_descriptor(&glibc_ops, STDERR_FILENO, NULL, 0);
            descriptor_table[STDERR_FILENO]->s->flags = flags;
            glibc_ops.fstat(STDERR_FILENO, &sbuf);
            descriptor_table[STDERR_FILENO]->s->mode = sbuf.st_mode;
            gen_mutex_unlock(&descriptor_table[STDERR_FILENO]->s->lock);
            gen_mutex_unlock(&descriptor_table[STDERR_FILENO]->lock);
        }
        /* now ready to go */
    }
    /* set up fork handlers for shared memory area - not sure this is
     * the best place to do it
     */
    pthread_atfork(parent_fork_begin, parent_fork_end, init_descriptor_area);

	/* initalize PVFS */ 
    /* this is very complex so most stuff needs to work
     * before we do this
     */
	PVFS_util_init_defaults(); 
    if (errno == EINPROGRESS)
    {
        errno = 0;
    }

    /* set up current working dir - need this one and previous one */
    // pvfs_cwd_init(1); /* expand sym links*/

    /* call other initialization routines */

    /* lib calls should never print user error messages */
    /* user can do that with return codes */
    PVFS_perror_gossip_silent();

#if PVFS_UCACHE_ENABLE
#if 0
    gossip_enable_file(UCACHE_LOG_FILE, "a");
    gossip_enable_stderr();
#endif

    /* ucache initialization - assumes shared memory previously 
     * acquired (using ucache daemon) 
     */
    rc = ucache_initialize();
    if (rc < 0)
    {
        /* ucache failed to initialize, so continue without it */
        /* Write a warning message in the ucache.log letting programmer know */
        ucache_enabled = 0;

        /* Enable the writing of the error message and */
        /* write the message to file. */
        /* gossip_set_debug_mask(1, GOSSIP_UCACHE_DEBUG);*/
        /* gossip_debug(GOSSIP_UCACHE_DEBUG, */
        /*    "WARNING: client caching configured enabled but couldn't "
         *    "inizialize\n");*/

    }
#endif

#ifdef PVFS_AIO_ENABLE
   /* initialize aio interface */
   rc = aiocommon_init();
   if (rc < 0)
   {
      gossip_debug(GOSSIP_CLIENT_DEBUG,
                   "PVFS AIO interface failed to initialize\n");
   }
#endif

    /* env_vars_struct_dump(&env_vars); */

    init_debug("finished with initialization\n");

    pvfs_lib_init_flag = 1;
    pvfs_initializing_flag = 0;
    gen_mutex_unlock(&rec_mutex);
    /* make sure errors in here don't carry out */
    errno = errno_in;
    return 0;
}

static void parent_fork_begin(void)
{
    init_debug("Parent preparing to fork\n");
    gen_mutex_lock(&shmctrl->shmctrl_lock);
    shmctrl->shmctrl_copy = 1;
    gen_mutex_unlock(&shmctrl->shmctrl_lock);
}

static void parent_fork_end(void)
{
    gen_mutex_lock(&shmctrl->shmctrl_lock);
    while (shmctrl->shmctrl_copy)
    {
        gen_cond_wait(&shmctrl->shmctrl_cond, &shmctrl->shmctrl_lock);
        init_debug1("Parent wakes flag = %d\n", shmctrl->shmctrl_copy);
    }
    gen_mutex_unlock(&shmctrl->shmctrl_lock);
    init_debug("Parent completes fork\n");
}

/* These functions manage a list of ancestor shared memory areas (PDLs)
 * so we can share descriptor status with ancestor processes.
 */

/* add a PDL to the list */
static void add_descriptor_area_list(char *newname,
                                     int newfd,
                                     int newsize,
                                     int64_t offset,
                                     pvfs_shmcontrol_t *new)
{
    pvfs_desc_list_t *pdl;
    int name_size;

    pdl = (pvfs_desc_list_t *)malloc(sizeof(pvfs_desc_list_t));
    if (!pdl)
    {
        glibc_ops.perror("Ran out of memory allocating a PDL");
    }
    if (newname)
    {
        name_size = strlen(newname);
        if (name_size > 0)
        {
            pdl->name = (char *)malloc(name_size + 1);
            memcpy(pdl->name, newname, name_size);
        }
    }
    pdl->ptr = new;
    pdl->offset = offset;
    pdl->size = newsize;
    pdl->fd = newfd;
    pdl->shares = 0; /* indicates how many items in the desc are shared */
    pdl->ptr->shmctrl_shared++; /* indicates we are sharing this desc */
    qlist_add(&pdl->link, &desc_list);
}


/* close and possbily delete a PDL on the list */
static void close_descriptor_area_list(pvfs_desc_list_t *pdl)
{
    int rc = 0;

    rc = glibc_ops.close(pdl->fd);
    if (rc < 0)
    {
        glibc_ops.perror("error closing a PDL\n");
    }
    pdl->fd = -1;
    rc = glibc_ops.munmap(pdl->ptr, pdl->size);
    if (rc < 0)
    {
        glibc_ops.perror("error unmapping a PDL\n");
    }
    pdl->size = 0;
    pdl->ptr = NULL;
    if (pdl->name)
    {
        free(pdl->name);
        pdl->name = NULL;
    }
    pdl->name = NULL;
    qlist_del(&pdl->link);
    free(pdl);
}

/* find a PDL on the list */
static pvfs_desc_list_t *find_descriptor_area_list(
                                  pvfs_descriptor_status *stat)
{
    pvfs_desc_list_t *pdl;
    qlist_for_each_entry(pdl, &desc_list, link)
    {
        int64_t diff = PDLOFF(stat, pdl) - PDLOFF(pdl->ptr->status_pool, pdl);
        if (diff >= 0 && diff < pdl->ptr->status_pool_size)
        {
            return pdl;
        }
    }
    init_debug("Desc list item to find not found\n");
    return NULL;
}

/* return self PDL */
static pvfs_desc_list_t *first_descriptor_area_list(void)
{
    return qlist_entry(desc_list.next, pvfs_desc_list_t, link);
}

/* This function is called when a process has just been fork()'d .
 * It's job is to copy the parent's descriptor area and set it up.  The
 * actual kernel descriptor table is handled by the kernel of course.
 * If the process calls exec() it will perform a similar copy but only
 * of files that are supposed to remain open (not having the CLOEXEC
 * flag).
 */
static void init_descriptor_area(void)
{
    int64_t parent_offset = 0; /* memory offset between parent and new desc */
    int d = 0;
    int dtable_size = 0;   /* number of slots in descriptor table */
    int dtable_count = 0;  /* number of used slots in descriptor table */
    int parentobj;
    pvfs_shmcontrol_t *parentctl;
    int parentsize GCC_UNUSED;
    pvfs_desc_list_t *pdl;

    init_debug("Forked, running init descriptor area\n");

    /* we just forked and we are running this so by definition we should
     * already have a descriptor table and we should have our parent's
     * desciptor table mmaped.  first verify this then move the existing
     * one to the parent pointers
     */

    if (shmobj == -1 || shmctrl == NULL)
    {
        /* something is wrong */
        gossip_lerr("just forked but don't have a descriptor table\n");
        return;
    }

    /* This pdl was our parent's - save pointers to it before we create
     * our new one.
     */
    pdl = first_descriptor_area_list();

    parentctl = pdl->ptr;
    parentsize = pdl->size;

    /* move parent pdl off of the magic fd */
    parentobj = glibc_ops.dup(pdl->fd);
    glibc_ops.close(pdl->fd);
    pdl->fd = parentobj;

    /* get ready to create new shm area */
    shmobj = -1;
    shmctrl = NULL;
    shmsize = 0;
    memset(shmobjpath, 0, sizeof(shmobjpath));

    /* Next we need to initialize the new shared memory space */
    init_descriptor_area_internal();

    /* add new descriptor table to list of known desc tables */
    add_descriptor_area_list(shmobjpath, shmobj, shmsize, 0L,shmctrl);

    /* The indexes for dpath, desc, and status should already exist and
     * all linux fds should aready be open so we should simply be able
     * to copy the contents of the share memery area.  The only real
     * problem is that the internal pointers will need to change.
     * We assume all live records in the tables are reachable via the
     * main descriptor table and multiple copies of dup'd records is OK.
     * ALso, open PVFS files point to a dup of the parent's shm object.
     * These need to be closed and reopened on the new shm object via a
     * call to dup2.
     */
    /* Note: an alternative implementation would be to copy the whole
     * shm area and then just update the pointers.  Assuming most of the
     * time there are a small number of open files, we can avoid a lot
     * of that - but it may be more efficienet to do it all at once.
     */

    parent_offset = ((char *)shmctrl - (char *)parentctl);
    /* copy the CWD */
    memcpy(shmctrl->shmctrl_cwd, parentctl->shmctrl_cwd, PVFS_PATH_MAX);

    /* set loop bounds */
    dtable_size = shmctrl->descriptor_table_size;
    if (parentctl->descriptor_table_size < dtable_size)
    {
        dtable_size = parentctl->descriptor_table_size;
    }
    dtable_count = parentctl->descriptor_table_count;

    /* copy fd_table */
    memcpy(shmctrl->fd_table,
           parentctl->fd_table,
           parentctl->fd_table_count * sizeof(int));
    shmctrl->fd_table_count = parentctl->fd_table_count;
    /* add parent to the fd_table */
    shmctrl->fd_table[shmctrl->fd_table_count++] = parentobj;

    /* Note that as this is a fork(), all of the pointers held by the
     * parent should be valid.  Only when we copy an object from the
     * parent's table into our own will we have to adjust addresses 
     */
    /* now convert descriptors */
    for (d = 0; d < dtable_size && dtable_count; d++)
    {
        if (parentctl->descriptor_table[d])
        {
            init_debug1("copying descriptor %d\n", d);
            /* stop when we find all used slots */
            dtable_count--;
            shmctrl->descriptor_table_count++;
            /* adjust descriptor address */
            shmctrl->descriptor_table[d] =
                    P2L(parentctl->descriptor_table[d], pvfs_descriptor);
            /* copy descriptor */
            memcpy(shmctrl->descriptor_table[d],
                   parentctl->descriptor_table[d],
                   sizeof(pvfs_descriptor));
            if (parentctl->descriptor_table[d]->s->fsops == &pvfs_ops)
            {
                /* we are going to share the status on this fd this fd
                 * may already be shared with and older process so we
                 * will have to find it in our list and indicate we are
                 * sharing one more fd with that processess.  Any shares
                 * our parent was already sharing will count as well.
                 * We mark the fd as shared, we increment the number of
                 * shares on the status, and we increment the number of
                 * shares we have with this specific process
                 */
                init_debug1("sharing descriptor status %d\n", d);
                /* set up a shared descriptor */
                shmctrl->descriptor_table[d]->shared_status = 1;
                pdl = find_descriptor_area_list(
                                parentctl->descriptor_table[d]->s);
                pdl->ptr->descriptor_table[d]->s->dup_cnt++;
                /* If this was local to the parent and not an older
                 * process then we should free the space used by the
                 * status and potentially the dpath in our indices
                 */
            }
            else
            {
                /* a glibc file so we can just copy the descriptor
                 * status because the actual sharing happens throught
                 * the kernel - adjust descriptor status address */
                shmctrl->descriptor_table[d]->s =
                        P2L(shmctrl->descriptor_table[d]->s,
                            pvfs_descriptor_status);
                /* copy descriptor status */
                memcpy(shmctrl->descriptor_table[d]->s,
                       parentctl->descriptor_table[d]->s,
                       sizeof(pvfs_descriptor_status));
                if (shmctrl->descriptor_table[d]->s->dpath)
                {
                    /* adjust directory path address */
                    shmctrl->descriptor_table[d]->s->dpath = 
                            P2L(shmctrl->descriptor_table[d]->s->dpath, char);
                    /* copy directory path */
                    strncpy(shmctrl->descriptor_table[d]->s->dpath,
                            parentctl->descriptor_table[d]->s->dpath,
                            shmctrl->path_table_size -
                                    (shmctrl->descriptor_table[d]->s->dpath -
                                     shmctrl->path_table));
                }
            }
        }
    }

    /* release the waiting parent */
    gen_mutex_lock(&parentctl->shmctrl_lock);
    parentctl->shmctrl_copy = 0;
    gen_cond_signal(&parentctl->shmctrl_cond);
    gen_mutex_unlock(&parentctl->shmctrl_lock);

    /* and the new shm area should be good to go */
    init_debug("init descriptor area done\n");
}

/* This function creats and initializes a new shared memory descriptor
 * area and sets up the various pointers in it.
 */
static void init_descriptor_area_internal(void)
{
    int rc = 0;
    struct rlimit rl; 
    int table_size = 0;
    int flags = 0;
    int tfd = -1;

    /* open the new program's shared memory object */
    /* we dup this FD to get FD's for pvfs files */
    flags = O_RDWR | O_CREAT | O_TRUNC;
    snprintf(shmobjpath,
             50,
             _PATH_DEV "shm/pvfs-%06d-%06d",
             (int)getuid(),
             (int)getpid());
    shmobj = glibc_ops.open(shmobjpath, flags, S_IRUSR | S_IWUSR);
    if (shmobj < 0)
    {
        glibc_ops.perror("failed to open shared object in pvfs_sys_init");
        exit(-1);
    }
    /* move to magic fd */
    if (shmobj != PVFS_SHMOBJ)
    {
        if ((tfd = glibc_ops.dup2(shmobj, PVFS_SHMOBJ)) == -1)
        {
            glibc_ops.perror("failed to move descriptor table to magic fd");
            exit(-1);
        }
        glibc_ops.close(shmobj);
        shmobj = tfd;
    }

    /* compute size of descriptor table */
	getrlimit(RLIMIT_NOFILE, &rl); 
    if (rl.rlim_max == RLIM_INFINITY)
    {
	    table_size = PVFS_NOFILE_MAX;
    }
    else
    {
	    table_size = rl.rlim_max;
    }
    if (table_size & 0x1)
    {
        table_size++; /* force table_size to an even number */
    }
    shmsize = sizeof(pvfs_shmcontrol_t) + 
              (table_size * 
                  (sizeof(pvfs_descriptor *) +
                   sizeof(pvfs_descriptor) +
                   sizeof(pvfs_descriptor_status))) +
               PATH_TABLE_SIZE +
               (FD_TABLE_SIZE * sizeof(int));

    /* set the size of the shared object */
    if (glibc_ops.ftruncate(shmobj, (off_t)shmsize) == -1)
    {
        glibc_ops.perror("failed to move truncate shmobj to size");
        exit(-1);
    }

    /* set up shared memory area */
    shmctrl = (pvfs_shmcontrol_t *)glibc_ops.mmap(NULL,
                                                  shmsize,
                                                  PROT_READ | PROT_WRITE,
                                                  MAP_SHARED,
                                                  shmobj,
                                                  0);
    if (shmctrl == ((void *)-1))
    {
        glibc_ops.perror("failed to malloc descriptor table");
        exit(-1);
    }

    /* unlink the /dev/shm entry - noone should need to open it again */
    glibc_ops.unlink(shmobjpath);
    memset(shmobjpath, 0, sizeof(shmobjpath));

    /* clear shared memory */
	memset(shmctrl, 0, shmsize);

    /* set up descriptor table */
    descriptor_table = (pvfs_descriptor **)&shmctrl[1];

    shmctrl->shm_magic = PVFS_SHM_MAGIC;
    shmctrl->shmctrl = shmctrl; /* this is needed for reference mapping */

    /* need to share this sync prm across processes */
    rc = gen_shared_mutex_init(&shmctrl->shmctrl_lock);
    if (rc < 0)
    {
        init_perror("failed to init shared shmctrl mutex");
    }

    /* need to share this sync prm across processes */
    rc = gen_shared_cond_init(&shmctrl->shmctrl_cond);
    if (rc < 0)
    {
        init_perror("failed to init shared condition var");
    }

    shmctrl->shmctrl_copy = 0;
    memset(shmctrl->shmctrl_cwd, 0, PVFS_PATH_MAX);

    shmctrl->descriptor_table = descriptor_table;
    shmctrl->descriptor_table_size = table_size;
    shmctrl->descriptor_table_count = 0;
    /* need to share this sync prm across processes */
    rc = gen_shared_mutex_init(&shmctrl->descriptor_table_lock);
    if (rc < 0)
    {
        init_perror("failed to init shared table mutex");
    }

    shmctrl->descriptor_pool = (pvfs_descriptor *)
                              &shmctrl->descriptor_table[table_size];
    shmctrl->descriptor_pool_size = table_size;

    shmctrl->status_pool = (pvfs_descriptor_status *)
                          &shmctrl->descriptor_pool[table_size];
    shmctrl->status_pool_size = table_size;

    shmctrl->path_table = (char *)&shmctrl->status_pool[table_size];
    shmctrl->path_table_size = PATH_TABLE_SIZE;

    shmctrl->fd_table = (int *)&shmctrl->path_table[PATH_TABLE_SIZE];
    shmctrl->fd_table_size = FD_TABLE_SIZE;
    shmctrl->fd_table_count = 0;
}

/*
 * Internal function used to adjust internal pointers in the descriptor
 * table area after an exec call.  The area was closed, unmapped, then
 * reopened and remapped but probably at a different address so the
 * pointers are off by a displacement.  parentctl should hve been
 * initialized to the original address and shmctrl is the new address.
 * Other addresses may have moved such as glibc_ops and pvfs_ops.  While
 * we are scanning we look for files that were marked FD_CLOEXEC and
 * close them and we dup the shmobj for each PVFS file as those would
 * have been closed as well.  Finally, since the indices have been
 * wiped we rebuild them by adding records for all unused space in the
 * corresponding pools.  Part of usrlib initialization.
 */
static void rebuild_descriptor_table(pvfs_desc_list_t *shmpdl)
{
    int d = 0;
    int dtable_size = 0;
    int dtable_count = 0;
    int flags = 0;
    int last = 0;
    pvfs_shmcontrol_t *scp;

    scp = shmpdl->ptr;

    init_debug("Rebuilding descriptor table\n");

    /* old pointers in the shared memory space plus parent_offset
     * gives the new pointer address.  This could easily be zero if mmap
     * maps to same spot but we also have other things to do in the
     * comming loop so we will just go ahead and add zero the pointers
     * which should be just fine.  There probably aren't that many.  We
     * could bypass that inside the loop if we wanted to.
     */
    init_debug1("rebuild_offset = %lld\n", shmpdl->offset);

    /* FIXME: stdout probably isn't set up yet - should be gossip anyway */
    /* printf("Called copy_parent_to_descriptor_table\n"); */

    /* first get the control record adjusted */
    init_debug1("P descriptor_table = %p\n", scp->descriptor_table);
    scp->descriptor_table = (pvfs_descriptor **)PDLOFF(scp->descriptor_table,
                                                       shmpdl);
    init_debug1("S descriptor_table = %p\n", scp->descriptor_table);

    init_debug1("P descriptor_pool = %p\n", scp->descriptor_pool);
    scp->descriptor_pool = (pvfs_descriptor *)PDLOFF(scp->descriptor_pool,
                                                     shmpdl);
    init_debug1("S descriptor_pool = %p\n", scp->descriptor_pool);

    init_debug1("P status_pool = %p\n", scp->status_pool);
    scp->status_pool = (pvfs_descriptor_status *)PDLOFF(scp->status_pool,
                                                        shmpdl);
    init_debug1("S status_pool = %p\n", scp->status_pool);

    init_debug1("P path_table = %p\n", scp->path_table);
    scp->path_table = (char *)PDLOFF(shmctrl->path_table, shmpdl);
    init_debug1("S path_table = %p\n", scp->path_table);

    init_debug1("P fd_table = %p\n", scp->fd_table);
    scp->fd_table = (int *)PDLOFF(scp->fd_table, shmpdl);
    init_debug1("S fd_table = %p\n", scp->fd_table);

    /* There is an inherent assumption that all useful items in the
     * tables can be reached via the main descriptor table.  We do not
     * scan the pools directly.  Also desc stat records may be copied
     * twice if they have been duped.  Should not be a problem.
     */

    dtable_size = scp->descriptor_table_size;
    dtable_count = scp->descriptor_table_count;

    for (d = 0; d < dtable_size && dtable_count; d++)
    {
        if (scp->descriptor_table[d])
        {
            pvfs_descriptor_status *st;

            init_debug1("adjusting descriptor %d\n", d);
            dtable_count--;
            /* adjust descriptor address */
            scp->descriptor_table[d] =
                    (pvfs_descriptor *)PDLOFF(scp->descriptor_table[d], shmpdl);
            /* adjust descriptor status address */
            st = scp->descriptor_table[d]->s;
            if (scp->descriptor_table[d]->shared_status)
            {
                pvfs_desc_list_t *pdl;
                pdl = find_descriptor_area_list(st);
                scp->descriptor_table[d]->s = 
                        (pvfs_descriptor_status *)PDLOFF(st, pdl);
                /* we were already sharing this so don't increament the
                 * counters in the other table, but do increment in the
                 * pdl so we know how many references to this table we
                 * have
                 */
                pdl->shares++;
            }
            else
            {
                /* not shared, adjust as normal */
                scp->descriptor_table[d]->s =
                        (pvfs_descriptor_status *)PDLOFF(st, shmpdl);
            }
            /* recreate fsops pointer */
            if (scp->descriptor_table[d]->s->pvfs_ref.fs_id)
            {
                scp->descriptor_table[d]->s->fsops = &pvfs_ops;
            }
            else
            {
                scp->descriptor_table[d]->s->fsops = &glibc_ops;
            }
            if (scp->descriptor_table[d]->s->dpath)
            {
                /* adjust directory path address */
                scp->descriptor_table[d]->s->dpath =
                     (char *)PDLOFF(scp->descriptor_table[d]->s->dpath, shmpdl);
            }
            if ((scp->descriptor_table[d]->fdflags & FD_CLOEXEC))
            {
                /* remove the entry */
                glibc_ops.close(scp->descriptor_table[d]->true_fd);
                if (scp->descriptor_table[d]->s->dpath)
                {
                    int len;
                    len = strlen(scp->descriptor_table[d]->s->dpath);
                    memset(scp->descriptor_table[d]->s->dpath, 0, len);
                }
                memset(scp->descriptor_table[d]->s,
                       0,
                       sizeof(pvfs_descriptor_status));
                memset(scp->descriptor_table[d],
                       0,
                       sizeof(pvfs_descriptor));
                scp->descriptor_table[d] = NULL;
                scp->descriptor_table_count--;
            }
            else
            {
                if (scp->descriptor_table[d]->s->fsops == &pvfs_ops)
                {
                    /* dup the shmobj for PVFS files */
                    glibc_ops.dup2(shmpdl->fd,
                                   scp->descriptor_table[d]->true_fd);
                    /* PVFS true_fd's are all CLOEXEC */
                    flags = glibc_ops.fcntl(
                                    scp->descriptor_table[d]->true_fd,
                                    F_GETFD);
                    glibc_ops.fcntl(scp->descriptor_table[d]->true_fd,
                                    F_SETFD,
                                    flags | FD_CLOEXEC);
                }
                /* we assume glibc files are still open */
            }
            if (scp->descriptor_table[d]->s->fent)
            {
                /* need to decide how to update this correctly */
                scp->descriptor_table[d]->s->fent = NULL;
            }
        }
    }

    /* now we need to rebuild the indices for desc, status, and dpath */
#   define HIT 1
#   define MISS 0
    /* This looks for unused segments (MISSes) of the tables and adds
     * records to the index representing contiguous runs of those unused
     * segments.  HITS are being used and we skip over them.
     */
#   define BUILD_INDEX(_list, _type, _size_exp, _in_use_exp)     \
    do {                                                         \
        int d = 0;                                               \
        index_rec_t *rec;                                        \
        last = HIT;                                              \
        rec = (_type *)malloc(sizeof(_type));                    \
        rec->first = 0;                                          \
        rec->size = 0;                                           \
        for (d = 0; d < (_size_exp); d++)                        \
        {                                                        \
            if (_in_use_exp) /* this is a HIT */                 \
            {                                                    \
                if (last == HIT)                                 \
                {                                                \
                    /* skip this one */                          \
                }                                                \
                else                                             \
                {                                                \
                    /* add record to list */                     \
                    qlist_add_tail(&(rec->link), (_list));       \
                    rec = (_type *)malloc(sizeof(_type));        \
                    rec->first = 0;                              \
                    rec->size = 0;                               \
                }                                                \
                last = HIT;                                      \
            }                                                    \
            else /* it was a MISS */                             \
            {                                                    \
                if (last == HIT)                                 \
                {                                                \
                    rec->first = d;                              \
                    rec->size = 1;                               \
                }                                                \
                else                                             \
                {                                                \
                    rec->size++;                                 \
                }                                                \
                last = MISS;                                     \
            }                                                    \
        }                                                        \
        d--;                                                     \
        if (!(_in_use_exp))                                      \
        {                                                        \
            qlist_add_tail(&(rec->link), (_list));               \
        }                                                        \
    } while (0) 

    /* descriptor pool index */
    BUILD_INDEX(&desc_index,
                index_rec_t,
                shmctrl->descriptor_pool_size,
                shmctrl->descriptor_pool[d].is_in_use);

    /* status pool index */
    BUILD_INDEX(&stat_index,
                index_rec_t,
                shmctrl->status_pool_size,
                shmctrl->status_pool[d].fsops);

    /* dpath pool index */
    BUILD_INDEX(&path_index,
                index_rec_t,
                shmctrl->path_table_size,
                (shmctrl->path_table[d] ||
                        (d > 0 && !shmctrl->path_table[d] &&
                                  shmctrl->path_table[d - 1])));

#   undef BUILD_INDEX
#   undef MISS
#   undef HIT

    descriptor_table = shmctrl->descriptor_table;
    shmpdl->offset = 0L;
    /* descriptor table should be ready to go */
    init_debug("Rebuilding done\n");
}

int pvfs_descriptor_table_size(void)
{
    return shmctrl->descriptor_table_size;
}

int pvfs_descriptor_table_next(int start)
{
    int flags;
    int i;
    for (i = start; i < shmctrl->descriptor_table_size; i++)
    {
        if (!descriptor_table[i])
        {
            flags = glibc_ops.fcntl(i, F_GETFL);
            if (flags < 0)
            {
                return i;
            }
        }
    }
    return -1;
}

/* allocate a pvfs_descriptor struct in the shared memory region
 * the fd is just a hint that can be used in allocation or ignored
 * This simply abstracts the management alorithm for this struct in the
 * shared memory region.
 */
static pvfs_descriptor *get_descriptor(void)
{
    int desc;
    pvfs_descriptor *pd;

    desc = pvfs_desc_alloc(&desc_index);
    if (desc < 0 || desc >= shmctrl->descriptor_pool_size)
    {
        gossip_lerr("bad descriptor number allocated");
        return NULL;
    }
    pd = &shmctrl->descriptor_pool[desc];
    if (!pd)
    {
        gossip_lerr("bad descriptor struct allocated");
        return NULL;
    }
    memset(pd, 0, sizeof(pvfs_descriptor));
    return pd;
}

/* unallocate a pvfs_descriptor struct in the shared memory region
 * the fd is just a hint that can be used in unallocation or ignored
 * This simply abstracts the management alorithm for this struct in the
 * shared memory region.
 */
static void put_descriptor(pvfs_descriptor *pd)
{
    int desc;
    desc = pd - shmctrl->descriptor_pool;
    if (desc < 0 || desc >= shmctrl->descriptor_pool_size)
    {
        gossip_lerr(" bad fd in put_descriptor\n");
        return;
    }
    memset(pd, 0, sizeof(pvfs_descriptor));
    pvfs_desc_free(desc, &desc_index);
}

/* allocate a pvfs_descriptor_status struct in the shared memory region
 * the fd is just a hint that can be used in allocation or ignored
 * This simply abstracts the management alorithm for this struct in the
 * shared memory region.
 */
static pvfs_descriptor_status *get_status(void)
{
    int dstat;
    pvfs_descriptor_status *pds;

    dstat = pvfs_desc_alloc(&stat_index);
    if (dstat < 0 || dstat >= shmctrl->status_pool_size)
    {
        gossip_lerr("bad desc status number allocated");
        return NULL;
    }
    pds = &shmctrl->status_pool[dstat];
    if (!pds)
    {
        gossip_lerr("bad desc status struct allocated");
        return NULL;
    }
    memset(pds, 0, sizeof(pvfs_descriptor_status));
    return pds;
}

/* unallocate a pvfs_descriptor_status struct in the shared memory region
 * the fd is just a hint that can be used in unallocation or ignored
 * This simply abstracts the management alorithm for this struct in the
 * shared memory region.
 */
static void put_status(pvfs_descriptor_status *pds)
{
    int dstat;
    dstat = pds - shmctrl->status_pool;
    if (dstat < 0 || dstat >= shmctrl->status_pool_size)
    {
        gossip_lerr(" bad fds in put_status\n");
        return;
    }
    memset(pds, 0, sizeof(pvfs_descriptor_status));
    pvfs_desc_free(dstat, &stat_index);
}

/* allocate the descriptor and descriptor_status for a given slot in the
 * descriptor_table - use the given status if not NULL.  Adjust counter
 * and deal with locks to ensure thread safety,
 */
static pvfs_descriptor *get_desc_table_entry(int newfd,
                                             pvfs_descriptor_status *ps)
{
    int rc;
    pvfs_descriptor *pd = NULL;

    if (newfd < 0 || newfd >= shmctrl->descriptor_table_size)
    {
        gossip_lerr(" bad fd in get_desc_table_entry");
        return NULL;
    }
    gen_mutex_lock(&shmctrl->descriptor_table_lock);
    if (descriptor_table[newfd] != NULL)
    {
        errno = EINVAL;
        gen_mutex_unlock(&shmctrl->descriptor_table_lock);
        return NULL;
    }

    /* allocate new descriptor */
    pd = get_descriptor();
    if (!pd)
    {
        gen_mutex_unlock(&shmctrl->descriptor_table_lock);
        return NULL;
    }
    rc = gen_shared_mutex_init(&pd->lock);
    if (rc < 0)
    {
        init_perror("failed to init shared descriptor mutex");
    }
    gen_mutex_lock(&pd->lock);

    descriptor_table[newfd] = pd;
	shmctrl->descriptor_table_count++;

    if (ps)
    {
        pd->s = ps;
    }
    else
    {
        pd->s = get_status();
        if (!pd->s)
        {
            gen_mutex_unlock(&pd->lock);
            put_descriptor(pd);
            pd = NULL;
            descriptor_table[newfd] = NULL;
            gen_mutex_unlock(&shmctrl->descriptor_table_lock);
            return NULL;
        }
        gen_shared_mutex_init(&pd->s->lock);
        if (rc < 0)
        {
            init_perror("failed to init shared desc status mutex");
        }
    }
    gen_mutex_lock(&pd->s->lock);

    gen_mutex_unlock(&shmctrl->descriptor_table_lock);
    return pd;
}

/**
 * Allocate a new pvfs_descriptor, initialize fsops to the given set
 * This is the routine called externally and deals with getting the
 * structures ready for use.
 *
 * fsops: IN points to system call methods
 * newfd: IN -1 to select a free fd, otherwise assume linux fd is open
 * file_ref: IN for pvfs files the handle, etc.
 * use_cache: IN flag indicates will use user level cache
 *
 * ON RETURN NEW PD AND STATUS ARE STILL LOCKED!
 */
 pvfs_descriptor *pvfs_alloc_descriptor(posix_ops *fsops,
                                        int newfd, 
                                        PVFS_object_ref *file_ref,
                                        int use_cache)
 {
    int dupfd = -1, flags = 0; 
    pvfs_descriptor *pd;
    /* insure one thread at a time is in */
    /* fd setup section */

    PVFS_INIT(pvfs_sys_init);
    gossip_debug(GOSSIP_USRINT_DEBUG,
                 "pvfs_alloc_descriptor called with %d\n", newfd);
    if (fsops == NULL)
    {
        errno = EINVAL;
        return NULL;
    }
    if (newfd == -1)
    {
        /* PVFS file allocate a real descriptor for it */
        newfd = dupfd = glibc_ops.dup(shmobj);
        flags = glibc_ops.fcntl(newfd, F_GETFD);
        glibc_ops.fcntl(newfd, F_SETFD, flags | FD_CLOEXEC);
    }
    else
    {
        if (fsops == &glibc_ops)
        {
            /* previously opened by glibc, make sure this is a valid fd */
            flags = glibc_ops.fcntl(newfd, F_GETFL);
            if (flags < 0)
            {
                return NULL;
            }
        }
        else
        {
            /* forcing a pvfs file to a specific fd, must be unused */
            flags = glibc_ops.fcntl(newfd, F_GETFL);
            if (flags >= 0)
            {
                /* fd is in use - should not happend */
                gossip_lerr("called pvfs_alloc_descriptor with a forced "
                            "fd that is in use\n");
                return NULL;
            }
            newfd = dupfd = glibc_ops.dup2(shmobj, newfd);
            flags = glibc_ops.fcntl(newfd, F_GETFD);
            glibc_ops.fcntl(newfd, F_SETFD, flags | FD_CLOEXEC);
        }
    }
    if (descriptor_table[newfd])
    {
        gossip_lerr("Trying to allocate descriptor where one "
                    "appears to already exist\n");
        return NULL;
    }

    pd = get_desc_table_entry(newfd, NULL);
    if (pd == NULL)
    {
        if (dupfd > -1)
        {
            glibc_ops.close(dupfd);
        }
        return NULL;
    }
    descriptor_table[newfd] = pd;

	/* fill in descriptor */
	pd->is_in_use = PVFS_FS;
	pd->fd = newfd;
	pd->true_fd = newfd;
	pd->fdflags = 0;

    /*
    if (!use_cache)
    {
	    pd->fdflags |= PVFS_FD_NOCACHE;
    }
    */
	pd->s->dup_cnt = 1;
	pd->s->fsops = fsops;
    if (file_ref)
    {
	    pd->s->pvfs_ref.fs_id = file_ref->fs_id;
	    pd->s->pvfs_ref.handle = file_ref->handle;
    }
    else
    {
        /* if this is not a PVFS file then the file_ref will be NULL */
	    pd->s->pvfs_ref.fs_id = 0;
	    pd->s->pvfs_ref.handle = 0LL;
    }

    pd->s->file_pointer = 0;
    pd->s->token = 0;
    /* these should be filled in by caller as needed */
    pd->s->dpath = NULL;
    pd->s->fent = NULL; /* not caching if left NULL */
    pd->s->flags = 0;
    pd->s->mode = 0;
    pd->s->mode_deferred = 0;

#if PVFS_UCACHE_ENABLE
    if (ucache_enabled && use_cache)
    {
        /* File reference won't always be passed in */
        if(file_ref != NULL)
        {
            /* We have the file identifiers
             * so insert file info into ucache
             * this fills in mtbl
             */
            ucache_open_file(&(file_ref->fs_id),
                             &(file_ref->handle), 
                             &(pd->s->fent));
        }
    }
#endif /* PVFS_UCACHE_ENABLE */

    /* NEW PD AND STATUS ARE STILL LOCKED */
    gossip_debug(GOSSIP_USRINT_DEBUG,
                 "\tpvfs_alloc_descriptor returns with %d\n", pd->fd);
    return pd;
}

/*
 * Function for duplicating a descriptor
 * used in dup, dup2, dup3, and fcntl calls
 * This is called externally to manage FDs for these calls
 */
int pvfs_dup_descriptor(int oldfd, int newfd, int flags, int fcntl_dup)
{
    int rc = 0;
    pvfs_descriptor *pd;

    gossip_debug(GOSSIP_USRINT_DEBUG,
                 "pvfs_dup_descriptor: called with %d\n", oldfd);
    PVFS_INIT(pvfs_sys_init);
    if (oldfd < 0 || oldfd >= shmctrl->descriptor_table_size ||
        !descriptor_table[oldfd] ||
        descriptor_table[oldfd]->is_in_use != PVFS_FS)
    {
        errno = EBADF;
        return -1;
    }
    if (newfd < -1 || newfd >= shmctrl->descriptor_table_size)
    {
        errno = EINVAL;
        return -1;
    }
    if (newfd == -1) /* dup */
    {
        newfd = glibc_ops.dup(shmobj);
        flags = glibc_ops.fcntl(newfd, F_GETFD);
        glibc_ops.fcntl(newfd, F_SETFD, flags | FD_CLOEXEC);
        if (newfd < 0)
        {
            gossip_debug(GOSSIP_USRINT_DEBUG,
                         "\npvfs_dup_descriptor: returns with %d\n", newfd);
            return newfd;
        }
    }
    else /* dup2, dup3, or fcntl */
    {
        /* see if requested fd is in use */
        if (descriptor_table[newfd] != NULL ||
            glibc_ops.fcntl(newfd, F_GETFL) != -1)
        {
            /* check for special case */
            if (newfd == oldfd)
            {
                gossip_debug(GOSSIP_USRINT_DEBUG,
                             "\tpvfs_dup_descriptor: returns with %d\n", oldfd);
                return newfd;
            }
            if (fcntl_dup) /* this is fcntl */
            {
                /* find smallest available fd >= newfd */
                newfd++; /* we know original newfd is in use */
                while (descriptor_table[newfd] ||
                       glibc_ops.fcntl(newfd, F_GETFL) != -1)
                {
                    newfd++;
                    if (newfd >= shmctrl->descriptor_table_size)
                    {
                        /* ran out of valid fds */
                        errno = EMFILE;
                        return -1;
                    }
                }
            }
            else /* this is dup2 or dup3 */
            {
                /* close old file in new slot */
                rc = pvfs_free_descriptor(newfd);
                if (rc < 0)
                {
                    gossip_debug(GOSSIP_USRINT_DEBUG,
                                "\tpvfs_dup_descriptor: returns with %d\n", rc);
                    return rc;
                }
            }
        }
        /* continuing with dup2, dup2, or fcntl */
        rc = glibc_ops.dup2(shmobj, newfd);
        flags = glibc_ops.fcntl(newfd, F_GETFD);
        glibc_ops.fcntl(newfd, F_SETFD, flags | FD_CLOEXEC);
        if (rc < 0)
        {
            gossip_debug(GOSSIP_USRINT_DEBUG,
                         "\tpvfs_dup_descriptor: returns with %d\n", rc);
            return rc;
        }
    }

    /* set up new pvfs_descriptor */
    pd = get_desc_table_entry(newfd, descriptor_table[oldfd]->s);
    if (pd == NULL)
    {
        glibc_ops.close(newfd);
        return -1;
    }
    descriptor_table[newfd] = pd;

	pd->is_in_use = PVFS_FS;
	pd->fd = newfd;
	pd->true_fd = newfd;
	pd->fdflags = flags;
    pd->s->dup_cnt++;
    if (descriptor_table[oldfd]->shared_status)
    {
        pd->shared_status = 1;
        shmctrl->shmctrl_shared++;
    }

    gen_mutex_unlock(&pd->s->lock);
    gen_mutex_unlock(&pd->lock);
    gossip_debug(GOSSIP_USRINT_DEBUG,
                 "\tpvfs_dup_descriptor: returns with %d\n", newfd);
    return newfd;
}

/*
 * Return a pointer to the pvfs_descriptor for the file descriptor or null
 * if there is no entry for the given file descriptor
 * should probably be inline if we can get at static table that way
 * This is the external lookup routine for FDs used in interfaces
 */
pvfs_descriptor *pvfs_find_descriptor(int fd)
{
    pvfs_descriptor *pd = NULL;
    struct stat sbuf;

    PVFS_INIT(pvfs_sys_init);
    if (fd < 0 || fd >= shmctrl->descriptor_table_size)
    {
        errno = EBADF;
        return NULL;
    }
    pd = descriptor_table[fd];
    if (!pd)
    {
        int flags = 0;
        int fdflags = 0;
        /* see if glibc opened this file without our knowing */
        flags = glibc_ops.fcntl(fd, F_GETFL);
        if (flags == -1)
        {
            /* apparently not - use errno set by fcntl */
            return NULL;
        }
        /* we will need the fdflags as well */
        fdflags = glibc_ops.fcntl(fd, F_GETFD);

        gossip_debug(GOSSIP_USRINT_DEBUG,
              "pvfs_find_descriptor: implicit alloc of descriptor %d\n", fd);

        /* this returns pd with a new pd->s both locked */
        pd = get_desc_table_entry(fd, NULL);
        if (!pd)
        {
            return NULL;
        }
        descriptor_table[fd] = pd;
    
	    /* fill in descriptor */
	    pd->is_in_use = PVFS_FS;
	    pd->fd = fd;
	    pd->true_fd = fd;
	    pd->fdflags = fdflags;
	    pd->s->dup_cnt = 1;
	    pd->s->fsops = &glibc_ops;
	    pd->s->pvfs_ref.fs_id = 0;
	    pd->s->pvfs_ref.handle = 0LL;
	    pd->s->flags = flags;
	    pd->s->dpath = NULL;
        glibc_ops.fstat(fd, &sbuf);
        pd->s->mode = sbuf.st_mode;
        if (S_ISDIR(sbuf.st_mode))
        {
            /* need to get path for this fd from /proc */
            /* we will use glibc to open /proc/self/# which */
            /* is a symbolic link, and then read the link */
            /* to get the path */
            char procpath[50];
            char dirpath[PVFS_PATH_MAX];
            int len;

            memset(procpath, 0, 50);
            memset(dirpath, 0, PVFS_PATH_MAX);
            sprintf(procpath, "/proc/self/fd/%d", fd);
            len = glibc_ops.readlink(procpath, dirpath, PVFS_PATH_MAX);
            if (len < 0)
            {
                /* error reading link */
                /* silent for now */
                /* we just won't have the path for this dir */
            }
            else
            {
                /* stash the path */
                pd->s->dpath = pvfs_dpath_insert(dirpath);
            }
        }
	    pd->s->mode = 0;
	    pd->s->file_pointer = 0;
	    pd->s->token = 0;
        pd->s->fent = NULL; /* not caching if left NULL */
        gen_mutex_unlock(&pd->s->lock);
    }
    else
    {
        /* locks here prevent a thread from getting */
        /* a pd that is not finish being allocated yet */
        gen_mutex_lock(&pd->lock);
        if (pd->is_in_use != PVFS_FS)
        {
            errno = EBADF;
            gen_mutex_unlock(&pd->lock);
            return NULL;
        }
    }
    gen_mutex_unlock(&pd->lock);
	return pd;
}

/*
 * External call for interfaces to unallocate FDs
 */
int pvfs_free_descriptor(int fd)
{
    int dup_cnt;
    pvfs_descriptor *pd = NULL;
    gossip_debug(GOSSIP_USRINT_DEBUG,
                 "pvfs_free_descriptor called with %d\n", fd);

    pd = descriptor_table[fd];
    if (pd == NULL)
    {
        int rc;
        /* may have gotten a close on previously unknown glibc fd */
        rc = glibc_ops.close(fd);
        gossip_debug(GOSSIP_USRINT_DEBUG,
                     "\tpvfs_free_descriptor returns %d\n", rc);
        return rc;
    }
    gen_mutex_lock(&pd->lock);

	/* clear out table entry */
    gen_mutex_lock(&shmctrl->descriptor_table_lock);
	descriptor_table[fd] = NULL;
    glibc_ops.close(pd->true_fd);

	/* keep up with used descriptors */
	--(shmctrl->descriptor_table_count);

    gen_mutex_unlock(&shmctrl->descriptor_table_lock);
    gen_mutex_lock(&pd->s->lock);
    
    /* descrement dup count */
    dup_cnt = --(pd->s->dup_cnt);

    /* if not sharing, see if last dup */
    if (dup_cnt <= 0 && !pd->shared_status)
    {
        /* not shared and last dup */
        if (pd->s->mode & pd->s->mode_deferred)
        {
            PVFS_sys_attr attr;
            /* there were deferred mode bits */
            iocommon_getattr(pd->s->pvfs_ref, &attr, PVFS_ATTR_DEFAULT_MASK);
            attr.perms &= ~(pd->s->mode_deferred);
            attr.mask = PVFS_ATTR_SYS_PERM;
            iocommon_setattr(pd->s->pvfs_ref, &attr);
        }
        /* free up dpath space */
        if (pd->s->dpath)
        {
            pvfs_dpath_remove(pd->s->dpath);
        }

#if PVFS_UCACHE_ENABLE
        /* What about caching of shared files? */
        /* Should we not clear the cache in the shared case too? */
        if (pd->s->fent)
        {
            int rc = 0;
            rc = ucache_close_file(pd->s->fent);
            if(rc == -1)
            {
                gossip_debug(GOSSIP_USRINT_DEBUG,
                             "\tpvfs_free_descriptor returns %d\n", rc);
                return rc;
            }
        }
#endif /* PVFS_UCACHE_ENABLE */

        gen_mutex_unlock(&pd->s->lock);
        /* status gets returned to pool here */
	    put_status(pd->s);
        pd->s = NULL;
    }
    else
    {
        int closed = 0;
        /* check for shared status */
        if (pd->shared_status)
        {
            /* this descriptor is shared with another process */
            pvfs_desc_list_t *pdl;
            pdl = find_descriptor_area_list(pd->s);
            if (--(pdl->shares))
            {
                closed = 1;
                gen_mutex_unlock(&pd->s->lock);
                close_descriptor_area_list(pdl);
            }
            /* cannot release the status because another process owns the
            * indices for it
            */
        }
        if (!closed)
        {
            gen_mutex_unlock(&pd->s->lock);
        }
    }
    gen_mutex_unlock(&pd->lock);
    put_descriptor(pd);

    gossip_debug(GOSSIP_USRINT_DEBUG, "\tpvfs_free_descriptor returns %d\n", 0);
	return 0;
}

int pvfs_put_cwd(char *buf, int size)
{
    memset(shmctrl->shmctrl_cwd, 0, PVFS_PATH_MAX);
    memcpy(shmctrl->shmctrl_cwd, buf, size);
    return 0;
}

int pvfs_len_cwd(void)
{
    return strnlen(shmctrl->shmctrl_cwd, PVFS_PATH_MAX);
}

int pvfs_get_cwd(char *buf, int size)
{
    memcpy(buf, shmctrl->shmctrl_cwd, size);
    return 0;
}

/*
 * These are used by some interfaces to generate randome values using an
 * independent stream saved in the global array rstate.
 */
void PINT_initrand(void)
{
    static int init_called = 0;
    pid_t pid;
    uid_t uid;
    gid_t gid;
    struct timeval time;
    char *oldstate;
    unsigned int seed;

    if (init_called)
    {
        return;
    }
    init_called = 1;
    pid = getpid();
    uid = getuid();
    gid = getgid();
    gettimeofday(&time, NULL);
    seed = (((pid << 16) ^ uid) ^ (gid << 8)) ^ time.tv_usec;
    oldstate = initstate(seed, rstate, 256);
    setstate(oldstate);
}

long int PINT_random(void)
{
    char *oldstate;
    long int rndval;

    PINT_initrand();
    oldstate = setstate(rstate);
    rndval = random();
    setstate(oldstate);
    return rndval;
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=4 sts=4 sw=4 expandtab
 */
