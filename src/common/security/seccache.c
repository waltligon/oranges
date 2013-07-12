/*
 * (C) 2013 Clemson University and Omnibond Systems LLC
 *
 * Security cache functions
 *
 * These functions are used to operate server-side caches for capabilities,
 * certificates and credentials.
 *
 * See COPYING in top-level directory.
 */

#include <string.h>
#include <malloc.h>
#include <time.h>

#include "pvfs2-types.h"
#include "pvfs2-debug.h"
#include "gossip.h"

#include "seccache.h"

/*** helper macros ***/
#define __LOCK_LOCK(__lock, __ret) do { \
                                       if (lock_lock((__lock)) != 0) \
                                       { \
                                           SECCACHE_EXIT_FN(); \
                                           return __ret; \
                                       } \
                                   } while (0)

#define LOCK_LOCK(__lock)      __LOCK_LOCK(__lock, -PVFS_EINVAL)

#define LOCK_LOCK_VOID(__lock) __LOCK_LOCK(__lock, )

#define LOCK_LOCK_NULL(__lock) __LOCK_LOCK(__lock, NULL)

/* note: log a warning but do not exit calling function */
#define LOCK_UNLOCK(__lock) do { \
                                if (lock_unlock((__lock)) != 0) \
                                { \
                                    gossip_err("Warning: could not unlock cache lock\n"); \
                                } \
                            } while (0)


/*** internal functions ***/

static void PINT_seccache_print_stats(seccache_t *cache)
{
    if (cache == NULL)
    {
        return;
    }

    cache->stat_count++;
    if (cache->stat_count % cache->stats_freq == 0)
    {
        gossip_debug(GOSSIP_SECCACHE_DEBUG, "*** %s cache statistics "
                     "***\n", cache->desc);
        gossip_debug(GOSSIP_SECCACHE_DEBUG, "*** entries: %llu inserts: %llu "
                     "removes: %llu\n", llu(cache->stats.entry_count), 
                     llu(cache->stats.inserts), 
                     llu(cache->stats.removed));
        gossip_debug(GOSSIP_SECCACHE_DEBUG, "*** lookups: %llu hits: %llu (%3.1f%%) misses: "
                     "%llu (%3.1f%%) expired: %llu\n", llu(cache->stats.lookups), 
                     llu(cache->stats.hits),
                     ((float) cache->stats.hits / cache->stats.lookups * 100),
                     llu(cache->stats.misses),
                     ((float) cache->stats.misses / cache->stats.lookups * 100),
                     llu(cache->stats.expired));
        cache->stat_count = 0;
    }
}


static int lock_init(seccache_lock_t *lock)
{
    int ret = -1;

    /* TODO: ability to disable locking */
#if SECCACHE_LOCK_TYPE == 0
    return 0;
#elif SECCACHE_LOCK_TYPE == 1
    pthread_mutexattr_t attr;
    ret = pthread_mutexattr_init(&attr);
    if (ret != 0) return -1;
    ret = pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    if (ret != 0) return -1;
    ret = pthread_mutex_init(lock, &attr);
    if (ret != 0) return -1;
#elif SECCACHE_LOCK_TYPE == 2
    ret = pthread_spin_init(lock, 1);
    if (ret != 0) return -1;
#elif SECCACHE_LOCK_TYPE == 3
    *lock = (seccache_lock_t) GEN_SHARED_MUTEX_INITIALIZER_NP;
    ret = 0;
#endif
    return ret;
}

/** lock_lock()
 * Acquires the lock referenced by lock and returns 0 on
 * Success; otherwise, returns -1 and sets errno.
 */
static inline int lock_lock(seccache_lock_t *lock)
{
    int ret = 0;
    
#if SECCACHE_LOCK_TYPE == 0
    return ret;
#elif SECCACHE_LOCK_TYPE == 1
    ret = pthread_mutex_lock(lock);
    return ret;
#elif SECCACHE_LOCK_TYPE == 2
    return pthread_spin_lock(lock);
#elif SECCACHE_LOCK_TYPE == 3
    ret = gen_mutex_lock(lock);
    return ret;
#endif
}

