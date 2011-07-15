/* 
 * (C) 2011 Clemson University and The University of Chicago 
 *
 * See COPYING in top-level directory.
 */

/** \file
 *  \ingroup usrint
 *
 *  PVFS2 user interface routines - pvfs version of posix system calls
 */
#include <usrint.h>
#include <linux/dirent.h>
#include <posix-ops.h>
#include <posix-pvfs.h>
#include <openfile-util.h>
#include <iocommon.h>

static mode_t mask_val = 0022; /* implements umask for pvfs library */

/* actual implementation of read and write are in these static funcs */

static ssize_t pvfs_prdwr64(int fd,
                            void *buf,
                            size_t count,
                            off64_t offset,
                            int which);

static ssize_t pvfs_rdwrv(int fd,
                          const struct iovec *vector,
                          size_t count,
                          int which);

/**
 *  pvfs_open
 */
int pvfs_open(const char *path, int flags, ...)
{
    va_list ap;
    int mode;
    PVFS_hint hints;
    char *newpath;
    pvfs_descriptor *pd;

    if (!path)
    {
        errno = EINVAL;
        return -1;
    }
    va_start(ap, flags);
    if (flags & O_CREAT)
        mode = va_arg(ap, int);
    else
        mode = 0777;
    if (flags & O_HINTS)
        hints = va_arg(ap, PVFS_hint);
    else
        hints = PVFS_HINT_NULL;
    va_end(ap);

    /* fully qualify pathname */
    newpath = pvfs_qualify_path(path);
    pd = iocommon_open(newpath, flags, hints, mode, NULL);
    if (newpath != path)
    {
        free(newpath);
    }
    if (!pd)
    {
        return -1;
    }
    return pd->fd;
}

/**
 * pvfs_open64
 */
int pvfs_open64(const char *path, int flags, ...)
{
    va_list ap;
    int mode;
    PVFS_hint hints;

    if (!path)
    {
        errno = EINVAL;
        return -1;
    }
    va_start(ap, flags);
    if (flags & O_CREAT)
    {
        mode = va_arg(ap, int);
    }
    else
    {
        mode = 0777;
    }
    if (flags & O_HINTS)
    {
        hints = va_arg(ap, PVFS_hint);
    }
    else
    {
        hints = PVFS_HINT_NULL;
    }
    va_end(ap);
    flags |= O_LARGEFILE;
    return pvfs_open(path, flags, mode);
}

/**
 * pvfs_openat
 */
int pvfs_openat(int dirfd, const char *path, int flags, ...)
{
    va_list ap;
    int mode;
    PVFS_hint hints;
    pvfs_descriptor *dpd, *fpd;

    if (!path)
    {
        errno - EINVAL;
        return -1;
    }
    va_start(ap, flags);
    if (flags & O_CREAT)
    {
        mode = va_arg(ap, int);
    }
    else
    {
        mode = 0777;
    }
    if (flags & O_HINTS)
    {
        hints = va_arg(ap, PVFS_hint);
    }
    else
    {
        hints = PVFS_HINT_NULL;
    }
    va_end(ap);
    if (path[0] == '/' || dirfd == AT_FDCWD)
    {
        return pvfs_open(path, flags, mode);
    }
    else
    {
        if (dirfd < 0)
        {
            errno = EBADF;
            return -1;
        }
        dpd = pvfs_find_descriptor(dirfd);
        if (!dpd)
        {
            return -1;
        }
        fpd = iocommon_open(path, flags, hints, mode, &dpd->pvfs_ref);
        if (!fpd)
        {
            return -1;
        }
        return fpd->fd;
    }
}

/**
 * pvfs_openat64
 */
int pvfs_openat64(int dirfd, const char *path, int flags, ...)
{
    va_list ap;
    int mode;
    PVFS_hint hints;
    pvfs_descriptor *pd;

    if (dirfd < 0)
    {
        errno = EBADF;
        return -1;
    }
    va_start(ap, flags);
    if (flags & O_CREAT)
    {
        mode = va_arg(ap, int);
    }
    else
    {
        mode = 0777;
    }
    if (flags & O_HINTS)
    {
        hints = va_arg(ap, PVFS_hint);
    }
    else
    {
        hints = PVFS_HINT_NULL;
    }
    va_end(ap);
    flags |= O_LARGEFILE;
    return pvfs_openat(dirfd, path, flags, mode);
}

/**
 * pvfs_creat wrapper
 */
int pvfs_creat(const char *path, mode_t mode, ...)
{
    return pvfs_open(path, O_RDWR | O_CREAT | O_EXCL, mode);
}

/**
 * pvfs_creat64 wrapper
 */
int pvfs_creat64(const char *path, mode_t mode, ...)
{
    return pvfs_open64(path, O_RDWR | O_CREAT | O_EXCL, mode);
}

/**
 * pvfs_unlink
 */
int pvfs_unlink(const char *path)
{
    int rc = 0;
    char *newpath;
    pvfs_descriptor *pd;

    newpath = pvfs_qualify_path(path);
    rc = iocommon_unlink(path, NULL);
    if (newpath != path)
    {
        free(newpath);
    }
    return rc;
}

/**
 * pvfs_unlinkat
 */
int pvfs_unlinkat(int dirfd, const char *path, int flags)
{
    int rc;
    pvfs_descriptor *pd;

    if (path[0] == '/' || dirfd == AT_FDCWD)
    {
        rc = iocommon_unlink(path, NULL);
    }
    else
    {
        if (dirfd < 0)
        {
            errno = EBADF;
            return -1;
        }       
        pd = pvfs_find_descriptor(dirfd);
        if (!pd)
        {
            errno = EBADF;
            return -1;
        }
        rc = iocommon_unlink(path, &pd->pvfs_ref);
    }
    return rc;
}

