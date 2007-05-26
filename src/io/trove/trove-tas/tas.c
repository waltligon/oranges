/*
 * Author Julian Kunkel
 * Date: 2005
 */

#include <stdio.h>
#include <string.h>

#ifdef HAVE_MALLOC_H
#include <malloc.h>
#endif
/* for unlink: */
#include <unistd.h>
#include <assert.h>

#include "gen-locks.h"
#include "pvfs2-internal.h"
#include "trove.h"
#include "trove-internal.h"
#include "gossip.h"
#include "trove-handle-mgmt.h"

extern struct TROVE_dspace_ops dbpf_dspace_ops;
extern struct TROVE_mgmt_ops dbpf_mgmt_ops;

/* some local defines */
#define TRUE 1
#define FALSE 0
#define DUMMY_OPNUM -1
#define RETURN_IMMEDIATE_COMPLETE 1
#define RETURN_TEST_COMPLETE 0

/*
 * There are two different implementations which may be choosen while compile time:
 * 1) A very fast queue implementation which pushes
 * the last used handle in front of the queue (speedup for second lookup).
 * However the queue implementation is for the creation of very much metafiles
 * slow because all handles have to be checked. There are some optimizations for
 * that implementation, but the creation optimization is not compatible to the
 * normal trove behaviour !
 * 2) a red black tree implementation. Takes more memory than the list,
 * but faster lookup because the depth <= 40 for 100.000 filehandles....
 * TAS_USE_QUEUE, TAS_USE_RED_BLACKTREE
 */


/* by default choose red-black-tree representation */
#if defined(TAS_USE_RED_BLACKTREE)
#elif defined(TAS_USE_QUEUE)
#else
/*    #warning "No internal datastructure for TROVE-TAS selected.\n Choosing Red-Black-Tree" */
#define TAS_USE_RED_BLACKTREE
#endif

#if defined(TAS_USE_RED_BLACKTREE)
#include "red-black-tree.h"
#elif defined(TAS_USE_QUEUE)

#include "tas-queue.h"
        /*
         * maximum number of key/value pairs to be read until the functions returns
         * that the key/value pair does not exist. This is a optimization for the creation of
         * files. However if more files exist in a directory than that number these files cannot
         * be accessed !!!! So be warned if you turn on QUEUE_MAXIMUM_KEYVAL_READ_OPTIZATION
         */
#define QUEUE_MAXIMUM_KEYVAL_READ_OPTIZATION

        /* set this value to at least ( number of clients x 10 ),
         * the number depends on the speed of the netwerk link.
         */
#define QUEUE_MAXIMUM_KEYVAL_READ_OPS 100
#endif

/* this optimizations can be turned off:
 * set the file size during IO operations, only small performance speedup.
 */
#define TAS_UPDATE_FILESIZE

/*
 * Do not check if handle is already created in dspace_create...
 */
#define NO_SAVE_CREATION



/* tas metadata + dspace cache.... */
struct keyvalpair_
{
#if defined(TAS_USE_RED_BLACKTREE)
    struct keyvalpair_ *next;
    struct keyvalpair_ *prev;   /* needed for fast keyval_iteration... */
#elif defined(TAS_USE_QUEUE)
    struct keyvalpair_ *next;
    int64_t key_hash;
#endif

    TROVE_keyval_s key;
    TROVE_keyval_s val;
};


typedef struct keyvalpair_ keyvalpair;


struct store_attributes_
{
    TROVE_ds_type type;
    PVFS_uid uid;
    PVFS_gid gid;
    PVFS_permissions mode;
    PVFS_time atime;
    PVFS_time ctime;
    PVFS_time mtime;
    uint32_t dfile_count;
    uint32_t dist_size;
};
typedef struct store_attributes_ store_attributes;



struct handle_
{

#if defined(TAS_USE_RED_BLACKTREE)
    red_black_tree *pair_tree;
    keyvalpair *pair_list;
#elif defined(TAS_USE_QUEUE)
    keyvalpair *pairs;
#endif

    PVFS_fs_id fs_id;
    PVFS_handle handle;

    PVFS_size b_size;           /* bstream size */
    PVFS_size k_size;           /* keyval size; # of keys */
    /* stored attributes are below here */

    store_attributes *attributes;
};

typedef struct handle_ handle_cache;


struct handle_fsid_
{
    PVFS_handle handle;
    PVFS_fs_id fs_id;
};
typedef struct handle_fsid_ handle_fsid;


static PVFS_size meta_count = 0;
static int initialised = 0;

static gen_mutex_t tas_meta_mutex = GEN_MUTEX_INITIALIZER;

static char storage_name[200] = "";
static char storage_path[200] = "";

TROVE_handle akt_handle_first;
/* number which seperates first and second handle range,
 * for example when this server is a meta and dataserver */
TROVE_handle handle_separator;
TROVE_handle akt_handle_second;

inline static void key_copy(TROVE_keyval_s * out_key_array,
                              int i, keyvalpair * pair)
{
    int max_read_size = pair->key.buffer_sz;
    if (out_key_array[i].buffer_sz < max_read_size)
    {
        max_read_size = out_key_array[i].buffer_sz;
    }
    out_key_array[i].read_sz = max_read_size;
    memcpy(out_key_array[i].buffer, pair->key.buffer, max_read_size);
}

inline static void val_copy(TROVE_keyval_s * out_val_array,
                              int i, keyvalpair * pair)
{
    int max_read_size = pair->val.buffer_sz;
    if (out_val_array[i].buffer_sz < max_read_size)
    {
        max_read_size = out_val_array[i].buffer_sz;
    }
    out_val_array[i].read_sz = max_read_size;
    memcpy(out_val_array[i].buffer, pair->val.buffer, max_read_size);
}


inline static void keyval_copy(TROVE_keyval_s * out_key_array,
                              TROVE_keyval_s * out_val_array,
                              int i, keyvalpair * pair)
{
    key_copy(out_key_array, i,pair);
    val_copy(out_val_array, i, pair);
}

#if defined(TAS_USE_RED_BLACKTREE)
        /* there might be conflicts in the red-black-tree for key_hashValues
         * a solution would be to use the full void* key and not a int64_t key
         * a order might be established by a lexical sort.
         * a other solution is to link
         * compare function for the red-black-tree */
static int compare_keyval_pairs(
    RBData * data,
    RBKey * key)
{
    TROVE_keyval_s ikey1 = ((keyvalpair *) data)->key;
    TROVE_keyval_s *ikey2 = (TROVE_keyval_s *) key;

    if (ikey1.buffer_sz > ikey2->buffer_sz)
    {   /* take right neigbour */
        return +1;
    }
    else if (ikey1.buffer_sz < ikey2->buffer_sz)
    {   /*take left neigbour */
        return -1;
    }
    /*same length, compare memory */
    return memcmp(ikey1.buffer, ikey2->buffer, ikey1.buffer_sz);
}

static int compare_keyval_pairs2(
    RBData * data,
    RBData * data2)
{
    TROVE_keyval_s ikey1 = ((keyvalpair *) data)->key;
    TROVE_keyval_s ikey2 = ((keyvalpair *) data2)->key;

    if (ikey1.buffer_sz > ikey2.buffer_sz)
    {   /*take right neigbour */
        return +1;
    }
    else if (ikey1.buffer_sz < ikey2.buffer_sz)
    {   /*take left neigbour */
        return -1;
    }
    /*same length, compare memory */
    return memcmp(ikey1.buffer, ikey2.buffer, ikey1.buffer_sz);
}

        /*for comparision of handles: */
static int compare_handle(
    RBData * data,
    RBKey * key)
{
    handle_cache *ikey1 = (handle_cache *) data;
    handle_fsid *ikey2 = (handle_fsid *) key;

    if (ikey1->handle > ikey2->handle)
    {
        return +1;
    }
    else if (ikey1->handle < ikey2->handle)
    {
        return -1;
    }

    if (ikey1->fs_id > ikey2->fs_id)
    {
        return +1;
    }
    else if (ikey1->fs_id < ikey2->fs_id)
    {
        return -1;
    }
    return 0;
}

static int compare_handle2(
    RBData * data,
    RBData * data2)
{
    handle_cache *ikey1 = (handle_cache *) data;
    handle_cache *ikey2 = (handle_cache *) data2;

    if (ikey1->handle > ikey2->handle)
    {
        return +1;
    }
    else if (ikey1->handle < ikey2->handle)
    {
        return -1;
    }

    if (ikey1->fs_id > ikey2->fs_id)
    {
        return +1;
    }
    else if (ikey1->fs_id < ikey2->fs_id)
    {
        return -1;
    }
    return 0;
}
#endif

#if defined(TAS_USE_RED_BLACKTREE)
static red_black_tree *tas_meta_cache = NULL;
#elif defined(TAS_USE_QUEUE)
static tas_queue *tas_meta_cache = NULL;
#endif

/* simple word sum is hashval */
static inline int64_t queue_key_hash_func(
    void *key,
    int count)
{
    int64_t sum = 0;
    int i;
    int64_t multiplier = 1;
    char *keyc = (char *) key;

    for (i = 0; i < count; i++)
    {
        sum = sum + multiplier * keyc[i];
        multiplier = multiplier * 10;
    }

    return sum;
}

