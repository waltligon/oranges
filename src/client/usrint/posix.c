/* 
 * (C) 2011 Clemson University and The University of Chicago 
 *
 * See COPYING in top-level directory.
 */

/** \file
 *  \ingroup usrint
 *
 *  PVFS2 user interface routines - wrappers for posix system calls
 */

/* this prevents headers from using inlines for 64 bit calls */

#define USRINT_SOURCE 1
#include "usrint.h"
#include "posix-ops.h"
#include "posix-pvfs.h"
#include "openfile-util.h"
#include "pvfs-path.h"

/**
 * function prototypes not defined in libc, though it is a linux
 * system call and we define it in the usr lib
 */

int getdents(unsigned int, struct dirent *, unsigned int);

#if __GLIBC__ <=2 && __GLIBC_MINOR__ < 30 //this might need to be changed in future versions of glibc
int getdents64(unsigned int, struct dirent64 *, unsigned int);
#endif

int flock(int, int);
int fadvise64(int, off64_t, off64_t, int);

/*
 * SYSTEM CALLS
 */

/*
 * open_internal wrapper
 */
static int open_internal(const char *path, int flags, va_list ap)
{
    int rc = 0;
    mode_t mode = 0;
    PVFS_hint hints;  /* need to figure out how to set default */
    pvfs_descriptor *pd;

    gossip_debug(GOSSIP_USRINT_DEBUG, "posix.c open: called with %s\n", path);
    if (flags & O_CREAT)
        mode = va_arg(ap, mode_t);
    else
        mode = 0777;
    if (flags & O_HINTS)
        hints = va_arg(ap, PVFS_hint);
    else
       hints = PVFS_HINT_NULL;

    if (!path)
    {
        errno = EFAULT;
        gossip_debug(GOSSIP_USRINT_DEBUG,
                     "\tposix.c open: returns with %d\n", -1);
        return -1;
    }
    if (is_pvfs_path(&path, 0))
    {
        /* this handles setup of the descriptor */
        rc = pvfs_open(path, flags, mode, hints);
    }
    else
    {
        struct stat sbuf;
        /* path unknown to FS so open with glibc */
        rc = glibc_ops.open(path, flags & 01777777, mode);
        if (rc < 0)
        {
            goto errorout;
        }
        /* set up the descriptor manually */
        gossip_debug(GOSSIP_USRINT_DEBUG,
                     "posix.c open calls pvfs_alloc_descriptor %d\n", rc);
        pd = pvfs_alloc_descriptor(&glibc_ops, rc, NULL, 0);
        if (!pd)
        {
            goto errorout;
        }
        pd->is_in_use = PVFS_FS;
        pd->s->flags = flags;
        glibc_ops.fstat(rc, &sbuf);
        pd->s->mode = sbuf.st_mode;
        if (S_ISDIR(sbuf.st_mode))
        {
            /* we assume path was qualified by is_pvfs_path() */
            pd->s->dpath = pvfs_dpath_insert(path);
        }
        gen_mutex_unlock(&pd->s->lock);
        gen_mutex_unlock(&pd->lock);
        rc = pd->fd;
    }
    goto cleanup;
errorout:
    rc = -1;
cleanup:
    PVFS_free_expanded(path);
    gossip_debug(GOSSIP_USRINT_DEBUG, "\tposix.c open: returns with %d\n", rc);
    return rc;
}

/*
 * open wrapper
 */
int open(const char *path, int flags, ...)
{
    int fd;
    va_list ap;
    va_start(ap, flags);
    fd = open_internal(path, flags, ap);
    va_end(ap);
    return fd;
}

/*
 * open64 wrapper
 */
int open64(const char *path, int flags, ...)
{
    int fd;
    va_list ap;
    va_start(ap, flags);
    fd = open_internal(path, flags|O_LARGEFILE, ap);
    va_end(ap);
    return fd;
}

static int openat_internal(int dirfd, const char *path, int flags, va_list ap)
{
    int fd; 
    int mode = 0;
    pvfs_descriptor *pd;

    if (!path)
    {
        errno = EFAULT;
        return -1;
    }

    if (flags & O_CREAT)
    {
        mode = va_arg(ap, int);
    }
    if (dirfd == AT_FDCWD || (path && path[0] == '/'))
    {
        fd = open(path, flags, mode);
    }
    else
    {
        pd = pvfs_find_descriptor(dirfd);
        if (pd && pd->is_in_use && dirfd == pd->fd &&
            pd->s && pd->s->fsops)
        {
            fd = pd->s->fsops->openat(pd->true_fd, path, flags, mode);
        }
        else
        {
            errno = EBADF;
            fd = -1;
        }
    }

    return fd;
}

int openat(int dirfd, const char *path, int flags, ...)
{
    int fd;
    va_list ap;
    va_start(ap, flags);
    fd = openat_internal(dirfd, path, flags, ap);
    va_end(ap);
    return fd;
}

int openat64(int dirfd, const char *path, int flags, ...)
{
    int fd;
    va_list ap;
    va_start(ap, flags);
    fd = openat_internal(dirfd, path, flags|O_LARGEFILE, ap);
    va_end(ap);
    return fd;
}

/*
 * creat wrapper
 */ 
int creat(const char *path, mode_t mode)
{ 
    return open(path, O_CREAT|O_WRONLY|O_TRUNC, mode); 
}

/*
 * creat64 wrapper
 */ 
int creat64(const char *path, mode_t mode)
{ 
    return open64(path, O_CREAT|O_WRONLY|O_TRUNC, mode); 
}

/*
 * unlink wrapper 
 */
int unlink(const char *path)
{   
    int rc = 0;
    if (!path)
    {
        errno = EFAULT;
        return -1;
    }
    if (is_pvfs_path(&path, 0))
    {
        rc = pvfs_ops.unlink(path);
    }
    else
    {
        rc = glibc_ops.unlink(path);
    }
    PVFS_free_expanded(path);
    return rc;
}

int unlinkat(int dirfd, const char *path, int flag)
{
    int rc = 0; 
    pvfs_descriptor *pd; 
    
    if (!path)
    {
        errno = EFAULT;
        return -1;
    }
    if (dirfd == AT_FDCWD || (path && path[0] == '/'))
    {
        if (is_pvfs_path(&path,0))
        {
            rc = pvfs_ops.unlinkat(AT_FDCWD, path, flag);
        }
        else
        {
            rc = glibc_ops.unlinkat(AT_FDCWD, path, flag);
        }
        PVFS_free_expanded(path);
    }
    else
    {
        pd = pvfs_find_descriptor(dirfd);
        if (pd &&
            pd->s && pd->s->fsops)
        {
            rc = pd->s->fsops->unlinkat(pd->true_fd, path, flag);
        }
        else
        {
            errno = EBADF;
            rc = -1;
        }
    }
    return rc;
}

/*
 * rename wrapper
 */
int rename (const char *old, const char *new)
{
    int rc = 0;
    int oldp, newp;
    if (!old || !new)
    {
        errno = EFAULT;
        return -1;
    }
    oldp = is_pvfs_path(&old,0);
    newp = is_pvfs_path(&new,0);
    if(oldp && newp)
    { 
        rc = pvfs_rename(old, new);
    }
    else if (!oldp && !newp)
    {
        rc = glibc_ops.rename(old, new);
    }
    else
    {
        errno = EXDEV;
        goto errorout;
    }
    goto cleanup;
errorout:
    rc = -1;
cleanup:
    PVFS_free_expanded(old);
    PVFS_free_expanded(new);
    return rc;
}

