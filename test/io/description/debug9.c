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

int result1 [] = {
	0 , 128,
	384 , 128,
	768 , 128,
	1152 , 128,
	1536 , 128,
	1920 , 128,
	2304 , 128,
	2688 , 128,
	3072 , 128,
	3456 , 128,
	3840 , 128,
	4224 , 128,
	4608 , 128,
	4992 , 128,
	5376 , 128,
	5760 , 128,
   -1
};

int result2 [] = {
	6144 , 128,
	6528 , 128,
	6912 , 128,
	7296 , 128,
	7680 , 128,
	8064 , 128,
	8448 , 128,
	8832 , 128,
	9216 , 128,
	9600 , 128,
	9984 , 128,
	10368 , 128,
	10752 , 128,
	11136 , 128,
	11520 , 128,
	11904 , 128,
   -1
};

int result3 [] = {
	12288 , 128,
	12672 , 128,
	13056 , 128,
	13440 , 128,
	13824 , 128,
	14208 , 128,
	14592 , 128,
	14976 , 128,
	15360 , 128,
	15744 , 128,
	16128 , 128,
	16512 , 128,
	16896 , 128,
	17280 , 128,
	17664 , 128,
	18048 , 128,
   -1
};

int result4 [] = {
	18432 , 128,
	18816 , 128,
	19200 , 128,
	19584 , 128,
	19968 , 128,
	20352 , 128,
	20736 , 128,
	21120 , 128,
   -1
};

void prtres(int *result)
{
   int *p = result;
   printf("Result should be:\n");
   while (*p != -1)
   {
      printf("\t%d\t%d\n",*p, *(p+1));
      p+=2;
   }
}


int main(int argc, char **argv)
{
	int i;
	PINT_Request *r1;
	PINT_Request *r2;
	PINT_Request_state *rs1;
	PINT_Request_state *rs2;
	PINT_Request_file_data rf1;
	PINT_Request_result seg1;

	/* PVFS_Process_request arguments */
	int retval;

	/* set up request */
	PVFS_Request_vector(20, 1024, 20*1024, PVFS_BYTE, &r1);

	/* set up request state */
	rs1 = PINT_New_request_state(r1);

	/* set up memory request */
	PVFS_Request_vector(160, 128, 3*128, PVFS_BYTE, &r2);
	rs2 = PINT_New_request_state(r2);

	/* set up file data for request */
	PINT_dist_initialize();
	rf1.server_nr = 0;
	rf1.server_ct = 4;
	rf1.fsize = 10000000;
	rf1.dist = PINT_dist_create("simple_stripe");
	rf1.extend_flag = 1;
	PINT_dist_lookup(rf1.dist);

	/* set up result struct */
	seg1.offset_array = (int64_t *)malloc(SEGMAX * sizeof(int64_t));
	seg1.size_array = (int64_t *)malloc(SEGMAX * sizeof(int64_t));
	seg1.bytemax = BYTEMAX;
	seg1.segmax = SEGMAX;
	seg1.bytes = 0;
	seg1.segs = 0;

   /* Turn on debugging */
	/* gossip_enable_stderr();
	 gossip_set_debug_mask(1,REQUEST_DEBUG); */

	/* skipping logical bytes */
	// PINT_REQUEST_STATE_SET_TARGET(rs1,(3 * 1024) + 512);
	// PINT_REQUEST_STATE_SET_FINAL(rs1,(6 * 1024) + 512);
	
	printf("\n************************************\n");
	printf("One request in CLIENT mode size 20*1K strided 20K server 0 of 4\n");
	printf("Simple stripe, default stripe size (64K)\n");
	printf("Offset 0M, file size 10000000, extend flag\n");
	printf("MemReq size 160*128 strided 3*128\n");
	printf("\n************************************\n");
	do
	{
		seg1.bytes = 0;
		seg1.segs = 0;

		/* process request */
		retval = PINT_Process_request(rs1, rs2, &rf1, &seg1, PINT_CLIENT);

		if(retval >= 0)
		{
			printf("results of PINT_Process_request():\n");
			printf("%d segments with %lld bytes\n", seg1.segs, Ld(seg1.bytes));
			for(i=0; i<seg1.segs; i++)
			{
				printf("  segment %d: offset: %d size: %d\n",
					i, (int)seg1.offset_array[i], (int)seg1.size_array[i]);
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
		prtres(result1);
		prtres(result2);
		prtres(result3);
		prtres(result4);
	}

	return 0;
}
