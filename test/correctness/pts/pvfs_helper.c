#include "pvfs_helper.h"

pvfs_helper_t pvfs_helper;

char *pvfs_test_files[NUM_TEST_FILES] =
{
    "/__test_pvfs_file_01",
    "/__test_pvfs_file_02",
    "/__test_pvfs_file_03",
    "/__test_pvfs_file_04",
    "/__test_pvfs_file_05"
};

extern int parse_pvfstab(char *fn,  pvfs_mntlist *mnt);

int initialize_sysint()
{
    int ret = -1;

    memset(&pvfs_helper,0,sizeof(pvfs_helper));

    ret = parse_pvfstab(NULL,&pvfs_helper.mnt);
    if (ret > -1)
    {
        /* init the system interface */
        ret = PVFS_sys_initialize(pvfs_helper.mnt,
                                  &pvfs_helper.resp_init);
        if(ret > -1)
        {
            pvfs_helper.initialized = 1;
            ret = 0;
        }
        else
        {
            fprintf(stderr, "Error: PVFS_sys_initialize() "
                    "failure. = %d\n", ret);
        }
    }
    else
    {
        fprintf(stderr, "Error: parse_pvfstab() failure.\n");
    }
    return ret;
}