/** lock_unlock()
 * Unlocks the lock.
 * If successful, return zero; otherwise, return -1 and sets errno.
 */
static inline int lock_unlock(seccache_lock_t *lock)
{
#if SECCACHE_LOCK_TYPE == 0
    return 0;
#elif SECCACHE_LOCK_TYPE == 1
    return pthread_mutex_unlock(lock);
#elif SECCACHE_LOCK_TYPE == 2
    return pthread_spin_unlock(lock);
#elif SECCACHE_LOCK_TYPE == 3
    return gen_mutex_unlock(lock);
#endif
}

/** lock_trylock
 * Tries the lock to see if it's available:
 * Returns 0 if lock has not been acquired ie: success
 * Otherwise, returns -1
 */
static inline int lock_trylock(seccache_lock_t *lock)
{
    int ret = -1;

#if (SECCACHE_LOCK_TYPE == 0)
    return 0;
#elif (SECCACHE_LOCK_TYPE == 1)
    ret = pthread_mutex_trylock(lock);
    if (ret != 0)
    {
        ret = -1;
    }
#elif (SECCACHE_LOCK_TYPE == 2)
    ret = pthread_spin_trylock(lock);
    if (ret != 0)
    {
        ret = -1;
    }
#elif SECCACHE_LOCK_TYPE == 3
    ret = gen_mutex_trylock(lock);
    if (ret != 0)
    {
        ret = -1;
    }
#endif
    if (ret == 0)
    {
        /* Unlock before leaving if lock wasn't already set */
        ret = lock_unlock(lock);
    }
    return ret;
}

/** PINT_seccache_rm_expired_entries
 * If 'all' is non-zero, then all hash table chains are scanned for expired
 * entries.
 *
 * If 'all' is zero, then only the hash table chain at index is scanned for expired
 * entries.
 *
 * In both cases, an expired entry's data is cleaned up, the
 * entry freed, and removed from the linked list.
 */
static int PINT_seccache_rm_expired_entries(seccache_t *cache,
                                             PVFS_boolean all,
                                             uint16_t index)
{
    seccache_entry_t now_entry;
    int hash_index;

    SECCACHE_ENTER_FN();

    if (cache == NULL)
    {
        SECCACHE_EXIT_FN();

        return -PVFS_EINVAL;
    }

    LOCK_LOCK(&cache->lock);

    now_entry.expiration = time(NULL);

    /* removes expired entries from all hash table chains */
    if (all)
    {
        gossip_debug(GOSSIP_SECCACHE_DEBUG,
            "Removing all entries with timeouts before %llu\n",
            llu(now_entry.expiration));
        for (hash_index = 0; hash_index < cache->hash_limit; hash_index++)
        {
            gossip_debug(GOSSIP_SECCACHE_DEBUG,
                "searching chain at hash_index = %d\n",
                hash_index);
            seccache_entry_t *rem_entry = NULL;
            while ((rem_entry = PINT_llist_rem(
                cache->hash_table[hash_index],
                &now_entry,
                cache->methods.expired)) != NULL)
            {
                cache->methods.debug("***** REMOVING *****", rem_entry->data);

                cache->methods.cleanup(rem_entry->data);

                cache->stats.removed++;
                cache->stats.entry_count--;
            }
        }
    }
    else
    {
        /* remove expired entries from the hash table chain at index */
        gossip_debug(GOSSIP_SECCACHE_DEBUG,
            "searching chain at hash_index = %d\n", index);
        seccache_entry_t * rem_entry = NULL;
        while ((rem_entry = PINT_llist_rem(
                cache->hash_table[index],
                &now_entry,
                cache->methods.expired)) != NULL)
        {
            cache->methods.debug("***** REMOVING *****", rem_entry->data);

            cache->methods.cleanup(rem_entry->data);

            free(rem_entry);

            cache->stats.removed++;
            cache->stats.entry_count--;
        }
    }

    LOCK_UNLOCK(&cache->lock);

    SECCACHE_EXIT_FN();

    return 0;
}   


