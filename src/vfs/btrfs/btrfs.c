/*
   Virtual File System: read-only btrfs file system.

   Copyright (C) 2026
   Free Software Foundation, Inc.

   This file is part of the Midnight Commander.

   The Midnight Commander is free software: you can redistribute it
   and/or modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation, either version 3 of the License,
   or (at your option) any later version.

   The Midnight Commander is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * \file
 * \brief Source: Virtual File System: read-only btrfs file system
 *
 * Browses a btrfs image file or block device like an archive:
 * /path/to/image.img/ubtrfs://some/dir
 *
 * The file system itself is read by libbtrfsro
 * (https://github.com/gereoffy/btrfs4mac). Directories are read lazily, when
 * they are entered, so large file systems open quickly.
 */

#include <config.h>

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include <btrfsro.h>

#include "lib/global.h"
#include "lib/unixcompat.h"
#include "lib/util.h"
#include "lib/widget.h"  // message()

#include "lib/vfs/vfs.h"
#include "lib/vfs/utilvfs.h"
#include "lib/vfs/xdirentry.h"
#include "lib/vfs/gc.h"  // vfs_stamp_create

#include "btrfs.h"

/*** global variables ****************************************************************************/

/*** file scope macro definitions ****************************************************************/

#define BTRFS_SUPER(super) ((btrfs_super_t *) (super))
#define BTRFS_DATA(ino)    ((btrfs_inode_data_t *) (ino)->user_data)

/* Symlink nesting limit while resolving paths */
#define BTRFS_MAX_SYMLINKS 32

/*** file scope type declarations ****************************************************************/

typedef struct
{
    struct vfs_s_super base;  // base class

    int fd;          // archive file or device
    gboolean local;  // fd is a real descriptor (pread), otherwise an mc one
    struct stat st;  // stat of the archive, to detect changes
    btrfsro_fs *fs;
} btrfs_super_t;

typedef struct
{
    struct btrfsro_inode inode;
    gboolean loaded;  // directory contents are in the subdir list
} btrfs_inode_data_t;

typedef struct
{
    char *name;
    struct btrfsro_inode inode;
} btrfs_dirent_t;

/*** forward declarations (file scope functions) *************************************************/

/*** file scope variables ************************************************************************/

static struct vfs_s_subclass btrfs_subclass;
static struct vfs_class *vfs_btrfs_ops = VFS_CLASS (&btrfs_subclass);

/* --------------------------------------------------------------------------------------------- */
/*** file scope functions ************************************************************************/
/* --------------------------------------------------------------------------------------------- */

static void
btrfs_log (void *ctx, int level, const char *msg)
{
    (void) ctx;

    // Warnings about the file system itself are not interesting in the panel
    if (level == BTRFSRO_LOG_ERROR)
        vfs_print_message ("btrfs: %s", msg);
}

/* --------------------------------------------------------------------------------------------- */

/* Device read callback of libbtrfsro. The library may call it from several threads: pread() is
 * fine for that, but mc_lseek() + mc_read() on an image inside another VFS are not thread safe.
 * libbtrfsro doesn't start threads itself, so all calls come from mc's thread. */

static int
btrfs_io_read (void *ctx, void *buf, uint64_t offset, size_t len)
{
    btrfs_super_t *arch = BTRFS_SUPER (ctx);
    size_t done = 0;

    while (done < len)
    {
        ssize_t n;

        if (arch->local)
            n = pread (arch->fd, (char *) buf + done, len - done, (off_t) (offset + done));
        else
        {
            const off_t pos = (off_t) (offset + done);

            if (mc_lseek (arch->fd, pos, SEEK_SET) != pos)
                return -EIO;
            n = mc_read (arch->fd, (char *) buf + done, len - done);
        }

        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return errno != 0 ? -errno : -EIO;
        }
        if (n == 0)
            return -EIO;
        done += (size_t) n;
    }
    return 0;
}

/* --------------------------------------------------------------------------------------------- */

static dev_t
btrfs_linux_rdev (uint64_t rdev)
{
    // Linux new_encode_dev() layout
    const unsigned int major = (unsigned int) ((rdev & 0xfff00) >> 8);
    const unsigned int minor = (unsigned int) ((rdev & 0xff) | ((rdev >> 12) & 0xfff00));

    return makedev (major, minor);
}

/* --------------------------------------------------------------------------------------------- */

