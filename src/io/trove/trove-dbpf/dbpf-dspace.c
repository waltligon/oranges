/*
 * (C) 2001 Clemson University and The University of Chicago
 *
 * See COPYING in top-level directory.
 */
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <db.h>
#include <time.h>
#include <stdlib.h>
#ifdef HAVE_MALLOC_H
#include <malloc.h>
#endif
#include <assert.h>

#include "gossip.h"
#include "pint-perf-counter.h"
#include "pint-event.h"
#include "trove-internal.h"
#include "trove-ledger.h"
#include "trove-handle-mgmt.h"
#include "dbpf.h"
#include "dbpf-op.h"
#include "dbpf-thread.h"
#include "dbpf-bstream.h"
#include "dbpf-op-queue.h"
#include "dbpf-attr-cache.h"
#include "dbpf-open-cache.h"

#include <pthread.h>
#include "dbpf-thread.h"
#include "pvfs2-internal.h"
#include "pint-perf-counter.h"

extern pthread_cond_t dbpf_op_completed_cond;
extern gen_mutex_t *dbpf_completion_queue_array_mutex[TROVE_MAX_CONTEXTS];

extern gen_mutex_t dbpf_attr_cache_mutex;

int64_t s_dbpf_metadata_writes = 0, s_dbpf_metadata_reads = 0;

static inline void organize_post_op_statistics(
    enum dbpf_op_type op_type, TROVE_op_id op_id)
{
    switch(op_type)
    {
        case KEYVAL_WRITE:
        case KEYVAL_REMOVE_KEY:
        case KEYVAL_WRITE_LIST:
        case KEYVAL_FLUSH:
        case DSPACE_REMOVE:
        case DSPACE_SETATTR:
            UPDATE_PERF_METADATA_WRITE();
            break;
        case KEYVAL_READ:
        case KEYVAL_READ_LIST:
        case KEYVAL_VALIDATE:
        case KEYVAL_ITERATE:
        case KEYVAL_ITERATE_KEYS:
        case DSPACE_ITERATE_HANDLES:
        case DSPACE_VERIFY:
        case DSPACE_GETATTR:
            UPDATE_PERF_METADATA_READ();
            break;
        case BSTREAM_READ_LIST:
            DBPF_EVENT_END(PVFS_EVENT_TROVE_READ_LIST, op_id); 
            break;
        case BSTREAM_WRITE_LIST:
            DBPF_EVENT_END(PVFS_EVENT_TROVE_WRITE_LIST, op_id); 
            break;
        default:
            break;
        case DSPACE_CREATE:
            UPDATE_PERF_METADATA_WRITE();
            DBPF_EVENT_END(PVFS_EVENT_TROVE_DSPACE_CREATE, op_id); 
            break;
    }
}

static int dbpf_dspace_iterate_handles_op_svc(struct dbpf_op *op_p);
static int dbpf_dspace_create_op_svc(struct dbpf_op *op_p);
static int dbpf_dspace_remove_op_svc(struct dbpf_op *op_p);
static int dbpf_dspace_verify_op_svc(struct dbpf_op *op_p);
static int dbpf_dspace_setattr_op_svc(struct dbpf_op *op_p);
static int dbpf_dspace_getattr_op_svc(struct dbpf_op *op_p);

static int dbpf_dspace_create(TROVE_coll_id coll_id,
                              TROVE_handle_extent_array *extent_array,
                              TROVE_handle *handle_p,
                              TROVE_ds_type type,
                              TROVE_keyval_s *hint,
                              TROVE_ds_flags flags,
                              void *user_ptr,
                              TROVE_context_id context_id,
                              TROVE_op_id *out_op_id_p)
{
    dbpf_queued_op_t *q_op_p = NULL;
    struct dbpf_op op;
    struct dbpf_op *op_p;
    struct dbpf_collection *coll_p = NULL;
    int ret;

    coll_p = dbpf_collection_find_registered(coll_id);
    if (coll_p == NULL)
    {
        return -TROVE_EINVAL;
    }

    ret = dbpf_op_init_queued_or_immediate(
        &op,
        &q_op_p,
        DSPACE_CREATE,
        coll_p,
        (handle_p ? *handle_p : TROVE_HANDLE_NULL),
        dbpf_dspace_create_op_svc,
        flags,
        NULL,
        user_ptr,
        context_id,
        &op_p);
    if(ret < 0)
    {
        return ret;
    }

    if (!extent_array || (extent_array->extent_count < 1))
    {
        return -TROVE_EINVAL;
    }

    DBPF_EVENT_START(PVFS_EVENT_TROVE_DSPACE_CREATE, op_p->id);

    /* this array is freed in dbpf-op.c:dbpf_queued_op_free */
    op_p->u.d_create.extent_array.extent_count =
        extent_array->extent_count;
    op_p->u.d_create.extent_array.extent_array =
        malloc(extent_array->extent_count * sizeof(TROVE_extent));

    if (op_p->u.d_create.extent_array.extent_array == NULL)
    {
        return -TROVE_ENOMEM;
    }

    memcpy(op_p->u.d_create.extent_array.extent_array,
           extent_array->extent_array,
           extent_array->extent_count * sizeof(TROVE_extent));

    op_p->u.d_create.out_handle_p = handle_p;
    op_p->u.d_create.type = type;

    PINT_perf_count(PINT_server_pc, PINT_PERF_METADATA_DSPACE_OPS,
                    1, PINT_PERF_ADD);

    return dbpf_queue_or_service(op_p, q_op_p, coll_p, out_op_id_p);
}

