
// Copyright (c) 2000 Walt Ligon, Clemson University, Scyld Computing Inc.
// All rights reserved.  This software is licensed under GPL.
// Author: Walt Ligon
// Date: Summer 2000

// $Header: /root/MIGRATE/CVS2SVN/cvs/pvfs2-1/src/io/description/pvfs-request.c,v 1.11 2003-07-23 12:40:22 neill Exp $
// $Log: not supported by cvs2svn $
// Revision 1.10  2003/07/23 12:05:33  neill
// added a DIST_DEBUG flag for gossip; gossipified some fprintfs in dist
// code; added a line in the server to log what it's logging.
//
// Revision 1.9  2003/07/17 23:22:44  pcarns
// header shuffling- you probably have to do a distclean to build after
// updating
//
// Revision 1.8  2003/07/02 20:00:44  walt
//
// hopefully updates reported before are now correct!
//
// Revision 1.7  2003/07/02 18:29:16  walt
// added refcount to requests, developed code for freeing requests
//
// Revision 1.6  2003/07/01 22:15:21  neill
// Code didn't compile before this...I don't know if this is correct.
//
// Revision 1.5  2003/07/01 20:19:03  walt
// added new mode to request processor to skip logical bytes
// cleaned up mode code
//
// Revision 1.4  2003/06/02 19:55:41  pcarns
// got rid of PVFS_count32 type; replaced with int32_t or uint32_t or int
// where appropriate
//
// Revision 1.3  2003/03/26 15:25:58  walt
// added a couple of comments
//
// Revision 1.2  2003/01/15 19:36:45  walt
// fixed some warnings - changed order of args for request commit
//
// Revision 1.1  2003/01/10 18:37:29  pcarns
// brought Walt's io description code over from the old pvfs2 tree
//
// Revision 1.2  2003/01/09 20:13:15  walt
// more changes to dist and req - filling out stuff for first major test
//
// Revision 1.1  2003/01/08 18:29:05  walt
//  major file reorganization added dist stuff
//
// Revision 1.2  2002/08/06 15:37:21  walt
// changed to PVFS types
//
// Revision 1.1  2002/05/30 21:57:01  walt
// source files for request and distribution processing
//
// Revision 1.5  2000/08/09 15:48:14  walt
// working version 0.02
//
//

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include "pvfs2-types.h"
#include "pint-request.h"
#include "pvfs2-request.h"
#include "pvfs2-debug.h"
#include "gossip.h"

#define PVFS_SUCCESS 0
#define PVFS_ERR_REQ -1

/* elementary reqs */

static struct PINT_Request PINT_CHAR =
		{0, 1, 0, 1, 1, 0, 1, 1, 1, 0, 1, -1, NULL, NULL};
PVFS_Request PVFS_CHAR = &PINT_CHAR;

static struct PINT_Request PINT_SHORT =
		{0, 1, 0, 1, 2, 0, 2, 1, 1, 0, 1, -1, NULL, NULL};
PVFS_Request PVFS_SHORT = &PINT_SHORT;

static struct PINT_Request PINT_INT =
		{0, 1, 0, 1, 4, 0, 4, 1, 1, 0, 1, -1, NULL, NULL};
PVFS_Request PVFS_INT = &PINT_INT;

static struct PINT_Request PINT_LONG =
		{0, 1, 0, 1, 4, 0, 4, 1, 1, 0, 1, -1, NULL, NULL};
PVFS_Request PVFS_LONG = &PINT_LONG;

static struct PINT_Request PINT_UNSIGNED_CHAR =
		{0, 1, 0, 1, 1, 0, 1, 1, 1, 0, 1, -1, NULL, NULL};
PVFS_Request PVFS_UNSIGNED_CHAR = &PINT_UNSIGNED_CHAR;

static struct PINT_Request PINT_UNSIGNED_SHORT =
		{0, 1, 0, 1, 2, 0, 2, 1, 1, 0, 1, -1, NULL, NULL};
PVFS_Request PVFS_UNSIGNED_SHORT = &PINT_UNSIGNED_SHORT;

static struct PINT_Request PINT_UNSIGNED =
		{0, 1, 0, 1, 4, 0, 4, 1, 1, 0, 1, -1, NULL, NULL};
PVFS_Request PVFS_UNSIGNED = &PINT_UNSIGNED;