static inline int keys_equal(
#ifndef TAS_USE_RED_BLACKTREE
    int key_hash1,
    int key_hash2,
#endif
    int key_len1,
    int key_len2,
    void *key1,
    void *key2)
{
    return
#ifndef TAS_USE_RED_BLACKTREE
        key_hash1 == key_hash2 &&
#endif
        key_len1 == key_len2 && memcmp(key1, key2, key_len1) == 0;
}



static handle_cache *allocate_new_cache_element(void)
{
    handle_cache *found = (handle_cache *) malloc(sizeof(handle_cache));
    memset(found, 0, sizeof(handle_cache));
    found->attributes = (store_attributes *) malloc(sizeof(store_attributes));
    memset(found->attributes, 0, sizeof(store_attributes));
    return found;
}

static void insert_cache_element(
    handle_cache * found,
    TROVE_handle new_handle,
    TROVE_coll_id coll_id,
    TROVE_ds_type type)
{
    found->fs_id = coll_id;
    found->attributes->type = type;
    found->handle = new_handle;

#if defined(TAS_USE_RED_BLACKTREE)
    found->pair_list = NULL;
    found->pair_tree = NULL;
    insert_key_into_tree((RBData *) found, tas_meta_cache);
#elif defined(TAS_USE_QUEUE)
    found->pairs = NULL;
    push_front_key(new_handle, found, tas_meta_cache);
#endif

    meta_count++;
}

static keyvalpair *find_pair_of_cached_entry(
    handle_cache * cache_entry,
    TROVE_keyval_s * key_p)
{
#if defined(TAS_USE_RED_BLACKTREE)
    if (cache_entry->pair_tree == NULL)
    {
        return FALSE;
    }
    tree_node *node = (tree_node *) (lookup_tree(key_p, cache_entry->pair_tree));

    if (node == NULL)
        return FALSE;
    return (keyvalpair *) node->data;
#elif defined(TAS_USE_QUEUE)
    keyvalpair *found_pair;
    int i = 0;
    int64_t key_hash = queue_key_hash_func(key_p->buffer, key_p->buffer_sz);
    found_pair = cache_entry->pairs;
    for (; found_pair != NULL; found_pair = found_pair->next)
    {
        if (keys_equal(
#ifndef TAS_USE_RED_BLACKTREE
                         found_pair->key_hash, key_hash,
#endif
                         found_pair->key.buffer_sz, key_p->buffer_sz,
                         found_pair->key.buffer, key_p->buffer))
        {
            /* key exists */
            if (i > 10)
            {
                gossip_err(" WARNING: readToMatchKEY/VALUE: %d\n", i);
            }
            return found_pair;
        }
        i++;
#ifdef QUEUE_MAXIMUM_KEYVAL_READ_OPTIZATION
        if (i > QUEUE_MAXIMUM_KEYVAL_READ_OPS)
            /*      printf(" KILLED %s\n",(char*)key_p->buffer); */
            return NULL;
#endif
    }

    if (i > 10)
    {
        gossip_err
            (" WARNING: readToMatchKey did not find value for key %s needed iterations: %d\n",
             (char *) key_p->buffer, i);
    }
#endif

    return NULL;
}

static int remove_pair_of_cached_entry(
    handle_cache * cache_entry,
    TROVE_keyval_s * key_p,
    TROVE_keyval_s * val_p)
{
    keyvalpair *found_pair;

#if defined(TAS_USE_RED_BLACKTREE)
    if (cache_entry->pair_tree == NULL)
    {
        return FALSE;
    }
    tree_node *node = lookup_tree(key_p, cache_entry->pair_tree);
    if( node == NULL )
    {
        return FALSE;
    }
    found_pair = (keyvalpair *) (node->data);
    if (found_pair != NULL)
    {
        /* copy value */

        keyval_copy(key_p, val_p, 0, found_pair);

        /* delete Node from Tree: */
        delete_node_from_tree(node, cache_entry->pair_tree);

        /* relink pair_list: */
        if (found_pair->next != NULL)
        {
            found_pair->next->prev = found_pair->prev;
        }
        if (found_pair->prev == NULL)
        {
            /* this is currently the head of the list */
            cache_entry->pair_list = found_pair->next;
            if (found_pair->next == NULL)
            {
                /* The Tree is now empty */
                free_empty_red_black_tree(&cache_entry->pair_tree);
            }
            else
            {
                found_pair->next->prev = NULL;
            }
        }
        else
        {       /* not head of cache_entry->pair_list */
            found_pair->prev->next = found_pair->next;
        }
        free(found_pair->key.buffer);
        free(found_pair->val.buffer);
        free(found_pair);
        /* free(node->key) is not necessary because data and key are equal */

        return TRUE;
    }
    return FALSE;
#elif defined(TAS_USE_QUEUE)
    keyvalpair *old;
    int i = 0;
    int key_hash = queue_key_hash_func(key_p->buffer, key_p->buffer_sz);
    found_pair = cache_entry->pairs;
    old = found_pair;

    for (; found_pair != NULL; old = found_pair, found_pair = found_pair->next)
    {
        if (keys_equal(
#ifndef TAS_USE_RED_BLACKTREE
                         found_pair->key_hash, key_hash,
#endif
                         found_pair->key.buffer_sz, key_p->buffer_sz,
                         found_pair->key.buffer, key_p->buffer))
        {
            /* key exists */

            if (old == found_pair)
            {   /* first entry */
                cache_entry->pairs = found_pair->next;
            }
            else
            {   /* not first entry */
                old->next = found_pair->next;
            }
            free(found_pair->key.buffer);
            free(found_pair->val.buffer);
            free(found_pair);

            if (i > 10)
            {
                gossip_err(" WARNING: readToMatchKEY/VALUE remove: %d\n", i);
            }
            return TRUE;
        }
        i++;
    }
    if (i > 10)
    {
        gossip_err
            (" WARNING: readToMatchKEY/VALUE remove did not find key: %d\n", i);
    }
    return FALSE;
#endif
}


static inline handle_cache *find_cached_entry(
    TROVE_handle handle,
    PVFS_fs_id fs_id)
{
#if defined(TAS_USE_RED_BLACKTREE)
    handle_fsid key;
    key.handle = handle;
    key.fs_id = fs_id;
    tree_node *node = lookup_tree(&key, tas_meta_cache);
    if (node == NULL)
    {
        return NULL;
    }
    return (handle_cache *) node->data;
#elif defined(TAS_USE_QUEUE)
    queue_elem *elem = lookup_queue(handle, tas_meta_cache);
    if (elem == NULL)
    {
        return NULL;
    }
    relink_front(elem, tas_meta_cache);  /* push element to front of cache */
    return (handle_cache *) elem->data;
#endif
}

static int remove_cached_entry(
    TROVE_handle handle,
    PVFS_fs_id fs_id)
{
    void *result = NULL;


#if defined(TAS_USE_RED_BLACKTREE)
    handle_fsid key;
    key.handle = handle;
    key.fs_id = fs_id;
    tree_node *node = lookup_tree(&key, tas_meta_cache);
    if (node == NULL)
        return FALSE;
    result = node->data;
    delete_node_from_tree(node, tas_meta_cache);
#elif defined(TAS_USE_QUEUE)
    result = delete_key_from_list(handle, tas_meta_cache);
#endif
    if (result != NULL)
    {
        free(((handle_cache *) result)->attributes);
        free((handle_cache *) result);
        meta_count--;
        return TRUE;
    }
    return FALSE;
}

static inline void insert_keyval_pair(
    handle_cache * found,
    keyvalpair * pair)
{
#if defined(TAS_USE_RED_BLACKTREE)
    if (found->pair_tree == NULL)
    {   /* create new RedBlackTree */
        found->pair_tree =
            new_red_black_tree(compare_keyval_pairs, compare_keyval_pairs2);

        pair->next = NULL;
        found->pair_list = pair;
    }
    else
    {
        pair->next = found->pair_list;
        found->pair_list->prev = pair;
        found->pair_list = pair;
    }
    pair->prev = NULL;
    insert_key_into_tree((void *) pair, found->pair_tree);

#elif defined(TAS_USE_QUEUE)
    int64_t key_hash = queue_key_hash_func(pair->key.buffer, pair->key.buffer_sz);
    pair->next = found->pairs;
    found->pairs = pair;
    pair->key_hash = key_hash;
#endif
}