static int dbpf_dspace_create_op_svc(struct dbpf_op *op_p)
{
    int ret = -TROVE_EINVAL;
    TROVE_ds_storedattr_s s_attr;
    TROVE_ds_attributes attr;
    TROVE_handle new_handle = TROVE_HANDLE_NULL;
    DBT key, data;
    TROVE_extent cur_extent;
    TROVE_object_ref ref = {TROVE_HANDLE_NULL, op_p->coll_p->coll_id};
    char filename[PATH_MAX + 1];

    memset(filename, 0, PATH_MAX + 1);

    cur_extent = op_p->u.d_create.extent_array.extent_array[0];

    /* check if we got a single specific handle */
    if ((op_p->u.d_create.extent_array.extent_count == 1) &&
        (cur_extent.first == cur_extent.last))
    {
        /*
          check if we MUST use the exact handle value specified;
          if caller requests a specific handle, honor it
        */
        if (op_p->flags & TROVE_FORCE_REQUESTED_HANDLE)
        {
            /*
              we should probably handle this error nicely;
              right now, it will fail later (gracefully) if this
              fails since the handle will already exist, but
              since we know it here, handle it here ?
            */
            new_handle = cur_extent.first;
            trove_handle_set_used(op_p->coll_p->coll_id, new_handle);
            gossip_debug(GOSSIP_TROVE_DEBUG, "new_handle was FORCED "
                         "to be %llu\n", llu(new_handle));
        }
        else if (cur_extent.first == TROVE_HANDLE_NULL)
        {
            /*
              if we got TROVE_HANDLE_NULL, the caller doesn't care
              where the handle comes from
            */
            new_handle = trove_handle_alloc(op_p->coll_p->coll_id);
        }
    }
    else
    {
        /*
          otherwise, we have to try to allocate a handle from
          the specified range that we're given
        */
        new_handle = trove_handle_alloc_from_range(
            op_p->coll_p->coll_id, &op_p->u.d_create.extent_array);
    }

    gossip_debug(GOSSIP_TROVE_DEBUG, "[%d extents] -- new_handle is %llu "
                 "(cur_extent is %llu - %llu)\n",
                 op_p->u.d_create.extent_array.extent_count,
                 llu(new_handle), llu(cur_extent.first),
                 llu(cur_extent.last));
    /*
      if we got a zero handle, we're either completely out of handles
      -- or else something terrible has happened
    */
    if (new_handle == TROVE_HANDLE_NULL)
    {
        gossip_err("Error: handle allocator returned a zero handle.\n");
        ret = -TROVE_ENOSPC;
        goto return_error;
    }

    memset(&s_attr, 0, sizeof(TROVE_ds_storedattr_s));
    s_attr.type = op_p->u.d_create.type;

    memset(&key, 0, sizeof(key));
    key.data = &new_handle;
    key.size = sizeof(new_handle);

    memset(&data, 0, sizeof(data));
    data.data = &s_attr;
    data.size = data.ulen = sizeof(TROVE_ds_storedattr_s);
    data.flags |= DB_DBT_USERMEM;

    /* check to see if handle is already used */
    ret = op_p->coll_p->ds_db->get(op_p->coll_p->ds_db, NULL, &key, &data, 0);
    if (ret == 0)
    {
        gossip_debug(GOSSIP_TROVE_DEBUG, "handle already exists...\n");
        ret = -TROVE_EEXIST;
        goto return_error;
    }
    else if ((ret != DB_NOTFOUND) && (ret != DB_KEYEMPTY))
    {
        gossip_err("error in dspace create (db_p->get failed).\n");
        ret = -dbpf_db_error_to_trove_error(ret);
        goto return_error;
    }
    
    /* check for old bstream files (these should not exist, but it is
     * possible if the db gets out of sync with the rest of the collection
     * somehow
     */
    DBPF_GET_BSTREAM_FILENAME(filename, PATH_MAX, my_storage_p->name,
                              op_p->coll_p->coll_id, llu(new_handle));
    ret = access(filename, F_OK);
    if(ret == 0)
    {
        char new_filename[PATH_MAX+1];
        memset(new_filename, 0, PATH_MAX+1);

        gossip_err("Warning: found old bstream file %s; "
                   "moving to stranded-bstreams.\n", 
                   filename);
        
        DBPF_GET_STRANDED_BSTREAM_FILENAME(new_filename, PATH_MAX,
                                           my_storage_p->name, 
                                           op_p->coll_p->coll_id,
                                           llu(new_handle));
        /* an old file exists.  Move it to the stranded subdirectory */
        ret = rename(filename, new_filename);
        if(ret != 0)
        {
            ret = -trove_errno_to_trove_error(errno);
            gossip_err("Error: trove failed to rename stranded bstream: %s\n",
                       filename);
            goto return_error;
        }
    }
     
    memset(&data, 0, sizeof(data));
    data.data = &s_attr;
    data.size = sizeof(s_attr);
    
    /* create new dataspace entry */
    ret = op_p->coll_p->ds_db->put(op_p->coll_p->ds_db, NULL, &key, &data, 0);
    if (ret != 0)
    {
        gossip_err("error in dspace create (db_p->put failed).\n");
        ret = -dbpf_db_error_to_trove_error(ret);
        goto return_error;
    }

    trove_ds_stored_to_attr(s_attr, attr, 0);

    /* add retrieved ds_attr to dbpf_attr cache here */
    ref.handle = new_handle;
    gen_mutex_lock(&dbpf_attr_cache_mutex);
    dbpf_attr_cache_insert(ref, &attr);
    gen_mutex_unlock(&dbpf_attr_cache_mutex);

    PINT_perf_count(PINT_server_pc, PINT_PERF_METADATA_DSPACE_OPS,
                    1, PINT_PERF_SUB);

    *op_p->u.d_create.out_handle_p = new_handle;
    return DBPF_OP_COMPLETE;

return_error:
    if (new_handle != TROVE_HANDLE_NULL)
    {
        trove_handle_free(op_p->coll_p->coll_id, new_handle);
    }
    return ret;
}

