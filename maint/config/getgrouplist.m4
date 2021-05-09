AC_DEFUN([AX_GETGROUPLIST],
[
	AC_REQUIRE([AC_EGREP_HEADER])
	AC_MSG_CHECKING(for getgrouplist in distro)
	dnl if this test passes, the distro does have it
	dnl if this test fails, the distro does not have it

	AC_EGREP_HEADER(getgrouplist, grp.h, 
	                AC_MSG_RESULT(yes) 
                        AC_DEFINE(HAVE_GETGROUPLIST, 1, Define if distro has getgrouplist), 
		        AC_EGREP_HEADER(getgrouplist, unistd.h, 
		                        AC_MSG_RESULT(yes) 
                                        AC_DEFINE(HAVE_GETGROUPLIST, 1, Define if distro has getgrouplist), 
		                        AC_MSG_RESULT(no)
                        ,) 
	,)


        dnl Does getgrouplist use gid_t or int for the group parameter?

        AC_MSG_CHECKING(is gid_t type used for groups in getgrouplist)
        AC_EGREP_HEADER(getgrouplist.*gid_t.*group, grp.h,
                        AC_MSG_RESULT(yes)
                        AC_DEFINE(HAVE_GETGROUPLIST_GID, 1, Define if getgrouplist() uses gid_t as 3rd param),
                        AC_EGREP_HEADER(getgrouplist.*gid_t.*group, unistd.h,
                                        AC_MSG_RESULT(yes)
                                        AC_DEFINE(HAVE_GETGROUPLIST_GID, 1, Define if getgrouplist() uses gid_t as 3rd param),
                                        AC_MSG_RESULT(no)
                        ,)
        ,)

        dnl Does getgrouplist use int for the group parameter?

	AC_MSG_CHECKING(is int type used for groups in getgrouplist)
        AC_EGREP_HEADER(getgrouplist.*int.\*, grp.h,
                        AC_MSG_RESULT(yes)
                        AC_DEFINE(HAVE_GETGROUPLIST_INT, 1, Define if getgrouplist() uses int as 3rd param),
                        AC_EGREP_HEADER(getgrouplist.*int.\*, unistd.h,
                                        AC_MSG_RESULT(yes)
                                        AC_DEFINE(HAVE_GETGROUPLIST_INT, 1, Define if getgrouplist() uses int as 3rd param),
                                        AC_MSG_RESULT(no)
                        ,)
        ,)
])