/**
 * pvfs_rename
 */
int pvfs_rename(const char *oldpath, const char *newpath)
{
    int rc;
    char *absoldpath, *absnewpath;

    absoldpath = pvfs_qualify_path(oldpath);
    absnewpath = pvfs_qualify_path(newpath);
    rc = iocommon_rename(NULL, absoldpath, NULL, absnewpath);
    if (oldpath != absoldpath)
    {
        free(absoldpath);
    }
    if (newpath != absnewpath)
    {
        free(absnewpath);
    }
    return rc;
}

/**
 * pvfs_renameat
 */
int pvfs_renameat(int olddirfd, const char *oldpath,
                  int newdirfd, const char *newpath)
{
    int rc;
    pvfs_descriptor *pd;
    PVFS_object_ref *olddirref, *newdirref;
    char *absoldpath, *absnewpath;

    if (!oldpath || !newpath)
    {
        errno = EINVAL;
        return -1;
    }
    if (oldpath[0] == '/' || olddirfd == AT_FDCWD)
    {
        olddirref = NULL;
        absoldpath = pvfs_qualify_path(oldpath);
    }
    else
    {
        if (olddirfd < 0)
        {
            errno = EBADF;
            return -1;
        }
        pd = pvfs_find_descriptor(olddirfd);
        if (!pd)
        {
            errno = EBADF;
            return -1;
        }
        olddirref = &pd->pvfs_ref;
        absoldpath = (char *)oldpath;
    }
    if (oldpath[0] == '/' || newdirfd == AT_FDCWD)
    {
        newdirref = NULL;
        absnewpath = pvfs_qualify_path(newpath);
    }
    else
    {
        if (newdirfd < 0)
        {
            errno = EBADF;
            return -1;
        }
        pd = pvfs_find_descriptor(newdirfd);
        if (!pd)
        {
            errno = EBADF;
            return -1;
        }
        newdirref = &pd->pvfs_ref;
        absnewpath = (char *)newpath;
    }
    rc = iocommon_rename(olddirref, absoldpath, newdirref, absnewpath);
    if (oldpath != absoldpath)
    {
        free(absoldpath);
    }
    if (newpath != absnewpath)
    {
        free(absnewpath);
    }
    return rc;
}

/**
 * pvfs_read wrapper
 */
ssize_t pvfs_read(int fd, void *buf, size_t count)
{
    int rc;

    if (fd < 0)
    {
        errno = EBADF;
        return -1;
    }
    pvfs_descriptor *pd = pvfs_find_descriptor(fd);
    if (!pd)
    {
        return -1;
    }
    rc = pvfs_prdwr64(fd, buf, count, pd->file_pointer, PVFS_IO_READ);
    if (rc < 0)
    {
        return -1;
    }
    pd->file_pointer += rc;
    return rc;
}

/**
 * pvfs_pread wrapper
 */
ssize_t pvfs_pread(int fd, void *buf, size_t count, off_t offset)
{
    return pvfs_prdwr64(fd, buf, count, (off64_t) offset, PVFS_IO_READ);
}

/**
 * pvfs_readv wrapper
 */
ssize_t pvfs_readv(int fd, const struct iovec *vector, int count)
{
    return pvfs_rdwrv(fd, vector, count, PVFS_IO_READ);
}

/**
 * pvfs_pread64 wrapper
 */
ssize_t pvfs_pread64( int fd, void *buf, size_t count, off64_t offset )
{
    return pvfs_prdwr64(fd, buf, count, offset, PVFS_IO_READ);
}

/**
 * pvfs_write wrapper
 */
ssize_t pvfs_write(int fd, const void *buf, size_t count)
{
    int rc;

    if (fd < 0)
    {
        errno = EBADF;
        return -1;
    }
    pvfs_descriptor *pd = pvfs_find_descriptor(fd);
    if (!pd)
    {
        return -1;
    }
    rc = pvfs_prdwr64(fd, (void *)buf, count, pd->file_pointer, PVFS_IO_WRITE);
    if (rc < 0)
    {
        return -1;
    }
    pd->file_pointer += rc;
    return rc;
}

/**
 * pvfs_pwrite wrapper
 */
ssize_t pvfs_pwrite(int fd, const void *buf, size_t count, off_t offset)
{
    return pvfs_prdwr64(fd, (void *)buf, count, (off64_t)offset, PVFS_IO_WRITE);
}

/**
 * pvfs_writev wrapper
 */
ssize_t pvfs_writev(int fd, const struct iovec *vector, int count)
{
    return pvfs_rdwrv(fd, vector, count, PVFS_IO_WRITE);
}

/**
 * pvfs_pwrite64 wrapper
 */
ssize_t pvfs_pwrite64(int fd, const void *buf, size_t count, off64_t offset)
{
    return pvfs_prdwr64(fd, (void *)buf, count, offset, PVFS_IO_WRITE);
}

/**
 * implements pread and pwrite with 64-bit file pointers
 */
static ssize_t pvfs_prdwr64(int fd,
                            void *buf,
                            size_t count,
                            off64_t offset,
                            int which)
{
    int rc;
    pvfs_descriptor* pd;
    PVFS_Request freq, mreq;

    memset(&freq, 0, sizeof(freq));
    memset(&mreq, 0, sizeof(mreq));

    /* Find the descriptor */
    pd = pvfs_find_descriptor(fd);
    if (!pd)
    {
        errno = EBADF;
        return -1;
    }

    rc = PVFS_Request_contiguous(count, PVFS_BYTE, &freq);
    rc = PVFS_Request_contiguous(count, PVFS_BYTE, &mreq);

    rc = iocommon_readorwrite(which, pd, offset, buf, mreq, freq);

    PVFS_Request_free(&freq);
    PVFS_Request_free(&mreq);

    return rc;
}

