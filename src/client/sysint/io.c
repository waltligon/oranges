/*
 * (C) 2001 Clemson University and The University of Chicago
 *
 * See COPYING in top-level directory.
 */

/* I/O Function Implementation */
#include <malloc.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
#include <limits.h>

#include <pinode-helper.h>
#include <pvfs2-sysint.h>
#include <pint-sysint-utils.h>
#include <pvfs2-req-proto.h>
#include <pvfs-distribution.h>
#include <pint-servreq.h>
#include <pint-bucket.h>
#include <PINT-reqproto-encode.h>
#include "pint-sysint-utils.h"
#include "pint-request.h"

extern job_context_id PVFS_sys_job_context;

/* TODO: try to do something to avoid so many mallocs
 * TODO: figure out if we have to do anything special for short
 * reads or writes
 * TODO: figure out what should be passed out in the system
 * interface response (more info on what completed, info on which
 * servers failed, etc.)
 */

static int io_req_ack_flow_array(bmi_addr_t* addr_array,
    struct PVFS_server_req* req_array,
    bmi_size_t max_resp_size,
    void** resp_encoded_array,
    struct PINT_decoded_msg* resp_decoded_array,
    flow_descriptor** flow_array,
    int* error_code_array,
    int array_size,
    PVFS_msg_tag_t* op_tag_array,
    PVFS_object_attr* attr_p,
    PVFS_pinode_reference pinode_refn, /* FGS replaced PVFS_sys_io request */
    PVFS_Request file_req,
    PVFS_offset file_req_offset,
    void* buffer, 
    PVFS_size buffer_size, 
    PVFS_credentials credentials, /* end changed by frank */
    enum PVFS_io_type type,
    enum PVFS_flowproto_type flow_type);

static void io_release_req_ack_flow_array(bmi_addr_t* addr_array,
    struct PVFS_server_req* req_array,
    bmi_size_t max_resp_size,
    void** resp_encoded_array,
    struct PINT_decoded_msg* resp_decoded_array,
    flow_descriptor** flow_array,
    int* error_code_array,
    int array_size);

static int io_find_target_dfiles(PVFS_Request file_req, PVFS_offset file_req_offset, 
    pinode* pinode_ptr, PVFS_handle* target_handle_array, 
    int* target_handle_count);

/* PVFS_sys_io()
 *
 * performs a read or write operation.  PVFS_sys_read() and
 * PVFS_sys_write() are aliases to this function.
 *
 * returns 0 on success, -errno on failure
 */
int PVFS_sys_io_old(PVFS_pinode_reference pinode_refn, PVFS_Request file_req, 
		    PVFS_offset file_req_offset, void* buffer, PVFS_size buffer_size, 
		    PVFS_credentials credentials, PVFS_sysresp_io *resp, 
		    enum PVFS_io_type type)
{
    pinode* pinode_ptr = NULL;
    uint32_t attr_mask = 0;
    int ret = -1;
    bmi_addr_t* addr_array = NULL;
    struct PVFS_server_req* req_array = NULL;
    struct PVFS_server_resp* tmp_resp = NULL;
    void** resp_encoded_array = NULL;
    struct PINT_decoded_msg* resp_decoded_array = NULL;
    int* error_code_array = NULL;
    flow_descriptor** flow_array = NULL;
    int i,j;
    int target_handle_count = 0;
    PVFS_handle* target_handle_array = NULL;
    int total_errors = 0;
    PVFS_msg_tag_t* op_tag_array = NULL;
    /* TODO: might want hooks to set this from app. level later */
    enum PVFS_flowproto_type flow_type = FLOWPROTO_DEFAULT;

    if((type != PVFS_IO_READ) && (type != PVFS_IO_WRITE))
    {
	return(-EINVAL);
    }

    if (file_req == NULL)
    {
	return(-EINVAL);
    }

    /* find a pinode for the target file */
    attr_mask = PVFS_ATTR_META_ALL|PVFS_ATTR_COMMON_ALL;
    ret = phelper_get_pinode(pinode_refn, &pinode_ptr,
	attr_mask, credentials);
    if(ret < 0)
    {
	return(ret);
    }