int renameat (int oldfd, const char *old, int newfd, const char *new)
{
    pvfs_descriptor *oldpd, *newpd;
    oldpd = pvfs_find_descriptor(oldfd);
    newpd = pvfs_find_descriptor(newfd);
    if (!old || !new)
    {
        errno = EFAULT;
        return -1;
    }
    if (!oldpd || !newpd)
    {
        errno = EBADF;
        return -1;
    }
    if (oldpd->s && oldpd->s->fsops && newpd->s &&
        oldpd->s->fsops == newpd->s->fsops)
    {
        return oldpd->s->fsops->renameat(oldpd->true_fd,
                                         old,
                                         newpd->true_fd,
                                         new);
    }
    else
    {
        errno = EXDEV;
        return -1;
    }
}

/* READING and WRITING SYSTEM CALL */

/*
 * read wrapper 
 */
ssize_t read(int fd, void *buf, size_t count)
{
    ssize_t rc = 0; 
    pvfs_descriptor *pd; 
    
    pd = pvfs_find_descriptor(fd);
    if (pd && pd->is_in_use && fd == pd->fd &&
        pd->s && pd->s->fsops)
    {
        rc = pd->s->fsops->read(pd->true_fd, buf, count);
    }
    else
    {
        errno = EBADF;
        rc = -1;
    }
    return rc;
}

/*
 * pread wrapper 
 */
ssize_t pread(int fd, void *buf, size_t nbytes, off_t offset)
{
    ssize_t rc = 0; 
    pvfs_descriptor *pd; 
    
    pd = pvfs_find_descriptor(fd); 
    if (pd && pd->is_in_use && fd == pd->fd &&
        pd->s && pd->s->fsops)
    {
        rc = pd->s->fsops->pread(pd->true_fd, (void *)buf, nbytes, offset); 
    }
    else
    {
        errno = EBADF;
        rc = -1;
    }
    return rc;
} 

/*
 * readv wrapper 
 */
ssize_t readv(int fd, const struct iovec *iov, int iovcnt)
{ 
    ssize_t rc = 0;
    pvfs_descriptor *pd; 
    
    pd = pvfs_find_descriptor(fd); 
    if (pd && pd->is_in_use && fd == pd->fd &&
        pd->s && pd->s->fsops)
    {
        rc = pd->s->fsops->readv(pd->true_fd, iov, iovcnt); 
    }
    else
    {
        errno = EBADF;
        rc = -1;
    }
    return rc;
}

/*
 * pread64 wrapper 
 */
ssize_t pread64(int fd, void *buf, size_t nbytes, off64_t offset)
{
    ssize_t rc = 0; 
    pvfs_descriptor *pd; 
    
    pd = pvfs_find_descriptor(fd); 
    if (pd && pd->is_in_use && fd == pd->fd &&
        pd->s && pd->s->fsops)
    {
        rc = pd->s->fsops->pread64(pd->true_fd, (void *)buf, nbytes, offset); 
    }
    else
    {
        errno = EBADF;
        rc = -1;
    }
    return rc;
}

/*
 * write wrapper 
 */
ssize_t write(int fd, const void *buf, size_t count)
{
    ssize_t rc = 0; 
    pvfs_descriptor *pd; 
    
    pd = pvfs_find_descriptor(fd);
    if (pd && pd->is_in_use && fd == pd->fd &&
        pd->s && pd->s->fsops)
    {
        rc = pd->s->fsops->write(pd->true_fd, (void *)buf, count);
    }
    else
    {
        errno = EBADF;
        rc = -1;
    }
    return rc;
}

/*
 * pwrite wrapper 
 */
ssize_t pwrite(int fd, const void *buf, size_t nbytes, off_t offset)
{
    ssize_t rc = 0;
    pvfs_descriptor *pd;
    
    pd = pvfs_find_descriptor(fd);
    if (pd && pd->is_in_use && fd == pd->fd &&
        pd->s && pd->s->fsops)
    {
        rc = pd->s->fsops->pwrite(pd->true_fd, buf, nbytes, offset);
    }
    else
    {
        errno = EBADF;
        rc = -1;
    }
    return rc;
}

/*
 * write wrapper 
 */
ssize_t writev(int fd, const struct iovec *iov, int iovcnt)
{ 
    ssize_t rc = 0;
    pvfs_descriptor *pd;
    
    pd = pvfs_find_descriptor(fd);
    if (pd && pd->is_in_use && fd == pd->fd)
    {
        rc = pd->s->fsops->writev(fd, iov, iovcnt);
        if (rc > 0)
        {
            gen_mutex_lock(&pd->s->lock);
            pd->s->file_pointer += rc;
            gen_mutex_unlock(&pd->s->lock);
        }
    }
    else
    {
        errno = EBADF;
        rc = -1;
    }
    return rc;
}

/*
 * pwrite64 wrapper 
 */
ssize_t pwrite64(int fd, const void *buf, size_t nbytes, off64_t offset)
{
    ssize_t rc = 0;
    pvfs_descriptor *pd;
    
    pd = pvfs_find_descriptor(fd);
    if (pd && pd->is_in_use && fd == pd->fd &&
        pd->s && pd->s->fsops)
    {
        rc = pd->s->fsops->pwrite64(pd->true_fd, buf, nbytes, offset);
    }
    else
    {
        errno = EBADF;
        rc = -1;
    }
    return rc;
}

/* SEEK system calls */

/*
 * lseek wrapper 
 */
off_t lseek(int fd, off_t offset, int whence)
{
    off64_t rc = lseek64(fd, (off64_t)offset, whence);
    if (rc & 0xffffffff00000000LLU)
    {
        errno = EFAULT;
        rc = -1;
    }
    return (off_t)rc;
}

/* 
 * lseek64 wrapper 
 */ 
off64_t lseek64(int fd, off64_t offset, int whence)
{
    off64_t rc = 0;
    pvfs_descriptor *pd;
    
    pd = pvfs_find_descriptor(fd);
    if (pd && pd->is_in_use && fd == pd->fd &&
        pd->s && pd->s->fsops)
    {
        rc = pd->s->fsops->lseek64(pd->true_fd, offset, whence);
    }
    else
    {
        errno = EBADF;
        rc = (off64_t)-1;
    }
    return rc;
}

int truncate(const char *path, off_t length)
{
    int rc = 0;
    if (!path)
    {
        errno = EFAULT;
        return -1;
    }
    if(is_pvfs_path(&path,0))
    { 
        rc = pvfs_truncate(path, length);
    }
    else
    {
        rc = glibc_ops.truncate(path, length);
    }
    PVFS_free_expanded(path);
    return rc;
}

int truncate64(const char *path, off64_t length)
{
    int rc = 0;
    if (!path)
    {
        errno = EFAULT;
        return -1;
    }
    if(is_pvfs_path(&path,0))
    { 
        rc = pvfs_truncate64(path, length);
    }
    else
    {
        rc = glibc_ops.truncate64(path, length);
    }
    PVFS_free_expanded(path);
    return rc;
}