/**
 * implements readv and writev
 */
static ssize_t pvfs_rdwrv(int fd,
                          const struct iovec *vector,
                          size_t count,
                          int which)
{
    int rc;
    pvfs_descriptor* pd;
    PVFS_Request freq, mreq;
    off64_t offset;
    void *buf;

    memset(&freq, 0, sizeof(freq));
    memset(&mreq, 0, sizeof(mreq));

    /* Find the descriptor */
    pd = pvfs_find_descriptor(fd);
    if(!pd)
    {
        return -1;
    }
    offset = pd->file_pointer;

    rc = PVFS_Request_contiguous(count, PVFS_BYTE, &freq);
    rc = pvfs_convert_iovec(vector, count, &mreq, &buf);

    rc = iocommon_readorwrite(which, pd, offset, buf, mreq, freq);

    if (rc >= 0)
    {
        pd->file_pointer += rc;
    }

    PVFS_Request_free(&freq);
    PVFS_Request_free(&mreq);

    return rc;
}

/**
 * pvfs_lseek wrapper
 */
off_t pvfs_lseek(int fd, off_t offset, int whence)
{
    return (off_t) pvfs_lseek64(fd, (off64_t)offset, whence);
}

/**
 * pvfs_lseek64
 */
off64_t pvfs_lseek64(int fd, off64_t offset, int whence)
{
    pvfs_descriptor* pd;

    if (fd < 0)
    {
        errno = EBADF;
        return -1;
    }
    /* Find the descriptor */
    pd = pvfs_find_descriptor(fd);
    if (!pd)
    {
        errno = EBADF;
        return -1;
    }

    iocommon_lseek(pd, offset, 1, whence);

    return pd->file_pointer;
}

/**
 * pvfs_truncate wrapper
 */
int pvfs_truncate(const char *path, off_t length)
{
    return pvfs_truncate64(path, (off64_t) length);
}

/**
 * pvfs_truncate64
 */
int pvfs_truncate64(const char *path, off64_t length)
{
    int rc;
    pvfs_descriptor *pd;

    if (!path)
    {
        errno = EINVAL;
        return -1;
    }
    pd = iocommon_open(path, O_WRONLY, PVFS_HINT_NULL, 0 , NULL);
    if (!pd)
    {
        return -1;
    }
    rc = iocommon_truncate(pd->pvfs_ref, length);
    pvfs_close(pd->fd);
    return rc;
}

/**
 * pvfs_allocate wrapper
 *
 * This isn't right but we dont' have a syscall to match this.
 * Best effort is to tuncate to thex size, which should guarantee
 * spaceis available starting at beginning (let alone offset)
 * extending to offset+length.
 *
 * Our truncate doesn't always allocate blocks either, since
 * the underlying FS may have a sparse implementation.
 */
int pvfs_fallocate(int fd, off_t offset, off_t length)
{
    if (offset < 0 || length < 0)
    {
        errno = EINVAL;
        return -1;
    }
    /* if (file_size < offset + length)
    /* {
     */
    return pvfs_ftruncate64(fd, (off64_t)(offset) + (off64_t)(length));
}

/**
 * pvfs_ftruncate wrapper
 */
int pvfs_ftruncate(int fd, off_t length)
{
    return pvfs_ftruncate64(fd, (off64_t) length);
}

/**
 * pvfs_ftruncate64
 */
int pvfs_ftruncate64(int fd, off64_t length)
{
    pvfs_descriptor *pd;
    
    if (fd < 0)
    {
        errno = EBADF;
        return -1;
    }
    pd = pvfs_find_descriptor(fd);
    if (!pd)
    {
        return -1;
    }
    return iocommon_truncate(pd->pvfs_ref, length);
}

/**
 * pvfs_close
 *
 * TODO: add open/close count to minimize metadata ops
 * this may only work if we have multi-user caching
 * which we don't for now
 */
int pvfs_close(int fd)
{
    pvfs_descriptor* pd;

    if (fd < 0)
    {
        errno = EBADF;
        return -1;
    }
    pd = pvfs_find_descriptor(fd);
    if (!pd)
    {
        errno = EBADF;
        return PVFS_FD_FAILURE;
    }

    /* flush buffers */
    pvfs_flush(fd);

    /* free descriptor */
    pvfs_free_descriptor(fd);

    return PVFS_FD_SUCCESS;
}

/**
 * pvfs_flush
 */
int pvfs_flush(int fd)
{
    pvfs_descriptor* pd;

#ifdef DEBUG
    pvfs_debug("in pvfs_flush(%ld)\n", fd);
#endif

    if (fd < 0)
    {
        errno = EBADF;
        return -1;
    }
    /* Find the descriptor */
    pd = pvfs_find_descriptor(fd);
    if (!pd)
    {
        errno = EBADF;
        return PVFS_FD_FAILURE;
    }

    /* tell the server to flush data to disk */
    return iocommon_fsync(pd);
}

/* various flavors of stat */
/**
 * pvfs_stat
 */
int pvfs_stat(const char *path, struct stat *buf)
{
    return pvfs_stat_mask(path, buf, PVFS_ATTR_SYS_ALL_NOHINT);
}

