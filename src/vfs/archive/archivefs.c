/*
   Virtual File System: archives read by libarchive.

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

/** \file
 *  \brief Source: Virtual File System: archives read by libarchive
 *
 * A read-only file system for the archives libarchive can read: zip (and jar, apk, ...), 7z, rar
 * (v4 and v5), cab, lha, iso9660 and others. Unlike the extfs helpers, it needs no external
 * programs, and the names of the files are taken as they are.
 *
 * When the archive is opened, the headers are read to build the tree. The data of the files is
 * read directly from the archive, without temporary files. libarchive reads the archive
 * sequentially, so the reader is kept open: a file after the last one read (as in a copy of a
 * directory) is reached by reading on, an earlier one by reading the archive from its start again
 * (only the headers for a zip file, which has a central directory). Seeking forward in a file
 * skips its data, seeking back reads it from its start again.
 */

#include <config.h>

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <archive.h>
#include <archive_entry.h>

#include "lib/global.h"
#include "lib/util.h"
#include "lib/widget.h"  // message()

#include "lib/vfs/vfs.h"
#include "lib/vfs/utilvfs.h"
#include "lib/vfs/xdirentry.h"
#include "lib/vfs/gc.h"  // vfs_rmstamp

#include "archivefs.h"

/*** global variables ****************************************************************************/

/*** file scope macro definitions ****************************************************************/

#define ARCHIVE_SUPER(super) ((archive_super_t *) (super))

// size of the reads of the archive file
#define ARCHIVE_BUF_SIZE (64 * 1024)

/*** file scope type declarations ****************************************************************/

typedef struct
{
    struct vfs_s_super base;  // base class

    vfs_path_t *vpath;  // the archive file
    struct stat st;     // stat of the archive file

    // the reader: kept open to extract the entries after the last extracted one
    struct archive *a;
    int fd;
    off_t next;         // the index of the next header to be read
    off_t current;      // the index of the entry whose data is read, -1 if none
    off_t current_pos;  // the offset of the next byte of its data
    char *buf;          // ARCHIVE_BUF_SIZE bytes for reading the file (the input of libarchive)
    char *skip_buf;     // ARCHIVE_BUF_SIZE bytes for the data skipped in a file

    // passwords of encrypted files given by the user, tried first for the next files
    GPtrArray *passwords;
    const char *entry_name;  // the name of the file being read, for the password dialog
    gboolean password_asked; // a password has been asked for this file
    gboolean password_cancelled;  // the password dialog has been cancelled: no error message
} archive_super_t;

/*** forward declarations (file scope functions) *************************************************/

/*** file scope variables ************************************************************************/

static struct vfs_s_subclass archive_subclass;
static struct vfs_class *vfs_archive_ops = VFS_CLASS (&archive_subclass);

/* --------------------------------------------------------------------------------------------- */
/*** file scope functions ************************************************************************/
/* --------------------------------------------------------------------------------------------- */
/* The callbacks of libarchive to read the archive file through the VFS: the archive can be in
 * another archive or on a remote host too. */

static la_ssize_t
arcfs_read_cb (struct archive *a, void *data, const void **buffer)
{
    archive_super_t *arch = (archive_super_t *) data;
    ssize_t n;

    n = mc_read (arch->fd, arch->buf, ARCHIVE_BUF_SIZE);
    if (n < 0)
    {
        archive_set_error (a, errno, "%s", unix_error_string (errno));
        return -1;
    }

    *buffer = arch->buf;
    return n;
}

/* --------------------------------------------------------------------------------------------- */

static la_int64_t
arcfs_seek_cb (struct archive *a, void *data, la_int64_t offset, int whence)
{
    archive_super_t *arch = (archive_super_t *) data;
    off_t pos;

    pos = mc_lseek (arch->fd, (off_t) offset, whence);
    if (pos == -1)
    {
        archive_set_error (a, errno, "%s", unix_error_string (errno));
        return ARCHIVE_FATAL;
    }

    return (la_int64_t) pos;
}

/* --------------------------------------------------------------------------------------------- */

static la_int64_t
arcfs_skip_cb (struct archive *a, void *data, la_int64_t request)
{
    archive_super_t *arch = (archive_super_t *) data;

    (void) a;

    // 0: not skipped, libarchive reads the data instead
    if (mc_lseek (arch->fd, (off_t) request, SEEK_CUR) == -1)
        return 0;

    return request;
}