    /* check permissions */
    ret = PINT_check_perms(pinode_ptr->attr, pinode_ptr->attr.perms,
                      credentials.uid, credentials.gid);
    if(ret < 0)
    {
	return(-EACCES);
    }

    ret = PINT_Dist_lookup(pinode_ptr->attr.u.meta.dist);
    if(ret < 0)
    {
	phelper_release_pinode(pinode_ptr);
	goto sys_io_out;
    }

    target_handle_array =
	(PVFS_handle*)malloc(pinode_ptr->attr.u.meta.dfile_count
	* sizeof(PVFS_handle));
    if(!target_handle_array)
    {
	phelper_release_pinode(pinode_ptr);
	ret = -errno;
	goto sys_io_out;
    }

    /* find out which handles must be included to service this
     * particular I/O request; hopefully we don't really have to
     * contact everyone, just the servers that hold the parts of
     * the file that we are interested in.
     */
    ret = io_find_target_dfiles(file_req, file_req_offset, pinode_ptr, 
	target_handle_array, &target_handle_count);
    if(ret < 0)
    {
	phelper_release_pinode(pinode_ptr);
	goto sys_io_out;
    }

    if (target_handle_count == 0) {
	/* not necessarily an error: we might have gotten a 
	 * zero-byte request.  that's fine.. less work for us! */
	phelper_release_pinode(pinode_ptr);
	resp->total_completed = 0;
	goto sys_io_out;
    }

    /* create storage for book keeping information */
    addr_array = (bmi_addr_t*)malloc(target_handle_count *
	sizeof(bmi_addr_t));
    req_array = (struct PVFS_server_req*)
	malloc(target_handle_count * 
	sizeof(struct PVFS_server_req));
    resp_encoded_array = (void**)malloc(target_handle_count *
	sizeof(void*));
    resp_decoded_array = (struct PINT_decoded_msg*)
	malloc(target_handle_count * 
	sizeof(struct PINT_decoded_msg));
    error_code_array = (int*)malloc(target_handle_count *
	sizeof(int));
    memset(error_code_array, 0, (target_handle_count*sizeof(int)));
    op_tag_array = (PVFS_msg_tag_t*)malloc(target_handle_count*
	sizeof(PVFS_msg_tag_t));
    for(i=0; i<target_handle_count; i++)
    {
	op_tag_array[i] = get_next_session_tag();
    }

    /* stuff for running flows */
    flow_array = (flow_descriptor**)malloc(target_handle_count *
	sizeof(flow_descriptor*));
    if(flow_array)
	memset(flow_array, 0, (target_handle_count *
	    sizeof(flow_descriptor*)));
	
    if(!addr_array || !req_array || !resp_encoded_array ||
	!resp_decoded_array || !error_code_array || !flow_array)
    {
	phelper_release_pinode(pinode_ptr);
	ret = -ENOMEM;
	gossip_lerr("Error: out of memory.\n");
	goto sys_io_out;
    }

    /* setup the I/O request to each data server */
    for(i=0; i<target_handle_count; i++)
    {
	/* resolve the address of the server */
	ret = PINT_bucket_map_to_server(
	    &(addr_array[i]),
	    target_handle_array[i],
	    pinode_refn.fs_id);
	if(ret < 0)
	{
	    phelper_release_pinode(pinode_ptr);
	    gossip_lerr("Error: can't map to server.\n");
	    goto sys_io_out;
	}

	/* fill in the I/O request */
	req_array[i].op = PVFS_SERV_IO;
	req_array[i].credentials = credentials;
	req_array[i].u.io.handle = target_handle_array[i];
	req_array[i].u.io.fs_id = pinode_refn.fs_id;
	req_array[i].u.io.flow_type = flow_type;
	/* TODO: this is silly */
	for(j=0; j<pinode_ptr->attr.u.meta.dfile_count; j++)
	{
	    if(target_handle_array[i] == pinode_ptr->attr.u.meta.dfile_array[j])
	    {
		req_array[i].u.io.server_nr = j;
		break;
	    }
	}
	req_array[i].u.io.server_ct =
	    pinode_ptr->attr.u.meta.dfile_count;
	req_array[i].u.io.file_req = file_req;
	req_array[i].u.io.file_req_offset = file_req_offset;
	req_array[i].u.io.io_dist = pinode_ptr->attr.u.meta.dist;
	req_array[i].u.io.io_type = type;
    }
   