/*** end internal functions ***/

/*** cache API functions ***/

/* returns a new security cache structure */
seccache_t * PINT_seccache_new(const char *desc,
                               seccache_methods_t *methods,
                               uint64_t hash_limit)
{
    seccache_t *cache = NULL;
    int i;

    SECCACHE_ENTER_FN();

    if (methods == NULL)
    {
        gossip_err("%s: NULL parameter\n", __func__);
        return NULL;
    }

    /* allocate cache struct */
    cache = (seccache_t *) malloc(sizeof(seccache_t));
    if (cache == NULL)
    {
        gossip_err("%s: no memory (cache struct)\n", __func__);
        return NULL;
    }
    memset(cache, 0, sizeof(seccache_t));

    /* set description */
    cache->desc = desc;

    /* initialize lock */
    lock_init(&cache->lock);

    /* assign functions */
    memcpy(&cache->methods, methods, sizeof(seccache_methods_t));

    /* default properties */
    cache->entry_limit = SECCACHE_ENTRY_LIMIT_DEFAULT;
    cache->size_limit = SECCACHE_SIZE_LIMIT_DEFAULT;
    cache->hash_limit = (hash_limit == 0) ? SECCACHE_HASH_LIMIT_DEFAULT :
                            hash_limit;
    cache->timeout = SECCACHE_TIMEOUT_DEFAULT;
    cache->stats_freq = SECCACHE_STATS_FREQ_DEFAULT;

    /* allocate linked lists */
    cache->hash_table = (PINT_llist_p *) 
        malloc(sizeof(PINT_llist_p) * cache->hash_limit);
    if (cache->hash_table == NULL)
    {
        gossip_err("%s: no memory (hash table)\n", __func__);        
        free(cache);
        return NULL;
    }
    memset(cache->hash_table, 0, sizeof(PINT_llist_p) * cache->hash_limit);

    for (i = 0; i < cache->hash_limit; i++)
    {
        cache->hash_table[i] = PINT_llist_new();
        if (cache->hash_table[i] == NULL)
        {
            goto seccache_new_mem_error;
        }
    }

    /* allocate space for hash chain head entries */
    for (i = 0; i < cache->hash_limit; i++)
    {
        seccache_entry_t *entry = NULL;
        entry = (seccache_entry_t *) malloc(sizeof(seccache_entry_t));
        if (entry == NULL)
        {
            goto seccache_new_mem_error;
        }

        entry->data = NULL;
        entry->data_size = 0;
        entry->expiration = 0xFFFFFFFF;

        PINT_llist_add_to_head(cache->hash_table[i], entry);
    }

    SECCACHE_EXIT_FN();

    return cache;

seccache_new_mem_error:

    /* free previous allocated entries */
    for (i = 0; i < cache->hash_limit; i++)
    {
        /* note: PINT_llist_free will ignore NULL entries */
        PINT_llist_free(cache->hash_table[i], cache->methods.cleanup);
    }

    free(cache->hash_table);

    free(cache);

    gossip_err("%s: no memory (hash table lists/entries)\n", __func__);

    SECCACHE_EXIT_FN();

    return NULL;

}

/* set a security cache property (entry max etc.) */
int PINT_seccache_set(seccache_t *cache,
                      seccache_prop_t prop,
                      uint64_t propval)
{
    int ret = 0;

    SECCACHE_ENTER_FN();

    LOCK_LOCK(&cache->lock);

    if (cache == NULL)
    {
        return -PVFS_EINVAL;
    }

    switch (prop)
    {
    case SECCACHE_ENTRY_LIMIT:
        cache->entry_limit = propval;
        break;

    case SECCACHE_SIZE_LIMIT:
        cache->size_limit = propval;
        break;

    case SECCACHE_HASH_LIMIT:
        gossip_err("%s: warning: cannot set hash limit after cache "
                   "initialization\n", __func__);
        ret = -PVFS_EINVAL;
        break;
        
    case SECCACHE_TIMEOUT:
        cache->timeout = (PVFS_time) propval;
        break;

    case SECCACHE_STATS_FREQ:
        cache->stats_freq = propval;
        break;

    default:
        ret = -PVFS_EINVAL;
    }

    LOCK_UNLOCK(&cache->lock);

    SECCACHE_EXIT_FN();

    return ret;
}