static int tas_initialize(
    char *stoname,
    TROVE_ds_flags flags)
{
    gossip_debug(GOSSIP_TROVE_DEBUG, "Tas Initialize\n");
    int ret = dbpf_mgmt_ops.initialize(stoname, flags);

    FILE *file = fopen(stoname, "r");
    if (file == 0)
    {   /* if storage does not exist... */
        return -1;
    }
#if defined(TAS_USE_RED_BLACKTREE)
    tas_meta_cache = new_red_black_tree(compare_handle, compare_handle2);
#elif defined(TAS_USE_QUEUE)
    tas_meta_cache = new_tas_queue();
#endif

    fclose(file);

    initialised = 1;
    meta_count = 0;

    gen_mutex_init(&tas_meta_mutex);

    sprintf(storage_path, "%s/tas", stoname);
    mkdir(storage_path, 511);
    sprintf(storage_name, "%s/tas/metadata.txt", stoname);

    printf("TAS options:\n");
    printf("\tCachetype ");


#if defined(TAS_USE_RED_BLACKTREE)
    printf("red-black-tree \n");
#elif defined(TAS_USE_QUEUE)
    printf("queue\n");
#endif

#ifdef QUEUE_MAXIMUM_KEYVAL_READ_OPTIZATION
    printf("\tWARNING QUEUE_MAXIMUM_KEYVAL_READ_OPTIZATION enabled with %d files\n"
           "\t Some file operations will not be possible with older files...\n",
           QUEUE_MAXIMUM_KEYVAL_READ_OPS);
#endif
#ifdef TAS_UPDATE_FILESIZE
    printf("\tFile write will set filesize properly\n");
#else
    printf
        ("\tFile write will NOT set filesize properly, only resize will resize files\n");
#endif

    return ret;
}


struct tas_finalizeData_
{
    FILE *file;
    int64_t handle_count;
    int64_t keyval_count;
    int64_t keyval_mem_size;
};
typedef struct tas_finalizeData_ tas_finalizeData;

static void keyvalwrite(
    void *data,
    void *funcData)
{
    tas_finalizeData *fdata = (tas_finalizeData *) funcData;
    keyvalpair *pair = (keyvalpair *) data;
    FILE *file = fdata->file;

    fprintf(file, "  Key-size:%d Val-size:%d  ", pair->key.buffer_sz,
            pair->val.buffer_sz);
    fwrite(pair->key.buffer, 1, pair->key.buffer_sz, file);
    fprintf(file, " Value:");
    fwrite(pair->val.buffer, 1, pair->val.buffer_sz, file);
    fprintf(file, "\n");

    /* count for memory usage */
    fdata->keyval_count++;
    fdata->keyval_mem_size += pair->key.buffer_sz + pair->val.buffer_sz;
}

static void datawrite(
    void *data,
    void *funcData)
{
    tas_finalizeData *fdata = (tas_finalizeData *) funcData;
    handle_cache *found = (handle_cache *) data;

    FILE *file = fdata->file;
    fprintf(file,
            "Handle:%Lu Type:%u FS-ID:%d UID:%u GID:%u MODE:%u DFILE_COUNT:%u DIST_SIZE:%u BYTE_SIZE:%llu KEYS:%llu CTIME:%llu MTIME:%llu ATIME:%llu\n",
            found->handle, found->attributes->type, found->fs_id,
            found->attributes->uid, found->attributes->gid,
            found->attributes->mode, found->attributes->dfile_count,
            found->attributes->dist_size, found->b_size, found->k_size,
            found->attributes->ctime, found->attributes->mtime,
            found->attributes->atime);
    fdata->handle_count++;

#if defined(TAS_USE_RED_BLACKTREE)
    if (found->pair_tree != NULL)
    {
        fdata->keyval_mem_size += sizeof(red_black_tree);
        iterate_red_black_tree(keyvalwrite, found->pair_tree, fdata);
    }
#elif defined(TAS_USE_QUEUE)
    keyvalpair *pair = found->pairs;
    for (; pair != NULL; pair = pair->next)
        keyvalwrite(pair, NULL, fdata);
#endif
}

 /* calculate the approximate memory usage... */
static int tas_finalize(
    void)
{
    gossip_debug(GOSSIP_TROVE_DEBUG,
                 "Tas finalize and save metadata to disk ... \n");

    if (!initialised || meta_count == 0)
        return RETURN_IMMEDIATE_COMPLETE;

    FILE *file = fopen(storage_name, "w");
    if (file == 0)
    {
        gossip_debug(GOSSIP_TROVE_DEBUG, " can not save metadata !!!!\n");
        return RETURN_IMMEDIATE_COMPLETE;
    }
    /* do not take care of memory managment because server is terminating... */

    fprintf(file, "Count:%llu\n", meta_count);
    tas_finalizeData *fdata = malloc(sizeof(tas_finalizeData));
    fdata->file = file;
    fdata->handle_count = 0;
    fdata->keyval_count = 0;
    fdata->keyval_mem_size = 0;

#if defined(TAS_USE_RED_BLACKTREE)
    iterate_red_black_tree(datawrite, tas_meta_cache, fdata);
#elif defined(TAS_USE_QUEUE)
    iterate_tas_queue(datawrite(elem->data, &elem->key, fdata), elem,
                      tas_meta_cache);
#endif

    fclose(file);

    /* calculate memory_usage */
    int64_t memory_usage;

#if defined(TAS_USE_RED_BLACKTREE)
    memory_usage =
        fdata->handle_count * (sizeof(store_attributes) + sizeof(handle_cache) +
                               sizeof(tree_node)) +
        fdata->keyval_count * (sizeof(keyvalpair) + sizeof(tree_node)) +
        fdata->keyval_mem_size;
#elif defined(TAS_USE_QUEUE)
    memory_usage =
        fdata->handle_count * (store_attributes) + sizeof(handle_cache) +
        sizeof(queue_elem)) + fdata->keyval_count * (sizeof(keyvalpair)) +
        fdata->keyval_mem_size;
#endif

    printf(" TAS handles:%lld keyval-pairs:%lld with buffers:%lld\n"
           "     approximate metadata memory usage: %lld Bytes - %f MByte \n",
           fdata->handle_count, fdata->keyval_count, fdata->keyval_mem_size,
           memory_usage, (double) memory_usage / 1024 / 1024);

    return dbpf_mgmt_ops.finalize();
}

static int tas_storage_create(char *stoname,
                              void *user_ptr, TROVE_op_id * out_op_id_p)
{
    gossip_debug(GOSSIP_TROVE_DEBUG,
                 "Tas tas_storage_create here: %s \n",
                 stoname);
    return dbpf_mgmt_ops.storage_create(stoname,
                                        user_ptr,
                                        out_op_id_p);
}

static int
    tas_storage_remove(char *stoname, void *user_ptr, TROVE_op_id * out_op_id_p)
{
    gossip_debug(GOSSIP_TROVE_DEBUG,
                 "Tas tas_storage_remove\n");
    if (initialised)
    {
        unlink(storage_name);
    }

    return dbpf_mgmt_ops.storage_remove(stoname,
                                        user_ptr,
                                        out_op_id_p);;
}

static int tas_collection_create(char *collname,
                                 TROVE_coll_id new_coll_id,
                                 void *user_ptr, TROVE_op_id * out_op_id_p)
{
    return dbpf_mgmt_ops.collection_create(collname,
                                           new_coll_id,
                                           user_ptr,
                                           out_op_id_p);
}

static int
    tas_collection_remove(char *collname,
                          void *user_ptr, TROVE_op_id * out_op_id_p)
{
    return dbpf_mgmt_ops.collection_remove(collname,
                                           user_ptr,
                                           out_op_id_p);
}

static int tas_collection_iterate(TROVE_ds_position * inout_position_p,
                                  TROVE_keyval_s * name_array,
                                  TROVE_coll_id * coll_id_array,
                                  int *inout_count_p,
                                  TROVE_ds_flags flags,
                                  TROVE_vtag_s * vtag,
                                  void *user_ptr, TROVE_op_id * out_op_id_p)
{
    return dbpf_mgmt_ops.collection_iterate(inout_position_p,
                                            name_array,
                                            coll_id_array,
                                            inout_count_p,
                                            flags,
                                            vtag,
                                            user_ptr,
                                            out_op_id_p);
}

static int
    tas_collection_lookup(char *collname,
                          TROVE_coll_id * out_coll_id_p,
                          void *user_ptr, TROVE_op_id * out_op_id_p)
{
    return dbpf_mgmt_ops.collection_lookup(collname,
                                           out_coll_id_p,
                                           user_ptr,
                                           out_op_id_p);
}



/*
 * For all bytestream operations except resize do not lookup handle if
 * resize of file is not selected.
 * Directly discard all I/O operations
 */


static int                      /* currently this function is not used within pvfs2 !!! */
  tas_bstream_read_at(TROVE_coll_id coll_id,
                      TROVE_handle handle,
                      void *buffer,
                      TROVE_size * inout_size_p,
                      TROVE_offset offset,
                      TROVE_ds_flags flags,
                      TROVE_vtag_s * out_vtag,
                      void *user_ptr,
                      TROVE_context_id context_id, TROVE_op_id * out_op_id_p)
{
    fprintf(stderr,
            "TROVE MODULE TAS: FUNCTION tas_bstream_read_at not implemented\n");
    gossip_debug(GOSSIP_TROVE_DEBUG,
                 "Tas tas_bstream_read_at\n");
    return 1;
}

static int                      /* currently this function is not used within pvfs2 !!! */
  tas_bstream_write_at(TROVE_coll_id coll_id,
                       TROVE_handle handle,
                       void *buffer,
                       TROVE_size * inout_size_p,
                       TROVE_offset offset,
                       TROVE_ds_flags flags,
                       TROVE_vtag_s * inout_vtag,
                       void *user_ptr,
                       TROVE_context_id context_id, TROVE_op_id * out_op_id_p)
{
    fprintf(stderr,
            "TROVE MODULE TAS: FUNCTION tas_bstream_write_at not implemented\n");
    gossip_debug(GOSSIP_TROVE_DEBUG,
                 "Tas tas_bstream_write_at\n");
    return RETURN_IMMEDIATE_COMPLETE;
}