static struct PINT_Request PINT_UNSIGNED_LONG =
		{0, 1, 0, 1, 4, 0, 4, 1, 1, 0, 1, -1, NULL, NULL};
PVFS_Request PVFS_UNSIGNED_LONG = &PINT_UNSIGNED_LONG;

static struct PINT_Request PINT_FLOAT =
		{0, 1, 0, 1, 4, 0, 4, 1, 1, 0, 1, -1, NULL, NULL};
PVFS_Request PVFS_FLOAT = &PINT_FLOAT;

static struct PINT_Request PINT_DOUBLE =
		{0, 1, 0, 1, 8, 0, 8, 1, 1, 0, 1, -1, NULL, NULL};
PVFS_Request PVFS_DOUBLE = &PINT_DOUBLE;

static struct PINT_Request PINT_LONG_DOUBLE =
		{0, 1, 0, 1, 8, 0, 8, 1, 1, 0, 1, -1, NULL, NULL};
PVFS_Request PVFS_LONG_DOUBLE = &PINT_LONG_DOUBLE;

static struct PINT_Request PINT_BYTE =
		{0, 1, 0, 1, 1, 0, 1, 1, 1, 0, 1, -1, NULL, NULL};
PVFS_Request PVFS_BYTE = &PINT_BYTE;

static struct PINT_Request PINT_PACKED =
		{0, 1, 0, 1, 1, 0, 1, 1, 1, 0, 1, -1, NULL, NULL};
PVFS_Request PVFS_PACKED = &PINT_PACKED;

/* int PVFS_Request_extent(PVFS_Request request, PVFS_size *extent); */

int PVFS_Request_contiguous(int32_t count, PVFS_Request oldreq,
		PVFS_Request *newreq) 
{
	return PVFS_Request_hvector(1, count, 0, oldreq, newreq);
}

int PVFS_Request_vector(int32_t count, int32_t blocklength,
		PVFS_offset stride, PVFS_Request oldreq, PVFS_Request *newreq)
{
	int64_t extent;
	if (oldreq == NULL)
		return PVFS_ERR_REQ;
	PVFS_Request_extent(oldreq, &extent);
	return PVFS_Request_hvector(count, blocklength, stride*extent,
			oldreq,newreq);
}

static int PINT_subreq(PVFS_offset offset, int32_t bsize,
		PVFS_size stride, int32_t count, PVFS_Request oldreq,
		PVFS_size oldext, PVFS_Request *newreq)
{
	if (oldreq == NULL)
		return PVFS_ERR_REQ;
	(*newreq)->offset = offset;
	(*newreq)->num_ereqs = bsize;
	(*newreq)->stride = stride;
	(*newreq)->num_blocks = count;
	(*newreq)->ub = offset + ((count - 1) * stride) + (bsize * oldext);
	(*newreq)->lb = offset;
	(*newreq)->aggregate_size = oldreq->aggregate_size * count * bsize;
	if (oldreq->aggregate_size != oldext)
		(*newreq)->num_contig_chunks = oldreq->num_contig_chunks * count * bsize;
	else if (stride != bsize * oldreq->aggregate_size &&
			stride != -(bsize * oldreq->aggregate_size))
		(*newreq)->num_contig_chunks = count;
	else
		(*newreq)->num_contig_chunks = 1;
	(*newreq)->depth = oldreq->depth + 1;
	(*newreq)->num_nested_req = oldreq->num_nested_req + 1;
	(*newreq)->committed = 0;
	(*newreq)->refcount = 0;
	(*newreq)->ereq = oldreq;
	return PVFS_SUCCESS;
}

int PVFS_Request_hvector(int32_t count, int32_t blocklength,
		PVFS_size stride, PVFS_Request oldreq, PVFS_Request *newreq)
{
	PVFS_size oldext;
	if (oldreq == NULL)
		return PVFS_ERR_REQ;
	PVFS_Request_extent(oldreq, &oldext);
	PINT_REQUEST_REFINC(oldreq);
	*newreq = (PINT_Request *)malloc(sizeof(struct PINT_Request));
	(*newreq)->sreq = NULL;
	PINT_subreq(0, blocklength, stride, count, oldreq, oldext, newreq);
	/* calculate statistics like ub, lb, depth, etc. */
	if (stride < 0)
	{
		(*newreq)->lb = (count - 1) * stride;
	}
	PINT_REQUEST_REFSET(*newreq);
	return PVFS_SUCCESS;
}