    /* run the I/O series for each server */
    ret = io_req_ack_flow_array(
	addr_array,
	req_array,
	PINT_encode_calc_max_size(PINT_ENCODE_RESP, PVFS_SERV_IO,
	    PINT_CLIENT_ENC_TYPE),
	resp_encoded_array,
	resp_decoded_array,
	flow_array,
	error_code_array,
	target_handle_count,
	op_tag_array,
	&pinode_ptr->attr,
	pinode_refn, /* FGS replaced PVFS_sys_io request */
	file_req,
	file_req_offset,
	buffer, 
	buffer_size, 
	credentials, /* end changed by frank */
	type,
	flow_type);
    if(ret < 0)
    {
	phelper_release_pinode(pinode_ptr);
	gossip_lerr("Error: io_req_ack_flow_array() failure.\n");
	goto sys_io_out;
    }

    phelper_release_pinode(pinode_ptr);

    resp->total_completed = 0;

    /* if this was a read, tally up the total I/O size based on
     * results of the flows
     */
    if(type == PVFS_IO_READ)
    {
	for(i=0; i<target_handle_count; i++)
	{
	    if(!error_code_array[i])
	    {
		resp->total_completed +=
		    flow_array[i]->total_transfered;
	    }
	}
    }

    /* release resources that were used while running the I/O */
    io_release_req_ack_flow_array(
	addr_array,
	req_array,
	PINT_encode_calc_max_size(PINT_ENCODE_RESP, PVFS_SERV_IO,
	    PINT_CLIENT_ENC_TYPE),
	resp_encoded_array,
	resp_decoded_array,
	flow_array,
	error_code_array,
	target_handle_count);

    /****************************************************************/

    /* if this was a write operation, then we need to wait for a
     * final ack from the data servers indicating the status of
     * the operation
     */
    if(type == PVFS_IO_WRITE)
    {
	/* wait for all of the final acks */
	ret = PINT_recv_ack_array(
	    addr_array,
	    PINT_encode_calc_max_size(PINT_ENCODE_RESP, 
		PVFS_SERV_WRITE_COMPLETION, PINT_CLIENT_ENC_TYPE),
	    resp_encoded_array,
	    resp_decoded_array,
	    error_code_array,
	    target_handle_count,
	    op_tag_array);
	if(ret < 0)
	{
	    gossip_lerr("Error: ack array failure.\n");
	    goto sys_io_out;
	}

	/* set an error code for each negative ack that we received */
	for(i=0; i<target_handle_count; i++)
	{
	    tmp_resp = (struct
		PVFS_server_resp*)resp_decoded_array[i].buffer;
	    if(!(error_code_array[i]) && tmp_resp->status)
		error_code_array[i] = tmp_resp->status;
	    if(!(error_code_array[i]) && !(tmp_resp->status))
		resp->total_completed +=
		    tmp_resp->u.write_completion.total_completed;
	}

	/* release resources */
	PINT_release_ack_array(
	    addr_array,
	    PINT_encode_calc_max_size(PINT_ENCODE_RESP, 
		PVFS_SERV_WRITE_COMPLETION, PINT_CLIENT_ENC_TYPE),
	    resp_encoded_array,
	    resp_decoded_array,
	    error_code_array,
	    target_handle_count);
    }

    /* default to reporting no errors, until we check our error
     * code array
     */
    ret = 0;

    /* find out how many errors we hit */
    for(i=0; i<target_handle_count; i++)
    {
	tmp_resp = (struct
	    PVFS_server_resp*)resp_decoded_array[i].buffer;
	if(error_code_array[i])
	{
	    total_errors ++;
	    gossip_lerr("Error: EIO.\n");
	    ret = -EIO;
	}
    }

    gossip_ldebug(CLIENT_DEBUG, 
	"%d servers contacted.\n", target_handle_count);
    gossip_ldebug(CLIENT_DEBUG,
	"%d servers experienced errors.\n", total_errors);
       
    /* drop through and pass out return value, successful cases go
     * through here also
     */
sys_io_out:
    