static int tas_bstream_resize(TROVE_coll_id coll_id,
                              TROVE_handle handle,
                              TROVE_size * inout_size_p,
                              TROVE_ds_flags flags,
                              TROVE_vtag_s * vtag,
                              void *user_ptr,
                              TROVE_context_id context_id,
                              TROVE_op_id * out_op_id_p)
{
    gossip_debug(GOSSIP_TROVE_DEBUG,
                 "TAS tas_bstream_resize\n");
    handle_cache *found = find_cached_entry(handle, coll_id);
    if (found == NULL)
    {   /* abort */
        gen_mutex_unlock(&tas_meta_mutex);
        return -TROVE_ENOENT;
    }
    found->b_size = *inout_size_p;
    *out_op_id_p = DUMMY_OPNUM;
    return RETURN_IMMEDIATE_COMPLETE;
}

static int tas_bstream_validate(TROVE_coll_id coll_id,
                                TROVE_handle handle,
                                TROVE_ds_flags flags,
                                TROVE_vtag_s * vtag,
                                void *user_ptr,
                                TROVE_context_id context_id,
                                TROVE_op_id * out_op_id_p)
{
    gossip_debug(GOSSIP_TROVE_DEBUG,
                 "TAS tas_bstream_validate\n");
    return RETURN_IMMEDIATE_COMPLETE;
}

static int tas_bstream_read_list(TROVE_coll_id coll_id,
                                 TROVE_handle handle,
                                 char **mem_offset_array,       /* put the real data here */
                                 TROVE_size * mem_size_array,   /* size of the memory buffers */
                                 int mem_count, /* number of memory buffers */
                                 TROVE_offset * stream_offset_array, TROVE_size * stream_size_array, int stream_count,  /* number of streams to read... */
                                 TROVE_size * out_size_p,       /* read bytes.... */
                                 TROVE_ds_flags flags, TROVE_vtag_s * vtag,
                                 /* contains the callback function trove_callback
                                  *the function is called from bmi_recv_callback_fn if
                                  *bsream_read_list returns 1 */
                                 void *user_ptr,
                                 TROVE_context_id context_id,
                                 TROVE_op_id * out_op_id_p)
{
    int i;
    TROVE_size readBytes = 0;
    for (i = 0; i < stream_count; i++)
    {
        readBytes += stream_size_array[i];
    }

    gossip_debug(GOSSIP_TROVE_DEBUG,
                 "TAS tas_bstream_read_list handle:%lld flags:%d readBytes:%lld\n",
                 lld(
    handle),
                 flags,
                 lld(
    readBytes));

    *out_size_p = readBytes;
    *out_op_id_p = DUMMY_OPNUM;

    return RETURN_IMMEDIATE_COMPLETE;
}

/* have a look to io/flow/bmi_recv_callback_fn, there this funktion is called... */
static int tas_bstream_write_list(TROVE_coll_id coll_id,
                                  TROVE_handle handle,
                                  char **mem_offset_array,      /* here comes the real data */
                                  TROVE_size * mem_size_array,  /* size of the memory buffers */
                                  int mem_count,        /* number of memory buffers */
                                  TROVE_offset * stream_offset_array, TROVE_size * stream_size_array, int stream_count, /* number of streams to write... */
                                  TROVE_size * out_size_p,      /* written bytes.... */
                                  TROVE_ds_flags flags,
                                  TROVE_vtag_s * vtag,
                                  void *user_ptr,
                                  TROVE_context_id context_id,
                                  TROVE_op_id * out_op_id_p)
{
    int i;

    TROVE_size writtenBytes = 0;
    for (i = 0; i < stream_count; i++)
    {
        writtenBytes += stream_size_array[i];
    }
    gossip_debug(GOSSIP_TROVE_DEBUG,
                 "TAS tas_bstream_write_list for handle:%lld flags:%d fake writtenBytes:%lld  mem_count:%d stream_count:%d SYNC:%d\n",
                 lld(
    handle),
                 flags,
                 lld(
    writtenBytes),
                 mem_count,
                 stream_count,
                 flags);
    *out_size_p = writtenBytes;

/* set the actual file size */
#ifdef TAS_UPDATE_FILESIZE
    gen_mutex_lock(&tas_meta_mutex);
    handle_cache *found = find_cached_entry(handle, coll_id);
    if (found == NULL)
    {   /* abort */
        gen_mutex_unlock(&tas_meta_mutex);
        return -TROVE_ENOENT;
    }
    /* get the last written byte and enlarge the file
     * if this byte is larger than filesize*/
    for (i = 0; i < stream_count; i++)
    {
        TROVE_size akt_pos = stream_size_array[i] + stream_offset_array[i];
        if (akt_pos > found->b_size)
        {
            found->b_size = akt_pos;
        }
    }
    gen_mutex_unlock(&tas_meta_mutex);
#endif

    *out_op_id_p = DUMMY_OPNUM;


    return RETURN_IMMEDIATE_COMPLETE;
}



static int tas_bstream_flush(TROVE_coll_id coll_id,
                             TROVE_handle handle,
                             TROVE_ds_flags flags,
                             void *user_ptr,
                             TROVE_context_id context_id,
                             TROVE_op_id * out_op_id_p)
{
    gossip_debug(GOSSIP_TROVE_DEBUG,
                 "Tas tas_bstream_flush\n");

    *out_op_id_p = DUMMY_OPNUM;
    return RETURN_IMMEDIATE_COMPLETE;
}

/*
 * Keyval operations:
 */

static int tas_keyval_read(TROVE_coll_id coll_id,
                           TROVE_handle handle,
                           TROVE_keyval_s * key_p,
                           TROVE_keyval_s * val_p,
                           TROVE_ds_flags flags,
                           TROVE_vtag_s * out_vtag,
                           void *user_ptr,
                           TROVE_context_id context_id,
                           TROVE_op_id * out_op_id_p)
{

    if (key_p->buffer_sz == 3 && strncmp(key_p->buffer, "de", 3) == 0)
    {
        gossip_debug(GOSSIP_TROVE_DEBUG, "lookup dir\n");
    }

    gossip_debug(GOSSIP_TROVE_DEBUG,
                 "Tas tas_keyval_read flags:%d handle:%lld key-length:%d key:%s CollID:%d\n",
                 flags,
                 lld( handle),
                 key_p->buffer_sz,
                 (char *) key_p->buffer,
                 coll_id);
    gen_mutex_lock(&tas_meta_mutex);
    handle_cache *found = find_cached_entry(handle, coll_id);
    if (found == NULL)
    {   /* abort */
        gen_mutex_unlock(&tas_meta_mutex);
        return -TROVE_ENOENT;
    }

    /* got handle; search for keyval */
    keyvalpair *found_pair = find_pair_of_cached_entry(found, key_p);
    if (found_pair == NULL)
    {
        gen_mutex_unlock(&tas_meta_mutex);
        return -TROVE_ENOENT;
    }

    if (val_p->buffer_sz < found_pair->val.buffer_sz)
    {
        gossip_debug(GOSSIP_TROVE_DEBUG,
                     " WARNING buffer to small tas read-keyval got:%d need:%d \n",
                     val_p->buffer_sz, found_pair->val.buffer_sz);
        memcpy(val_p->buffer, found_pair->val.buffer, val_p->buffer_sz);
        val_p->read_sz = val_p->buffer_sz;
    }
    else
    {
        memcpy(val_p->buffer, found_pair->val.buffer,
               found_pair->val.buffer_sz);
        val_p->read_sz = found_pair->val.buffer_sz;
    }
    gen_mutex_unlock(&tas_meta_mutex);
    *out_op_id_p = DUMMY_OPNUM;
    return RETURN_IMMEDIATE_COMPLETE;
}