int ftruncate(int fd, off_t length)
{
    int rc = 0;
    pvfs_descriptor *pd;
    
    pd = pvfs_find_descriptor(fd);
    if (pd && pd->is_in_use && fd == pd->fd &&
        pd->s && pd->s->fsops)
    {
        rc = pd->s->fsops->ftruncate(pd->true_fd, length);
    }
    else
    {
        errno = EBADF;
        rc = (off64_t)-1;
    }
    return rc;
}

int ftruncate64(int fd, off64_t length)
{
    int rc = 0;
    pvfs_descriptor *pd;
    
    pd = pvfs_find_descriptor(fd);
    if (pd && pd->is_in_use && fd == pd->fd &&
        pd->s && pd->s->fsops)
    {
        rc = pd->s->fsops->ftruncate64(pd->true_fd, length);
    }
    else
    {
        errno = EBADF;
        rc = (off64_t)-1;
    }
    return rc;
}

#ifdef _XOPEN_SOURCE
int posix_fallocate(int fd, off_t offset, off_t length)
{
    int rc = 0;
    pvfs_descriptor *pd;
    
    pd = pvfs_find_descriptor(fd);
    if (pd && pd->is_in_use && fd == pd->fd &&
        pd->s && pd->s->fsops)
    {
        rc = pd->s->fsops->fallocate(pd->true_fd, offset, length);
    }
    else
    {
        errno = EBADF;
        rc = (off64_t)-1;
    }
    return rc;
}
#endif

/*
 * close wrapper 
 */
int close(int fd)
{
    int rc = 0;
    gossip_debug(GOSSIP_USRINT_DEBUG, "posix.c close: called with %d\n", fd);
    
    rc = pvfs_close(fd);
    gossip_debug(GOSSIP_USRINT_DEBUG, "\tposix.c close: returns %d\n", rc);
    return rc;
}

#if 0
int flush(int fd)
{
    int rc = 0;
    pvfs_descriptor *pd;
    
    pd = pvfs_find_descriptor(fd);
    if (pd)
    {
        rc = pd->s->fsops->flush(pd->true_fd);
    }
    else
    {
        errno = EBADF;
        rc = (off64_t)-1;
    }
    return rc;
}
#endif

/* various flavors of stat */
int stat(const char *path, struct stat *buf)
{
    int rc = 0;
    if (!path || !buf)
    {
        errno = EFAULT;
        return -1;
    }
    if(is_pvfs_path(&path,0))
    { 
        rc = pvfs_stat(path, buf);
    }
    else
    {
        rc = glibc_ops.stat(path, buf);
    }
    PVFS_free_expanded(path);
    return rc;
}

int __xstat(int ver, const char *path, struct stat *buf)
{
    return stat(path, buf);
}

int stat64(const char *path, struct stat64 *buf)
{
    int rc = 0;
    if (!path || !buf)
    {
        errno = EFAULT;
        return -1;
    }
    if(is_pvfs_path(&path,0))
    { 
        rc = pvfs_stat64(path, buf);
    }
    else
    {
        rc = glibc_ops.stat64(path, buf);
    }
    PVFS_free_expanded(path);
    return rc;
}

int __xstat64(int ver, const char *path, struct stat64 *buf)
{
    return stat64(path, buf);
}

int fstat(int fd, struct stat *buf)
{
    int rc = 0;
    pvfs_descriptor *pd;
    
    pd = pvfs_find_descriptor(fd);
    if (pd && pd->is_in_use && fd == pd->fd &&
        pd->s && pd->s->fsops)
    {
        rc = pd->s->fsops->fstat(pd->true_fd, buf);
    }
    else
    {
        errno = EBADF;
        rc = -1;
    }
    return rc;
}

int __fxstat(int ver, int fd, struct stat *buf)
{
    return fstat(fd, buf);
}

int fstat64(int fd, struct stat64 *buf)
{
    int rc = 0;
    pvfs_descriptor *pd;
    
    pd = pvfs_find_descriptor(fd);
    if (pd && pd->is_in_use && fd == pd->fd &&
        pd->s && pd->s->fsops)
    {
        rc = pd->s->fsops->fstat64(pd->true_fd, buf);
    }
    else
    {
        errno = EBADF;
        rc = -1;
    }
    return rc;
}

int __fxstat64(int ver, int fd, struct stat64 *buf)
{
    return fstat64(fd, buf);
}

int fstatat(int fd, const char *path, struct stat *buf, int flag)
{
    int rc = 0; 
    pvfs_descriptor *pd; 
    
    if (fd == AT_FDCWD || (path && path[0] == '/'))
    {
        if (flag & AT_SYMLINK_NOFOLLOW)
        {
            rc = lstat(path, buf);
        }
        else
        {
            rc = stat(path, buf);
        }
    }
    else
    {
        pd = pvfs_find_descriptor(fd);
        if (pd && pd->is_in_use && fd == pd->fd &&
            pd->s && pd->s->fsops)
        {
            rc = pd->s->fsops->fstatat(pd->true_fd, path, buf, flag);
        }
        else
        {
            errno = EBADF;
            rc = -1;
        }
    }
    return rc;
}

int __fxstatat(int ver, int fd, const char *path, struct stat *buf, int flag)
{
    return fstatat(fd, path, buf, flag);
}

int fstatat64(int fd, const char *path, struct stat64 *buf, int flag)
{
    int rc = 0; 
    pvfs_descriptor *pd; 
    
    if (fd == AT_FDCWD || (path && path[0] == '/'))
    {
        if (flag & AT_SYMLINK_NOFOLLOW)
        {
            rc = lstat64(path, buf);
        }
        else
        {
            rc = stat64(path, buf);
        }
    }
    else
    {
        pd = pvfs_find_descriptor(fd);
        if (pd && pd->is_in_use && fd == pd->fd &&
            pd->s && pd->s->fsops)
        {
            rc = pd->s->fsops->fstatat64(pd->true_fd, path, buf, flag);
        }
        else
        {
            errno = EBADF;
            rc = -1;
        }
    }
    return rc;
}

int __fxstatat64(int ver, int fd, const char *path, struct stat64 *buf, int flag)
{
    return fstatat64(fd, path, buf, flag);
}

int lstat(const char *path, struct stat *buf)
{
    int rc = 0;
    if (!path || !buf)
    {
        errno = EFAULT;
        return -1;
    }
    if(is_pvfs_path(&path,1))
    { 
        rc =  pvfs_lstat(path, buf);
    }
    else
    {
        rc = glibc_ops.lstat(path, buf);
    }
    PVFS_free_expanded(path);
    return rc;
}

int __lxstat(int ver, const char *path, struct stat *buf)
{
    return lstat(path, buf);
}

int lstat64(const char *path, struct stat64 *buf)
{
    int rc = 0;
    if (!path || !buf)
    {
        errno = EFAULT;
        return -1;
    }
    if(is_pvfs_path(&path,1))
    { 
        rc = pvfs_lstat64(path, buf);
    }
    else
    {
        rc = glibc_ops.lstat64(path, buf);
    }
    PVFS_free_expanded(path);
    return rc;
}

int __lxstat64(int ver, const char *path, struct stat64 *buf)
{
    return lstat64(path, buf);
}

int futimesat(int dirfd, const char *path, const struct timeval times[2])
{
    int rc = 0; 
    pvfs_descriptor *pd; 
    
    if (dirfd == AT_FDCWD || (path && path[0] == '/'))
    {
        utimes(path, times);
    }
    else
    {
        pd = pvfs_find_descriptor(dirfd);
        if (pd && pd->is_in_use && dirfd == pd->fd &&
            pd->s && pd->s->fsops)
        {
            rc = pd->s->fsops->futimesat(pd->true_fd, path, times);
        }
        else
        {
            errno = EBADF;
            rc = -1;
        }
    }
    return rc;
}