static int dbpf_dspace_remove(TROVE_coll_id coll_id,
                              TROVE_handle handle,
                              TROVE_ds_flags flags,
                              void *user_ptr,
                              TROVE_context_id context_id,
                              TROVE_op_id *out_op_id_p)
{
    dbpf_queued_op_t *q_op_p = NULL;
    struct dbpf_op op;
    struct dbpf_op *op_p;
    struct dbpf_collection *coll_p = NULL;
    int ret;

    coll_p = dbpf_collection_find_registered(coll_id);
    if (coll_p == NULL)
    {
        return -TROVE_EINVAL;
    }

    ret = dbpf_op_init_queued_or_immediate(
        &op, &q_op_p,
        DSPACE_REMOVE,
        coll_p,
        handle,
        dbpf_dspace_remove_op_svc,
        flags,
        NULL,
        user_ptr,
        context_id,
        &op_p);
    if(ret < 0)
    {
        return ret;
    }

    PINT_perf_count(PINT_server_pc, PINT_PERF_METADATA_DSPACE_OPS,
                    1, PINT_PERF_ADD);

    return dbpf_queue_or_service(op_p, q_op_p, coll_p, out_op_id_p);
}

static int dbpf_dspace_remove_op_svc(struct dbpf_op *op_p)
{
    int count = 0;
    int ret = -TROVE_EINVAL;
    DBT key;
    TROVE_object_ref ref = {op_p->handle, op_p->coll_p->coll_id};

    memset(&key, 0, sizeof(key));
    key.data = &op_p->handle;
    key.size = sizeof(TROVE_handle);

    ret = op_p->coll_p->ds_db->del(op_p->coll_p->ds_db, NULL, &key, 0);
    switch (ret)
    {
        case DB_NOTFOUND:
            gossip_err("tried to remove non-existant dataspace\n");
            ret = -TROVE_ENOENT;
            goto return_error;
        default:
            op_p->coll_p->ds_db->err(
                op_p->coll_p->ds_db, ret, "dbpf_dspace_remove");
            ret = -dbpf_db_error_to_trove_error(ret);
            goto return_error;
        case 0:
            gossip_debug(GOSSIP_TROVE_DEBUG, "removed dataspace with "
                         "handle %llu\n", llu(op_p->handle));
            break;
    }

    /* if this attr is in the dbpf attr cache, remove it */
    gen_mutex_lock(&dbpf_attr_cache_mutex);
    dbpf_attr_cache_remove(ref);
    gen_mutex_unlock(&dbpf_attr_cache_mutex);

    /* move bstream if it exists to removable-bstreams. Not a fatal
     * error if this fails (may not have ever been created)
     */
    ret = dbpf_open_cache_remove(op_p->coll_p->coll_id, op_p->handle);

    /*
     * Notify deletion thread that we have something to do.
     */
    if (ret == 0)
    {
        gen_mutex_lock(
            & dbpf_op_queue_mutex[OP_QUEUE_BACKGROUND_FILE_REMOVAL]);
        pthread_cond_signal(
            & dbpf_op_incoming_cond[OP_QUEUE_BACKGROUND_FILE_REMOVAL]);
        gen_mutex_unlock(
            & dbpf_op_queue_mutex[OP_QUEUE_BACKGROUND_FILE_REMOVAL]);
    }

    /* remove the keyval entries for this handle if any exist.
     * this way seems a bit messy to me, i.e. we're operating
     * on keyval databases directly here instead of going through
     * the trove keyval interfaces.  It does allow us to perform the cleanup
     * of a handle without having to post more operations though.
     */
    ret = PINT_dbpf_keyval_iterate(
        op_p->coll_p->keyval_db,
        op_p->handle,
        op_p->coll_p->pcache,
        NULL,
        NULL,
        &count,
        TROVE_ITERATE_START,
        PINT_dbpf_keyval_remove);
    if(ret != 0 && ret != -TROVE_ENOENT)
    {
        goto return_error;
    }

    /* we still do a non-coalesced sync of the keyval db here
     * because we're in a dspace operation
     */
    DBPF_DB_SYNC_IF_NECESSARY(op_p, op_p->coll_p->keyval_db, ret);
    if(ret < 0)
    {
        goto return_error;
    }

    PINT_perf_count(PINT_server_pc, PINT_PERF_METADATA_DSPACE_OPS,
                    1, PINT_PERF_SUB);

    /* return handle to free list */
    trove_handle_free(op_p->coll_p->coll_id,op_p->handle);
    return DBPF_OP_COMPLETE;

return_error:
    return ret;
}

static int dbpf_dspace_iterate_handles(TROVE_coll_id coll_id,
                                       TROVE_ds_position *position_p,
                                       TROVE_handle *handle_array,
                                       int *inout_count_p,
                                       TROVE_ds_flags flags,
                                       TROVE_vtag_s *vtag,
                                       void *user_ptr,
                                       TROVE_context_id context_id,
                                       TROVE_op_id *out_op_id_p)
{
    dbpf_queued_op_t *q_op_p = NULL;
    struct dbpf_op op;
    struct dbpf_op *op_p;
    struct dbpf_collection *coll_p = NULL;
    int ret;

    coll_p = dbpf_collection_find_registered(coll_id);
    if (coll_p == NULL)
    {
        return -TROVE_EINVAL;
    }

    ret = dbpf_op_init_queued_or_immediate(
        &op, &q_op_p,
        DSPACE_ITERATE_HANDLES,
        coll_p,
        TROVE_HANDLE_NULL,
        dbpf_dspace_iterate_handles_op_svc,
        flags,
        NULL,
        user_ptr,
        context_id,
        &op_p);
    if(ret < 0)
    {
        return ret;
    }

   /* initialize op-specific members */
    op_p->u.d_iterate_handles.handle_array = handle_array;
    op_p->u.d_iterate_handles.position_p = position_p;
    op_p->u.d_iterate_handles.count_p = inout_count_p;

    return dbpf_queue_or_service(op_p, q_op_p, coll_p, out_op_id_p);
}