static int tas_keyval_write(TROVE_coll_id coll_id,
                            TROVE_handle handle,
                            TROVE_keyval_s * key_p,
                            TROVE_keyval_s * val_p,
                            TROVE_ds_flags flags,
                            TROVE_vtag_s * inout_vtag,
                            void *user_ptr,
                            TROVE_context_id context_id,
                            TROVE_op_id * out_op_id_p)
{
    gossip_debug(GOSSIP_TROVE_DEBUG,
                 "Tas tas_keyval_write flags:%d handle:%lld val-length:%d "
                 "key:%s CollID:%d\n",
                 flags,
                 lld(handle),
                 val_p->buffer_sz,
                 (char *) key_p->buffer,
                 coll_id);

    gen_mutex_lock(&tas_meta_mutex);
    handle_cache *found = find_cached_entry(handle, coll_id);
    if (found == NULL)
    {
        gen_mutex_unlock(&tas_meta_mutex);
        return -TROVE_ENOENT;
    }

    /* If TROVE_NOOVERWRITE flag was set, make sure that we don't create the
     * key if it exists see dbpf-keyval.c
     * TROVE_NOOVERWRITE
     * if TROVE_ONLYOVERWRITE flag was set, make sure that the key exists
     * before overwriting it */

    /* got handle; search for keyval */
    keyvalpair *found_pair = NULL;

    /*
     * find_pair_of_cached_entry(found,key_p)
     * it should never happen that a keyval exist for the list implementation
     * The old value will be keeped and not be overwritten, advantages:
     * faster because possible existing old key need not to be found
     * history, see how keys are manipulated
     * Disadvantage: more memory usage, slow down search for other keys
     *      if multiple exist....
     */


#if defined(TAS_USE_RED_BLACKTREE)
    found_pair = find_pair_of_cached_entry(found, key_p);
#endif

    if (found_pair == NULL)
    {
        found_pair = (keyvalpair *) malloc(sizeof(keyvalpair));
        found_pair->key.buffer = malloc(key_p->buffer_sz);
        found_pair->key.buffer_sz = key_p->buffer_sz;
        memcpy(found_pair->key.buffer, key_p->buffer, key_p->buffer_sz);

        insert_keyval_pair(found, found_pair);

        found->k_size++;
    }
    else
    {   /* free memory first: */
        free(found_pair->val.buffer);
    }

    found_pair->val.buffer = malloc(val_p->buffer_sz);
    found_pair->val.buffer_sz = val_p->buffer_sz;
    memcpy(found_pair->val.buffer, val_p->buffer, val_p->buffer_sz);


    gen_mutex_unlock(&tas_meta_mutex);
    return RETURN_IMMEDIATE_COMPLETE;
}

static int tas_keyval_remove(TROVE_coll_id coll_id,
                             TROVE_handle handle,
                             TROVE_keyval_s * key_p,
                             TROVE_keyval_s * val_p,
                             TROVE_ds_flags flags,
                             TROVE_vtag_s * vtag,
                             void *user_ptr,
                             TROVE_context_id context_id,
                             TROVE_op_id * out_op_id_p)
{
    gossip_debug(GOSSIP_TROVE_DEBUG,
                 "Tas tas_keyval_remove\n");

    gen_mutex_lock(&tas_meta_mutex);
    handle_cache *found = find_cached_entry(handle, coll_id);
    if (found == NULL)
    {
        gen_mutex_unlock(&tas_meta_mutex);
        return -TROVE_ENOENT;
    }

    /* got handle; search for keyval */
    if (! remove_pair_of_cached_entry(found, key_p, val_p))
    {
        gen_mutex_unlock(&tas_meta_mutex);
        return -TROVE_ENOENT;
    }

    found->k_size--;

    gen_mutex_unlock(&tas_meta_mutex);
    *out_op_id_p = DUMMY_OPNUM;
    return RETURN_IMMEDIATE_COMPLETE;
}

static int tas_keyval_validate(TROVE_coll_id coll_id,
                               TROVE_handle handle,
                               TROVE_ds_flags flags,
                               TROVE_vtag_s * inout_vtag,
                               void *user_ptr,
                               TROVE_context_id context_id,
                               TROVE_op_id * out_op_id_p)
{
    gossip_debug(GOSSIP_TROVE_DEBUG,
                 "Tas tas_keyval_validate\n");
    fprintf(stderr,
            "TROVE MODULE TAS: FUNCTION tas_keyval_validate not implemented\n");
    *out_op_id_p = DUMMY_OPNUM;
    return RETURN_IMMEDIATE_COMPLETE;
}



/*
 * reads count keyword/value pairs from the provided logical position in the
 * keyval space.
 */
static int tas_keyval_iterate(TROVE_coll_id coll_id,
                              TROVE_handle handle,
                              TROVE_ds_position * inout_position_p,
                              TROVE_keyval_s * out_key_array,
                              TROVE_keyval_s * out_val_array,
                              int *inout_count_p,
                              TROVE_ds_flags flags,
                              TROVE_vtag_s * inout_vtag,
                              void *user_ptr,
                              TROVE_context_id context_id,
                              TROVE_op_id * out_op_id_p)
{
    if (*inout_position_p == TROVE_ITERATE_START)
    {
        *inout_position_p = 0;
    }

    gossip_debug(GOSSIP_TROVE_DEBUG,
                 "Tas tas_keyval_iterate, pos: %d\n", (int) *inout_position_p );


    gen_mutex_lock(&tas_meta_mutex);
    handle_cache *found = find_cached_entry(handle, coll_id);
    if (found == NULL)
    {
        gen_mutex_unlock(&tas_meta_mutex);
        return -TROVE_ENOENT;
    }

    int32_t in_read_count = *inout_count_p;
    int32_t in_read_position = *inout_position_p;

    int32_t remainingKeyValues = found->k_size - in_read_position;
    gossip_debug(GOSSIP_TROVE_DEBUG, " read_count %d read_pos %d remain %d \n",
                 in_read_count, in_read_position, remainingKeyValues);
    /* set read count, if less keys available */
    if (remainingKeyValues <= in_read_count)
    {
        *inout_count_p = remainingKeyValues;    /* number of keypairs to read */
        in_read_count = remainingKeyValues;
        *inout_position_p = TROVE_ITERATE_END;
    }else{
        *inout_position_p = in_read_position + *inout_count_p;
    }

    if (*inout_count_p <= 0)
    {
        gen_mutex_unlock(&tas_meta_mutex);
        *inout_count_p = 0;
        *inout_position_p = TROVE_ITERATE_END;
        return RETURN_IMMEDIATE_COMPLETE;
    }

    gossip_debug(GOSSIP_TROVE_DEBUG, " read_count %d read_pos %d remain:%d newoutpos:%d \n",
                 in_read_count, in_read_position, remainingKeyValues, (int)  *inout_position_p);

    /* skip to in_read_position... */
    keyvalpair *found_pair;


#if defined(TAS_USE_RED_BLACKTREE)
    /* go to leftmost node: */
    found_pair = found->pair_list;
#elif defined(TAS_USE_QUEUE)
    found_pair = found->pairs;
#endif

    for (; in_read_position > 0;
         found_pair = found_pair->next, in_read_position--);

    /* fill the array... */
    int i = 0;
    for (; i < in_read_count; found_pair = found_pair->next, i++)
    {
        keyval_copy(out_key_array, out_val_array, i, found_pair);
    }

    gen_mutex_unlock(&tas_meta_mutex);

    return RETURN_IMMEDIATE_COMPLETE;
}

static int tas_keyval_iterate_keys(TROVE_coll_id coll_id,
                                   TROVE_handle handle,
                                   TROVE_ds_position * inout_position_p,
                                   TROVE_keyval_s * out_key_array,
                                   int *inout_count_p,
                                   TROVE_ds_flags flags,
                                   TROVE_vtag_s * vtag,
                                   void *user_ptr,
                                   TROVE_context_id context_id,
                                   TROVE_op_id * out_op_id_p)
{
    gossip_debug(GOSSIP_TROVE_DEBUG,
                 "Tas tas_keyval_iterate_keys\n");

    if (*inout_position_p == TROVE_ITERATE_START)
    {
        *inout_position_p = 0;
    }

    gossip_debug(GOSSIP_TROVE_DEBUG,
                 "Tas tas_keyval_iterate, pos: %d\n", (int) *inout_position_p );


    gen_mutex_lock(&tas_meta_mutex);
    handle_cache *found = find_cached_entry(handle, coll_id);
    if (found == NULL)
    {
        gen_mutex_unlock(&tas_meta_mutex);
        return -TROVE_ENOENT;
    }

    int32_t in_read_count = *inout_count_p;
    int32_t in_read_position = *inout_position_p;

    int32_t remainingKeyValues = found->k_size - in_read_position;
    gossip_debug(GOSSIP_TROVE_DEBUG, " read_count %d read_pos %d remain %d \n",
                 in_read_count, in_read_position, remainingKeyValues);
    /* set read count, if less keys available */
    if (remainingKeyValues <= in_read_count)
    {
        *inout_count_p = remainingKeyValues;    /* number of keypairs to read */
        in_read_count = remainingKeyValues;
        *inout_position_p = TROVE_ITERATE_END;
    }else{
        *inout_position_p = in_read_position + *inout_count_p;
    }

    if (*inout_count_p <= 0)
    {
        gen_mutex_unlock(&tas_meta_mutex);
        *inout_count_p = 0;
        *inout_position_p = TROVE_ITERATE_END;
        return RETURN_IMMEDIATE_COMPLETE;
    }

    gossip_debug(GOSSIP_TROVE_DEBUG, " read_count %d read_pos %d remain:%d newoutpos:%d \n",
                 in_read_count, in_read_position, remainingKeyValues, (int)  *inout_position_p);

    /* skip to in_read_position... */
    keyvalpair *found_pair;


#if defined(TAS_USE_RED_BLACKTREE)
    /* go to leftmost node: */
    found_pair = found->pair_list;
#elif defined(TAS_USE_QUEUE)
    found_pair = found->pairs;
#endif

    for (; in_read_position > 0;
         found_pair = found_pair->next, in_read_position--);

    /* fill the array... */
    int i = 0;
    for (; i < in_read_count; found_pair = found_pair->next, i++)
    {
        key_copy(out_key_array, i, found_pair);
    }

    gen_mutex_unlock(&tas_meta_mutex);

    return RETURN_IMMEDIATE_COMPLETE;
}