int PVFS_Request_indexed(int32_t count, int32_t *blocklengths,
		PVFS_size *displacements, PVFS_Request oldreq, PVFS_Request *newreq)
{
	PINT_Request *dt;
	int64_t oldext;
	*newreq = NULL;
	if (oldreq == NULL)
		return PVFS_ERR_REQ;
	PVFS_Request_extent(oldreq, &oldext);
	PINT_REQUEST_REFINC(oldreq);
	while (count--)
	{
		dt = *newreq;
		*newreq = (PINT_Request *)malloc(sizeof(struct PINT_Request));
		(*newreq)->sreq = dt;
		PINT_subreq(displacements[count]*oldext, blocklengths[count],
				0, 1, oldreq, oldext, newreq);
		/* calculate statistics like ub, lb, depth, etc. */
		if ((*newreq)->sreq)
		{
			if ((*newreq)->lb > (*newreq)->sreq->lb)
				(*newreq)->lb = (*newreq)->sreq->lb;
			if ((*newreq)->ub < (*newreq)->sreq->ub)
				(*newreq)->ub = (*newreq)->sreq->ub;
			if ((*newreq)->depth < (*newreq)->sreq->depth)
				(*newreq)->depth = (*newreq)->sreq->depth;
			(*newreq)->aggregate_size = (*newreq)->aggregate_size +
						(*newreq)->sreq->aggregate_size;
			(*newreq)->num_contig_chunks = (*newreq)->num_contig_chunks +
						(*newreq)->sreq->num_contig_chunks;
			if ((*newreq)->sreq)
			{
				/* contribution of ereq handled in subreq */
				(*newreq)->num_nested_req += (*newreq)->sreq->num_nested_req + 1;
				/* this tries to deal with non-tree request graphs */
				if ((*newreq)->ereq == (*newreq)->sreq->ereq)
				{
					(*newreq)->num_nested_req -= (*newreq)->ereq->num_nested_req + 1;
				}
			}
		}
	}
	PINT_REQUEST_REFSET(*newreq);
	return PVFS_SUCCESS;
}

int PVFS_Request_hindexed(int32_t count, int32_t *blocklengths,
		PVFS_size *displacements, PVFS_Request oldreq, PVFS_Request *newreq)
{
	PINT_Request *dt;
	int64_t oldext;
	*newreq = NULL;
	if (oldreq == NULL)
		return PVFS_ERR_REQ;
	PVFS_Request_extent(oldreq, &oldext);
	PINT_REQUEST_REFINC(oldreq);
	while (count--)
	{
		dt = *newreq;
		*newreq = (PINT_Request *)malloc(sizeof(struct PINT_Request));
		(*newreq)->sreq = dt;
		PINT_subreq(displacements[count], blocklengths[count], 0, 1,
				oldreq, oldext, newreq);
		/* calculate statistics like ub, lb, depth, etc. */
		if ((*newreq)->sreq)
		{
			if ((*newreq)->lb > (*newreq)->sreq->lb)
				(*newreq)->lb = (*newreq)->sreq->lb;
			if ((*newreq)->ub < (*newreq)->sreq->ub)
				(*newreq)->ub = (*newreq)->sreq->ub;
			if ((*newreq)->depth < (*newreq)->sreq->depth)
				(*newreq)->depth = (*newreq)->sreq->depth;
			(*newreq)->aggregate_size = (*newreq)->aggregate_size +
						(*newreq)->sreq->aggregate_size;
			(*newreq)->num_contig_chunks = (*newreq)->num_contig_chunks +
						(*newreq)->sreq->num_contig_chunks;
			if ((*newreq)->sreq)
			{
				/* contribution of ereq handled in subreq */
				(*newreq)->num_nested_req += (*newreq)->sreq->num_nested_req + 1;
				/* this tries to deal with non-tree request graphs */
				if ((*newreq)->ereq == (*newreq)->sreq->ereq)
				{
					(*newreq)->num_nested_req -= (*newreq)->ereq->num_nested_req + 1;
				}
			}
		}
	}
	PINT_REQUEST_REFSET(*newreq);
	return PVFS_SUCCESS;
}