    if(op_tag_array)
	free(op_tag_array);
    if(addr_array)
	free(addr_array);
    if(req_array)
	free(req_array);
    if(resp_encoded_array)
	free(resp_encoded_array);
    if(resp_decoded_array)
	free(resp_decoded_array);
    if(error_code_array)
	free(error_code_array);
    if(target_handle_array)
	free(target_handle_array);
    if(flow_array)
	free(flow_array);

    return(ret);
}

/* io_find_target_dfiles()
 *
 * determines what subset of the datafiles actually contain data that we
 * are interested in for this request
 *
 * returns 0 on success, -errno on failure
 *
 * TODO: make this step more efficient 
 */
static int io_find_target_dfiles(PVFS_Request file_req, PVFS_offset file_req_offset, 
    pinode* pinode_ptr, PVFS_handle* target_handle_array, 
    int* target_handle_count)
{
    struct PINT_Request_state* req_state = NULL;
    PINT_Request_file_data tmp_file_data;
    int i = 0;
    int ret = -1;
    PINT_Request_result tmp_result;

    *target_handle_count = 0;

    req_state = PINT_New_request_state(file_req);
    if(!req_state)
    {
	return(-ENOMEM);
    }

    for(i=0; i<pinode_ptr->attr.u.meta.dfile_count; i++)
    {
	/* NOTE: we don't have to give an accurate file size here,
	 * as long as we set the extend flag to tell the I/O req
	 * processor to continue past eof if needed
	 */
	tmp_file_data.fsize = 0;  
	tmp_file_data.dist = pinode_ptr->attr.u.meta.dist;
	tmp_file_data.server_nr = i;
	tmp_file_data.server_ct = pinode_ptr->attr.u.meta.dfile_count;
	tmp_file_data.extend_flag = 1;

	/* if a file datatype offset was specified, go ahead and skip ahead 
	 * before calculating
	 */
	if(file_req_offset)
	{
	    memset(&tmp_result, 0, sizeof(PINT_Request_result));
	    tmp_result.bytemax = file_req_offset;
	    tmp_result.segmax = INT_MAX;
#if 0
	    ret = PINT_Process_request(req_state, &tmp_file_data,
		&segmax, NULL, NULL, &offset, &bytemax,
		&eof_flag, PINT_CKSIZE_LOGICAL_SKIP);
#endif
	    ret = PINT_Process_request(req_state, NULL, &tmp_file_data,
		&tmp_result, PINT_CKSIZE_LOGICAL_SKIP);
	    if(ret < 0)
	    {
		PINT_Free_request_state(req_state);
		return(ret);
	    }
	    if(PINT_REQUEST_STATE_OFFSET(req_state) == -1)
	    {
		/* no data here */
		continue;
	    }
	}

	memset(&tmp_result, 0, sizeof(PINT_Request_result));
	tmp_result.bytemax = 1;
	tmp_result.segmax = 1;
#if 0
	ret = PINT_Process_request(req_state, &tmp_file_data,
	    &segmax, NULL, NULL, &offset, &bytemax, &eof_flag,
	    PINT_CKSIZE);
#endif
	ret = PINT_Process_request(req_state, NULL, &tmp_file_data,
	    &tmp_result, PINT_CKSIZE);
	if(ret < 0)
	{
	    PINT_Free_request_state(req_state);
	    return(ret);
	}

	/* did we find that any data belongs to this handle? */
	if(tmp_result.bytes)
	{
	    target_handle_array[*target_handle_count] =
		pinode_ptr->attr.u.meta.dfile_array[i]; 
	    (*target_handle_count)++;
	}
    }
    PINT_Free_request_state(req_state);

    return(0);
}

/* io_req_ack_flow_array()
 *
 * TODO: use timeouts rather than infinite blocking
 * TODO: try to avoid mallocing so much 
 *
 * Carries out the three steps of sending a request, receiving and
 * acknowledgement, and running a flow for N servers
 *
 * NOTE: this function will skip an indices with a nonzero
 * error_code_array entry; so remember to zero it out before
 * calling if needed.
 *
 * returns 0 on success, -errno on failure
 */