static int dbpf_dspace_iterate_handles_op_svc(struct dbpf_op *op_p)
{
    int ret = -TROVE_EINVAL, i = 0;
    DBC *dbc_p = NULL;
    DBT key, data;
    db_recno_t recno;
    TROVE_ds_storedattr_s s_attr;
    TROVE_handle dummy_handle = TROVE_HANDLE_NULL;

    if (*op_p->u.d_iterate_handles.position_p == TROVE_ITERATE_END)
    {
        /* already hit end of keyval space; return 1 */
        *op_p->u.d_iterate_handles.count_p = 0;
        return 1;
    }

    /* get a cursor */
    ret = op_p->coll_p->ds_db->cursor(op_p->coll_p->ds_db, NULL, &dbc_p, 0);
    if (ret != 0)
    {
        ret = -dbpf_db_error_to_trove_error(ret);
        gossip_err("failed to get a cursor\n");
        goto return_error;
    }

    /* we have two choices here: 'seek' to a specific key by either a:
     * specifying a key or b: using record numbers in the db.  record
     * numbers will serialize multiple modification requests. We are
     * going with record numbers for now.
     */

    /* an uninitialized cursor will start reading at the beginning of
     * the database (first record) when used with DB_NEXT, so we don't
     * need to position with DB_FIRST.
     */
    if (*op_p->u.d_iterate_handles.position_p != TROVE_ITERATE_START)
    {
        assert(sizeof(recno) < sizeof(dummy_handle));

        /* we need to position the cursor before we can read new
         * entries.  we will go ahead and read the first entry as
         * well, so that we can use the same loop below to read the
         * remainder in this or the above case.
         */
        dummy_handle = (TROVE_handle)
            (*op_p->u.d_iterate_handles.position_p);
        memset(&key, 0, sizeof(key));
        key.data  = &dummy_handle;
        key.size  = key.ulen = sizeof(dummy_handle);
        key.flags |= DB_DBT_USERMEM;

        memset(&data, 0, sizeof(data));
        data.data = &s_attr;
        data.size = data.ulen = sizeof(s_attr);
        data.flags |= DB_DBT_USERMEM;

        ret = dbc_p->c_get(dbc_p, &key, &data, DB_SET_RECNO);
        if (ret == DB_NOTFOUND)
        {
            goto return_ok;
        }
        else if (ret != 0)
        {
            ret = -dbpf_db_error_to_trove_error(ret);
            gossip_err("failed to set cursor position at %llu\n",
                       llu(dummy_handle));
            goto return_error;
        }
    }

    /* read handles until we run out of handles or space in buffer */
    for (i = 0; i < *op_p->u.d_iterate_handles.count_p; i++)
    {
        memset(&key, 0, sizeof(key));
        key.data = &op_p->u.d_iterate_handles.handle_array[i];
        key.size = key.ulen = sizeof(TROVE_handle);
        key.flags |= DB_DBT_USERMEM;

        memset(&data, 0, sizeof(data));
        data.data = &s_attr;
        data.size = data.ulen = sizeof(s_attr);
        data.flags |= DB_DBT_USERMEM;

        ret = dbc_p->c_get(dbc_p, &key, &data, DB_NEXT);
        if (ret == DB_NOTFOUND)
        {
            goto return_ok;
        }
        else if (ret != 0)
        {
            ret = -dbpf_db_error_to_trove_error(ret);
            gossip_err("c_get failed on iteration %d\n", i);
            goto return_error;
        }
    }

  return_ok:
    if (ret == DB_NOTFOUND)
    {
        /* if off the end of the database, return TROVE_ITERATE_END */
        *op_p->u.d_iterate_handles.position_p = TROVE_ITERATE_END;
    }
    else
    {
        /* get the record number to return.
         *
         * note: key field is ignored by c_get in this case
         */
        memset(&key, 0, sizeof(key));
        key.data = &dummy_handle;
        key.size = key.ulen = sizeof(dummy_handle);
        key.flags |= DB_DBT_USERMEM;

        memset(&data, 0, sizeof(data));
        data.data = &recno;
        data.size = data.ulen = sizeof(recno);
        data.flags |= DB_DBT_USERMEM;

        ret = dbc_p->c_get(dbc_p, &key, &data, DB_GET_RECNO);
        if (ret == DB_NOTFOUND)
        {
            gossip_debug(GOSSIP_TROVE_DEBUG, "iterate -- notfound\n");
        }
        else if (ret != 0)
        {
            gossip_debug(GOSSIP_TROVE_DEBUG, "iterate -- some other "
                         "failure @ recno\n");
            ret = -dbpf_db_error_to_trove_error(ret);
        }
        *op_p->u.d_iterate_handles.position_p = recno;
    }
    /* 'position' points to record we just read, or is set to END */

    *op_p->u.d_iterate_handles.count_p = i;

    if (dbc_p)
    {
        dbc_p->c_close(dbc_p);
    }

    return 1;

return_error:
    *op_p->u.d_iterate_handles.count_p = i;
    PVFS_perror_gossip("dbpf_dspace_iterate_handles_op_svc", ret);

    if (dbc_p)
    {
        dbc_p->c_close(dbc_p);
    }

    return ret;
}

static int dbpf_dspace_verify(TROVE_coll_id coll_id,
                              TROVE_handle handle,
                              TROVE_ds_type *type_p,
                              TROVE_ds_flags flags,
                              void *user_ptr,
                              TROVE_context_id context_id,
                              TROVE_op_id *out_op_id_p)
{
    dbpf_queued_op_t *q_op_p = NULL;
    struct dbpf_op op;
    struct dbpf_op *op_p;
    int ret;

    struct dbpf_collection *coll_p = NULL;

    coll_p = dbpf_collection_find_registered(coll_id);
    if (coll_p == NULL)
    {
        return -TROVE_EINVAL;
    }

    ret = dbpf_op_init_queued_or_immediate(
        &op, &q_op_p,
        DSPACE_VERIFY,
        coll_p,
        handle,
        dbpf_dspace_verify_op_svc,
        flags,
        NULL,
        user_ptr,
        context_id,
        &op_p);
    if(ret < 0)
    {
        return ret;
    }

   /* initialize op-specific members */
    op_p->u.d_verify.type_p = type_p;

    return dbpf_queue_or_service(op_p, q_op_p, coll_p, out_op_id_p);
}