int PVFS_Request_struct(int32_t count, int32_t *blocklengths,
		PVFS_size *displacements, PVFS_Request *oldreqs, PVFS_Request *newreq)
{
	PINT_Request *dt;
	int64_t oldext;
	*newreq = NULL;
	if (oldreqs == NULL)
		return PVFS_ERR_REQ;
	while (count--)
	{
		if (oldreqs[count] == NULL)
			return PVFS_ERR_REQ;
		PVFS_Request_extent(oldreqs[count], &oldext);
		dt = *newreq;
		*newreq = (PINT_Request *)malloc(sizeof(struct PINT_Request));
		(*newreq)->sreq = dt;
		PINT_subreq(displacements[count], blocklengths[count],
				0, 1, oldreqs[count], oldext, newreq);
		PINT_REQUEST_REFINC(oldreqs[count]);
		/* calculate statistics like ub, lb, depth, etc. */
		if ((*newreq)->sreq)
		{
			if ((*newreq)->lb > (*newreq)->sreq->lb)
				(*newreq)->lb = (*newreq)->sreq->lb;
			if ((*newreq)->ub < (*newreq)->sreq->ub)
				(*newreq)->ub = (*newreq)->sreq->ub;
			if ((*newreq)->depth < (*newreq)->sreq->depth)
				(*newreq)->depth = (*newreq)->sreq->depth;
			(*newreq)->aggregate_size = (*newreq)->aggregate_size +
						(*newreq)->sreq->aggregate_size;
			(*newreq)->num_contig_chunks = (*newreq)->num_contig_chunks +
						(*newreq)->sreq->num_contig_chunks;
			if ((*newreq)->sreq)
			{
				/* contribution of ereq handled in subreq */
				(*newreq)->num_nested_req += (*newreq)->sreq->num_nested_req + 1;
				/* this tries to deal with non-tree request graphs */
				if ((*newreq)->ereq == (*newreq)->sreq->ereq)
				{
					(*newreq)->num_nested_req -= (*newreq)->ereq->num_nested_req + 1;
				}
			}
		}
	}
	PINT_REQUEST_REFSET(*newreq);
	return PVFS_SUCCESS;
}

int PVFS_Address(void* location, PVFS_offset *address)
{
	address = location;
	return PVFS_SUCCESS;
}

int PVFS_Request_extent(PVFS_Request request, PVFS_size *extent)
{
	if (request == NULL)
		return PVFS_ERR_REQ;
	*extent = request->ub - request->lb;
	return PVFS_SUCCESS;
}

int PVFS_Request_size(PVFS_Request request, PVFS_size *size)
{
	if (request == NULL)
		return PVFS_ERR_REQ;
	*size = request->aggregate_size;
	return PVFS_SUCCESS;
}

int PVFS_Request_lb(PVFS_Request request, PVFS_size *displacement)
{
	if (request == NULL)
		return PVFS_ERR_REQ;
	*displacement = request->lb;
	return PVFS_SUCCESS;
}

int PVFS_Request_ub(PVFS_Request request, PVFS_size *displacement)
{
	if (request == NULL)
		return PVFS_ERR_REQ;
	*displacement = request->ub;
	return PVFS_SUCCESS;
}

/* This function will take the request that points to all the 
 * contained types, separate out each of the types and then lay them out in a
 * contiguous region of memory. A pointer to this contiguous region will
 * then be passed back in the argument
 */
int PVFS_Request_commit(PVFS_Request *reqp)
{
	PVFS_Request region = NULL;
	PVFS_Request req;

	/* check pointer to pointer */
	if (reqp == NULL)
	{
		gossip_lerr("PVFS_Request_commit: NULL pointer argument\n");
		return PVFS_ERR_REQ;
	}

	req = *reqp;

	/* now check the pointer */
	if (req == NULL)
	{
		gossip_lerr("PVFS_Request_commit: pointer to NULL pointer argument\n");
		return PVFS_ERR_REQ;
	}

	/* this is a committed request - can't re-commit */
	if (req->committed)
	{
		gossip_lerr("PVFS_Request_commit: pointer to commited request\n");
		return PVFS_ERR_REQ;
	}
	       
	/* Allocate memory for contiguous region */
	if(req->num_nested_req > 0)
	{
		int index = 0;
		region = (PVFS_Request)malloc(req->num_nested_req *
				sizeof(struct PINT_Request));
		if (region == NULL)
		{
			gossip_lerr("PVFS_Request_commit: Memory cannot be allocated\n");
			return PVFS_ERR_REQ;
  		}   
		/* pack the request */
  		PINT_Request_commit(region, req, &index);
	}
	/* return the pointer to the memory region */
	*reqp = region;
	return PVFS_SUCCESS;
}