/* returns security cache property or -1 on error */
uint64_t PINT_seccache_get(seccache_t *cache,
                           seccache_prop_t prop)
{
    if (cache == NULL)
    {
        return -PVFS_EINVAL;
    }

    switch (prop)
    {
    case SECCACHE_ENTRY_LIMIT:
        return cache->entry_limit;        

    case SECCACHE_SIZE_LIMIT:
        return cache->size_limit;

    case SECCACHE_HASH_LIMIT:
        return cache->hash_limit;

    case SECCACHE_TIMEOUT:
        return (uint64_t) cache->timeout;

    case SECCACHE_STATS_FREQ:
        return cache->stats_freq;
    }

    return 0xFFFFFFFFFFFFFFFF;
}

/* lock cache for special operations */
int PINT_seccache_lock(seccache_t *cache)
{
    if (cache == NULL)
    {
        return -PVFS_EINVAL;
    }

    LOCK_LOCK(&cache->lock);

    return 0;
}

/* unlock cache */
int PINT_seccache_unlock(seccache_t *cache)
{
    if (cache == NULL)
    {
        return -PVFS_EINVAL;
    }

    LOCK_UNLOCK(&cache->lock);

    return 0;
}

void PINT_seccache_reset_stats(seccache_t *cache)
{
    /* acquire the lock */
    LOCK_LOCK_VOID(&cache->lock);

    /* clear the stats struct */
    memset(&cache->stats, 0, sizeof(seccache_stats_t));

    /* release the lock */
    LOCK_UNLOCK(&cache->lock);
}

/* deletes cache, freeing all memory */
void PINT_seccache_cleanup(seccache_t *cache)
{
    int i;

    /* free hash table lists */
    for (i = 0; i < cache->hash_limit; i++)
    {
        /* note: PINT_llist_free will ignore NULL entries */
        PINT_llist_free(cache->hash_table[i], cache->methods.cleanup);
    }

    /* free hash table */
    free(cache->hash_table);

    /* free cache struct */
    free(cache);
}

/* create an entry using data (not inserted in cache).
   Data is copied */
/*** TODO: remove?*/
#if 0
seccache_entry_t *PINT_seccache_new(void *data,
                                    PVFS_size data_size)
{
    seccache_entry_t *entry;

    if (data == NULL || data_size == 0)
    {
        return NULL;
    }

    /* allocate entry */
    entry = (seccache_entry_t *) malloc(sizeof(seccache_entry_t));
    if (entry == NULL)
    {
        return NULL;
    }

    /* allocate data */
    entry->data = (seccache_entry_t *) malloc(data_size);
    if (entry->data == NULL)
    {
        free(entry);
        return NULL;
    }

    /* copy data */
    memcpy(entry->data, data, data_size);
    entry->data_size = data_size;

    entry->expiration = 0xFFFFFFFF;

    return entry;
}

/* free entry memory */
void PINT_seccache_cleanup_entry(seccache_entry_t *entry)
{
    if (entry->data != NULL)
    {
        free(entry->data);
    }

    free(entry);
}
#endif /* #if 0 */