static int tas_keyval_read_list(TROVE_coll_id coll_id,
                                TROVE_handle handle,
                                TROVE_keyval_s * key_array,
                                TROVE_keyval_s * val_array,
                                TROVE_ds_state * err_array,
                                int count,
                                TROVE_ds_flags flags,
                                TROVE_vtag_s * out_vtag,
                                void *user_ptr,
                                TROVE_context_id context_id,
                                TROVE_op_id * out_op_id_p)
{
    int i;
      gossip_debug(GOSSIP_TROVE_DEBUG,
                   "Tas tas_keyval_read_list\n");
      gen_mutex_lock(&tas_meta_mutex);
    handle_cache *found = find_cached_entry(handle, coll_id);
    if (found == NULL)
    {   /* abort */
        gen_mutex_unlock(&tas_meta_mutex);
        return -TROVE_ENOENT;
    }

    /* got handle; search for keyval */
    for (i = 0; i < count; i++)
    {
        keyvalpair *found_pair = find_pair_of_cached_entry(found, &key_array[i]);
        TROVE_keyval_s *val_p;
        if (found_pair != NULL)
        {
            err_array[i] = 0;
        }
        else
        {
            err_array[i] = -TROVE_ENOENT;
            continue;
        }
        val_p = &val_array[i];

        if (val_p->buffer_sz < found_pair->val.buffer_sz)
        {
            gossip_debug(GOSSIP_TROVE_DEBUG,
                         " WARNING buffer to small tas read-keyval got:%d need:%d \n",
                         val_p->buffer_sz, found_pair->val.buffer_sz);
            memcpy(val_p->buffer, found_pair->val.buffer, val_p->buffer_sz);
            val_p->read_sz = val_p->buffer_sz;
        }
        else
        {
            memcpy(val_p->buffer, found_pair->val.buffer,
                   found_pair->val.buffer_sz);
            val_p->read_sz = found_pair->val.buffer_sz;
        }
    }
    gen_mutex_unlock(&tas_meta_mutex);
    *out_op_id_p = DUMMY_OPNUM;
    return RETURN_IMMEDIATE_COMPLETE;
}

static int tas_keyval_write_list(TROVE_coll_id coll_id,
                                 TROVE_handle handle,
                                 TROVE_keyval_s * key_array,
                                 TROVE_keyval_s * val_array,
                                 int count,
                                 TROVE_ds_flags flags,
                                 TROVE_vtag_s * inout_vtag,
                                 void *user_ptr,
                                 TROVE_context_id context_id,
                                 TROVE_op_id * out_op_id_p)
{
    fprintf(stderr,
            "TROVE MODULE TAS: FUNCTION tas_keyval_write_list not implemented\n");
    gossip_debug(GOSSIP_TROVE_DEBUG,
                 "Tas tas_keyval_write_list\n");
    return RETURN_IMMEDIATE_COMPLETE;
}

static int tas_keyval_flush(TROVE_coll_id coll_id,
                            TROVE_handle handle,
                            TROVE_ds_flags flags,
                            void *user_ptr,
                            TROVE_context_id context_id,
                            TROVE_op_id * out_op_id_p)
{
    fprintf(stderr,
            "TROVE MODULE TAS: FUNCTION tas_keyval_flush not implemented\n");
    gossip_debug(GOSSIP_TROVE_DEBUG,
                 "Tas tas_keyval_flush\n");
    return 1;
}

static int tas_keyval_get_handle_info(TROVE_coll_id coll_id,
                                      TROVE_handle handle,
                                      TROVE_ds_flags flags,
                                      TROVE_keyval_handle_info * info,
                                      void *user_ptr,
                                      TROVE_context_id context_id,
                                      TROVE_op_id * out_op_id_p)
{
    gen_mutex_lock(&tas_meta_mutex);
    handle_cache *found = find_cached_entry(handle, coll_id);
    if (found == NULL)
    {   /* abort */
        gen_mutex_unlock(&tas_meta_mutex);
        return -TROVE_ENOENT;
    }
    if (flags & TROVE_KEYVAL_HANDLE_COUNT)
    {
        info->count = found->k_size;
    }
    gen_mutex_unlock(&tas_meta_mutex);
    return RETURN_IMMEDIATE_COMPLETE;
}

static int tas_dspace_create(TROVE_coll_id coll_id,
                             TROVE_handle_extent_array * extent_array,
                             TROVE_handle * handle,
                             TROVE_ds_type type,
                             TROVE_keyval_s * hint,     /* TODO: figure out what this is! */
                             TROVE_ds_flags flags,
                             void *user_ptr,
                             TROVE_context_id context_id,
                             TROVE_op_id * out_op_id_p)
{
    if (!extent_array || (
    extent_array->extent_count < 1))
    {
        return -TROVE_EINVAL;
    }

    TROVE_extent cur_extent = extent_array->extent_array[0];
    TROVE_handle new_handle = TROVE_HANDLE_NULL;
    /* check if we got a single specific handle */
    if ((extent_array->extent_count == 1) &&
        (cur_extent.first == cur_extent.last))
    {
        /*
           check if we MUST use the exact handle value specified;
           if caller requests a specific handle, honor it
         */
        if (flags & TROVE_FORCE_REQUESTED_HANDLE)
        {
            new_handle = *handle;
            if (new_handle < handle_separator)
            {
                if (akt_handle_first <= new_handle)
                {
                    akt_handle_first = new_handle + 1;
                }
            }
            else
            {
                if (akt_handle_second <= new_handle)
                {
                    akt_handle_second = new_handle + 1;
                }
            }
        }
        else if (cur_extent.first == TROVE_HANDLE_NULL)
        {
            /*
               if we got TROVE_HANDLE_NULL, the caller doesn't care
               where the handle comes from
             */
            printf("%llu cur_extent.first == TROVE_HANDLE_NULL\n", *handle);
            /* new_handle = getArbitraryHandle(coll_id,*handle); */
        }
    }
    else
    {
        /*
           otherwise, we have to try to allocate a handle from
           the specified range that we're given
           Ignore that circumstance for now !!!!
         */

        if (cur_extent.last < handle_separator)
        {
            new_handle = akt_handle_first;
            akt_handle_first++;
        }
        else
        {
            new_handle = akt_handle_second;
            akt_handle_second++;
        }
    }

    *handle = new_handle;

    gossip_debug(GOSSIP_TROVE_DEBUG,
                 "Tas tas_dspace_create Type:%d flag:%d extent_count->%u new_handle->%lld cur_extent.first:%lld cur_extent.last:%lld\n",
                 type, flags, extent_array->extent_count, lld(new_handle),
                 lld(cur_extent.first), lld(cur_extent.last));
    /*
       if we got a zero handle, we're either completely out of handles
       -- or else something terrible has happened
     */
    if (new_handle == TROVE_HANDLE_NULL)
    {
        gossip_err("Error: handle allocator returned a zero handle.\n");
        gen_mutex_unlock(&tas_meta_mutex);
        return -TROVE_ENOSPC;
    }

    handle_cache *found = NULL;
#ifndef NO_SAVE_CREATION
    found = find_cached_entry(new_handle, coll_id);
#endif
    if (found == NULL)
    {   /* create new metadata structure... */
        found = allocate_new_cache_element();
        insert_cache_element(found, new_handle, coll_id, type);
    }
    else
    {
        gossip_debug(GOSSIP_TROVE_DEBUG, "ERROR handle exists already %lld\n",
                     lld(new_handle));
    }
    gen_mutex_unlock(&tas_meta_mutex);

    *out_op_id_p = DUMMY_OPNUM;
    return RETURN_IMMEDIATE_COMPLETE;
}

static int tas_dspace_remove(TROVE_coll_id coll_id,
                             TROVE_handle handle,
                             TROVE_ds_flags flags,
                             void *user_ptr,
                             TROVE_context_id context_id,
                             TROVE_op_id * out_op_id_p)
{
    gossip_debug(GOSSIP_TROVE_DEBUG,
                 "Tas tas_dspace_remove\n");

    gen_mutex_lock(&tas_meta_mutex);

    if (!remove_cached_entry(
    handle,
    coll_id))
    {
        gen_mutex_unlock(&tas_meta_mutex);
        return -TROVE_ENOENT;
    }

    gen_mutex_unlock(&tas_meta_mutex);
    return RETURN_IMMEDIATE_COMPLETE;
}

static int tas_dspace_iterate_handles(TROVE_coll_id coll_id,
                                      TROVE_ds_position * position_p,
                                      TROVE_handle * handle_array,
                                      int *inout_count_p,
                                      TROVE_ds_flags flags,
                                      TROVE_vtag_s * vtag,
                                      void *user_ptr,
                                      TROVE_context_id context_id,
                                      TROVE_op_id * out_op_id_p)
{
    gossip_debug(GOSSIP_TROVE_DEBUG,
                 "Tas tas_dspace_iterate_handles \n");
    fprintf(stderr,
            "TROVE MODULE TAS: FUNCTION tas_dspace_iterate_handles not implemented\n");
    *out_op_id_p = DUMMY_OPNUM;
    return 1;
}