int utimes(const char *path, const struct timeval times[2])
{
    int rc = 0;
    if (!path)
    {
        errno = EFAULT;
        return -1;
    }
    if(is_pvfs_path(&path,0))
    { 
        rc = pvfs_utimes(path, times);
    }
    else
    {
        rc = glibc_ops.utimes(path, times);
    }
    PVFS_free_expanded(path);
    return rc;
}

int utime(const char *path, const struct utimbuf *buf)
{
    int rc = 0;
    if (!path || !buf)
    {
        errno = EFAULT;
        return -1;
    }
    if(is_pvfs_path(&path,0))
    { 
        rc = pvfs_utime(path, buf);
    }
    else
    {
        rc = glibc_ops.utime(path, buf);
    }
    PVFS_free_expanded(path);
    return rc;
}

int futimes(int fd, const struct timeval times[2])
{
    int rc = 0;
    pvfs_descriptor *pd;
    
    pd = pvfs_find_descriptor(fd);
    if (pd && pd->is_in_use && fd == pd->fd &&
        pd->s && pd->s->fsops)
    {
        rc = pd->s->fsops->futimes(pd->true_fd, times);
    }
    else
    {
        errno = EBADF;
        rc = -1;
    }
    return rc;
}

int dup(int oldfd)
{
    int rc = 0;
    pvfs_descriptor *pd;
    
    pd = pvfs_find_descriptor(oldfd);
    if (pd && pd->is_in_use && oldfd == pd->fd &&
        pd->s && pd->s->fsops)
    {
        rc = pd->s->fsops->dup(pd->true_fd);
    }
    else
    {
        errno = EBADF;
        rc = -1;
    }
    return rc;
}

int dup2(int oldfd, int newfd)
{
    int rc = 0;
    pvfs_descriptor *pd;
    
    pd = pvfs_find_descriptor(oldfd);
    if (pd && pd->is_in_use && oldfd == pd->fd &&
        pd->s && pd->s->fsops)
    {
        rc = pd->s->fsops->dup2(pd->true_fd, newfd);
    }
    else
    {
        errno = EBADF;
        rc = -1;
    }
    return rc;
}

int dup3(int oldfd, int newfd, int flags)
{
    int rc = 0;
    pvfs_descriptor *pd;
    
    pd = pvfs_find_descriptor(oldfd);
    if (pd && pd->is_in_use && oldfd == pd->fd &&
        pd->s && pd->s->fsops)
    {
        rc = pd->s->fsops->dup3(pd->true_fd, newfd, flags);
    }
    else
    {
        errno = EBADF;
        rc = -1;
    }
    return rc;
}

int chown(const char *path, uid_t owner, gid_t group)
{
    int rc = 0;
    if (!path)
    {
        errno = EFAULT;
        return -1;
    }
    if(is_pvfs_path(&path,0))
    { 
        rc = pvfs_chown(path, owner, group);
    }
    else
    {
        rc = glibc_ops.chown(path, owner, group);
    }
    PVFS_free_expanded(path);
    return rc;
}

int fchown(int fd, uid_t owner, gid_t group)
{
    int rc = 0;
    pvfs_descriptor *pd;
    
    pd = pvfs_find_descriptor(fd);
    if (pd && pd->is_in_use && fd == pd->fd &&
        pd->s && pd->s->fsops)
    {
        rc = pd->s->fsops->fchown(pd->true_fd, owner, group);
    }
    else
    {
        errno = EBADF;
        rc = -1;
    }
    return rc;
}

int fchownat(int dirfd, const char *path, uid_t owner, gid_t group, int flag)
{
    int rc = 0; 
    pvfs_descriptor *pd; 
    
    if (dirfd == AT_FDCWD || (path && path[0] == '/'))
    {
        chown(path, owner, group);
    }
    else
    {
        pd = pvfs_find_descriptor(dirfd);
        if (pd && pd->is_in_use && dirfd == pd->fd &&
            pd->s && pd->s->fsops)
        {
            rc = pd->s->fsops->fchownat(pd->true_fd, path, owner, group, flag);
        }
        else
        {
            errno = EBADF;
            rc = -1;
        }
    }
    return rc;
}

int lchown(const char *path, uid_t owner, gid_t group)
{
    int rc = 0;
    if (!path)
    {
        errno = EFAULT;
        return -1;
    }
    if(is_pvfs_path(&path,1))
    { 
        rc = pvfs_lchown(path, owner, group);
    }
    else
    {
        rc = glibc_ops.lchown(path, owner, group);
    }
    PVFS_free_expanded(path);
    return rc;
}

int chmod(const char *path, mode_t mode)
{
    int rc = 0;
    if (!path)
    {
        errno = EFAULT;
        return -1;
    }
    if(is_pvfs_path(&path,0))
    { 
        rc = pvfs_chmod(path, mode);
    }
    else
    {
        rc = glibc_ops.chmod(path, mode);
    }
    PVFS_free_expanded(path);
    return rc;
}

int fchmod(int fd, mode_t mode)
{
    int rc = 0;
    pvfs_descriptor *pd;
    
    pd = pvfs_find_descriptor(fd);
    if (pd && pd->is_in_use && fd == pd->fd &&
        pd->s && pd->s->fsops)
    {
        rc = pd->s->fsops->fchmod(pd->true_fd, mode);
    }
    else
    {
        errno = EBADF;
        rc = -1;
    }
    return rc;
}

int fchmodat(int dirfd, const char *path, mode_t mode, int flag)
{
    int rc = 0; 
    pvfs_descriptor *pd; 
    
    if (!path)
    {
        errno = EFAULT;
        return -1;
    }
    if (dirfd == AT_FDCWD || (path && path[0] == '/'))
    {
        chmod(path, mode);
    }
    else
    {
        pd = pvfs_find_descriptor(dirfd);
        if (pd && pd->is_in_use && dirfd == pd->fd &&
            pd->s && pd->s->fsops)
        {
            rc = pd->s->fsops->fchmodat(pd->true_fd, path, mode, flag);
        }
        else
        {
            errno = EBADF;
            rc = -1;
        }
    }
    return rc;
}

int mkdir(const char *path, mode_t mode)
{
    int rc = 0;
    if (!path)
    {
        errno = EFAULT;
        return -1;
    }
    if(is_pvfs_path(&path,0))
    { 
        rc = pvfs_mkdir(path, mode);
    }
    else
    {
        rc = glibc_ops.mkdir(path, mode);
    }
    PVFS_free_expanded(path);
    return rc;
}

int mkdirat(int dirfd, const char *path, mode_t mode)
{
    int rc = 0; 
    pvfs_descriptor *pd; 
    
    if (!path)
    {
        errno = EFAULT;
        return -1;
    }
    if (dirfd == AT_FDCWD || (path && path[0] == '/'))
    {
        mkdir(path, mode);
    }
    else
    {
        pd = pvfs_find_descriptor(dirfd);
        if (pd && pd->is_in_use && dirfd == pd->fd &&
            pd->s && pd->s->fsops)
        {
            rc = pd->s->fsops->mkdirat(pd->true_fd, path, mode);
        }
        else
        {
            errno = EBADF;
            rc = -1;
        }
    }
    return rc;
}

