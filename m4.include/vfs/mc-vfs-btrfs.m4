dnl Read-only btrfs filesystem support (libbtrfsro from btrfs4mac)
AC_DEFUN([mc_VFS_BTRFS],
[
    AC_ARG_ENABLE([vfs-btrfs],
                  AS_HELP_STRING([--enable-vfs-btrfs], [Support for btrfs filesystem images [auto]]))
    if test "$enable_vfs" != "no" -a x"$enable_vfs_btrfs" != x"no"; then
        PKG_CHECK_MODULES(BTRFSRO, [btrfsro], [found_btrfsro=yes], [:])
        if test x"$found_btrfsro" = "xyes"; then
            mc_VFS_ADDNAME([btrfs])
            AC_DEFINE([ENABLE_VFS_BTRFS], [1], [Support for btrfs filesystem])
            MCLIBS="$MCLIBS $BTRFSRO_LIBS"
            enable_vfs_btrfs="yes"
        else
            if test x"$enable_vfs_btrfs" = x"yes"; then
                dnl user explicitly requested feature
                AC_MSG_ERROR([btrfsro library not found])
            fi
            enable_vfs_btrfs="no"
        fi
    fi
    AM_CONDITIONAL([ENABLE_VFS_BTRFS], [test "$enable_vfs" = "yes" -a x"$enable_vfs_btrfs" = x"yes"])
])