static struct vfs_s_inode *
btrfs_new_inode (struct vfs_class *me, struct vfs_s_super *super, const struct btrfsro_inode *bi)
{
    btrfs_super_t *arch = BTRFS_SUPER (super);
    struct btrfsro_stat bst;
    struct stat st;
    struct vfs_s_inode *ino;
    btrfs_inode_data_t *data;

    memset (&st, 0, sizeof (st));
    if (btrfsro_stat (arch->fs, bi, &bst) == 0)
    {
        st.st_mode = (mode_t) bst.mode;
        st.st_uid = bst.uid;
        st.st_gid = bst.gid;
        st.st_size = (off_t) bst.size;
        st.st_rdev = btrfs_linux_rdev (bst.rdev);
        st.st_atime = (time_t) bst.atime.sec;
        st.st_mtime = (time_t) bst.mtime.sec;
        st.st_ctime = (time_t) bst.ctime.sec;
#ifdef HAVE_STRUCT_STAT_ST_BLKSIZE
        st.st_blksize = 4096;
#endif
#ifdef HAVE_STRUCT_STAT_ST_BLOCKS
        st.st_blocks = (blkcnt_t) (bst.nbytes / 512);
#endif
    }
    else
    {
        // Show the entry anyway, reading it will report the error
        st.st_mode = (bi->type == BTRFSRO_FT_DIR ? S_IFDIR | 0555 : S_IFREG | 0444);
    }

    ino = vfs_s_new_inode (me, super, &st);
    // Keep the real inode numbers, so hardlinks are recognized
    ino->st.st_ino = (ino_t) (bi->ino ^ ((bi->subvol - 5) << 40));
    ino->st.st_dev = super->root != NULL ? super->root->st.st_dev : ino->st.st_dev;

    data = g_new0 (btrfs_inode_data_t, 1);
    data->inode = *bi;
    ino->user_data = data;

    if (S_ISLNK (ino->st.st_mode))
    {
        char target[MC_MAXPATHLEN];

        if (btrfsro_readlink (arch->fs, bi, target, sizeof (target)) >= 0)
            ino->linkname = g_strdup (target);
    }

    return ino;
}

/* --------------------------------------------------------------------------------------------- */

static int
btrfs_collect_dirent (void *ctx, const char *name, size_t name_len,
                      const struct btrfsro_inode *entry, uint64_t index)
{
    GPtrArray *list = (GPtrArray *) ctx;
    btrfs_dirent_t *de;

    (void) index;

    de = g_new (btrfs_dirent_t, 1);
    de->name = g_strndup (name, name_len);
    de->inode = *entry;
    g_ptr_array_add (list, de);
    return 0;
}

/* --------------------------------------------------------------------------------------------- */

static void
btrfs_free_dirent (gpointer p)
{
    btrfs_dirent_t *de = (btrfs_dirent_t *) p;

    g_free (de->name);
    g_free (de);
}

/* --------------------------------------------------------------------------------------------- */
/** Read the contents of a directory into its subdir list, once. */

static int
btrfs_load_dir (struct vfs_class *me, struct vfs_s_inode *dir)
{
    btrfs_super_t *arch = BTRFS_SUPER (dir->super);
    btrfs_inode_data_t *data = BTRFS_DATA (dir);
    GPtrArray *list;
    guint i;
    int ret;

    if (data == NULL || data->loaded)
        return 0;

    list = g_ptr_array_new_with_free_func (btrfs_free_dirent);
    ret = btrfsro_readdir (arch->fs, &data->inode, 0, btrfs_collect_dirent, list);
    if (ret < 0)
    {
        g_ptr_array_free (list, TRUE);
        me->verrno = -ret;
        return -1;
    }

    for (i = 0; i < list->len; i++)
    {
        const btrfs_dirent_t *de = (const btrfs_dirent_t *) g_ptr_array_index (list, i);
        struct vfs_s_inode *ino;
        struct vfs_s_entry *ent;

        /* st_nlink is the reference count of the inode in the vfs_s layer, so the
           real link count of the file cannot be shown */
        ino = btrfs_new_inode (me, dir->super, &de->inode);
        ent = vfs_s_new_entry (me, de->name, ino);
        vfs_s_insert_entry (me, dir, ent);
    }

    g_ptr_array_free (list, TRUE);
    data->loaded = TRUE;
    return 0;
}

/* --------------------------------------------------------------------------------------------- */
/**
 * Resolve @path relative to directory @dir, loading directories on the way.
 * Symlinks are followed in the middle of the path, and at the end unless
 * @follow is LINK_NO_FOLLOW. Absolute symlink targets are taken relative to
 * the root of the file system.
 */

