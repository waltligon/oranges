/*
 * (C) 2001 Clemson University and The University of Chicago
 *
 * See COPYING in top-level directory.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/pagemap.h>
#include "pvfs2-kernel.h"

/* FIXME: NEED PVFS2_READDIR_START defined in pvfs2-sysint.h */
#define PVFS2_READDIR_START (INT_MAX-1)

extern kmem_cache_t *op_cache;
extern struct list_head pvfs2_request_list;
extern spinlock_t pvfs2_request_list_lock;

/* shared file/dir operations defined in file.c */
extern int pvfs2_open(
    struct inode *inode,
    struct file *file);
extern int pvfs2_release(
    struct inode *inode,
    struct file *file);

/*
  should return 0 when we're done traversing a directory;
  return a negative value on error, or a positive value otherwise.
  (i.e. if we don't call filldir for ALL entries, return a
  positive value)

  If the filldir call-back returns non-zero, then readdir should
  assume that it has had enough, and should return as well.
*/
static int pvfs2_readdir(
    struct file *file,
    void *dirent,
    filldir_t filldir)
{
    int pos = 0, ret = 0;
    ino_t ino = 0;
    struct dentry *dentry = file->f_dentry;
    pvfs2_kernel_op_t *new_op = NULL;
    pvfs2_inode_t *pvfs2_inode = PVFS2_I(dentry->d_inode);
    int need_revalidate = (file->f_version != dentry->d_inode->i_version);

    /*
       if the directory we're reading has changed between
       calls to us, restart directory traversal from scratch
     */
    if (need_revalidate)
    {
	file->f_pos = 0;
    }

    /* pick up from where we left off */
    pos = file->f_pos;

    pvfs2_print("pvfs2: pvfs2_readdir called on %s (pos = %d)\n",
		dentry->d_name.name, pos);

    switch (pos)
    {
	/*
	   if we're just starting, populate the "." and ".." entries
	   of the current directory; these always appear
	 */
    case 0:
	ino = dentry->d_inode->i_ino;
	if (filldir(dirent, ".", 1, pos, ino, DT_DIR) < 0)
	{
	    break;
	}
	file->f_pos++;
	pos++;
	/* drop through */
    case 1:
	ino = parent_ino(dentry);
	if (filldir(dirent, "..", 2, pos, ino, DT_DIR) < 0)
	{
	    break;
	}
	file->f_pos++;
	pos++;
	/* drop through */
    default:
	/* handle the normal cases here */
	new_op = kmem_cache_alloc(op_cache, SLAB_KERNEL);
	if (!new_op)
	{
	    pvfs2_error("pvfs2: pvfs2_readdir -- "
			"kmem_cache_alloc failed!\n");
	    return -ENOMEM;
	}
	new_op->upcall.type = PVFS2_VFS_OP_READDIR;
	if (pvfs2_inode && pvfs2_inode->refn.handle && pvfs2_inode->refn.fs_id)
	{
	    new_op->upcall.req.readdir.refn = pvfs2_inode->refn;
	}
	else
	{
	    new_op->upcall.req.readdir.refn.handle =
		pvfs2_ino_to_handle(dentry->d_inode->i_ino);
	    new_op->upcall.req.readdir.refn.fs_id =
		PVFS2_SB(dentry->d_inode->i_sb)->fs_id;
	}
	new_op->upcall.req.readdir.max_dirent_count = MAX_DIRENT_COUNT;

	/* NOTE:
	   the position we send to the readdir upcall is out of
	   sync with file->f_pos since pvfs2 doesn't include the
	   "." and ".." entries that we added above.

	   so the proper pvfs2 position is (pos - 2), except where
	   pos == 0.  In that case, pos is PVFS2_READDIR_START.
	 */
	new_op->upcall.req.readdir.token =
	    (pos == 2 ? PVFS2_READDIR_START : (PVFS_ds_position) (pos - 2));

	/* post req and wait for request to be serviced here */
	add_op_to_request_list(new_op);
	if ((ret = wait_for_matching_downcall(new_op)) != 0)
	{
	    /*
	       NOTE: we can't free the op here unless we're SURE
	       it wasn't put on the invalidated list.
	       For now, wait_for_matching_downcall just doesn't
	       put anything on the invalidated list.
	     */
	    pvfs2_error("pvfs2: pvfs2_readdir -- wait failed (%x).  "
			"op invalidated (not really)\n", ret);
	    goto error_exit;
	}

	/* need to check downcall.status value */
	pvfs2_print("Readdir downcall status is %d (dirent_count "
		    "is %d)\n", new_op->downcall.status,
		    new_op->downcall.resp.readdir.dirent_count);
	if (new_op->downcall.status == 0)
	{
	    int i = 0, len = 0;
	    ino_t current_ino = 0;
	    char *current_entry = NULL;

	    /* store the position token */
	    pvfs2_inode->readdir_token = new_op->downcall.resp.readdir.token;

	    for (i = 0; i < new_op->downcall.resp.readdir.dirent_count; i++)
	    {
		len = new_op->downcall.resp.readdir.d_name_len[i];
		current_entry = &new_op->downcall.resp.readdir.d_name[i][0];
		current_ino =
		    pvfs2_handle_to_ino(new_op->downcall.resp.readdir.refn[i].
					handle);

		pvfs2_print("pvfs2: pvfs2_readdir -- Calling filldir "
			    "on %s\n", current_entry);
		if (filldir(dirent, current_entry, len, pos,
			    current_ino, DT_UNKNOWN) < 0)
		{
		    break;
		}
		file->f_pos++;
		pos++;
	    }
	}

      error_exit:
	/* when request is serviced properly, free req op struct */
	op_release(new_op);
	break;
    }

    file->f_version = dentry->d_inode->i_version;
    update_atime(dentry->d_inode);
    return 0;
}

struct file_operations pvfs2_dir_operations = {
    .read = generic_read_dir,
    .readdir = pvfs2_readdir,
    .llseek = generic_file_llseek,
    .open = pvfs2_open,
    .release = pvfs2_release
};

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 noexpandtab
 */
