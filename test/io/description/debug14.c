/*
 * (C) 2002 Clemson University.
 *
 * See COPYING in top-level directory.
 */       

#include <stdlib.h>
#include <stdio.h>
#include <pvfs2-types.h>
#include <gossip.h>
#include <pvfs2-debug.h>

#include <pint-distribution.h>
#include <pint-dist-utils.h>
#include <pvfs2-request.h>
#include <pint-request.h>

#define SEGMAX 16
#define BYTEMAX (4*1024*1024)

int main(int argc, char **argv)
{
	int i;
	PINT_Request *r1;
	PINT_Request *r2;
	PINT_Request_state *rs1;
	PINT_Request_state *rs2;
	PINT_Request_file_data rf1;
	PINT_Request_result seg1;
	static int32_t blen[2] = {20, 200};
	static PVFS_offset disp[2] = {10, 100};

	/* PVFS_Process_request arguments */
	int retval;

	/* set up request state */
	PVFS_Request_hindexed(2, blen, disp, PVFS_BYTE, &r1);
	rs1 = PINT_New_request_state(r1);

	/* set up memory request */
	PVFS_Request_contiguous(220, PVFS_BYTE, &r2);
	rs2 = PINT_New_request_state(r2);

	/* set up file data for request */
	PINT_dist_initialize();
	rf1.server_nr = 0;
	rf1.server_ct = 4;
	rf1.fsize = 6000;
	rf1.dist = PINT_dist_create("simple_stripe");
	rf1.extend_flag = 0;
	PINT_dist_lookup(rf1.dist);

	/* set up result struct */
	seg1.offset_array = (int64_t *)malloc(SEGMAX * sizeof(int64_t));
	seg1.size_array = (int64_t *)malloc(SEGMAX * sizeof(int64_t));
	seg1.bytemax = BYTEMAX;
	seg1.segmax = SEGMAX;
	seg1.bytes = 0;
	seg1.segs = 0;
	
	/* skip into the file datatype */
	PINT_REQUEST_STATE_SET_TARGET(rs1, 500);
	PINT_REQUEST_STATE_SET_FINAL(rs1,720);

   /* Turn on debugging */
	// gossip_enable_stderr();
	// gossip_set_debug_mask(1,GOSSIP_REQUEST_DEBUG); 

	/* skipping logical bytes */
	// PINT_REQUEST_STATE_SET_TARGET(rs1,(3 * 1024) + 512);
	// PINT_REQUEST_STATE_SET_FINAL(rs1,(6 * 1024) + 512);
	
	printf("\n************************************\n");
	do
	{
		static struct {
			PVFS_offset	o;
			PVFS_size s;
		} expected[]= {
			{0, 220},
		};
		int r = 0;
		PINT_REQUEST_STATE_RESET(rs1);
		PINT_REQUEST_STATE_RESET(rs2);
		seg1.bytes = 0;
		seg1.segs = 0;

		/* process request */
		retval = PINT_Process_request(rs1, rs2, &rf1, &seg1, PINT_CLIENT);

		if(retval >= 0)
		{
			printf("results of PINT_Process_request():\n");
			printf("%d segments with %lld bytes\n", seg1.segs, Ld(seg1.bytes));
			for(i=0; i<seg1.segs; i++, r++)
			{
				printf("  segment %d: offset: %d size: %d\n",
					i, (int)seg1.offset_array[i], (int)seg1.size_array[i]);
				if (seg1.offset_array[i] != expected[r].o ||
						seg1.size_array[i] != expected[r].s)
				{
					printf("\treturned unexpected result:\n");
					printf("\t r: %d offset: %lld size: %lld\n",
							r, expected[r].o, expected[r].s);
				}
			}
		}

	} while(!PINT_REQUEST_DONE(rs1) && retval >= 0);
	
	if(retval < 0)
	{
		fprintf(stderr, "Error: PINT_Process_request() failure.\n");
		return(-1);
	}
	if(PINT_REQUEST_DONE(rs1))
	{
		printf("**** request done.\n");
	}
	
	printf("\n************************************\n");
	do
	{
		static struct {
			PVFS_offset	o;
			PVFS_size s;
		} expected[]= {
			{720, 180},
			{970, 40},
		};
		int r = 0;
		PINT_REQUEST_STATE_RESET(rs1);
		seg1.bytes = 0;
		seg1.segs = 0;

		/* process request */
		retval = PINT_Process_request(rs1, NULL, &rf1, &seg1, PINT_SERVER);

		if(retval >= 0)
		{
			printf("results of PINT_Process_request():\n");
			printf("%d segments with %lld bytes\n", seg1.segs, Ld(seg1.bytes));
			for(i=0; i<seg1.segs; i++, r++)
			{
				printf("  segment %d: offset: %d size: %d\n",
					i, (int)seg1.offset_array[i], (int)seg1.size_array[i]);
				if (seg1.offset_array[i] != expected[r].o ||
						seg1.size_array[i] != expected[r].s)
				{
					printf("\treturned unexpected result:\n");
					printf("\t r: %d offset: %lld size: %lld\n",
							r, expected[r].o, expected[r].s);
				}
			}
		}

	} while(!PINT_REQUEST_DONE(rs1) && retval >= 0);
	
	if(retval < 0)
	{
		fprintf(stderr, "Error: PINT_Process_request() failure.\n");
		return(-1);
	}
	if(PINT_REQUEST_DONE(rs1))
	{
		printf("**** request done.\n");
	}

	return 0;
}