static int dbpf_dspace_verify_op_svc(struct dbpf_op *op_p)
{
    int ret = -TROVE_EINVAL;
    DBT key, data;
    TROVE_ds_storedattr_s s_attr;

    memset(&key, 0, sizeof(key));
    key.data = &op_p->handle;
    key.size = sizeof(TROVE_handle);

    memset(&data, 0, sizeof(data));
    data.data = &s_attr;
    data.size = data.ulen = sizeof(s_attr);
    data.flags |= DB_DBT_USERMEM;

    /* check to see if dspace handle is used (ie. object exists) */
    ret = op_p->coll_p->ds_db->get(op_p->coll_p->ds_db, NULL, &key, &data, 0);
    if (ret == 0)
    {
        /* object exists */
    }
    else if (ret == DB_NOTFOUND)
    {
        /* no error in access, but object does not exist */
        ret = -TROVE_ENOENT;
        goto return_error;
    }
    else
    {
        /* error in accessing database */
        ret = -dbpf_db_error_to_trove_error(ret);
        goto return_error;
    }

    /* copy type value back into user's memory */
    *op_p->u.d_verify.type_p = s_attr.type;

    return 1;

return_error:
    return ret;
}

static int dbpf_dspace_getattr(TROVE_coll_id coll_id,
                               TROVE_handle handle,
                               TROVE_ds_attributes_s *ds_attr_p,
                               TROVE_ds_flags flags,
                               void *user_ptr,
                               TROVE_context_id context_id,
                               TROVE_op_id *out_op_id_p)
{
    dbpf_queued_op_t *q_op_p = NULL;
    struct dbpf_op op;
    struct dbpf_op *op_p;
    struct dbpf_collection *coll_p = NULL;
    TROVE_object_ref ref = {handle, coll_id};
    int ret;

    /* fast path cache hit; skips queueing */
    gen_mutex_lock(&dbpf_attr_cache_mutex);
    if (dbpf_attr_cache_ds_attr_fetch_cached_data(ref, ds_attr_p) == 0)
    {
#if 0
        gossip_debug(
            GOSSIP_TROVE_DEBUG, "ATTRIB: retrieved "
            "attributes from CACHE for key %llu\n  uid = %d, mode = %d, "
            "type = %d, dfile_count = %d, dist_size = %d\n",
            llu(handle), (int)ds_attr_p->uid, (int)ds_attr_p->mode,
            (int)ds_attr_p->type, (int)ds_attr_p->dfile_count,
            (int)ds_attr_p->dist_size);
#endif
        gossip_debug(GOSSIP_DBPF_ATTRCACHE_DEBUG, "dspace_getattr fast "
                     "path attr cache hit on %llu\n (dfile_count=%d | "
                     "dist_size=%d | data_size=%lld)\n", llu(handle),
                     ds_attr_p->dfile_count, ds_attr_p->dist_size,
                     lld(ds_attr_p->b_size));

        UPDATE_PERF_METADATA_READ();
        gen_mutex_unlock(&dbpf_attr_cache_mutex);
        return 1;
    }
    gen_mutex_unlock(&dbpf_attr_cache_mutex);

    coll_p = dbpf_collection_find_registered(coll_id);
    if (coll_p == NULL)
    {
        return -TROVE_EINVAL;
    }

    ret = dbpf_op_init_queued_or_immediate(
        &op, &q_op_p,
        DSPACE_GETATTR,
        coll_p,
        handle,
        dbpf_dspace_getattr_op_svc,
        flags,
        NULL,
        user_ptr,
        context_id,
        &op_p);
    if(ret < 0)
    {
        return ret;
    }

   /* initialize op-specific members */
    op_p->u.d_getattr.attr_p = ds_attr_p;

    return dbpf_queue_or_service(op_p, q_op_p, coll_p, out_op_id_p);
}

static int dbpf_dspace_setattr(TROVE_coll_id coll_id,
                               TROVE_handle handle,
                               TROVE_ds_attributes_s *ds_attr_p,
                               TROVE_ds_flags flags,
                               void *user_ptr,
                               TROVE_context_id context_id,
                               TROVE_op_id *out_op_id_p)
{
    dbpf_queued_op_t *q_op_p = NULL;
    struct dbpf_op op;
    struct dbpf_op *op_p;
    struct dbpf_collection *coll_p = NULL;
    int ret;

    coll_p = dbpf_collection_find_registered(coll_id);
    if (coll_p == NULL)
    {
        return -TROVE_EINVAL;
    }

    ret = dbpf_op_init_queued_or_immediate(
        &op, &q_op_p,
        DSPACE_SETATTR,
        coll_p,
        handle,
        dbpf_dspace_setattr_op_svc,
        flags,
        NULL,
        user_ptr,
        context_id,
        &op_p);
    if(ret < 0)
    {
        return ret;
    }

   /* initialize op-specific members */
    op_p->u.d_setattr.attr_p = ds_attr_p;

    PINT_perf_count(PINT_server_pc, PINT_PERF_METADATA_DSPACE_OPS,
                    1, PINT_PERF_ADD);

    return dbpf_queue_or_service(op_p, q_op_p, coll_p, out_op_id_p);
}

static int dbpf_dspace_setattr_op_svc(struct dbpf_op *op_p)
{
    int ret = -TROVE_EINVAL;
    DBT key, data;
    TROVE_ds_storedattr_s s_attr;
    TROVE_object_ref ref = {op_p->handle, op_p->coll_p->coll_id};

    memset(&key, 0, sizeof(key));
    key.data = &op_p->handle;
    key.size = sizeof(TROVE_handle);
    
    memset(&data, 0, sizeof(data));
    data.data = &s_attr;
    data.size = sizeof(s_attr);

    trove_ds_attr_to_stored((*op_p->u.d_setattr.attr_p), s_attr);

#if 0
    gossip_debug(GOSSIP_TROVE_DEBUG, "ATTRIB: dspace_setattr storing "
                 "attributes (2) on key %llu\n uid = %d, mode = %d, "
                 "type = %d, dfile_count = %d, dist_size = %d\n",
                 llu(op_p->handle), (int) s_attr.uid, (int) s_attr.mode,
                 (int) s_attr.type, (int) s_attr.dfile_count,
                 (int) s_attr.dist_size);
#endif

    ret = op_p->coll_p->ds_db->put(
        op_p->coll_p->ds_db, NULL, &key, &data, 0);
    if (ret != 0)
    {
        op_p->coll_p->ds_db->err(
            op_p->coll_p->ds_db, ret, "DB->put setattr");
        ret = -dbpf_db_error_to_trove_error(ret);
        goto return_error;
    }

    /* now that the disk is updated, update the cache if necessary */
    gen_mutex_lock(&dbpf_attr_cache_mutex);
    dbpf_attr_cache_ds_attr_update_cached_data(
        ref, op_p->u.d_setattr.attr_p);
    gen_mutex_unlock(&dbpf_attr_cache_mutex);

    PINT_perf_count(PINT_server_pc, PINT_PERF_METADATA_DSPACE_OPS,
                    1, PINT_PERF_SUB);

    return DBPF_OP_COMPLETE;
    
return_error:
    return ret;
}