/* --------------------------------------------------------------------------------------------- */
/**
 * Ask the user the password of an encrypted file. libarchive calls this again while the
 * password is wrong, and remembers the given ones for the next encrypted files.
 */

static const char *
arcfs_passphrase_cb (struct archive *a, void *data)
{
    archive_super_t *arch = (archive_super_t *) data;
    char *title;
    char *password;

    (void) a;

    title = g_strdup_printf (arch->password_asked ? _ ("Wrong password, try again: %s")
                                                  : _ ("Password for %s"),
                             arch->entry_name != NULL ? arch->entry_name : arch->base.name);
    password = vfs_get_password (title);
    g_free (title);

    if (password == NULL)
    {
        arch->password_cancelled = TRUE;
        return NULL;
    }

    arch->password_asked = TRUE;

    if (arch->passwords == NULL)
        arch->passwords = g_ptr_array_new_with_free_func (g_free);
    g_ptr_array_add (arch->passwords, password);

    return password;
}

/* --------------------------------------------------------------------------------------------- */

static void
arcfs_reader_close (archive_super_t *arch)
{
    if (arch->a != NULL)
    {
        archive_read_free (arch->a);
        arch->a = NULL;
    }

    if (arch->fd != -1)
    {
        mc_close (arch->fd);
        arch->fd = -1;
    }

    arch->next = 0;
    arch->current = -1;
}

/* --------------------------------------------------------------------------------------------- */
/** Open the archive for reading from its start. */

static gboolean
arcfs_reader_open (archive_super_t *arch)
{
    arcfs_reader_close (arch);

    arch->fd = mc_open (arch->vpath, O_RDONLY);
    if (arch->fd == -1)
    {
        message (D_ERROR, MSG_ERROR, _ ("Cannot open archive\n%s\n%s"), arch->base.name,
                 unix_error_string (errno));
        return FALSE;
    }

    if (arch->buf == NULL)
        arch->buf = g_malloc (ARCHIVE_BUF_SIZE);

    arch->a = archive_read_new ();
    archive_read_support_filter_all (arch->a);
    archive_read_support_format_all (arch->a);
    archive_read_set_read_callback (arch->a, arcfs_read_cb);
    archive_read_set_seek_callback (arch->a, arcfs_seek_cb);
    archive_read_set_skip_callback (arch->a, arcfs_skip_cb);
    archive_read_set_callback_data (arch->a, arch);
    archive_read_set_passphrase_callback (arch->a, arch, arcfs_passphrase_cb);

    // the passwords given before
    if (arch->passwords != NULL)
    {
        guint i;

        for (i = 0; i < arch->passwords->len; i++)
            archive_read_add_passphrase (arch->a, g_ptr_array_index (arch->passwords, i));
    }

    if (archive_read_open1 (arch->a) != ARCHIVE_OK)
    {
        message (D_ERROR, MSG_ERROR, _ ("Cannot open archive\n%s\n%s"), arch->base.name,
                 archive_error_string (arch->a));
        arcfs_reader_close (arch);
        return FALSE;
    }

    return TRUE;
}

/* --------------------------------------------------------------------------------------------- */
/**
 * Read the next header.
 *
 * @return ARCHIVE_OK (also for a warning), ARCHIVE_EOF, or an error
 */

static int
arcfs_reader_next (archive_super_t *arch, struct archive_entry **entry)
{
    int r;

    r = archive_read_next_header (arch->a, entry);
    if (r == ARCHIVE_WARN)
        r = ARCHIVE_OK;
    if (r == ARCHIVE_OK)
        arch->next++;

    return r;
}

/* --------------------------------------------------------------------------------------------- */
/** The path of an entry: without "./" and "/" at the start and "/" at the end. */

static char *
arcfs_entry_path (struct archive_entry *entry)
{
    const char *name;
    char *path, *p;
    size_t len;

    name = archive_entry_pathname (entry);
    if (name == NULL)
        // it can't be converted to the charset of the locale
        name = archive_entry_pathname_utf8 (entry);
    if (name == NULL)
        return NULL;

    while (TRUE)
    {
        if (IS_PATH_SEP (name[0]))
            name++;
        else if (name[0] == '.' && IS_PATH_SEP (name[1]))
            name += 2;
        else
            break;
    }

    path = g_strdup (name);
    for (len = strlen (path); len > 0 && IS_PATH_SEP (path[len - 1]); len--)
        path[len - 1] = '\0';

    // empty components ("a//b")
    for (p = path; (p = strstr (p, PATH_SEP_STR PATH_SEP_STR)) != NULL;)
        memmove (p, p + 1, strlen (p));

    if (path[0] == '\0' || strcmp (path, ".") == 0)
    {
        g_free (path);
        return NULL;
    }

    return path;
}

