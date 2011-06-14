/* Copyright (C) 2011 Omnibond, LLC
   User cache functions */

#include <Windows.h>
#include "pvfs2.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "gen-locks.h"

#include "client-service.h"
#include "user-cache.h"

/* amount of time cache mgmt thread sleeps */
#define USER_THREAD_SLEEP_TIME    60000

struct qhash_table *user_cache;

gen_mutex_t user_cache_mutex;

int user_compare(void *key, 
                 struct qhash_head *link)
{
    struct user_entry *entry = qhash_entry(link, struct user_entry, hash_link);

    return !stricmp((char *) key, entry->user_name);
}

int add_user(char *user_name, 
             PVFS_credentials *credentials,
             ASN1_UTCTIME *expires)
{
    struct qhash_head *link;
    struct user_entry *entry;

    /* search for existing entry -- delete if found */
    gen_mutex_lock(&user_cache_mutex);
    link = qhash_search(user_cache, user_name);
    if (link != NULL)
    {        
        DbgPrint("   add_user: deleting user %s\n", user_name);
        qhash_del(link);
        free(qhash_entry(link, struct user_entry, hash_link));
    }
    gen_mutex_unlock(&user_cache_mutex);

    /* allocate entry */
    entry = (struct user_entry *) calloc(1, sizeof(struct user_entry));
    if (entry == NULL)
    {
        DbgPrint("   add_user: out of memory\n");
        return -1;
    }
            
    /* add to hash table */
    strncpy(entry->user_name, user_name, 256);
    entry->credentials.uid = credentials->uid;
    entry->credentials.gid = credentials->gid;
    entry->expires = expires;

    gen_mutex_lock(&user_cache_mutex);
    qhash_add(user_cache, &entry->user_name, &entry->hash_link);
    DbgPrint("   add_user: adding user %s (%u:%u) expires %s\n", 
        user_name, credentials->uid, credentials->gid, 
        expires != NULL ? expires->data : "never");
    gen_mutex_unlock(&user_cache_mutex);

    return 0;
}

int get_cached_user(char *user_name, 
                    PVFS_credentials *credentials)
{
    struct qhash_head *link;
    struct user_entry *entry;

    /* locate user by user_name */
    gen_mutex_lock(&user_cache_mutex);
    link = qhash_search(user_cache, user_name);
    if (link != NULL)
    {
        /* if cache hit -- return credentials */
        entry = qhash_entry(link, struct user_entry, hash_link);
        credentials->uid = entry->credentials.uid;
        credentials->gid = entry->credentials.gid;

        DbgPrint("   get_cached_user: hit for %s (%u:%u)\n", user_name,
            credentials->uid, credentials->gid);

        gen_mutex_unlock(&user_cache_mutex);

        return 0;
    }

    gen_mutex_unlock(&user_cache_mutex);

    /* cache miss */
    return 1;
}

/* remove user entry -- note user_cache_mutex 
   should be locked */
/* *** not currently needed
int remove_user(char *user_name)
{
    struct qhash_head *link; 
    
    link = qhash_search_and_remove(user_cache, user_name);
    if (link != NULL)
    {
        free(qhash_entry(link, struct user_entry, hash_link));
    }

    return 0;
}
*/

unsigned int user_cache_thread(void *options)
{
    int i;
    struct qhash_head *head;
    struct user_entry *entry;
    time_t now;

    /* remove expired user entries from user cache */
    do
    {        
        Sleep(USER_THREAD_SLEEP_TIME);

        DbgPrint("user_cache_thread: checking\n");

        now = time(NULL);

        gen_mutex_lock(&user_cache_mutex);
        for (i = 0; i < user_cache->table_size; i++)
        {
            head = qhash_search_at_index(user_cache, i);
            if (head != NULL)
            {    
                entry = qhash_entry(head, struct user_entry, hash_link);
                if (entry->expires != NULL && 
                    ASN1_UTCTIME_cmp_time_t(entry->expires, now) == -1)
                {   
                    DbgPrint("user_cache_thread: removing %s\n", entry->user_name);
                    qhash_del(head);
                    free(entry);
                }
            }
        }
        gen_mutex_unlock(&user_cache_mutex);

        DbgPrint("user_cache_thread: check complete\n");

    } while (1);

    return 0;
}