static struct vfs_s_entry *
btrfs_walk (struct vfs_class *me, struct vfs_s_inode *dir, const char *path, int follow, int flags,
            int depth)
{
    struct vfs_s_inode *root = dir->super->root;
    struct vfs_s_entry *ent = NULL;
    const char *p = path;

    if (depth > BTRFS_MAX_SYMLINKS)
    {
        me->verrno = ELOOP;
        return NULL;
    }

    while (TRUE)
    {
        const char *rest;
        size_t len;
        GList *iter;

        while (IS_PATH_SEP (*p))
            p++;
        if (*p == '\0')
            break;

        for (len = 0; p[len] != '\0' && !IS_PATH_SEP (p[len]); len++)
            ;
        rest = p + len;
        while (IS_PATH_SEP (*rest))
            rest++;

        if (len == 1 && p[0] == '.')
        {
            p = rest;
            continue;
        }
        if (len == 2 && p[0] == '.' && p[1] == '.')
        {
            if (dir->ent != NULL && dir->ent->dir != NULL)
                dir = dir->ent->dir;
            ent = dir->ent;
            p = rest;
            continue;
        }

        if (btrfs_load_dir (me, dir) != 0)
            return NULL;

        for (iter = g_queue_peek_head_link (dir->subdir); iter != NULL; iter = g_list_next (iter))
        {
            ent = VFS_ENTRY (iter->data);
            if (strlen (ent->name) == len && strncmp (ent->name, p, len) == 0)
                break;
        }
        if (iter == NULL)
        {
            me->verrno = ENOENT;
            return NULL;
        }

        if (S_ISLNK (ent->ino->st.st_mode) && ent->ino->linkname != NULL
            && (*rest != '\0' || follow != LINK_NO_FOLLOW))
        {
            const char *target = ent->ino->linkname;

            ent = btrfs_walk (me, IS_PATH_SEP (*target) ? root : dir, target, LINK_FOLLOW, FL_NONE,
                              depth + 1);
            if (ent == NULL)
                return NULL;
        }

        if (*rest != '\0')
        {
            if (!S_ISDIR (ent->ino->st.st_mode))
            {
                me->verrno = ENOTDIR;
                return NULL;
            }
            dir = ent->ino;
        }
        p = rest;
    }

    if (ent != NULL && (flags & FL_DIR) != 0 && S_ISDIR (ent->ino->st.st_mode)
        && btrfs_load_dir (me, ent->ino) != 0)
        return NULL;

    return ent;
}

/* --------------------------------------------------------------------------------------------- */

static struct vfs_s_entry *
btrfs_find_entry (struct vfs_class *me, struct vfs_s_inode *root, const char *a_path, int follow,
                  int flags)
{
    struct vfs_s_entry *ent;
    char *path;

    if ((flags & (FL_MKFILE | FL_MKDIR)) != 0)
    {
        me->verrno = EROFS;
        return NULL;
    }

    path = g_strdup (a_path);
    canonicalize_pathname_custom (path, CANON_PATH_ALL & (~CANON_PATH_REMDOUBLEDOTS));
    ent = btrfs_walk (me, root, path, follow, flags, 0);
    g_free (path);
    return ent;
}

/* --------------------------------------------------------------------------------------------- */

static void
btrfs_free_inode (struct vfs_class *me, struct vfs_s_inode *ino)
{
    (void) me;

    g_free (ino->user_data);
    ino->user_data = NULL;
}

/* --------------------------------------------------------------------------------------------- */

static struct vfs_s_super *
btrfs_new_archive (struct vfs_class *me)
{
    btrfs_super_t *arch;

    arch = g_new0 (btrfs_super_t, 1);
    arch->base.me = me;
    arch->fd = -1;

    return VFS_SUPER (arch);
}

/* --------------------------------------------------------------------------------------------- */

static void
btrfs_free_archive (struct vfs_class *me, struct vfs_s_super *archive)
{
    btrfs_super_t *arch = BTRFS_SUPER (archive);

    (void) me;

    if (arch->fs != NULL)
    {
        btrfsro_unmount (arch->fs);
        arch->fs = NULL;
    }
    if (arch->fd != -1)
    {
        if (arch->local)
            close (arch->fd);
        else
            mc_close (arch->fd);
        arch->fd = -1;
    }
}

/* --------------------------------------------------------------------------------------------- */