/* --------------------------------------------------------------------------------------------- */
/** Whether the files of the archive format have an owner. */

static gboolean
arcfs_format_has_owner (int format)
{
    switch (format & ARCHIVE_FORMAT_BASE_MASK)
    {
    case ARCHIVE_FORMAT_CPIO:
    case ARCHIVE_FORMAT_TAR:
    case ARCHIVE_FORMAT_ISO9660:
    case ARCHIVE_FORMAT_AR:
    case ARCHIVE_FORMAT_XAR:
        return TRUE;
    default:
        return FALSE;
    }
}

/* --------------------------------------------------------------------------------------------- */
/** Add an entry of the archive to the tree. */

static void
arcfs_add_entry (struct vfs_class *me, struct vfs_s_super *super, struct archive_entry *entry,
                 off_t index)
{
    archive_super_t *arch = ARCHIVE_SUPER (super);
    struct vfs_s_inode *dir, *inode;
    struct vfs_s_entry *ent;
    struct stat st;
    char *path, *name;
    const char *hardlink;

    path = arcfs_entry_path (entry);
    if (path == NULL)
        return;

    st = *archive_entry_stat (entry);
    if (!arcfs_format_has_owner (archive_format (arch->a)) && st.st_uid == 0 && st.st_gid == 0)
    {
        // as the owner of the archive
        st.st_uid = arch->st.st_uid;
        st.st_gid = arch->st.st_gid;
    }
    if ((st.st_mode & S_IFMT) == 0)
        st.st_mode |= S_IFREG;
    st.st_nlink = 1;

    name = strrchr (path, PATH_SEP);
    if (name == NULL)
    {
        dir = super->root;
        name = path;
    }
    else
    {
        *name = '\0';
        dir = vfs_s_find_inode (me, super, path, LINK_FOLLOW, FL_MKDIR);
        *name = PATH_SEP;
        name++;
    }

    if (dir == NULL)
    {
        g_free (path);
        return;
    }

    ent = VFS_SUBCLASS (me)->find_entry (me, dir, name, LINK_NO_FOLLOW, FL_NONE);
    if (ent != NULL)
    {
        if (S_ISDIR (ent->ino->st.st_mode) && S_ISDIR (st.st_mode))
        {
            // a directory made for its files before its own entry: take its attributes
            ent->ino->st.st_mode = st.st_mode;
            ent->ino->st.st_uid = st.st_uid;
            ent->ino->st.st_gid = st.st_gid;
            vfs_copy_stat_times (&st, &ent->ino->st);
            g_free (path);
            return;
        }

        // a later version of the file: replace the earlier one
        vfs_s_free_entry (me, ent);
    }

    hardlink = archive_entry_hardlink (entry);
    if (hardlink != NULL)
    {
        struct archive_entry *target_entry;
        char *target;

        // the path of the target, cleaned the same way
        target_entry = archive_entry_new ();
        archive_entry_copy_pathname (target_entry, hardlink);
        target = arcfs_entry_path (target_entry);
        archive_entry_free (target_entry);

        inode =
            target == NULL ? NULL : vfs_s_find_inode (me, super, target, LINK_NO_FOLLOW, FL_NONE);
        g_free (target);

        if (inode != NULL && !S_ISDIR (inode->st.st_mode))
        {
            inode->st.st_nlink++;
            ent = vfs_s_new_entry (me, name, inode);
            vfs_s_insert_entry (me, dir, ent);
            g_free (path);
            return;
        }
        // the target is missing: a file of its own, which may have its data here (tar)
    }

    inode = vfs_s_new_inode (me, super, &st);
    inode->data_offset = index;

    if (S_ISLNK (st.st_mode))
    {
        const char *target;

        target = archive_entry_symlink (entry);
        if (target == NULL)
            target = archive_entry_symlink_utf8 (entry);
        inode->linkname = g_strdup (target != NULL ? target : "");
    }

    ent = vfs_s_new_entry (me, name, inode);
    vfs_s_insert_entry (me, dir, ent);

    g_free (path);
}

/* --------------------------------------------------------------------------------------------- */