int PVFS_Request_free(PVFS_Request *req)
{
	PVFS_Request reqp;
	if (req == NULL)
	{
		gossip_lerr("PVFS_Request_free: NULL pointer argument\n");
		return PVFS_ERR_REQ;
	}
	if (*req == NULL)
	{
		gossip_lerr("PVFS_Request_free: pointer to NULL pointer argument\n");
		return PVFS_ERR_REQ;
	}
	if ((*req)->refcount <= 0)
	{
		/* if refcount is 0 then it has already been freed */
		/* if less than 0 it should not be freed */
		/* can't be sure if this is users's variable or not */
		return PVFS_SUCCESS;
	}
	PINT_REQUEST_REFDEC(*req);
	if ((*req)->refcount > 0)
	{
		/* not ready to free this yet */
		*req = NULL;
		return PVFS_SUCCESS;
	}
	if ((*req)->committed)
	{
		/* these are contiguous and have no external refs */
		free(*req);
		*req = NULL;
		return PVFS_SUCCESS;
	}
	/* this deals with the sreq chain */
	reqp = (*req)->sreq;
	while (reqp)
	{
		PVFS_Request reqp_next;
		PVFS_Request_free(&(reqp->ereq));
		/* this is a little awkward but it works */
		reqp_next = reqp->sreq;
		free(reqp);
		reqp = reqp_next;
	}
	/* now deal with the main struct */
	PVFS_Request_free(&((*req)->ereq));
	free(*req);
	*req = NULL;
	return PVFS_SUCCESS;
}

#if 0
int PVFS_Request_commit(PVFS_Request *request)
{
	if (request == NULL)
		return PVFS_ERR_REQ;
	return PVFS_SUCCESS;
}

int PVFS_Request_free(PVFS_Request *request)
{
	if (request == NULL)
		return PVFS_ERR_REQ;
	return PVFS_SUCCESS;
}
int PVFS_Get_elements(PVFS_Status *status, PVFS_Request request, int *count)
{
	if (request == NULL)
		return PVFS_ERR_REQ;
	return PVFS_SUCCESS;
}
int PVFS_Pack_size(int incount, PVFS_Request request, PVFS_Comm comm, int *size) 
{
	if (request == NULL)
		return PVFS_ERR_REQ;
	*size = (request->size * incount) + PVFS_BSEND_OVERHEAD;
	return PVFS_SUCCESS;
}
#endif

void PVFS_Dump_request(PVFS_Request req)
{
	gossip_debug(REQUEST_DEBUG,"**********************\n");
	gossip_debug(REQUEST_DEBUG,"address:\t%x\n",(unsigned int)req);
	gossip_debug(REQUEST_DEBUG,"offset:\t\t%d\n",(int)req->offset);
	gossip_debug(REQUEST_DEBUG,"num_ereqs:\t%d\n",(int)req->num_ereqs);
	gossip_debug(REQUEST_DEBUG,"num_blocks:\t%d\n",(int)req->num_blocks);
	gossip_debug(REQUEST_DEBUG,"stride:\t\t%d\n",(int)req->stride);
	gossip_debug(REQUEST_DEBUG,"ub:\t\t%d\n",(int)req->ub);
	gossip_debug(REQUEST_DEBUG,"lb:\t\t%d\n",(int)req->lb);
	gossip_debug(REQUEST_DEBUG,"agg_size:\t%d\n",(int)req->aggregate_size);
	gossip_debug(REQUEST_DEBUG,"num_chunk:\t%d\n",(int)req->num_contig_chunks);
	gossip_debug(REQUEST_DEBUG,"depth:\t\t%d\n",(int)req->depth);
	gossip_debug(REQUEST_DEBUG,"num_nest:\t%d\n",(int)req->num_nested_req);
	gossip_debug(REQUEST_DEBUG,"commit:\t\t%d\n",(int)req->committed);
	gossip_debug(REQUEST_DEBUG,"ereq:\t\t%x\n",(int)req->ereq);
	gossip_debug(REQUEST_DEBUG,"sreq:\t\t%x\n",(int)req->sreq);
	gossip_debug(REQUEST_DEBUG,"**********************\n");
}