static int io_req_ack_flow_array(bmi_addr_t* addr_array,
    struct PVFS_server_req* req_array,
    bmi_size_t max_resp_size,
    void** resp_encoded_array,
    struct PINT_decoded_msg* resp_decoded_array,
    flow_descriptor** flow_array,
    int* error_code_array,
    int array_size,
    PVFS_msg_tag_t* op_tag_array,
    PVFS_object_attr* attr_p,
    PVFS_pinode_reference pinode_refn, /* FGS replaced PVFS_sys_io request */
    PVFS_Request file_req,
    PVFS_offset file_req_offset,
    void* buffer, 
    PVFS_size buffer_size, 
    PVFS_credentials credentials, /* end changed by frank */
    enum PVFS_io_type type,
    enum PVFS_flowproto_type flow_type)
{
    int i;
    int ret = -1;
    struct PINT_encoded_msg* req_encoded_array = NULL;
    job_id_t* id_array = NULL;
    job_status_s* status_array = NULL;
    int* index_array = NULL;
    int count;
    int need_to_test = 0;
    PINT_Request_file_data* file_data_array = NULL;
    struct PVFS_server_resp* tmp_resp = NULL;
    int recvs_to_complete = 0;
    int flows_to_complete = 0;
    int flows_to_post = 0;
    int* step_array = NULL;
    enum io_step
    {
	STEP_INIT,
	STEP_RECV,
	STEP_DONE,
    };

    /* allocate some bookkeeping fields */
    req_encoded_array = (struct PINT_encoded_msg*)malloc(array_size *
	sizeof(struct PINT_encoded_msg));
    status_array = (job_status_s*)malloc(2 * array_size *
	sizeof(job_status_s));
    index_array = (int*)malloc(2 * array_size * sizeof(int));
    id_array = (job_id_t*)malloc(2 * array_size * sizeof(job_id_t));
    file_data_array = (PINT_Request_file_data*)malloc(
	array_size*sizeof(PINT_Request_file_data));
    step_array = (int*)malloc(array_size * sizeof(int));

    if(!req_encoded_array || !status_array || !id_array ||
	!index_array || !file_data_array || !step_array)
    {
	ret = -ENOMEM;
	gossip_lerr("Error: out of memory.\n");
	goto array_out;
    }

    /* clear some of the arrays for safety */
    memset(resp_encoded_array, 0, (array_size*sizeof(void*)));
    memset(req_encoded_array, 0, (array_size*sizeof(struct
	PINT_encoded_msg)));
    memset(resp_decoded_array, 0, (array_size*sizeof(struct
	PINT_decoded_msg)));
    for(i=0; i<array_size; i++)
    {
        step_array[i] = STEP_INIT;
    }

    /* encode all of the requests */
    for(i=0; i<array_size; i++)
    {
	if(!(error_code_array[i]))
	{
	    ret = PINT_encode(&(req_array[i]), PINT_ENCODE_REQ,
		&(req_encoded_array[i]), addr_array[i], PINT_CLIENT_ENC_TYPE);
	    if(ret < 0)
	    {
		error_code_array[i] = ret;
	    }
	}
    }

    /* post a bunch of sends */
    /* keep up with job ids, and the number of immediate completions */
    memset(id_array, 0, (2 * array_size * sizeof(job_id_t)));
    for(i=0; i<array_size; i++)
    {
	if(!(error_code_array[i]))
	{
	    ret = job_bmi_send_list(
		req_encoded_array[i].dest, 
		req_encoded_array[i].buffer_list,
		req_encoded_array[i].size_list,
		req_encoded_array[i].list_count,
		req_encoded_array[i].total_size,
		op_tag_array[i],
		req_encoded_array[i].buffer_type,
		1,
		NULL,
		0,
		&(status_array[i]),
		&(id_array[i]),
		PVFS_sys_job_context);
	    if(ret < 0)
	    {
		/* immediate error */
		error_code_array[i] = ret;
		id_array[i] = 0;
	    }
	    else if (ret == 1)
	    {
		/* immediate completion */
		error_code_array[i] = status_array[i].error_code;
		id_array[i] = 0;
	    }
	    else
	    {
		need_to_test++;
	    }
	}
    }