static struct vfs_s_super *
arcfs_new_archive (struct vfs_class *me)
{
    archive_super_t *arch;

    arch = g_new0 (archive_super_t, 1);
    arch->base.me = me;
    arch->fd = -1;
    arch->current = -1;

    return VFS_SUPER (arch);
}

/* --------------------------------------------------------------------------------------------- */

static void
arcfs_free_archive (struct vfs_class *me, struct vfs_s_super *super)
{
    archive_super_t *arch = ARCHIVE_SUPER (super);

    (void) me;

    arcfs_reader_close (arch);
    vfs_path_free (arch->vpath, TRUE);
    arch->vpath = NULL;
    MC_PTR_FREE (arch->buf);
    MC_PTR_FREE (arch->skip_buf);
    if (arch->passwords != NULL)
    {
        g_ptr_array_free (arch->passwords, TRUE);
        arch->passwords = NULL;
    }
}

/* --------------------------------------------------------------------------------------------- */

static int
arcfs_open_archive (struct vfs_s_super *super, const vfs_path_t *vpath,
                    const vfs_path_element_t *vpath_element)
{
    struct vfs_class *me = vpath_element->class;
    archive_super_t *arch = ARCHIVE_SUPER (super);
    struct vfs_s_inode *root;
    struct stat st;
    int r;

    super->name = g_strdup (vfs_path_as_str (vpath));
    arch->vpath = vfs_path_clone (vpath);

    if (mc_stat (vpath, &arch->st) != 0)
    {
        message (D_ERROR, MSG_ERROR, _ ("Cannot open archive\n%s\n%s"), super->name,
                 unix_error_string (errno));
        return -1;
    }

    if (!arcfs_reader_open (arch))
        return -1;

    st = arch->st;
    st.st_mode = (arch->st.st_mode & 07777) | S_IFDIR;
    st.st_mode |= (st.st_mode & 0444) >> 2;  // set eXec where Read is
    root = vfs_s_new_inode (me, super, &st);
    root->st.st_nlink++;
    root->st.st_dev = VFS_SUBCLASS (me)->rdev++;
    super->root = root;

    while (TRUE)
    {
        struct archive_entry *entry;
        const off_t index = arch->next;

        r = arcfs_reader_next (arch, &entry);
        if (r != ARCHIVE_OK)
            break;

        arcfs_add_entry (me, super, entry, index);
    }

    if (r != ARCHIVE_EOF)
    {
        // show what has been read
        message (D_ERROR, MSG_ERROR, _ ("Error reading archive\n%s\n%s"), super->name,
                 archive_error_string (arch->a));
        if (arch->next == 0)
        {
            arcfs_reader_close (arch);
            return -1;
        }
    }

    // don't keep the file open while only the tree is used
    arcfs_reader_close (arch);

    return 0;
}

/* --------------------------------------------------------------------------------------------- */

static void *
arcfs_super_check (const vfs_path_t *vpath)
{
    static struct stat sb;

    return mc_stat (vpath, &sb) == 0 ? &sb : NULL;
}

/* --------------------------------------------------------------------------------------------- */

static int
arcfs_super_same (const vfs_path_element_t *vpath_element, struct vfs_s_super *parc,
                  const vfs_path_t *vpath, void *cookie)
{
    struct stat *archive_stat = cookie;  // stat of main archive

    (void) vpath_element;

    if (strcmp (parc->name, vfs_path_as_str (vpath)) != 0)
        return 0;

    // Has the cached archive been changed on the disk?
    if (ARCHIVE_SUPER (parc)->st.st_mtime != archive_stat->st_mtime
        || ARCHIVE_SUPER (parc)->st.st_size != archive_stat->st_size)
    {
        // Yes, reload!
        vfs_archive_ops->free ((vfsid) parc);
        vfs_rmstamp (vfs_archive_ops, (vfsid) parc);
        return 2;
    }

    // Hasn't been modified, give it a new timeout
    vfs_stamp (vfs_archive_ops, (vfsid) parc);
    return 1;
}

/* --------------------------------------------------------------------------------------------- */
/**
 * Make the reader ready to read the data of @ino from offset @pos: read the headers up to it
 * (from the start of the archive if it has been passed), and skip the data before @pos.
 */

