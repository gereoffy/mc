dnl Archives read by libarchive (zip, 7z, rar, cab, lha, iso9660, compressed tar, ...)
AC_DEFUN([mc_VFS_ARCHIVEFS],
[
    AC_ARG_ENABLE([vfs-archive],
                  AS_HELP_STRING([--enable-vfs-archive], [Support for archives read by libarchive [auto]]))

    dnl the VFS of these archives in mc.ext.ini: the extfs helpers without libarchive
    VFS_ZIP=uzip
    VFS_7Z=u7z
    VFS_RAR=urar
    VFS_CAB=ucab
    VFS_LHA=ulha
    VFS_ISO9660=iso9660
    dnl compressed tar: libarchive decompresses it while reading, tarfs to a temporary file
    VFS_TARC=utar

    if test "$enable_vfs" != "no" -a x"$enable_vfs_archive" != x"no"; then
        PKG_CHECK_MODULES(LIBARCHIVE, [libarchive >= 3.2], [found_libarchive=yes], [:])
        if test x"$found_libarchive" = "xyes"; then
            mc_VFS_ADDNAME([archive])
            AC_DEFINE([ENABLE_VFS_ARCHIVE], [1], [Support for archives read by libarchive])
            MCLIBS="$MCLIBS $LIBARCHIVE_LIBS"
            enable_vfs_archive="yes"
            VFS_ZIP=uarchive
            VFS_7Z=uarchive
            VFS_RAR=uarchive
            VFS_CAB=uarchive
            VFS_LHA=uarchive
            VFS_ISO9660=uarchive
            VFS_TARC=uarchive
        else
            if test x"$enable_vfs_archive" = x"yes"; then
                dnl user explicitly requested feature
                AC_MSG_ERROR([libarchive not found])
            fi
            enable_vfs_archive="no"
        fi
    fi

    AC_SUBST(VFS_ZIP)
    AC_SUBST(VFS_7Z)
    AC_SUBST(VFS_RAR)
    AC_SUBST(VFS_CAB)
    AC_SUBST(VFS_LHA)
    AC_SUBST(VFS_ISO9660)
    AC_SUBST(VFS_TARC)

    AM_CONDITIONAL([ENABLE_VFS_ARCHIVE], [test "$enable_vfs" = "yes" -a x"$enable_vfs_archive" = x"yes"])
])