int rmdir(const char *path)
{
    int rc = 0;
    if (!path)
    {
        errno = EFAULT;
        return -1;
    }
    if(is_pvfs_path(&path,0))
    { 
        rc = pvfs_rmdir(path);
    }
    else
    {
        rc = glibc_ops.rmdir(path);
    }
    PVFS_free_expanded(path);
    return rc;
}

#if __GLIBC_PREREQ (2,5)
ssize_t readlink(const char *path, char *buf, size_t bufsiz)
#else
int readlink(const char *path, char *buf, size_t bufsiz)
#endif
{
    int rc = 0;
    if (!path || !buf)
    {
        errno = EFAULT;
        return -1;
    }
    if(is_pvfs_path(&path,1))
    { 
        rc = pvfs_readlink(path, buf, bufsiz);
    }
    else
    {
        rc = glibc_ops.readlink(path, buf, bufsiz);
    }
    PVFS_free_expanded(path);
    return rc;
}

ssize_t readlinkat(int dirfd, const char *path, char *buf, size_t bufsiz)
{
    int rc = 0; 
    pvfs_descriptor *pd; 
    
    if (!path || !buf)
    {
        errno = EFAULT;
        return -1;
    }
    if (dirfd == AT_FDCWD || (path && path[0] == '/'))
    {
        readlink(path, buf, bufsiz);
    }
    else
    {
        pd = pvfs_find_descriptor(dirfd);
        if (pd && pd->is_in_use && dirfd == pd->fd &&
            pd->s && pd->s->fsops)
        {
            rc = pd->s->fsops->readlinkat(pd->true_fd, path, buf, bufsiz);
        }
        else
        {
            errno = EBADF;
            rc = -1;
        }
    }
    return rc;
}

int symlink(const char *oldpath, const char *newpath)
{
    int rc = 0;
    if (!oldpath || !newpath)
    {
        errno = EFAULT;
        return -1;
    }
    if(is_pvfs_path(&newpath,0))
    { 
        rc = pvfs_symlink(oldpath, newpath);
    }
    else
    {
        rc = glibc_ops.symlink(oldpath, newpath);
    }
    PVFS_free_expanded(newpath);
    return rc;
}

int symlinkat(const char *oldpath, int newdirfd, const char *newpath)
{
    int rc = 0; 
    pvfs_descriptor *pd; 
    
    if (!oldpath || !newpath)
    {
        errno = EFAULT;
        return -1;
    }
    if (newdirfd == AT_FDCWD || (newpath && newpath[0] == '/'))
    {
        symlink(oldpath, newpath);
    }
    else
    {
        pd = pvfs_find_descriptor(newdirfd);
        if (pd && pd->is_in_use && newdirfd == pd->fd &&
            pd->s && pd->s->fsops)
        {
            rc = pd->s->fsops->symlinkat(oldpath, pd->true_fd, newpath);
        }
        else
        {
            errno = EBADF;
            rc = -1;
        }
    }
    return rc;
}

int link(const char *oldpath, const char *newpath)
{
    int rc = 0;
    if (!oldpath || !newpath)
    {
        errno = EFAULT;
        return -1;
    }
    if(is_pvfs_path(&oldpath,0))
    { 
        rc = pvfs_link(oldpath, newpath);
    }
    else
    {
        rc = glibc_ops.link(oldpath, newpath);
    }
    PVFS_free_expanded(oldpath);
    return rc;
}

int linkat(int olddirfd, const char *old,
           int newdirfd, const char *new, int flags)
{
    pvfs_descriptor *oldpd, *newpd;
    oldpd = pvfs_find_descriptor(olddirfd);
    newpd = pvfs_find_descriptor(newdirfd);
    if (!old || !new)
    {
        errno = EFAULT;
        return -1;
    }
    if ((!oldpd) || (!oldpd->is_in_use) || (olddirfd != oldpd->fd) ||
        (!newpd) || (!newpd->is_in_use) || (newdirfd != newpd->fd) ||
        (!oldpd->s) || (!newpd->s) || (!oldpd->s->fsops))
    {
        errno = EBADF;
        return -1;
    }
    if (oldpd->s->fsops == newpd->s->fsops)
    {
        return oldpd->s->fsops->linkat(oldpd->true_fd,
                                       old,
                                       newpd->true_fd,
                                       new,
                                       flags);
    }
    else
    {
        errno = EXDEV;
        return -1;
    }
}

/**
 * According to man page count is ignored
 */
int posix_readdir(unsigned int fd, struct dirent *dirp, unsigned int count)
{
    int rc = 0;
    pvfs_descriptor *pd;
    
    if (!dirp)
    {
        errno = EFAULT;
        return -1;
    }
    pd = pvfs_find_descriptor(fd);
    if (pd && pd->is_in_use && fd == pd->fd &&
        pd->s && pd->s->fsops)
    {
        rc = pd->s->fsops->readdir(pd->true_fd, dirp, count);
    }
    else
    {
        errno = EBADF;
        rc = -1;
    }
    return rc;
}


/**
 * man page calls last arg count but is ambiguous if it is number
 * of bytes or number of records to read.  The former appears to be
 * true thus we rename the argument
 */
int getdents(unsigned int fd, struct dirent *dirp, unsigned int size)
{
    int rc = 0;
    pvfs_descriptor *pd;
    
    if (!dirp)
    {
        errno = EFAULT;
        return -1;
    }
    pd = pvfs_find_descriptor(fd);
    if (pd && pd->is_in_use && fd == pd->fd &&
        pd->s && pd->s->fsops)
    {
        rc = pd->s->fsops->getdents(pd->true_fd, dirp, size);
    }
    else
    {
        errno = EBADF;
        rc = -1;
    }
    return rc;
}
#if __GLIBC__ <=2 && __GLIBC_MINOR__ < 30
int getdents64(unsigned int fd, struct dirent64 *dirp, unsigned int size)
{
    int rc = 0;
    pvfs_descriptor *pd;
    
    if (!dirp)
    {
        errno = EFAULT;
        return -1;
    }
    pd = pvfs_find_descriptor(fd);
    if (pd && pd->is_in_use && fd == pd->fd &&
        pd->s && pd->s->fsops)
    {
        rc = pd->s->fsops->getdents64(pd->true_fd, dirp, size);
    }
    else
    {
        errno = EBADF;
        rc = -1;
    }
    return rc;
}
#endif

/* linux discourages using readdir system calls, so for now
 * we will leave them out - there are stdio calls that can
 * be used
 */

int access(const char *path, int mode)
{
    int rc = 0;
    if (!path)
    {
        errno = EFAULT;
        return -1;
    }
    if(is_pvfs_path(&path,0))
    { 
        rc = pvfs_access(path, mode);
    }
    else
    {
        rc = glibc_ops.access(path, mode);
    }
    PVFS_free_expanded(path);
    return rc;
}

int faccessat(int dirfd, const char *path, int mode, int flags)
{
    int rc = 0; 
    pvfs_descriptor *pd; 
    
    if (!path)
    {
        errno = EFAULT;
        return -1;
    }
    if (dirfd == AT_FDCWD || (path && path[0] == '/'))
    {
        access(path, mode);
    }
    else
    {
        pd = pvfs_find_descriptor(dirfd);
        if (pd && pd->is_in_use && dirfd == pd->fd &&
            pd->s && pd->s->fsops)
        {
            rc = pd->s->fsops->faccessat(pd->true_fd, path, mode, flags);
        }
        else
        {
            errno = EBADF;
            rc = -1;
        }
    }
    return rc;
}