/* locates an entry given the specified data */
seccache_entry_t * PINT_seccache_lookup(seccache_t *cache, 
                                        void *data)
{
    seccache_entry_t *curr_entry, now_entry;
    uint16_t index = 0;

    SECCACHE_ENTER_FN();
    
    if (cache == NULL || data == NULL)
    {
        SECCACHE_EXIT_FN();

        return NULL;
    }

    /* compute the hash table index using the data */
    index = cache->methods.get_index(data);

    /* acquire the lock */
    LOCK_LOCK_NULL(&cache->lock);

    /* locate the entry in the chain using the cache's compare function */
    curr_entry = (seccache_entry_t *) PINT_llist_search(
        cache->hash_table[index],
        data,
        cache->methods.compare);

    /* unlock the cache lock */
    LOCK_UNLOCK(&cache->lock);

    gossip_debug(GOSSIP_SECCACHE_DEBUG, "%s cache: %s\n",
                 cache->desc, (curr_entry != NULL) ? "hit" : "miss");

    /* check expiration */
    if (curr_entry != NULL)
    {        
        now_entry.expiration = time(NULL);
        /* 0 returned if expired */
        if (cache->methods.expired(&now_entry, curr_entry) == 0)
        {            
            gossip_debug(GOSSIP_SECCACHE_DEBUG, "%s cache: entry %p expired\n",
                         cache->desc, curr_entry);

            PINT_seccache_remove(cache, curr_entry->data);

            curr_entry = NULL;
            cache->stats.expired++;
        }
        else
        {
            /* update expiration */
            cache->methods.set_expired(curr_entry, cache->timeout);
        }
        cache->stats.hits++;
    }
    else
    {
        cache->stats.misses++;
    }

    cache->stats.lookups++;

    PINT_seccache_print_stats(cache);

    SECCACHE_EXIT_FN();

    return curr_entry;
}

/* inserts an entry with the given data */
int PINT_seccache_insert(seccache_t *cache,
                         void *data,
                         PVFS_size data_size)
{
    seccache_entry_t *entry;
    uint16_t index = 0;

    SECCACHE_ENTER_FN();

    if (cache == NULL || data == NULL)
    {
        SECCACHE_EXIT_FN();

        return -PVFS_EINVAL;
    }

    /* create new entry */
    entry = cache->methods.new_entry(data);

    /* compute the hash table index */
    index = cache->methods.get_index(data);

    /* remove expired entries in this chain */
    PINT_seccache_rm_expired_entries(cache, 0, index);

    cache->methods.set_expired(entry, cache->timeout);

    cache->methods.debug("Caching", entry->data);

    /* acquire the lock */
    LOCK_LOCK(&cache->lock);

    /* insert the entry into the chain */
    if (PINT_llist_add_to_head(cache->hash_table[index], entry) < 0)
    {
        SECCACHE_EXIT_FN();

        return -PVFS_ENOMEM;
    }

    gossip_debug(GOSSIP_SECCACHE_DEBUG, "%s cache: entry %p added to the "
                 "head of the linked list @ index = %d\n", cache->desc, 
                 entry, index);

    /* unlock the cache lock */
    LOCK_UNLOCK(&cache->lock);

    cache->stats.inserts++;
    cache->stats.entry_count++;

    SECCACHE_EXIT_FN();

    return 0;
}

/* removes an entry that matches the specified data */
int PINT_seccache_remove(seccache_t *cache,
                         seccache_entry_t *entry)
{
    uint16_t index = 0;
    seccache_entry_t *rem_entry;

    SECCACHE_ENTER_FN();

    if (cache == NULL || entry == NULL)
    {
        SECCACHE_EXIT_FN();

        return -PVFS_EINVAL;
    }

    /* lock cache */
    LOCK_LOCK(&cache->lock);

    /* compute the hash table index */
    index = cache->methods.get_index(entry);

    /* remove entry */
    rem_entry = (seccache_entry_t *) PINT_llist_rem(
        cache->hash_table[index],
        entry->data,
        cache->methods.compare);

    /* unlock cache */
    LOCK_UNLOCK(&cache->lock);

    /* free memory */
    if (rem_entry != NULL)
    {
        gossip_debug(GOSSIP_SECCACHE_DEBUG, "%s cache: removed entry %p at "
                     "index %hd\n", cache->desc, rem_entry, index);

        cache->methods.cleanup(rem_entry);

        free(rem_entry);
        cache->stats.removed++;
        cache->stats.entry_count--;
    }

    SECCACHE_EXIT_FN();

    return 0;
}