int pvfs_stat_mask(const char *path, struct stat *buf, uint32_t mask)
{
    int rc;
    char *newpath;
    pvfs_descriptor *pd;

    newpath = pvfs_qualify_path(path);
    pd = iocommon_open(newpath, O_RDONLY, PVFS_HINT_NULL, 0, NULL);
    if (newpath != path)
    {
        free(newpath);
    }
    rc = iocommon_stat(pd, buf, mask);
    pvfs_close(pd->fd);
    return rc;
}

/**
 * pvfs_stat64
 */
int pvfs_stat64(const char *path, struct stat64 *buf)
{
    int rc;
    char *newpath;
    pvfs_descriptor *pd;

    newpath = pvfs_qualify_path(path);
    pd = iocommon_open(newpath, O_RDONLY, PVFS_HINT_NULL, 0, NULL);
    if (newpath != path)
    {
        free(newpath);
    }
    rc = iocommon_stat64(pd, buf, PVFS_ATTR_SYS_ALL_NOHINT);
    pvfs_close(pd->fd);
    return rc;
}

/**
 * pvfs_fstat
 */
int pvfs_fstat(int fd, struct stat *buf)
{
    return pvfs_fstat_mask(fd, buf, PVFS_ATTR_SYS_ALL_NOHINT);
}

int pvfs_fstat_mask(int fd, struct stat *buf, uint32_t mask)
{
    pvfs_descriptor *pd;

    if (fd < 0)
    {
        errno = EBADF;
        return -1;
    }
    pd = pvfs_find_descriptor(fd);
    if (!pd)
    {
        errno = EBADF;
        return -1;
    }
    return iocommon_stat(pd, buf, mask);
}

/**
 * pvfs_fstat64
 */
int pvfs_fstat64(int fd, struct stat64 *buf)
{
    pvfs_descriptor *pd;

    if (fd < 0)
    {
        errno = EBADF;
        return -1;
    }
    pd = pvfs_find_descriptor(fd);
    if (!pd)
    {
        errno = EBADF;
        return -1;
    }
    return iocommon_stat64(pd, buf, PVFS_ATTR_SYS_ALL_NOHINT);
}

/**
 * pvfs_fstatat
 */
int pvfs_fstatat(int fd, char *path, struct stat *buf, int flag)
{
    int rc;
    pvfs_descriptor *pd, *pd2;

    if (path[0] == '/' || fd == AT_FDCWD)
    {
        if (flag & AT_SYMLINK_NOFOLLOW)
        {
            rc = pvfs_lstat(path, buf);
        }
        else
        {
            rc = pvfs_stat(path, buf);
        }
    }
    else
    {
        int flags = O_RDONLY;
        if (flag & AT_SYMLINK_NOFOLLOW)
        {
            flags |= O_NOFOLLOW;
        }
        if (fd < 0)
        {
            errno = EBADF;
            return -1;
        }
        pd = pvfs_find_descriptor(fd);
        if (!pd)
        {
            return -1;
        }
        pd2 = iocommon_open(path, flags, PVFS_HINT_NULL, 0, &pd->pvfs_ref);
        rc = iocommon_stat(pd2, buf, PVFS_ATTR_SYS_ALL_NOHINT);
        pvfs_close(pd2->fd);
    }
    return rc;
}

/**
 * pvfs_fstatat64
 */
int pvfs_fstatat64(int fd, char *path, struct stat64 *buf, int flag)
{
    int rc;
    pvfs_descriptor *pd, *pd2;

    if (path[0] == '/' || fd == AT_FDCWD)
    {
        if (flag & AT_SYMLINK_NOFOLLOW)
        {
            rc = pvfs_lstat64(path, buf);
        }
        else
        {
            rc = pvfs_stat64(path, buf);
        }
    }
    else
    {
        int flags = O_RDONLY;
        if (flag & AT_SYMLINK_NOFOLLOW)
        {
            flags |= O_NOFOLLOW;
        }
        if (fd < 0)
        {
            errno = EBADF;
            return -1;
        }
        pd = pvfs_find_descriptor(fd);
        if (!pd)
        {
            errno = EBADF;
            return -1;
        }
        pd2 = iocommon_open(path, flags, PVFS_HINT_NULL, 0, &pd->pvfs_ref);
        rc = iocommon_stat64(pd2, buf, PVFS_ATTR_SYS_ALL_NOHINT);
        pvfs_close(pd2->fd);
    }
    return rc;
}

/**
 * pvfs_lstat
 */
int pvfs_lstat(const char *path, struct stat *buf)
{
    return pvfs_lstat_mask(path, buf, PVFS_ATTR_SYS_ALL_NOHINT);
}

int pvfs_lstat_mask(const char *path, struct stat *buf, uint32_t mask)
{
    int rc;
    char *newpath;
    pvfs_descriptor *pd;

    newpath = pvfs_qualify_path(path);
    pd = iocommon_open(newpath, O_RDONLY|O_NOFOLLOW, PVFS_HINT_NULL, 0, NULL);
    if (newpath != path)
    {
       free(newpath);
    }
    rc = iocommon_stat(pd, buf, mask);
    pvfs_close(pd->fd);
    return rc;
}

/**
 * pvfs_lstat64
 */
int pvfs_lstat64(const char *path, struct stat64 *buf)
{
    int rc;
    char *newpath;
    pvfs_descriptor *pd;

    newpath = pvfs_qualify_path(path);
    pd = iocommon_open(newpath, O_RDONLY|O_NOFOLLOW, PVFS_HINT_NULL, 0, NULL);
    if (newpath != path)
    {
        free(newpath);
    }
    rc = iocommon_stat64(pd, buf, PVFS_ATTR_SYS_ALL_NOHINT);
    pvfs_close(pd->fd);
    return rc;
}