static int tas_dspace_verify(TROVE_coll_id coll_id,
                             TROVE_handle handle,
                             TROVE_ds_type * type,      /* TODO: define types! */
                             TROVE_ds_flags flags,
                             void *user_ptr,
                             TROVE_context_id context_id,
                             TROVE_op_id * out_op_id_p)
{
    gossip_debug(GOSSIP_TROVE_DEBUG,
                 "Tas tas_dspace_verify\n");
    *out_op_id_p = DUMMY_OPNUM;

    handle_cache *found = find_cached_entry(handle, coll_id);

    if (found == NULL)
    {
        return -TROVE_ENOENT;
    }

    * type = found->attributes->type;
    return RETURN_IMMEDIATE_COMPLETE;
}

static int tas_dspace_getattr(TROVE_coll_id coll_id,
                              TROVE_handle handle,
                              TROVE_ds_attributes_s * ds_attr_p,
                              TROVE_ds_flags flags,
                              void *user_ptr,
                              TROVE_context_id context_id,
                              TROVE_op_id * out_op_id_p)
{
    gossip_debug(GOSSIP_TROVE_DEBUG,
                 "Tas tas_dspace_getattr handle: %lld flags: %d \n",
                 lld(
    handle),
                 flags);

    gen_mutex_lock(&tas_meta_mutex);

    handle_cache *found = find_cached_entry(handle, coll_id);

    if (found != NULL)
    {
        *out_op_id_p = DUMMY_OPNUM;
        ds_attr_p->type = found->attributes->type;
        ds_attr_p->handle = handle;
        ds_attr_p->fs_id = found->fs_id;
        ds_attr_p->uid = found->attributes->uid;
        ds_attr_p->gid = found->attributes->gid;
        ds_attr_p->mode = found->attributes->mode;
        ds_attr_p->ctime = found->attributes->ctime;
        ds_attr_p->mtime = found->attributes->mtime;
        ds_attr_p->atime = found->attributes->atime;
        ds_attr_p->dfile_count = found->attributes->dfile_count;
        ds_attr_p->dist_size = found->attributes->dist_size;

        ds_attr_p->b_size = found->b_size;

        gen_mutex_unlock(&tas_meta_mutex);
        return RETURN_IMMEDIATE_COMPLETE;
    }
    gen_mutex_unlock(&tas_meta_mutex);

    return -1;
}

static int tas_dspace_getattr_list(TROVE_coll_id coll_id,
                                   int nhandles,
                                   TROVE_handle * handle_array,
                                   TROVE_ds_attributes_s * ds_attr_p,
                                   TROVE_ds_state * error_array,
                                   TROVE_ds_flags flags,
                                   void *user_ptr,
                                   TROVE_context_id context_id,
                                   TROVE_op_id * out_op_id_p)
{
    return -TROVE_EINVAL;
}

static int tas_dspace_setattr(TROVE_coll_id coll_id,
                              TROVE_handle handle,
                              TROVE_ds_attributes_s * ds_attr_p,
                              TROVE_ds_flags flags,
                              void *user_ptr,
                              TROVE_context_id context_id,
                              TROVE_op_id * out_op_id_p)
{
    gossip_debug(GOSSIP_TROVE_DEBUG,
                 "Tas tas_dspace_setattr handle:%lld uid:%d gid:%d\n",
                 lld(
    handle),
                 ds_attr_p->uid,
                 ds_attr_p->gid);

    gen_mutex_lock(&tas_meta_mutex);
    handle_cache *found = find_cached_entry(handle, coll_id);

    if (found != NULL)
    {
        found->attributes->type = ds_attr_p->type;
        if (found->handle != handle)
        {
            /* found->fs_id != ds_attr_p->fs_id */
            printf("Fatal ERROR tas_dspace_setattr it is not allowed "
                   "to change the handle number or filesystem id!!\n");
            exit(1);
        }
        found->attributes->uid = ds_attr_p->uid;
        found->attributes->gid = ds_attr_p->gid;
        found->attributes->mode = ds_attr_p->mode;
        found->attributes->ctime = ds_attr_p->ctime;
        found->attributes->mtime = ds_attr_p->mtime;
        found->attributes->atime = ds_attr_p->atime;
        found->attributes->dfile_count = ds_attr_p->dfile_count;
        found->attributes->dist_size = ds_attr_p->dist_size;

        /* take special care for:
         * found->b_size = ds_attr_p->b_size;
         * found->k_size = ds_attr_p->k_size; */

        gen_mutex_unlock(&tas_meta_mutex);

        return RETURN_IMMEDIATE_COMPLETE;
    }

    gen_mutex_unlock(&tas_meta_mutex);
    return -TROVE_EEXIST;
}

static int tas_dspace_cancel(TROVE_coll_id coll_id,
                             TROVE_op_id ds_id,
                             TROVE_context_id context_id)
{
    fprintf(stderr,
            "TROVE MODULE TAS: FUNCTION tas_dspace_cancel not implemented\n");
    gossip_debug(GOSSIP_TROVE_DEBUG, "Tas tas_dspace_cancel\n");
    return 1;
}

static int tas_dspace_test(TROVE_coll_id coll_id,
                           TROVE_op_id ds_id,
                           TROVE_context_id context_id,
                           int *out_count_p,
                           TROVE_vtag_s * vtag,
                           void **returned_user_ptr_p,
                           TROVE_ds_state * out_state_p, int max_idle_time_ms)
{
    return dbpf_dspace_ops.dspace_test(coll_id,
                                       ds_id,
                                       context_id,
                                       out_count_p,
                                       vtag,
                                       returned_user_ptr_p,
                                       out_state_p,
                                       max_idle_time_ms);
}

static int tas_dspace_testsome(TROVE_coll_id coll_id,
                               TROVE_context_id context_id,
                               TROVE_op_id * ds_id_array,
                               int *inout_count_p,
                               int *out_index_array,
                               TROVE_vtag_s * vtag_array,
                               void **returned_user_ptr_array,
                               TROVE_ds_state * out_state_array,
                               int max_idle_time_ms)
{
    return dbpf_dspace_ops.dspace_testsome(coll_id,
                                           context_id,
                                           ds_id_array,
                                           inout_count_p,
                                           out_index_array,
                                           vtag_array,
                                           returned_user_ptr_array,
                                           out_state_array,
                                           max_idle_time_ms);
}

static int tas_dspace_testcontext(TROVE_coll_id coll_id,
                                  TROVE_op_id * ds_id_array,
                                  int *inout_count_p,
                                  TROVE_ds_state * state_array,
                                  void **user_ptr_array,
                                  int max_idle_time_ms,
                                  TROVE_context_id context_id)
{
    return dbpf_dspace_ops.dspace_testcontext(coll_id,
                                              ds_id_array,
                                              inout_count_p,
                                              state_array,
                                              user_ptr_array,
                                              max_idle_time_ms,
                                              context_id);
}