static gboolean
arcfs_seek_data (struct vfs_class *me, struct vfs_s_super *super, struct vfs_s_inode *ino,
                 off_t pos)
{
    archive_super_t *arch = ARCHIVE_SUPER (super);

    // the data of an encrypted file is decrypted with the first read
    arch->entry_name = ino->ent != NULL ? ino->ent->name : NULL;
    arch->password_asked = FALSE;
    arch->password_cancelled = FALSE;

    if (arch->a == NULL || arch->current != ino->data_offset || arch->current_pos > pos)
    {
        struct archive_entry *entry = NULL;

        arch->current = -1;

        if (arch->a == NULL || ino->data_offset < arch->next)
            if (!arcfs_reader_open (arch))
            {
                me->verrno = EIO;
                return FALSE;
            }

        while (arch->next <= ino->data_offset)
        {
            const int r = arcfs_reader_next (arch, &entry);

            if (r != ARCHIVE_OK)
            {
                message (D_ERROR, MSG_ERROR, _ ("Error reading archive\n%s\n%s"), super->name,
                         r == ARCHIVE_EOF ? _ ("The archive has been changed.")
                                          : archive_error_string (arch->a));
                arcfs_reader_close (arch);
                me->verrno = EIO;
                return FALSE;
            }
        }

        arch->current = ino->data_offset;
        arch->current_pos = 0;
    }

    // skip the data before pos
    while (arch->current_pos < pos)
    {
        const size_t want = (size_t) MIN ((off_t) ARCHIVE_BUF_SIZE, pos - arch->current_pos);
        la_ssize_t n;

        if (arch->skip_buf == NULL)
            arch->skip_buf = g_malloc (ARCHIVE_BUF_SIZE);

        n = archive_read_data (arch->a, arch->skip_buf, want);

        if (n <= 0)
        {
            if (n < 0 && !arch->password_cancelled)
                message (D_ERROR, MSG_ERROR, _ ("Error reading archive\n%s\n%s"), super->name,
                         archive_error_string (arch->a));
            arcfs_reader_close (arch);
            me->verrno = EIO;
            return FALSE;
        }
        arch->current_pos += n;
    }

    return TRUE;
}

/* --------------------------------------------------------------------------------------------- */
/**
 * Read the data of a file directly from the archive. Reading on is fast; a seek back reads the
 * data of the file from its start again.
 */

static ssize_t
arcfs_read (void *fh, char *buffer, size_t count)
{
    vfs_file_handler_t *file = VFS_FILE_HANDLER (fh);
    struct vfs_s_super *super = VFS_FILE_HANDLER_SUPER (fh);
    struct vfs_class *me = super->me;
    archive_super_t *arch = ARCHIVE_SUPER (super);
    la_ssize_t n;

    if (count == 0 || file->pos >= file->ino->st.st_size)
        return 0;

    if (!arcfs_seek_data (me, super, file->ino, file->pos))
        return -1;

    n = archive_read_data (arch->a, buffer, count);
    if (n < 0)
    {
        if (!arch->password_cancelled)
            message (D_ERROR, MSG_ERROR, _ ("Error reading archive\n%s\n%s"), super->name,
                     archive_error_string (arch->a));
        arcfs_reader_close (arch);
        me->verrno = EIO;
        return -1;
    }

    arch->current_pos += n;
    file->pos += n;

    return (ssize_t) n;
}

/* --------------------------------------------------------------------------------------------- */

static int
arcfs_fh_open (struct vfs_class *me, vfs_file_handler_t *fh, int flags, mode_t mode)
{
    (void) fh;
    (void) mode;

    if ((flags & O_ACCMODE) != O_RDONLY)
    {
        me->verrno = EROFS;
        return -1;
    }

    return 0;
}

/* --------------------------------------------------------------------------------------------- */
/*** public functions ****************************************************************************/
/* --------------------------------------------------------------------------------------------- */

void
vfs_init_archivefs (void)
{
    vfs_init_subclass (&archive_subclass, "archivefs", VFSF_READONLY, "uarchive");
    vfs_archive_ops->setctl = NULL;
    vfs_archive_ops->read = arcfs_read;
    archive_subclass.archive_check = arcfs_super_check;
    archive_subclass.archive_same = arcfs_super_same;
    archive_subclass.new_archive = arcfs_new_archive;
    archive_subclass.open_archive = arcfs_open_archive;
    archive_subclass.free_archive = arcfs_free_archive;
    archive_subclass.fh_open = arcfs_fh_open;
    vfs_register_class (vfs_archive_ops);
}

/* --------------------------------------------------------------------------------------------- */