/**
 * pvfs_dup
 */
int pvfs_dup(int oldfd)
{
    return pvfs_dup_descriptor(oldfd, -1);
}

/**
 * pvfs_dup2
 */
int pvfs_dup2(int oldfd, int newfd)
{
    return pvfs_dup_descriptor(oldfd, newfd);
}

/**
 * pvfs_chown
 */
int pvfs_chown(const char *path, uid_t owner, gid_t group)
{
    int rc;
    char *newpath;
    pvfs_descriptor *pd;

    newpath = pvfs_qualify_path(path);
    pd = iocommon_open(newpath, O_RDONLY, PVFS_HINT_NULL, 0, NULL);
    if (newpath != path)
    {
        free(newpath);
    }
    rc = iocommon_chown(pd, owner, group);
    pvfs_close(pd->fd);
    return rc;
}

/**
 * pvfs_fchown
 */
int pvfs_fchown(int fd, uid_t owner, gid_t group)
{
    pvfs_descriptor *pd;

    if (fd < 0)
    {
        errno = EBADF;
        return -1;
    }
    pd = pvfs_find_descriptor(fd);
    if (!pd)
    {
        errno = EBADF;
        return -1;
    }
    return iocommon_chown(pd, owner, group);
}

/**
 * pvfs_fchownat
 */
int pvfs_fchownat(int fd, const char *path, uid_t owner, gid_t group, int flag)
{
    int rc;
    pvfs_descriptor *pd, *pd2;

    if (path[0] == '/' || fd == AT_FDCWD)
    {
        if (flag & AT_SYMLINK_NOFOLLOW)
        {
            rc = pvfs_lchown(path, owner, group);
        }
        else
        {
            rc = pvfs_chown(path, owner, group);
        }
    }
    else
    {
        int flags = O_RDONLY;
        if (flag & AT_SYMLINK_NOFOLLOW)
        {
            flags |= O_NOFOLLOW;
        }
        if (fd < 0)
        {
            errno = EBADF;
            return -1;
        }
        pd = pvfs_find_descriptor(fd);
        if (!pd)
        {
            return -1;
        }
        pd2 = iocommon_open(path, flags, PVFS_HINT_NULL, 0, &pd->pvfs_ref);
        rc = iocommon_chown(pd2, owner, group);
        pvfs_close(pd2->fd);
    }
    return rc;
}

/**
 * pvfs_lchown
 */
int pvfs_lchown(const char *path, uid_t owner, gid_t group)
{
    int rc;
    char *newpath;
    pvfs_descriptor *pd;

    newpath = pvfs_qualify_path(path);
    pd = iocommon_open(newpath, O_RDONLY|O_NOFOLLOW, PVFS_HINT_NULL, 0, NULL);
    if (newpath != path)
    {
        free(newpath);
    }
    rc = iocommon_chown(pd, owner, group);
    pvfs_close(pd->fd);
    return rc;
}

/**
 * pvfs_chmod
 */
int pvfs_chmod(const char *path, mode_t mode)
{
    int rc;
    char *newpath;
    pvfs_descriptor *pd;

    newpath = pvfs_qualify_path(path);
    pd = iocommon_open(newpath, O_RDONLY, PVFS_HINT_NULL, 0, NULL);
    if (newpath != path)
    {
        free(newpath);
    }
    rc = iocommon_chmod(pd, mode);
    pvfs_close(pd->fd);
    return rc;
}

/**
 * pvfs_fchmod
 */
int pvfs_fchmod(int fd, mode_t mode)
{
    pvfs_descriptor *pd;

    if (fd < 0)
    {
        errno = EBADF;
        return -1;
    }
    pd = pvfs_find_descriptor(fd);
    if (!pd)
    {
        errno = EBADF;
        return -1;
    }
    return iocommon_chmod(pd, mode);
}

/**
 * pvfs_fchmodat
 */
int pvfs_fchmodat(int fd, const char *path, mode_t mode, int flag)
{
    int rc;
    pvfs_descriptor *pd, *pd2;

    if (path[0] == '/' || fd == AT_FDCWD)
    {
        rc = pvfs_chmod(path, mode);
    }
    else
    {
        int flags = O_RDONLY;
        if (fd < 0)
        {
            errno = EBADF;
            return -1;
        }
        pd = pvfs_find_descriptor(fd);
        if (!pd)
        {
            return -1;
        }
        pd2 = iocommon_open(path, flags, PVFS_HINT_NULL, 0, &pd->pvfs_ref);
        rc = iocommon_chmod(pd2, mode);
        pvfs_close(pd2->fd);
    }
    return rc;
}

/**
 * pvfs_mkdir
 */
int pvfs_mkdir(const char *path, mode_t mode)
{
    int rc;
    char *newpath;

    newpath = pvfs_qualify_path(path);
    rc = iocommon_make_directory(newpath, (mode & ~mask_val & 0777), NULL);
    if (newpath != path)
    {
        free(newpath);
    }
    return rc;
}

/**
 * pvfs_mkdirat
 */
int pvfs_mkdirat(int dirfd, const char *path, mode_t mode)
{
    int rc;
    pvfs_descriptor *pd, *pd2;

    if (path[0] == '/' || dirfd == AT_FDCWD)
    {
        rc = pvfs_mkdir(path, mode);
    }
    else
    {
        if (dirfd < 0)
        {
            errno = EBADF;
            return -1;
        }
        pd = pvfs_find_descriptor(dirfd);
        if (!pd)
        {
            errno = EBADF;
            return -1;
        }
        rc = iocommon_make_directory(path,
                                     (mode & ~mask_val & 0777),
                                     &pd->pvfs_ref);
    }
    return rc;
}