static int tas_collection_setinfo(TROVE_method_id method_id,
                                  TROVE_coll_id coll_id,
                                  TROVE_context_id context_id,
                                  int option, void *parameter)
{
    gossip_debug(GOSSIP_TROVE_DEBUG,
                 "Tas tas_collection_setinfo %d, \n",
                 option);
    int ret = 1;                /* -TROVE_EINVAL; */

    switch (option)
    {
    case TROVE_COLLECTION_HANDLE_RANGES:
        {
            gossip_debug(GOSSIP_TROVE_DEBUG,
                         "TROVE_COLLECTION_HANDLE_RANGES: %s %d\n",
                         (char *) parameter, context_id);
            /* necessary to get statfs working */
            ret = trove_set_handle_ranges(
                coll_id, context_id, (char *)parameter);
            /* set minimum handles according to handle range... */
            TROVE_handle minMetaHandle,
              maxMetaHandle,
              minDatafileHandle,
              maxDatafileHandle;
              ret = sscanf((char *) parameter, "%llu-%llu,%llu-%llu",
                           &minMetaHandle, &maxMetaHandle, &minDatafileHandle,
                           &maxDatafileHandle);
            if (ret == 4)
            {   /* set only once !!! */
                akt_handle_first = minMetaHandle;
                akt_handle_second = minDatafileHandle;
                handle_separator = minDatafileHandle;
                if (akt_handle_second < akt_handle_first)
                {
                    printf("error in parsing handle range !!! %s \n",
                           (char *) parameter);
                    exit(1);
                }
            }
            else if (ret == 2)
            {   /* only datafiles OR metadata ! */
                ret = sscanf((char *) parameter, "%llu-%llu",
                             &minDatafileHandle, &maxDatafileHandle);
                akt_handle_first = minDatafileHandle;
                akt_handle_second = 0;
                handle_separator = maxDatafileHandle + 1;
            }
            else
            {
                return 0;
            }

            FILE *file;
            printf("read metadata from file: %s \n", storage_name);
            file = fopen(storage_name, "r");
            if (file == 0)
            {
                printf(" no metadata information available \n");
                return RETURN_IMMEDIATE_COMPLETE;
            }

            PVFS_size i;

            handle_cache *found = (handle_cache *) malloc(sizeof(handle_cache));
            found->attributes =
                (store_attributes *) malloc(sizeof(store_attributes));

            ret = fscanf(file, "Count:%llu\n", &meta_count);
            if (ret != 1)
            {
                meta_count = 0;
                free(found);

                fclose(file);
                return RETURN_IMMEDIATE_COMPLETE;
            }
            for (i = 0; i < meta_count; i++)
            {
                bzero(found->attributes, sizeof(store_attributes));
                ret =
                    fscanf(file,
                           "Handle:%llu Type:%u FS-ID:%d UID:%u GID:%u MODE:%u DFILE_COUNT:%u DIST_SIZE:%u BYTE_SIZE:%llu KEYS:%llu CTIME:%llu MTIME:%llu ATIME:%llu\n",
                           &(found->handle), (int *) &(found->attributes->type),
                           &found->fs_id, &found->attributes->uid,
                           &found->attributes->gid, &found->attributes->mode,
                           &found->attributes->dfile_count,
                           &found->attributes->dist_size, &found->b_size,
                           &found->k_size, &found->attributes->ctime,
                           &found->attributes->mtime,
                           &found->attributes->atime);

                if (ret == 13)
                {
                    if (found->handle < handle_separator)
                    {
                        if (akt_handle_first <= found->handle)
                        {
                            akt_handle_first = found->handle + 1;
                        }
                    }
                    else if (akt_handle_second == 0)
                    {
                        printf("ERROR akt_handle_second = 0 \n");
                    }
                    else
                    {
                        if (akt_handle_second <= found->handle)
                        {
                            akt_handle_second = found->handle + 1;
                        }
                    }

                    keyvalpair *akt_pair;

#if defined(TAS_USE_RED_BLACKTREE)
                    insert_key_into_tree((RBData *) found, tas_meta_cache);
                    found->pair_list = NULL;
                    found->pair_tree = NULL;
#elif defined(TAS_USE_QUEUE)
                    found->pairs = NULL;
                    push_front_key(found->handle, found, tas_meta_cache);
#endif

                    akt_pair = NULL;

                    /* read key->value pairs: */
                    int i;
                    for (i = 0; i < found->k_size; i++)
                    {
                        int key_sz, val_sz;
                        akt_pair = (keyvalpair *) malloc(sizeof(keyvalpair));

                        fscanf(file, "  Key-size:%d Val-size:%d  ", &key_sz,
                               &val_sz);
                        akt_pair->key.buffer_sz = key_sz;
                        akt_pair->val.buffer_sz = val_sz;

                        akt_pair->key.buffer = malloc(key_sz);
                        akt_pair->val.buffer = malloc(val_sz);
                        fread(akt_pair->key.buffer, 1, key_sz, file);
                        fscanf(file, " Value:");
                        fread(akt_pair->val.buffer, 1, val_sz, file);
                        fscanf(file, "\n");

                        insert_keyval_pair(found, akt_pair);
                    }

                    found = (handle_cache *) malloc(sizeof(handle_cache));
                    found->attributes =
                        (store_attributes *) malloc(sizeof(store_attributes));
                }
                else
                {
                    printf(" Less elements than expected read %llu of %llu \n",
                           i, meta_count);
                    break;
                }
            }
            free(found->attributes);
            free(found);

            fclose(file);

            ret = 1;
            break;
        }
    case TROVE_COLLECTION_HANDLE_TIMEOUT:
        ret = 1;
        gossip_debug(GOSSIP_TROVE_DEBUG, "TROVE_COLLECTION_HANDLE_TIMEOUT\n");
        break;
    case TROVE_COLLECTION_ATTR_CACHE_KEYWORDS:
        gossip_debug(GOSSIP_TROVE_DEBUG,
                     "TROVE_COLLECTION_ATTR_CACHE_KEYWORDS: %s\n",
                     (char *) parameter);
        ret = 1;
        break;
    case TROVE_COLLECTION_ATTR_CACHE_SIZE:
        gossip_debug(GOSSIP_TROVE_DEBUG,
                     "TROVE_COLLECTION_ATTR_CACHE_SIZE: %d\n",
                     *((int *) parameter));
        ret = 1;
        break;
    case TROVE_COLLECTION_ATTR_CACHE_MAX_NUM_ELEMS:
        gossip_debug(GOSSIP_TROVE_DEBUG,
                     "TROVE_COLLECTION_ATTR_CACHE_MAX_NUM_ELEMS: %d\n",
                     *((int *) parameter));
        ret = 1;
        break;
    case TROVE_COLLECTION_ATTR_CACHE_INITIALIZE:
        gossip_debug(GOSSIP_TROVE_DEBUG,
                     "TROVE_COLLECTION_ATTR_CACHE_INITIALIZE \n");
        ret = 1;
        break;
    }

    return ret;
}

static int tas_collection_getinfo(TROVE_coll_id coll_id,
                                  TROVE_context_id context_id,
                                  TROVE_coll_getinfo_options opt,
                                  void *parameter)
{

    gossip_debug(GOSSIP_TROVE_DEBUG,
                 "Tas tas_collection_getinfo\n");

    switch(opt)
    {
        case PVFS_COLLECTION_STATFS:
            {
                TROVE_statfs *tmp_trove_statfs = (TROVE_statfs *)parameter;

                tmp_trove_statfs->fs_id = coll_id;

                tmp_trove_statfs->bytes_available = ((uint64_t) 1000) * 1024 * 1024;
                tmp_trove_statfs->bytes_total = ((uint64_t) 10000) * 1024 * 1024;

                return RETURN_IMMEDIATE_COMPLETE;
            }
    }

     fprintf(stderr,
                  "TROVE MODULE TAS: FUNCTION tas_collection_geteattr not implemented for option"
                  " %d\n", (int) opt);

    return RETURN_IMMEDIATE_COMPLETE;
}

static int tas_collection_seteattr(TROVE_coll_id coll_id,
                                   TROVE_keyval_s * key_p,
                                   TROVE_keyval_s * val_p,
                                   TROVE_ds_flags flags,
                                   void *user_ptr,
                                   TROVE_context_id context_id,
                                   TROVE_op_id * out_op_id_p)
{
    gossip_debug(GOSSIP_TROVE_DEBUG,
                 "Tas tas_collection_seteattr Key:%s \n",
                 (
    char *) key_p->buffer);
    if (key_p->buffer_sz == 12
        && strncmp(key_p->buffer, "root_handle", 12) == 0)
    {
        /* rootHandle = *((TROVE_handle *) val_p->buffer); */
    }
    else
          fprintf(stderr,
                  "TROVE MODULE TAS: FUNCTION tas_collection_seteattr not implemented\n");
    return 1;
}

static int tas_collection_geteattr(TROVE_coll_id coll_id,
                                   TROVE_keyval_s * key_p,
                                   TROVE_keyval_s * val_p,
                                   TROVE_ds_flags flags,
                                   void *user_ptr,
                                   TROVE_context_id context_id,
                                   TROVE_op_id * out_op_id_p)
{
    gossip_debug(GOSSIP_TROVE_DEBUG,
                 "Tas tas_collection_geteattr Key:%s\n",
                 (
    char *) key_p->buffer);
    fprintf(stderr,
            "TROVE MODULE TAS: FUNCTION tas_collection_geteattr not implemented\n");
    return 1;
}


static int tas_open_context(TROVE_coll_id coll_id,
                            TROVE_context_id * context_id)
{
    gossip_debug(GOSSIP_TROVE_DEBUG, "Tas tas_collection_geteattr\n");
    *context_id = 1;
    return RETURN_IMMEDIATE_COMPLETE;
}

static int tas_close_context(TROVE_coll_id coll_id,
                             TROVE_context_id context_id)
{
    gossip_debug(GOSSIP_TROVE_DEBUG, "Tas tas_close_context\n");
    return RETURN_IMMEDIATE_COMPLETE;
}



/*
 * Function pointers to the tas implementation are set in trove-mgmt.c
 */

struct TROVE_mgmt_ops tas_mgmt_ops =
{
        .initialize = tas_initialize,
        .finalize = tas_finalize,
        tas_storage_create,
        tas_storage_remove,
        tas_collection_create,
        tas_collection_remove,
        tas_collection_lookup,
        tas_collection_iterate,
        tas_collection_setinfo,
        tas_collection_getinfo,
        tas_collection_seteattr, tas_collection_geteattr};

struct TROVE_dspace_ops tas_dspace_ops =
{
        tas_dspace_create,
        tas_dspace_remove,
        tas_dspace_iterate_handles,
        tas_dspace_verify,
        tas_dspace_getattr,
        tas_dspace_getattr_list,
        tas_dspace_setattr,
        tas_dspace_cancel,
        tas_dspace_test, tas_dspace_testsome, tas_dspace_testcontext};


struct TROVE_keyval_ops tas_keyval_ops =
{
        tas_keyval_read,
        tas_keyval_write,
        tas_keyval_remove,
        tas_keyval_validate,
        tas_keyval_iterate,
        tas_keyval_iterate_keys,
        tas_keyval_read_list,
        tas_keyval_write_list, tas_keyval_flush, tas_keyval_get_handle_info};


struct TROVE_bstream_ops tas_bstream_ops =
{
        tas_bstream_read_at,
        tas_bstream_write_at,
        tas_bstream_resize,
        tas_bstream_validate,
        tas_bstream_read_list, tas_bstream_write_list, tas_bstream_flush};

struct TROVE_context_ops tas_context_ops =
{
        tas_open_context,
        tas_close_context
        };