int flock(int fd, int op)
{
    int rc = 0;
    pvfs_descriptor *pd;
    
    pd = pvfs_find_descriptor(fd);
    if (pd && pd->is_in_use && fd == pd->fd &&
        pd->s && pd->s->fsops)
    {
        rc = pd->s->fsops->flock(pd->true_fd, op);
    }
    else
    {
        errno = EBADF;
        rc = -1;
    }
    return rc;
}

int fcntl(int fd, int cmd, ...)
{
    int rc = 0;
    long arg;
    struct flock *lock;
    pvfs_descriptor *pd;
    va_list ap;
    
    va_start(ap, cmd);
    pd = pvfs_find_descriptor(fd);
    if (pd && pd->is_in_use && fd == pd->fd &&
        pd->s && pd->s->fsops)
    {
        switch (cmd)
        {
        case F_GETLK:
        case F_SETLK:
        case F_SETLKW:
            lock = va_arg(ap, struct flock *);
            rc = pd->s->fsops->fcntl(pd->true_fd, cmd, lock);
            break;
        default:
            arg = va_arg(ap, long);
            rc = pd->s->fsops->fcntl(pd->true_fd, cmd, arg);
            break;
        }
    }
    else
    {
        errno = EBADF;
        rc = -1;
    }
    va_end(ap);
    return rc;
}

void sync(void)
{
    pvfs_sync();
    glibc_ops.sync();
}

int fsync(int fd)
{
    int rc = 0;
    pvfs_descriptor *pd;
    
    pd = pvfs_find_descriptor(fd);
    if (pd && pd->is_in_use && fd == pd->fd &&
        pd->s && pd->s->fsops)
    {
        rc = pd->s->fsops->fsync(pd->true_fd);
    }
    else
    {
        errno = EBADF;
        rc = -1;
    }
    return rc;
}

int fdatasync(int fd)
{
    int rc = 0;
    pvfs_descriptor *pd;
    
    pd = pvfs_find_descriptor(fd);
    if (pd && pd->is_in_use && fd == pd->fd &&
        pd->s && pd->s->fsops)
    {
        rc = pd->s->fsops->fdatasync(pd->true_fd);
    }
    else
    {
        errno = EBADF;
        rc = -1;
    }
    return rc;
}

int posix_fadvise(int fd, off_t offset, off_t length, int advice)
{
    int rc = 0;
    pvfs_descriptor *pd;
    
    pd = pvfs_find_descriptor(fd);
    if (pd && pd->is_in_use && fd == pd->fd &&
        pd->s && pd->s->fsops)
    {
        rc = pd->s->fsops->fadvise(pd->true_fd, offset, length, advice);
    }
    else
    {
        errno = EBADF;
        rc = -1;
    }
    return rc;
}

/** GlibC doesn't seem to have fadvise or fadvise64
 *  It does have posix_fadvise  Linux has system calls
 *  for fadvise and fadvise64.  Coreutils defines its
 *  own fadvise as operating on a file pointer so this
 *  is commented out here - seems rather arbitrary though
 */
#if 0
int fadvise(int fd, off_t offset, off_t len, int advice)
{
    int rc = 0;
    pvfs_descriptor *pd;
    
    pd = pvfs_find_descriptor(fd);
    if (pd & pd->is_in_use && fd == pd->fd)
    {
        rc = pd->s->fsops->fadvise(pd->true_fd, offset, len, advice);
    }
    else
    {
        errno = EBADF;
        rc = -1;
    }
    return rc;
}
#endif

int fadvise64(int fd, off64_t offset, off64_t len, int advice)
{
    int rc = 0;
    pvfs_descriptor *pd;
    
    pd = pvfs_find_descriptor(fd);
    if (pd && pd->is_in_use && fd == pd->fd &&
        pd->s && pd->s->fsops)
    {
        rc = pd->s->fsops->fadvise64(pd->true_fd, offset, len, advice);
    }
    else
    {
        errno = EBADF;
        rc = -1;
    }
    return rc;
}

int statfs(const char *path, struct statfs *buf)
{
    int rc = 0;
    if (!path || !buf)
    {
        errno = EFAULT;
        return -1;
    }
    if(is_pvfs_path(&path,0))
    { 
        rc = pvfs_statfs(path, buf);
    }
    else
    {
        rc = glibc_ops.statfs(path, buf);
    }
    PVFS_free_expanded(path);
    return rc;
}

int statfs64(const char *path, struct statfs64 *buf)
{
    int rc = 0;
    if (!path || !buf)
    {
        errno = EFAULT;
        return -1;
    }
    if(is_pvfs_path(&path,0))
    { 
        rc = pvfs_statfs64(path, buf);
    }
    else
    {
        rc = glibc_ops.statfs64(path, buf);
    }
    PVFS_free_expanded(path);
    return rc;
}

int fstatfs(int fd, struct statfs *buf)
{
    int rc = 0;
    pvfs_descriptor *pd;
    
    if (!buf)
    {
        errno = EFAULT;
        return -1;
    }
    pd = pvfs_find_descriptor(fd);
    if (pd && pd->is_in_use && fd == pd->fd &&
        pd->s && pd->s->fsops)
    {
        rc = pd->s->fsops->fstatfs(pd->true_fd, buf);
    }
    else
    {
        errno = EBADF;
        rc = -1;
    }
    return rc;
}

int fstatfs64(int fd, struct statfs64 *buf)
{
    int rc = 0;
    pvfs_descriptor *pd;
    
    if (!buf)
    {
        errno = EFAULT;
        return -1;
    }
    pd = pvfs_find_descriptor(fd);
    if (pd && pd->is_in_use && fd == pd->fd &&
        pd->s && pd->s->fsops)
    {
        rc = pd->s->fsops->fstatfs64(pd->true_fd, buf);
    }
    else
    {
        errno = EBADF;
        rc = -1;
    }
    return rc;
}

int statvfs(const char *path, struct statvfs *buf)
{
    int rc = 0;
    if (!path || !buf)
    {
        errno = EFAULT;
        return -1;
    }
    if(is_pvfs_path(&path,0))
    { 
        rc = pvfs_statvfs(path, buf);
    }
    else
    {
        rc = glibc_ops.statvfs(path, buf);
    }
    PVFS_free_expanded(path);
    return rc;
}

int fstatvfs(int fd, struct statvfs *buf)
{
    int rc = 0;
    pvfs_descriptor *pd;
    
    if (!buf)
    {
        errno = EFAULT;
        return -1;
    }
    pd = pvfs_find_descriptor(fd);
    if (pd && pd->is_in_use && fd == pd->fd &&
        pd->s && pd->s->fsops)
    {
        rc = pd->s->fsops->fstatvfs(pd->true_fd, buf);
    }
    else
    {
        errno = EBADF;
        rc = -1;
    }
    return rc;
}

int mknod(const char *path, mode_t mode, dev_t dev)
{
    int rc = 0;
    if (!path)
    {
        errno = EFAULT;
        return -1;
    }
    if(is_pvfs_path(&path,0))
    { 
        rc = pvfs_mknod(path, mode, dev);
    }
    else
    {
        rc = glibc_ops.mknod(path, mode, dev);
    }
    PVFS_free_expanded(path);
    return rc;
}