    /* see if anything needs to be tested for completion */
    if(need_to_test)
    {
	count = array_size;
	ret = job_testsome(id_array, &count, index_array, NULL,
	    status_array, -1, PVFS_sys_job_context);
	if(ret < 0)
	{
	    /* TODO: there is no real way cleanup from this right now */
	    gossip_lerr(
		"Error: io_req_ack_flow_array() critical failure.\n");
	    exit(-1);
	}
	    
	/* all sends are complete now, fill in error codes */
	for(i=0; i<count; i++)
	{
	    error_code_array[index_array[i]] =
		status_array[i].error_code;
	}
    }

    /* release request encodings */
    for(i=0; i<array_size; i++)
    {
	if(req_encoded_array[i].total_size)
	{
	    PINT_encode_release(&(req_encoded_array[i]),
		PINT_ENCODE_REQ);
	}
    }

    /* allocate room for responses */
    for(i=0; i<array_size; i++)
    {
	if(!(error_code_array[i]))
	{
	    resp_encoded_array[i] = BMI_memalloc(addr_array[i], max_resp_size,
		BMI_RECV);
	    if(!resp_encoded_array[i])
	    {
		error_code_array[i] = -ENOMEM;
	    }
	}
    }

    /*****************************************************/

    /* post a bunch of receives */
    /* keep up with job ids and the number of immediate completions */
    memset(id_array, 0, (2 * array_size * sizeof(job_id_t)));
    need_to_test = 0;
    for(i=0; i<array_size; i++)
    {
	/* skip servers that have already experienced communication
	 * failure 
	 */
	if(!(error_code_array[i]))
	{
	    ret = job_bmi_recv(
		addr_array[i],
		resp_encoded_array[i], 
		max_resp_size, 
		op_tag_array[i],
		BMI_PRE_ALLOC,
		NULL, 
		0,
		&(status_array[i]), 
		&(id_array[i]),
		PVFS_sys_job_context);
	    if(ret < 0)
	    {
		/* immediate error */
		error_code_array[i] = ret;
		id_array[i] = 0;
	    }
	    else if (ret == 1)
	    {
		/* immediate completion */
		error_code_array[i] = status_array[i].error_code;
		id_array[i] = 0;
		step_array[i] = STEP_RECV;
	    }
	    else
	    {
		need_to_test++;
	    }
	}
    }

    recvs_to_complete = need_to_test;
    flows_to_complete = 0;
    flows_to_post = 0;
    for(i=0; i<array_size; i++)
    {
	if(step_array[i] == STEP_RECV && !error_code_array[i])
	    flows_to_post++;
    }