/**
 * pvfs_rmdir
 */
int pvfs_rmdir(const char *path)
{
    int rc;
    char *newpath;

    newpath = pvfs_qualify_path(path);
    rc = iocommon_rmdir(newpath, NULL);
    if (newpath != path)
    {
        free(newpath);
    }
    return rc;
}

/**
 * readlink fills buffer with contents of a symbolic link
 *
 */
ssize_t pvfs_readlink(const char *path, char *buf, size_t bufsiz)
{
    int rc;
    char *newpath;
    pvfs_descriptor *pd;

    newpath = pvfs_qualify_path(path);
    pd = iocommon_open(newpath, O_RDONLY | O_NOFOLLOW, PVFS_HINT_NULL, 0, NULL);
    if (newpath != path)
    {
       free(newpath);
    }
    /* this checks that it is a valid symlink and sets errno if not */
    rc = iocommon_readlink(pd, buf, bufsiz);
    pvfs_close(pd->fd);
    return rc;
}

int pvfs_readlinkat(int fd, const char *path, char *buf, size_t bufsiz)
{
    int rc;
    pvfs_descriptor *pd, *pd2;

    if (path[0] == '/' || fd == AT_FDCWD)
    {
        rc = pvfs_readlink(path, buf, bufsiz);
    }
    else
    {
        int flags = O_RDONLY | O_NOFOLLOW;
        if (fd < 0)
        {
            errno = EBADF;
            return -1;
        }
        pd = pvfs_find_descriptor(fd);
        if (!pd)
        {
            return -1;
        }
        pd2 = iocommon_open(path, flags, PVFS_HINT_NULL, 0, &pd->pvfs_ref);
        if(!pd2)
        {
            return -1;
        }
        rc = iocommon_readlink(pd2, buf, bufsiz);
        pvfs_close(pd2->fd);
    }
    return rc;
}

int pvfs_symlink(const char *oldpath, const char *newpath)
{
    int rc = 0;
    char *abspath;
    abspath = pvfs_qualify_path(newpath);
    rc =  iocommon_symlink(abspath, oldpath, NULL);
    if (abspath != newpath)
    {
       free(abspath);
    }
    return rc;
}

int pvfs_symlinkat(const char *oldpath, int newdirfd, const char *newpath)
{
    pvfs_descriptor *pd;

    if (newpath[0] == '/' || newdirfd == AT_FDCWD)
    {
        return pvfs_symlink(oldpath, newpath);
    }
    else
    {
        if (newdirfd < 0)
        {
            errno = EBADF;
            return -1;
        }
        pd = pvfs_find_descriptor(newdirfd);
        if (!pd)
        {
            errno = EBADF;
            return -1;
        }
    }
    return iocommon_symlink(newpath, oldpath, &pd->pvfs_ref);
}

/**
 * PVFS does not have hard links
 */
ssize_t pvfs_link(const char *oldpath, const char *newpath)
{
    fprintf(stderr, "pvfs_link not implemented\n");
    errno = ENOSYS;
    return -1;
}

/**
 * PVFS does not have hard links
 */
int pvfs_linkat(int olddirfd, const char *oldpath,
                int newdirfd, const char *newpath, int flags)
{
    fprintf(stderr, "pvfs_linkat not implemented\n");
    errno = ENOSYS;
    return -1;
}

/**
 * this reads exactly one dirent, count is ignored
 */
int pvfs_readdir(unsigned int fd, struct dirent *dirp, unsigned int count)
{
    return pvfs_getdents(fd, dirp, 1);
}

/**
 * this reads multiple dirents, up to count
 */
int pvfs_getdents(unsigned int fd, struct dirent *dirp, unsigned int count)
{
    pvfs_descriptor *pd;

    if (fd < 0)
    {
        errno = EBADF;
        return -1;
    }
    pd = pvfs_find_descriptor(fd);
    if (!pd)
    {
        errno = EBADF;
        return -1;
    }
    return iocommon_getdents(pd, dirp, count);
}

int pvfs_getdents64(unsigned int fd, struct dirent64 *dirp, unsigned int count)
{
    fprintf(stderr, "pvfs_getdents64 not implemented\n");
    errno = ENOSYS;
    return -1;
}

int pvfs_access(const char *path, int mode)
{
    int rc = 0;
    char *newpath;
    newpath = pvfs_qualify_path(path);
    rc = iocommon_access(path, mode, 0, NULL);
    if (newpath != path)
    {
        free(newpath);
    }
    return rc;
}

int pvfs_faccessat(int fd, const char *path, int mode, int flags)
{
    pvfs_descriptor *pd;

    if (path[0] == '/' || fd == AT_FDCWD)
    {
        return pvfs_access(path, mode);
    }
    else
    {
        if (fd < 0)
        {
            errno = EBADF;
            return -1;
        }
        pd = pvfs_find_descriptor(fd);
        if(!pd)
        {
            errno = EBADF;
            return -1;
        }
    }
    return iocommon_access(path, mode, flags, &pd->pvfs_ref);
}

int pvfs_flock(int fd, int op)
{
    errno = ENOSYS;
    fprintf(stderr, "pvfs_flock not implemented\n");
    return -1;
}

int pvfs_fcntl(int fd, int cmd, ...)
{
    va_list ap;
    long arg;
    struct flock *lock;
    errno = ENOSYS;
    fprintf(stderr, "pvfs_fcntl not implemented\n");
    return -1;
}