int mknodat(int dirfd, const char *path, mode_t mode, dev_t dev)
{            
    int rc = 0; 
    pvfs_descriptor *pd; 
    
    if (!path)
    {
        errno = EFAULT;
        return -1;
    }
    if (dirfd == AT_FDCWD || path[0] == '/')
    {
        mknod(path, mode, dev);
    }
    else
    {
        pd = pvfs_find_descriptor(dirfd);
        if (pd && pd->is_in_use && dirfd == pd->fd &&
            pd->s && pd->s->fsops)
        {
            rc = pd->s->fsops->mknodat(pd->true_fd, path, mode, dev);
        }
        else
        {
            errno = EBADF;
            rc = -1;
        }
    }
    return rc;
}

ssize_t sendfile(int outfd, int infd, off_t *offset, size_t count)
{
    return sendfile64(outfd, infd, (off64_t *)offset, count);
}

ssize_t sendfile64(int outfd, int infd, off64_t *offset, size_t count)
{
    int rc = 0;
    pvfs_descriptor *inpd, *outpd;
    
    inpd = pvfs_find_descriptor(infd);
    outpd = pvfs_find_descriptor(outfd);
    if (inpd && inpd->is_in_use && infd == inpd->fd &&
        outpd && outpd->is_in_use && outfd == outpd->fd &&
        inpd->s && inpd->s->fsops)
    {
        rc = inpd->s->fsops->sendfile64(outpd->true_fd, inpd->true_fd,
                                     offset, count);
    }
    else
    {
        errno = EBADF;
        rc = -1;
    }
    return rc;
}

int setxattr(const char *path, const char *name,
             const void *value, size_t size, int flags)
{
    int rc = 0;
    if (!path)
    {
        errno = EFAULT;
        return -1;
    }
    if(is_pvfs_path(&path, 0))
    { 
        rc = pvfs_setxattr(path, name, value, size, flags);
    }
    else
    {
        if (glibc_ops.setxattr)
        {
            rc = glibc_ops.setxattr(path, name, value, size, flags);
        }
        else
        {
            errno = ENOTSUP;
            goto errorout;
        }
    }
    goto cleanup;
errorout:
    rc = -1;
cleanup:
    PVFS_free_expanded(path);
    return rc;
}

int lsetxattr(const char *path, const char *name,
              const void *value, size_t size, int flags)
{
    int rc = 0;
    if (!path)
    {
        errno = EFAULT;
        return -1;
    }
    if(is_pvfs_path(&path,1))
    { 
        rc = pvfs_lsetxattr(path, name, value, size, flags);
    }
    else
    {
        if (glibc_ops.lsetxattr)
        {
            rc = glibc_ops.lsetxattr(path, name, value, size, flags);
        }
        else
        {
            errno = ENOTSUP;
            goto errorout;
        }
    }
    goto cleanup;
errorout:
    rc = -1;
cleanup:
    PVFS_free_expanded(path);
    return rc;
}

int fsetxattr(int fd, const char *name,
              const void *value, size_t size, int flags)
{
    int rc = 0;
    pvfs_descriptor *pd;
    
    pd = pvfs_find_descriptor(fd);
    if (pd && pd->is_in_use && fd == pd->fd &&
        pd->s && pd->s->fsops)
    {
        if (pd->s->fsops->fsetxattr)
        {
            rc = pd->s->fsops->fsetxattr(pd->true_fd, name, value, size, flags);
        }
        else
        {
            errno = ENOTSUP;
            rc = -1;
        }
    }
    else
    {
        errno = EBADF;
        rc = -1;
    }
    return rc;
}

ssize_t getxattr(const char *path, const char *name,
                 void *value, size_t size)
{
    int rc = 0;
    if (!path)
    {
        errno = EFAULT;
        return -1;
    }
    if (is_pvfs_path(&path, 0))
    { 
        rc = pvfs_getxattr(path, name, value, size);
    }
    else
    {
        if (glibc_ops.getxattr)
        {
            rc = glibc_ops.getxattr(path, name, value, size);
        }
        else
        {
            errno = ENOTSUP;
            goto errorout;
        }
    }
    goto cleanup;
errorout:
    rc = -1;
cleanup:
    PVFS_free_expanded(path);
    return rc;
}

ssize_t lgetxattr(const char *path, const char *name,
                  void *value, size_t size)
{
    int rc = 0;
    if (!path)
    {
        errno = EFAULT;
        return -1;
    }
    if(is_pvfs_path(&path, 1))
    { 
        rc = pvfs_lgetxattr(path, name, value, size);
    }
    else
    {
        if (glibc_ops.lgetxattr)
        {
            rc = glibc_ops.lgetxattr(path, name, value, size);
        }
        else
        {
            errno = ENOTSUP;
            goto errorout;
        }
    }
    goto cleanup;
errorout:
    rc = -1;
cleanup:
    PVFS_free_expanded(path);
    return rc;
}

ssize_t fgetxattr(int fd, const char *name, void *value,
                  size_t size)
{
    int rc = 0;
    pvfs_descriptor *pd;
    
    pd = pvfs_find_descriptor(fd);
    if (pd && pd->is_in_use && fd == pd->fd &&
        pd->s && pd->s->fsops)
    {
        if (pd->s->fsops->fgetxattr)
        {
            rc = pd->s->fsops->fgetxattr(pd->true_fd, name, value, size);
        }
        else
        {
            errno = ENOTSUP;
            rc = -1;
        }
    }
    else
    {
        errno = EBADF;
        rc = -1;
    }
    return rc;
}

ssize_t listxattr(const char *path, char *list, size_t size)
{
    int rc = 0;
    if (!path)
    {
        errno = EFAULT;
        return -1;
    }
    if(is_pvfs_path(&path,0))
    { 
        rc = pvfs_listxattr(path, list, size);
    }
    else
    {
        if (glibc_ops.listxattr)
        {
            rc = glibc_ops.listxattr(path, list, size);
        }
        else
        {
            errno = ENOTSUP;
            goto errorout;
        }
    }
    goto cleanup;
errorout:
    rc = -1;
cleanup:
    PVFS_free_expanded(path);
    return rc;
}

ssize_t llistxattr(const char *path, char *list, size_t size)
{
    int rc = 0;
    if (!path)
    {
        errno = EFAULT;
        return -1;
    }
    if(is_pvfs_path(&path,1))
    { 
        rc = pvfs_llistxattr(path, list, size);
    }
    else
    {
        if (glibc_ops.llistxattr)
        {
            rc = glibc_ops.llistxattr(path, list, size);
        }
        else
        {
            errno = ENOTSUP;
            goto errorout;
        }
    }
    goto cleanup;
errorout:
    rc = -1;
cleanup:
    PVFS_free_expanded(path);
    return rc;
}

ssize_t flistxattr(int fd, char *list, size_t size)
{
    int rc = 0;
    pvfs_descriptor *pd;
    
    pd = pvfs_find_descriptor(fd);
    if (pd && pd->is_in_use && fd == pd->fd &&
        pd->s && pd->s->fsops)
    {
        if (pd->s->fsops->flistxattr)
        {
            rc = pd->s->fsops->flistxattr(pd->true_fd, list, size);
        }
        else
        {
            errno = ENOTSUP;
            rc = -1;
        }
    }
    else
    {
        errno = EBADF;
        rc = -1;
    }
    return rc;
}