static int dbpf_dspace_getattr_op_svc(struct dbpf_op *op_p)
{
    int ret = -TROVE_EINVAL;
    DBT key, data;
    TROVE_ds_storedattr_s s_attr;
    TROVE_ds_attributes *attr = NULL;
    TROVE_size b_size;
    struct stat b_stat;
    TROVE_object_ref ref = {op_p->handle, op_p->coll_p->coll_id};
    struct open_cache_ref tmp_ref;

    /* get an fd for the bstream so we can check size */
    ret = dbpf_open_cache_get(
        op_p->coll_p->coll_id, op_p->handle, 0, &tmp_ref);
    if (ret < 0)
    {
        b_size = 0;
    }
    else
    {
        ret = DBPF_FSTAT(tmp_ref.fd, &b_stat);
        dbpf_open_cache_put(&tmp_ref);
        if (ret < 0)
        {
            ret = -TROVE_EBADF;
            goto return_error;
        }
        b_size = (TROVE_size)b_stat.st_size;
    }

    memset(&key, 0, sizeof(key));
    key.data = &op_p->handle;
    key.size = sizeof(TROVE_handle);

    memset(&data, 0, sizeof(data));
    memset(&s_attr, 0, sizeof(TROVE_ds_storedattr_s));
    data.data = &s_attr;
    data.size = data.ulen = sizeof(TROVE_ds_storedattr_s);
    data.flags |= DB_DBT_USERMEM;

    ret = op_p->coll_p->ds_db->get(op_p->coll_p->ds_db, 
                                   NULL, &key, &data, 0);
    if (ret != 0)
    {
        op_p->coll_p->ds_db->err(op_p->coll_p->ds_db, ret, "DB->get");
        ret = -dbpf_db_error_to_trove_error(ret);
        goto return_error;
    }

    gossip_debug(
        GOSSIP_TROVE_DEBUG, "ATTRIB: retrieved attributes "
        "from DISK for key %llu\n\tuid = %d, mode = %d, type = %d, "
        "dfile_count = %d, dist_size = %d\n\tb_size = %lld\n",
        llu(op_p->handle), (int)s_attr.uid, (int)s_attr.mode,
        (int)s_attr.type, (int)s_attr.dfile_count, (int)s_attr.dist_size,
        llu(b_size));

    attr = op_p->u.d_getattr.attr_p;
    trove_ds_stored_to_attr(s_attr, *attr, b_size);

    /* add retrieved ds_attr to dbpf_attr cache here */
    gen_mutex_lock(&dbpf_attr_cache_mutex);
    dbpf_attr_cache_insert(ref, attr);
    gen_mutex_unlock(&dbpf_attr_cache_mutex);

    return 1;
    
return_error:
    return ret;
}

static int dbpf_dspace_cancel(
    TROVE_coll_id coll_id,
    TROVE_op_id id,
    TROVE_context_id context_id)
{
    int ret = -TROVE_ENOSYS;
    enum dbpf_op_state state = OP_UNITIALIZED;
    gen_mutex_t *context_mutex = NULL;
    dbpf_queued_op_t *cur_op = NULL;

    gossip_debug(GOSSIP_TROVE_DEBUG, "dbpf_dspace_cancel called for "
                 "id %llu.\n", llu(id));

    assert(dbpf_completion_queue_array[context_id]);
    context_mutex = dbpf_completion_queue_array_mutex[context_id];
    assert(context_mutex);
    
    /*
     * Protect against removal from completion queue while we read state
     * cases:
     *  1) op is completed or canceled => finish
     *  2) op is pending in a op_queue => find out which queue => cancel op
     *  3) op is serviced => finish, we cannot do anything.
     */
    gen_mutex_lock(context_mutex);
    cur_op = id_gen_safe_lookup(id);
    if (cur_op == NULL)
    {
        gen_mutex_unlock(context_mutex);
        gossip_err("Invalid operation to test against\n");
        return -TROVE_EINVAL;
    }

    state = dbpf_op_get_status(cur_op);
    
    switch(state)
    {
        case OP_QUEUED:
        {
            enum dbpf_op_type type = cur_op->queue_type; 
            /*
             * Another thread might draw the object from the pending queue while we
             * are here so we have to double check to avoid this case !
             */
            gossip_debug(GOSSIP_TROVE_DEBUG,
                         "op %p is queued: handling\n", cur_op);
                         
            gen_mutex_lock(& dbpf_op_queue_mutex[ type ] );
            state = dbpf_op_get_status(cur_op);
            if ( state == OP_QUEUED )
            {
                /*
                 * Now we are sure that the object is still pending !
                 * dequeue and complete the op in canceled state 
                 */
                dbpf_op_queue_remove(cur_op);    
                dbpf_move_op_to_completion_queue_nolock(cur_op, 0, OP_CANCELED);
                gossip_debug(
                    GOSSIP_TROVE_DEBUG, "op %p is canceled\n", cur_op);          
                pthread_cond_signal(& dbpf_op_completed_cond);
                       
            }else{
                gossip_debug(
                    GOSSIP_TROVE_DEBUG, "op %p could not be canceled, is drawn"
                    " from pending queue\n", cur_op);
            }
            gen_mutex_unlock(& dbpf_op_queue_mutex[ type ] );
            
            ret = 0;
        }
        break;
        case OP_IN_SERVICE:
        {
            /*
              for bstream i/o op, try an aio_cancel.  for other ops,
              there's not much we can do other than let the op
              complete normally
            */
            if ((cur_op->op.type == BSTREAM_READ_LIST) ||
                (cur_op->op.type == BSTREAM_WRITE_LIST))
            {
                ret = aio_cancel(cur_op->op.u.b_rw_list.fd,
                                 cur_op->op.u.b_rw_list.aiocb_array);
                gossip_debug(
                    GOSSIP_TROVE_DEBUG, "aio_cancel returned %s\n",
                    ((ret == AIO_CANCELED) ? "CANCELED" :
                     "NOT CANCELED"));
                /*
                  NOTE: the normal aio notification method takes care
                  of completing the op and moving it to the completion
                  queue
                */
            }
            else
            {
                gossip_debug(
                    GOSSIP_TROVE_DEBUG, "op is in service: ignoring "
                    "operation type %d\n", cur_op->op.type);
            }
            ret = 0;
        }
        break;
        case OP_COMPLETED:
        case OP_CANCELED:
            /* easy cancelation case; do nothing */
            gossip_debug(
                GOSSIP_TROVE_DEBUG, "op is completed: ignoring\n");
            ret = 0;
            break;
        default:
            gossip_err("Invalid dbpf_op state found (%d)\n", state);
            assert(0);
    }    
    
    gen_mutex_unlock(context_mutex);

    return ret;
}


