/*
 * (C) 2002 Clemson University and The University of Chicago
 *
 * See COPYING in top-level directory.
 */

#ifndef __MKSPACE_H
#define __MKSPACE_H

#include "pvfs2-internal.h"
#include "trove.h"

#define PVFS2_MKSPACE_GOSSIP_VERBOSE 1
#define PVFS2_MKSPACE_STDERR_VERBOSE 2

int pvfs2_mkspace(
    char *data_path,
    char *meta_path,
    char *collection,
    TROVE_coll_id coll_id,
    TROVE_handle root_handle,
    char *meta_handle_ranges,
    char *data_handle_ranges,
    int create_collection_only,
    int verbose,
    struct server_configuration_s *config);

int pvfs2_rmspace(
    char *data_path,
    char *meta_path,
    char *collection,
    TROVE_coll_id coll_id,
    int remove_collection_only,
    int verbose,
    struct server_configuration_s *config);

#endif /* __MKSPACE_H */