/* sync all disk data */
void pvfs_sync(void )
{
    errno = ENOSYS;
    fprintf(stderr, "pvfs_sync not implemented\n");
}

/* sync file, but not dir it is in */
int pvfs_fsync(int fd)
{
    int rc = 0;

    rc = pvfs_flush(fd); /* as close as we have for now */
    return rc;
}

/* does not sync file metadata */
int pvfs_fdatasync(int fd)
{
    int rc = 0;

    rc = pvfs_flush(fd); /* as close as we have for now */
    return rc;
}

int pvfs_fadvise(int fd, off_t offset, off_t len, int advice)
{
    return pvfs_fadvise64(fd, (off64_t) offset, (off64_t)len, advice);
}

/** fadvise implementation
 *
 * technically this is a hint, so doing nothing is still success
 */
int pvfs_fadvise64(int fd, off64_t offset, off64_t len, int advice)
{
    switch (advice)
    {
    case POSIX_FADV_NORMAL:
    case POSIX_FADV_RANDOM:
    case POSIX_FADV_SEQUENTIAL:
    case POSIX_FADV_WILLNEED:
    case POSIX_FADV_DONTNEED:
    case POSIX_FADV_NOREUSE:
        break;
    default:
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int pvfs_statfs(const char *path, struct statfs *buf)
{
    int rc;
    char *newpath;
    pvfs_descriptor *pd;

    newpath = pvfs_qualify_path(path);
    pd = iocommon_open(newpath, O_RDONLY, PVFS_HINT_NULL, 0, NULL);
    if (newpath != path)
    {
        free(newpath);
    }
    rc = iocommon_statfs(pd, buf);
    pvfs_close(pd->fd);
    return rc;
}

int pvfs_statfs64(const char *path, struct statfs64 *buf)
{
    int rc;
    char *newpath;
    pvfs_descriptor *pd;

    newpath = pvfs_qualify_path(path);
    pd = iocommon_open(newpath, O_RDONLY, PVFS_HINT_NULL, 0, NULL);
    if (newpath != path)
    {
        free(newpath);
    }
    rc = iocommon_statfs(pd, buf);
    pvfs_close(pd->fd);
    return rc;
}
                 
int pvfs_fstatfs(int fd, struct statfs *buf)
{
    pvfs_descriptor *pd;

    if (fd < 0)
    {
        errno = EBADF;
        return -1;
    }
    pd = pvfs_find_descriptor(fd);
    if (!pd)
    {
        errno = EBADF;
        return -1;
    }
    return iocommon_statfs(pd, buf);
}

int pvfs_fstatfs64(int fd, struct statfs64 *buf)
{
    pvfs_descriptor *pd;

    if (fd < 0)
    {
        errno = EBADF;
        return -1;
    }
    pd = pvfs_find_descriptor(fd);
    if (!pd)
    {
        errno = EBADF;
        return -1;
    }
    return iocommon_statfs64(pd, buf);
}

int pvfs_mknod(const char *path, mode_t mode, dev_t dev)
{
    return pvfs_mknodat(AT_FDCWD, path, mode, dev);
}

int pvfs_mknodat(int dirfd, const char *path, mode_t mode, dev_t dev)
{
    int fd;
    int s_type = mode & S_IFMT;
    
    switch (dev)
    {
    case S_IFREG:
        fd = pvfs_openat(dirfd, path, O_CREAT|O_EXCL|O_RDONLY, mode & 0x777);
        if (fd < 0)
        {
            return -1;
        }
        pvfs_close(fd);
        break;
    case S_IFCHR:
    case S_IFBLK:
    case S_IFIFO:
    case S_IFSOCK:
    default:
        errno = EINVAL;
        return -1;
    }
    return 0;
}

ssize_t pvfs_sendfile(int outfd, int infd, off_t offset, size_t count)
{
    return pvfs_sendfile64(outfd, infd, (off64_t) offset, count);
}
                 
ssize_t pvfs_sendfile64(int outfd, int infd, off64_t offset, size_t count)
{
    pvfs_descriptor *inpd, *outpd;

    inpd = pvfs_find_descriptor(infd);
    outpd = pvfs_find_descriptor(outfd);  /* this should be  a socket */
    if (!inpd || !outpd)
    {
        errno = EBADF;
        return -1;
    }
    return iocommon_sendfile(outpd->true_fd, inpd, offset, count);
}

int pvfs_setxattr(const char *path,
                  const char *name,
                  const void *value,
                  size_t size,
                  int flags)
{
    fprintf(stderr, "pvfs_setxattr not implemented\n");
    errno = ENOSYS;
    return -1;
}

int pvfs_lsetxattr(const char *path,
                   const char *name,
                   const void *value,
                   size_t size,
                   int flags)
{
    fprintf(stderr, "pvfs_lsetxattr not implemented\n");
    errno = ENOSYS;
    return -1;
}

int pvfs_fsetxattr(int fd,
                   const char *name,
                   const void *value,
                   size_t size,
                   int flags)
{
    fprintf(stderr, "pvfs_fsetxattr not implemented\n");
    errno = ENOSYS;
    return -1;
}

int pvfs_getxattr(const char *path,
                  const char *name,
                  void *value,
                  size_t size)
{
    fprintf(stderr, "pvfs_getxattr not implemented\n");
    errno = ENOSYS;
    return -1;
}

int pvfs_lgetxattr(const char *path,
                   const char *name,
                   void *value,
                   size_t size)
{
    fprintf(stderr, "pvfs_lgetxattr not implemented\n");
    errno = ENOSYS;
    return -1;
}

int pvfs_fgetxattr(int fd,
                   const char *name,
                   void *value,
                   size_t size)
{
    fprintf(stderr, "pvfs_fgetxattr not implemented\n");
    errno = ENOSYS;
    return -1;
}

int pvfs_listxattr(const char *path,
                   char *list,
                   size_t size)
{
    fprintf(stderr, "pvfs_listxattr not implemented\n");
    errno = ENOSYS;
    return -1;
}

int pvfs_llistxattr(const char *path,
                    char *list,
                    size_t size)
{
    fprintf(stderr, "pvfs_llistxattr not implemented\n");
    errno = ENOSYS;
    return -1;
}

int pvfs_flistxattr(int fd,
                    char *list,
                    size_t size)
{
    fprintf(stderr, "pvfs_flistxattr not implemented\n");
    errno = ENOSYS;
    return -1;
}

int pvfs_removexattr(const char *path,
                     const char *name)
{
    fprintf(stderr, "pvfs_removexattr not implemented\n");
    errno = ENOSYS;
    return -1;
}

int pvfs_lremovexattr(const char *path,
                      const char *name)
{
    fprintf(stderr, "pvfs_lremovexattr not implemented\n");
    errno = ENOSYS;
    return -1;
}

int pvfs_fremovexattr(int fd,
                      const char *name)
{
    fprintf(stderr, "pvfs_fremovexattr not implemented\n");
    errno = ENOSYS;
    return -1;
}

/**
 * pvfs_umask
 */
mode_t pvfs_umask(mode_t mask)
{
    mode_t old_mask = mask_val;
    mask_val = mask & 0777;
    return old_mask;
}

mode_t pvfs_getumask(void)
{
    return mask_val;
}

int pvfs_getdtablesize(void)
{
    return pvfs_descriptor_table_size();
}

posix_ops pvfs_ops = 
{
    .open = pvfs_open,
    .open64 = pvfs_open64,
    .openat = pvfs_openat,
    .openat64 = pvfs_openat64,
    .creat = pvfs_creat,
    .creat64 = pvfs_creat64,
    .unlink = pvfs_unlink,
    .unlinkat = pvfs_unlinkat,
    .rename = pvfs_rename,
    .renameat = pvfs_renameat,
    .read = pvfs_read,
    .pread = pvfs_pread,
    .readv = pvfs_readv,
    .pread64 = pvfs_pread64,
    .write = pvfs_write,
    .pwrite = pvfs_pwrite,
    .writev = pvfs_writev,
    .pwrite64 = pvfs_pwrite64,
    .lseek = pvfs_lseek,
    .lseek64 = pvfs_lseek64,
    .truncate = pvfs_truncate,
    .truncate64 = pvfs_truncate64,
    .ftruncate = pvfs_ftruncate,
    .ftruncate64 = pvfs_ftruncate64,
    .fallocate = pvfs_fallocate,
    .close = pvfs_close,
    .flush = pvfs_flush,
    .stat = pvfs_stat,
    .stat64 = pvfs_stat64,
    .fstat = pvfs_fstat,
    .fstat64 = pvfs_fstat64,
    .fstatat = pvfs_fstatat,
    .fstatat64 = pvfs_fstatat64,
    .lstat = pvfs_lstat,
    .lstat64 = pvfs_lstat64,
    .dup = pvfs_dup,
    .dup2 = pvfs_dup2,
    .chown = pvfs_chown,
    .fchown = pvfs_fchown,
    .fchownat = pvfs_fchownat,
    .lchown = pvfs_lchown,
    .chmod = pvfs_chmod,
    .fchmod = pvfs_fchmod,
    .fchmodat = pvfs_fchmodat,
    .mkdir = pvfs_mkdir,
    .mkdirat = pvfs_mkdirat,
    .rmdir = pvfs_rmdir,
    .readlink = pvfs_readlink,
    .readlinkat = pvfs_readlinkat,
    .symlink = pvfs_symlink,
    .symlinkat = pvfs_symlinkat,
    .link = pvfs_link,
    .linkat = pvfs_linkat,
    .readdir = pvfs_readdir,
    .getdents = pvfs_getdents,
    .getdents64 = pvfs_getdents64,
    .access = pvfs_access,
    .faccessat = pvfs_faccessat,
    .flock = pvfs_flock,
    .fcntl = pvfs_fcntl,
    .sync = pvfs_sync,
    .fsync = pvfs_fsync,
    .fdatasync = pvfs_fdatasync,
    .fadvise = pvfs_fadvise,
    .fadvise64 = pvfs_fadvise64,
    .statfs = statfs,             /* this one is probably special */
    .statfs64 = pvfs_statfs64,
    .fstatfs = pvfs_fstatfs,
    .fstatfs64 = pvfs_fstatfs64,
    .mknod = pvfs_mknod,
    .mknodat = pvfs_mknodat,
    .sendfile = pvfs_sendfile,
    .sendfile64 = pvfs_sendfile64,
    .setxattr = pvfs_setxattr,
    .lsetxattr = pvfs_lsetxattr,
    .fsetxattr = pvfs_fsetxattr,
    .getxattr = pvfs_getxattr,
    .lgetxattr = pvfs_lgetxattr,
    .fgetxattr = pvfs_fgetxattr,
    .listxattr = pvfs_listxattr,
    .llistxattr = pvfs_llistxattr,
    .flistxattr = pvfs_flistxattr,
    .removexattr = pvfs_removexattr,
    .lremovexattr = pvfs_lremovexattr,
    .fremovexattr = pvfs_fremovexattr,
    .umask = pvfs_umask,
    .getumask = pvfs_getumask,
    .getdtablesize = pvfs_getdtablesize,
};

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