/* dbpf_dspace_test()
 *
 * Returns 0 if not completed, 1 if completed (successfully or with
 * error).
 *
 * The error state of the completed operation is returned via the
 * state_p, more to follow on this...
 *
 * out_count gets the count of completed operations too...
 *
 * Removes completed operations from the queue.
 */
static int dbpf_dspace_test(
    TROVE_coll_id coll_id,
    TROVE_op_id id,
    TROVE_context_id context_id,
    int *out_count_p,
    TROVE_vtag_s *vtag,
    void **returned_user_ptr_p,
    TROVE_ds_state *state_p,
    int max_idle_time_ms)
{
    int ret = -TROVE_EINVAL;
    dbpf_queued_op_t *cur_op = NULL;
    enum dbpf_op_state state = 0;
    gen_mutex_t *context_mutex = NULL;

    *out_count_p = 0;
    assert(dbpf_completion_queue_array[context_id]);
    context_mutex = dbpf_completion_queue_array_mutex[context_id];
    assert(context_mutex);
    cur_op = id_gen_safe_lookup(id);
    if (cur_op == NULL)
    {
        gossip_err("Invalid operation to test against\n");
        return ret;
    }

    gen_mutex_lock(context_mutex);

    /* check the state of the current op to see if it's completed */
    state = dbpf_op_get_status(cur_op);

    /* if the op is not completed, wait for up to max_idle_time_ms */
    if ((state != OP_COMPLETED) && (state != OP_CANCELED))
    {
        struct timeval base;
        struct timespec wait_time;

        /* compute how long to wait */
        gettimeofday(&base, NULL);
        wait_time.tv_sec = base.tv_sec +
            (max_idle_time_ms / 1000);
        wait_time.tv_nsec = base.tv_usec * 1000 + 
            ((max_idle_time_ms % 1000) * 1000000);
        if (wait_time.tv_nsec > 1000000000)
        {
            wait_time.tv_nsec = wait_time.tv_nsec - 1000000000;
            wait_time.tv_sec++;
        }

        ret = pthread_cond_timedwait(&dbpf_op_completed_cond,
                                     context_mutex, &wait_time);

        if (ret == ETIMEDOUT)
        {
            goto op_not_completed;
        }
        else
        {
            /* some op completed, check if it's the one we're testing */
            state = dbpf_op_get_status(cur_op);

            if ((state == OP_COMPLETED) || (state == OP_CANCELED))
            {
                goto op_completed;
            }
            goto op_not_completed;
        }
    }
    else
    {
      op_completed:
        assert(!dbpf_op_queue_empty(dbpf_completion_queue_array[context_id]));

        /* pull the op out of the context specific completion queue */
        dbpf_op_queue_remove(cur_op);
        gen_mutex_unlock(context_mutex);

        *out_count_p = 1;
        *state_p = cur_op->state;

        if (returned_user_ptr_p != NULL)
        {
            *returned_user_ptr_p = cur_op->op.user_ptr;
        }

        organize_post_op_statistics(cur_op->op.type, cur_op->op.id);
        dbpf_queued_op_free(cur_op);
        return 1;
    }

  op_not_completed:
    gen_mutex_unlock(context_mutex);
    return 0;
}

static int dbpf_dspace_testcontext(
    TROVE_coll_id coll_id,
    TROVE_op_id *ds_id_array,
    int *inout_count_p,
    TROVE_ds_state *state_array,
    void** user_ptr_array,
    int max_idle_time_ms,
    TROVE_context_id context_id)
{
    int ret = 0;
    dbpf_queued_op_t *cur_op = NULL;
    int out_count = 0, limit = *inout_count_p;
    gen_mutex_t *context_mutex = NULL;
    void **user_ptr_p = NULL;

    assert(dbpf_completion_queue_array[context_id]);

    context_mutex = dbpf_completion_queue_array_mutex[context_id];
    assert(context_mutex);

    assert(inout_count_p);
    *inout_count_p = 0;

    /*
      check completion queue for any completed ops and return
      them in the provided ds_id_array (up to inout_count_p).
      otherwise, cond_timedwait for max_idle_time_ms.

      we will only sleep if there is nothing to do; otherwise 
      we return whatever we find ASAP
    */
    gen_mutex_lock(context_mutex);
    if (dbpf_op_queue_empty(dbpf_completion_queue_array[context_id]))
    {
        struct timeval base;
        struct timespec wait_time;

        /* compute how long to wait */
        gettimeofday(&base, NULL);
        wait_time.tv_sec = base.tv_sec +
            (max_idle_time_ms / 1000);
        wait_time.tv_nsec = base.tv_usec * 1000 + 
            ((max_idle_time_ms % 1000) * 1000000);
        if (wait_time.tv_nsec > 1000000000)
        {
            wait_time.tv_nsec = wait_time.tv_nsec - 1000000000;
            wait_time.tv_sec++;
        }

        ret = pthread_cond_timedwait(&dbpf_op_completed_cond,
                                     context_mutex, &wait_time);

        if (ret == ETIMEDOUT)
        {
            /* we timed out without being awoken- this means there is
             * no point in checking the completion queue, we should just
             * return
             */
            gen_mutex_unlock(context_mutex);
            *inout_count_p = 0;
            return(0);
        }
    }

    while(!dbpf_op_queue_empty(dbpf_completion_queue_array[context_id]) &&
          (out_count < limit))
    {
        cur_op = dbpf_op_pop_front_nolock(
            dbpf_completion_queue_array[context_id]);
        assert(cur_op);

        state_array[out_count] = cur_op->state;

        user_ptr_p = &user_ptr_array[out_count];
        if (user_ptr_p != NULL)
        {
            *user_ptr_p = cur_op->op.user_ptr;
        }
        ds_id_array[out_count] = cur_op->op.id;

        organize_post_op_statistics(cur_op->op.type, cur_op->op.id);
        dbpf_queued_op_free(cur_op);

        out_count++;
    }
    gen_mutex_unlock(context_mutex);

    *inout_count_p = out_count;
    ret = 0;
    return ret;
}