    while(recvs_to_complete || flows_to_post || flows_to_complete)
    {
	/* post any flows that we can */
	for(i=0; i<array_size; i++)
	{
	    if(step_array[i] == STEP_RECV && !error_code_array[i])
	    {
		flows_to_post--;

		ret = PINT_decode(resp_encoded_array[i], PINT_DECODE_RESP,
		    &(resp_decoded_array[i]), addr_array[i],
		    PINT_encode_calc_max_size(PINT_ENCODE_RESP, PVFS_SERV_IO,
			PINT_CLIENT_ENC_TYPE));
		if(ret < 0)
		{
		    error_code_array[i] = ret;
		    continue;
		}

		tmp_resp = (struct
		    PVFS_server_resp*)resp_decoded_array[i].buffer;
		if(tmp_resp->status)
		{
		    /* don't post a flow; we got a negative ack */
		    error_code_array[i] = tmp_resp->status;
		    continue;
		}

		flow_array[i] = PINT_flow_alloc();
		if(!flow_array[i])
		{
		    /* don't post a flow; we are out of memory */
		    error_code_array[i] = -ENOMEM;
		    continue;
		}

		/* setup a flow */
		flow_array[i]->file_data = &(file_data_array[i]);
		flow_array[i]->file_data->fsize =
		    tmp_resp->u.io.bstream_size;
		flow_array[i]->file_data->dist =
		    attr_p->u.meta.dist;
		flow_array[i]->file_data->server_nr = req_array[i].u.io.server_nr;
		flow_array[i]->file_data->server_ct =
		    attr_p->u.meta.dfile_count;
		flow_array[i]->file_req = file_req;
		flow_array[i]->file_req_offset = file_req_offset;
		flow_array[i]->mem_req = NULL;
		flow_array[i]->flags = 0;
		flow_array[i]->tag = op_tag_array[i];
		flow_array[i]->user_ptr = NULL;
		flow_array[i]->type = flow_type;

		if(type == PVFS_IO_READ)
		{
		    flow_array[i]->file_data->extend_flag = 0;
		    flow_array[i]->src.endpoint_id = BMI_ENDPOINT;
		    flow_array[i]->src.u.bmi.address = addr_array[i];
		    flow_array[i]->dest.endpoint_id = MEM_ENDPOINT;
		    flow_array[i]->dest.u.mem.size = buffer_size;
		    flow_array[i]->dest.u.mem.buffer = buffer;
		}
		else
		{
		    flow_array[i]->file_data->extend_flag = 1;
		    flow_array[i]->src.endpoint_id = MEM_ENDPOINT;
		    flow_array[i]->src.u.mem.size = buffer_size;
		    flow_array[i]->src.u.mem.buffer = buffer;
		    flow_array[i]->dest.endpoint_id = BMI_ENDPOINT;
		    flow_array[i]->dest.u.bmi.address = addr_array[i];
		}

		ret = job_flow(
		    flow_array[i],
		    NULL,
		    0,
		    &(status_array[0]),
		    &(id_array[i+array_size]),
		    PVFS_sys_job_context);
		if(ret < 0)
		{
		    error_code_array[i] = ret;
		    id_array[i+array_size] = 0;
		}
		else if(ret == 1)
		{
		    error_code_array[i] = status_array[0].error_code;
		    id_array[i+array_size] = 0;
		}
		else
		{
		    flows_to_complete++;
		}
		/* last step, regardless of if the flow completed
		 * or not
		 */
		step_array[i] = STEP_DONE;
	    }
	}

	/* test to see what's finished */
	if(recvs_to_complete || flows_to_complete)
	{
	    count = array_size * 2;
	    ret = job_testsome(id_array, &count, index_array, NULL,
		status_array, 1, PVFS_sys_job_context);
	    if(ret < 0)
	    {
		/* TODO: there is no real way cleanup from this right now */
		gossip_lerr(
		    "Error: io_req_ack_flow_array() critical failure.\n");
		exit(-1);
	    }
	    
	    /* handle whatever finished */
	    for(i=0; i<count; i++)
	    {
		/* see if it was a flow */
		if(index_array[i] >= array_size)
		{
		    step_array[index_array[i]-array_size] = STEP_DONE;
		    error_code_array[index_array[i]-array_size] = 
			status_array[i].error_code;
		    id_array[index_array[i]] = 0;
		    flows_to_complete--;
		}
		/* otherwise, a recv completed */
		else
		{
		    step_array[index_array[i]] = STEP_RECV;
		    error_code_array[index_array[i]] = 
			status_array[i].error_code;
		    id_array[index_array[i]] = 0;
		    recvs_to_complete--;
		    if(!status_array[i].error_code)
			flows_to_post++;
		}
	    }
	}
    }

    ret = 0;

array_out:

    if(step_array)
	free(step_array);
    if(req_encoded_array)
	free(req_encoded_array);
    if(status_array)
	free(status_array);
    if(id_array)
	free(id_array);
    if(index_array)
	free(index_array);
    if(file_data_array)
	free(file_data_array);

    return(ret);
}


/* io_release_req_ack_flow_array()
 *
 * releases the resources used by io_req_ack_flow_array()
 *
 * no return value
 */
static void io_release_req_ack_flow_array(bmi_addr_t* addr_array,
    struct PVFS_server_req* req_array,
    bmi_size_t max_resp_size,
    void** resp_encoded_array,
    struct PINT_decoded_msg* resp_decoded_array,
    flow_descriptor** flow_array,
    int* error_code_array,
    int array_size)
{
    int i;

    for(i=0; i<array_size; i++)
    {
	if(resp_decoded_array[i].buffer)
	{
	    PINT_decode_release(&(resp_decoded_array[i]),
		PINT_DECODE_RESP);
	}
	if(resp_encoded_array[i])
	{
	    BMI_memfree(addr_array[i], resp_encoded_array[i],
		max_resp_size, BMI_RECV);
	}
	if(flow_array[i])
	{
	    PINT_flow_free(flow_array[i]);
	}
    }

    return;
}


/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 noexpandtab
 */