int removexattr(const char *path, const char *name)
{
    int rc = 0;
    if (!path)
    {
        errno = EFAULT;
        return -1;
    }
    if(is_pvfs_path(&path,0))
    { 
        rc = pvfs_removexattr(path, name);
    }
    else
    {
        if (glibc_ops.removexattr)
        {
            rc = glibc_ops.removexattr(path, name);
        }
        else
        {
            errno = ENOTSUP;
            goto errorout;
        }
    }
    goto cleanup;
errorout:
    rc = -1;
cleanup:
    PVFS_free_expanded(path);
    return rc;
}

int lremovexattr(const char *path, const char *name)
{
    int rc = 0;
    if (!path)
    {
        errno = EFAULT;
        return -1;
    }
    if(is_pvfs_path(&path,1))
    { 
        rc = pvfs_lremovexattr(path, name);
    }
    else
    {
        if (glibc_ops.lremovexattr)
        {
            rc = glibc_ops.lremovexattr(path, name);
        }
        else
        {
            errno = ENOTSUP;
            goto errorout;
        }
    }
    goto cleanup;
errorout:
    rc = -1;
cleanup:
    PVFS_free_expanded(path);
    return rc;
}

int fremovexattr(int fd, const char *name)
{
    int rc = 0;
    pvfs_descriptor *pd;
    
    pd = pvfs_find_descriptor(fd);
    if (pd && pd->is_in_use && fd == pd->fd &&
        pd->s && pd->s->fsops)
    {
        if (pd->s->fsops->fremovexattr)
        {
            rc = pd->s->fsops->fremovexattr(pd->true_fd, name);
        }
        else
        {
            errno = ENOTSUP;
            rc = -1;
        }
    }
    else
    {
        errno = EBADF;
        rc = -1;
    }
    return rc;
}

/* these functions allow the library to take over 
.* management of the currrent working directory
 * all of the actual code is in the pvfs versions
 * of these functions - these are only wrappers
 * to catch calls to libc
 *
 * if the kernel module is used to mount the FS
 * then there is no need for these
 */
#if PVFS_USRINT_CWD

int chdir(const char *path)
{
    return pvfs_chdir(path);
}

int fchdir(int fd)
{
    return pvfs_fchdir(fd);
}

char *getcwd(char *buf, size_t size)
{
    return pvfs_getcwd(buf, size);
}

char *get_current_dir_name(void)
{
    return pvfs_get_current_dir_name();
}

char *getwd(char *buf)
{
    return pvfs_getwd(buf);
}

mode_t umask(mode_t mask)
{
    return pvfs_umask(mask);
}

mode_t getumask(void)
{
    return pvfs_getumask();
}

int getdtablesize(void)
{
    return pvfs_getdtablesize();
}

#endif /* PVFS_USRINT_CWD */

/* add a configure option to enable this */
#if 0
struct posix_ops_s ofs_sys_ops =
{ 
    .snprintf = NULL,
    .open = open,
    .open64 = open64,
    .openat = openat,
    .openat64 = openat64,
    .creat = creat,
    .creat64 = creat64,
    .unlink = unlink,
    .unlinkat = unlinkat,
    .rename = rename,
    .renameat = renameat,
    .read = read,
    .pread = pread,
    .readv = readv,
    .pread64 = pread64,
    .write = write,
    .pwrite = pwrite,
    .writev = writev,
    .pwrite64 = pwrite64,
    .lseek = lseek,
    .lseek64 = lseek64,
    .perror = perror,
    .truncate = truncate,
    .truncate64 = truncate64,
    .ftruncate = ftruncate,
    .ftruncate64 = ftruncate64,
    .fallocate = posix_fallocate,
    .close = close,
    .stat = stat,
    .stat64 = stat64,
    .fstat = fstat,
    .fstat64 = fstat64,
    .fstatat = fstatat,
    .fstatat64 = fstatat64,
    .lstat = lstat,
    .lstat64 = lstat64,
    .futimesat = futimesat,
    .utimes = utimes,
    .utime = utime,
    .futimes = futimes,
    .dup = dup,
    .dup2 = dup2,
    .dup3 = dup3,
    .chown = chown,
    .fchown = fchown,
    .fchownat = fchownat,
    .lchown = lchown,
    .chmod = chmod,
    .fchmod = fchmod,
    .fchmodat = fchmodat,
    .mkdir = mkdir,
    .mkdirat = mkdirat,
    .rmdir = rmdir,
    .readlink = readlink,
    .readlinkat = readlinkat,
    .symlink = symlink,
    .symlinkat = symlinkat,
    .link = link,
    .linkat = linkat,
    .readdir = posix_readdir,
    .getdents = getdents,
    .getdents64 = getdents64,
    .access = access,
    .faccessat = faccessat,
    .flock = flock,
    .fcntl = fcntl,
    .sync = sync,
    .fsync = fsync,
    .fdatasync = fdatasync,
    .fadvise = posix_fadvise,
    .fadvise64 = fadvise64,
    .statfs = statfs,
    .statfs64 = statfs64,
    .fstatfs = fstatfs,
    .fstatfs64 = fstatfs64,
    .statvfs = statvfs,
    .fstatvfs = fstatvfs,
    .mknod = mknod,
    .mknodat = mknodat,
    .sendfile = sendfile,
    .sendfile64 = sendfile64,
#ifdef HAVE_ATTR_XATTR_H
    .setxattr = setxattr,
    .lsetxattr = lsetxattr,
    .fsetxattr = fsetxattr,
    .getxattr = getxattr,
    .lgetxattr = lgetxattr,
    .fgetxattr = fgetxattr,
    .listxattr = listxattr,
    .llistxattr = llistxattr,
    .flistxattr = flistxattr,
    .removexattr = removexattr,
    .lremovexattr = lremovexattr,
    .fremovexattr = fremovexattr,
#endif
    .socket = NULL,
    .accept = NULL,
    .bind = NULL,
    .connect = NULL,
    .getpeername = NULL,
    .getsockname = NULL,
    .getsockopt = NULL,
    .setsockopt = NULL,
    .ioctl = NULL,
    .listen = NULL,
    .recv = NULL,
    .recvfrom = NULL,
    .recvmsg = NULL,
    .send = NULL,
    .sendto = NULL,
    .sendmsg = NULL,
    .shutdown = NULL,
    .socketpair = NULL,
    .pipe = NULL,
    .umask = umask,
    .getumask = getumask,
    .getdtablesize = getdtablesize,
    .mmap = mmap,
    .munmap = munmap,
    .msync = msync
#if 0
    , .acl_delete_def_file = acl_delete_def_file
    , .acl_get_fd = acl_get_fd
    , .acl_get_file = acl_get_file
    , .acl_set_fd = acl_set_fd
    , .acl_set_file = acl_set_file
#endif
    , .getfscreatecon =  getfscreatecon
    , .getfilecon = getfilecon
    , .lgetfilecon = lgetfilecon
    , .fgetfilecon = fgetfilecon
    , .setfscreatecon = setfscreatecon
    , .setfilecon = setfilecon
    , .lsetfilecon = lsetfilecon
    , .fsetfilecon = fsetfilecon
};
#endif

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