/* dbpf_dspace_testsome()
 *
 * Returns 0 if nothing completed, 1 if something is completed
 * (successfully or with error).
 *
 * The error state of the completed operation is returned via the
 * state_p.
 */
static int dbpf_dspace_testsome(
    TROVE_coll_id coll_id,
    TROVE_context_id context_id,
    TROVE_op_id *ds_id_array,
    int *inout_count_p,
    int *out_index_array,
    TROVE_vtag_s *vtag_array,
    void **returned_user_ptr_array,
    TROVE_ds_state *state_array,
    int max_idle_time_ms)
{
    int i = 0, out_count = 0, ret = 0;
    enum dbpf_op_state state = OP_UNITIALIZED;
    int wait = 0;
    dbpf_queued_op_t *cur_op = NULL;
    gen_mutex_t *context_mutex = NULL;
    void **returned_user_ptr_p = NULL;

    assert(dbpf_completion_queue_array[context_id]);

    context_mutex = dbpf_completion_queue_array_mutex[context_id];
    assert(context_mutex);

    gen_mutex_lock(context_mutex);
  scan_for_completed_ops:

    assert(inout_count_p);
    for (i = 0; i < *inout_count_p; i++)
    {
        cur_op = id_gen_safe_lookup(ds_id_array[i]);
        if (cur_op == NULL)
        {
            gen_mutex_unlock(context_mutex);
            gossip_err("Invalid operation to testsome against\n");
            return -TROVE_EINVAL;
        }

        /* check the state of the current op to see if it's completed */
        state = dbpf_op_get_status(cur_op);

        if ((state == OP_COMPLETED) || (state == OP_CANCELED))
        {
            assert(!dbpf_op_queue_empty(
                       dbpf_completion_queue_array[context_id]));

            /* pull the op out of the context specific completion queue */
            dbpf_op_queue_remove(cur_op);

            state_array[out_count] = cur_op->state;

            returned_user_ptr_p = &returned_user_ptr_array[out_count];
            if (returned_user_ptr_p != NULL)
            {
                *returned_user_ptr_p = cur_op->op.user_ptr;
            }
            organize_post_op_statistics(cur_op->op.type, cur_op->op.id);
            dbpf_queued_op_free(cur_op);
        }
        ret = (((state == OP_COMPLETED) ||
                (state == OP_CANCELED)) ? 1 : 0);
        if (ret != 0)
        {
            /* operation is done and we are telling the caller;
             * ok to pull off queue now.
             */
            out_index_array[out_count] = i;
            out_count++;
        }
    }

    /* if no op completed, wait for up to max_idle_time_ms */
    if ((wait == 0) && (out_count == 0) && (max_idle_time_ms > 0))
    {
        struct timeval base;
        struct timespec wait_time;

        /* compute how long to wait */
        gettimeofday(&base, NULL);
        wait_time.tv_sec = base.tv_sec +
            (max_idle_time_ms / 1000);
        wait_time.tv_nsec = base.tv_usec * 1000 + 
            ((max_idle_time_ms % 1000) * 1000000);
        if (wait_time.tv_nsec > 1000000000)
        {
            wait_time.tv_nsec = wait_time.tv_nsec - 1000000000;
            wait_time.tv_sec++;
        }

        ret = pthread_cond_timedwait(&dbpf_op_completed_cond,
                                     context_mutex, &wait_time);

        if (ret != ETIMEDOUT)
        {
            /*
              since we were signaled awake (rather than timed out
              while sleeping), we're going to rescan ops here for
              completion.  if nothing completes the second time
              around, we're giving up and won't be back here.
            */
            wait = 1;

            goto scan_for_completed_ops;
        }
    }

    gen_mutex_unlock(context_mutex);

    *inout_count_p = out_count;
    return ((out_count > 0) ? 1 : 0);
}

int PINT_trove_dbpf_ds_attr_compare(
    DB * dbp, const DBT * a, const DBT * b)
{
    const TROVE_handle * handle_a;
    const TROVE_handle * handle_b;

    handle_a = (const TROVE_handle *) a->data;
    handle_b = (const TROVE_handle *) b->data;

    if(*handle_a == *handle_b)
    {
        return 0;
    }

    return (*handle_a < *handle_b) ? -1 : 1;
}

struct TROVE_dspace_ops dbpf_dspace_ops =
{
    dbpf_dspace_create,
    dbpf_dspace_remove,
    dbpf_dspace_iterate_handles,
    dbpf_dspace_verify,
    dbpf_dspace_getattr,
    dbpf_dspace_setattr,
    dbpf_dspace_cancel,
    dbpf_dspace_test,
    dbpf_dspace_testsome,
    dbpf_dspace_testcontext
};

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