static int
btrfs_open_archive (struct vfs_s_super *archive, const vfs_path_t *vpath,
                    const vfs_path_element_t *vpath_element)
{
    struct vfs_class *me = vpath_element->class;
    btrfs_super_t *arch = BTRFS_SUPER (archive);
    struct btrfsro_io io;
    struct btrfsro_inode root_inode;
    struct vfs_s_inode *root;
    int ret;

    if (mc_stat (vpath, &arch->st) != 0)
    {
        message (D_ERROR, MSG_ERROR, _ ("Cannot open btrfs file system\n%s"),
                 vfs_path_as_str (vpath));
        ERRNOR (ENOENT, -1);
    }

    arch->local = vfs_file_is_local (vpath);
    if (arch->local)
        arch->fd = open (vfs_path_get_last_path_str (vpath), O_RDONLY);
    else
        arch->fd = mc_open (vpath, O_RDONLY);
    if (arch->fd == -1)
    {
        const int err = errno;

        message (D_ERROR, MSG_ERROR, _ ("Cannot open btrfs file system\n%s\n%s"),
                 vfs_path_as_str (vpath), unix_error_string (err));
        ERRNOR (err, -1);
    }

    memset (&io, 0, sizeof (io));
    io.read = btrfs_io_read;
    io.ctx = arch;
    // Devices report no size; reads past their end fail by themselves
    io.size = S_ISREG (arch->st.st_mode) ? (uint64_t) arch->st.st_size : 0;

    ret = btrfsro_mount (&io, &arch->fs);
    if (ret < 0)
    {
        message (D_ERROR, MSG_ERROR, _ ("Cannot open btrfs file system\n%s\n%s"),
                 vfs_path_as_str (vpath), unix_error_string (-ret));
        ERRNOR (-ret, -1);
    }

    archive->name = g_strdup (vfs_path_as_str (vpath));

    btrfsro_root (arch->fs, &root_inode);
    root = btrfs_new_inode (me, archive, &root_inode);
    root->st.st_nlink++;
    root->st.st_dev = VFS_SUBCLASS (me)->rdev++;
    archive->root = root;

    // The root is listed through the root inode directly, load it now
    if (btrfs_load_dir (me, root) != 0)
    {
        message (D_ERROR, MSG_ERROR, _ ("Cannot read btrfs root directory\n%s"),
                 vfs_path_as_str (vpath));
        return -1;
    }

    return 0;
}

/* --------------------------------------------------------------------------------------------- */

static void *
btrfs_super_check (const vfs_path_t *vpath)
{
    static struct stat stat_buf;

    return (mc_stat (vpath, &stat_buf) != 0) ? NULL : &stat_buf;
}

/* --------------------------------------------------------------------------------------------- */

static int
btrfs_super_same (const vfs_path_element_t *vpath_element, struct vfs_s_super *parc,
                  const vfs_path_t *vpath, void *cookie)
{
    const struct stat *archive_stat = (const struct stat *) cookie;

    (void) vpath_element;

    if (strcmp (parc->name, vfs_path_as_str (vpath)) != 0)
        return 0;

    // Has the image been changed on the disk?
    if (S_ISREG (archive_stat->st_mode) && BTRFS_SUPER (parc)->st.st_mtime < archive_stat->st_mtime)
    {
        vfs_btrfs_ops->free ((vfsid) parc);
        vfs_rmstamp (vfs_btrfs_ops, (vfsid) parc);
        return 2;
    }

    vfs_stamp (vfs_btrfs_ops, (vfsid) parc);
    return 1;
}

/* --------------------------------------------------------------------------------------------- */

static ssize_t
btrfs_read (void *fh, char *buffer, size_t count)
{
    vfs_file_handler_t *file = VFS_FILE_HANDLER (fh);
    struct vfs_s_super *super = VFS_FILE_HANDLER_SUPER (fh);
    btrfs_inode_data_t *data = BTRFS_DATA (file->ino);
    ssize_t res;

    res = btrfsro_read (BTRFS_SUPER (super)->fs, &data->inode, (uint64_t) file->pos, buffer, count);
    if (res < 0)
    {
        super->me->verrno = (int) -res;
        return -1;
    }

    file->pos += res;
    return res;
}

/* --------------------------------------------------------------------------------------------- */

static int
btrfs_fh_open (struct vfs_class *me, vfs_file_handler_t *fh, int flags, mode_t mode)
{
    (void) fh;
    (void) mode;

    if ((flags & O_ACCMODE) != O_RDONLY)
        ERRNOR (EROFS, -1);
    return 0;
}

/* --------------------------------------------------------------------------------------------- */
/*** public functions ****************************************************************************/
/* --------------------------------------------------------------------------------------------- */

void
vfs_init_btrfs (void)
{
    btrfsro_set_log_handler (btrfs_log, NULL, BTRFSRO_LOG_ERROR);

    vfs_init_subclass (&btrfs_subclass, "btrfs", VFSF_READONLY, "ubtrfs");
    vfs_btrfs_ops->read = btrfs_read;
    vfs_btrfs_ops->setctl = NULL;
    btrfs_subclass.find_entry = btrfs_find_entry;
    btrfs_subclass.archive_check = btrfs_super_check;
    btrfs_subclass.archive_same = btrfs_super_same;
    btrfs_subclass.new_archive = btrfs_new_archive;
    btrfs_subclass.open_archive = btrfs_open_archive;
    btrfs_subclass.free_archive = btrfs_free_archive;
    btrfs_subclass.free_inode = btrfs_free_inode;
    btrfs_subclass.fh_open = btrfs_fh_open;
    vfs_register_class (vfs_btrfs_ops);
}

/* --------------------------------------------------------------------------------------------- */
