# GNU Midnight Commander — gereoffy's fork

This is a fork of [GNU Midnight Commander](https://github.com/MidnightCommander/mc).
It follows the upstream `master` branch closely and adds the features and fixes
listed below. Most fixes are also offered upstream as pull requests; the fork
carries them until they are merged there.

For the description of Midnight Commander itself see [README](README) and the
[upstream site](https://midnight-commander.org/).

A source archive of the current `master`, which builds with `./configure && make`
(no autotools needed), is published for every push:
[mc-latest.tar.gz](https://github.com/gereoffy/mc/releases/latest/download/mc-latest.tar.gz).

## Only in this fork

### Archives

* **Archive file system based on libarchive** (`uarchive://`, `--enable-vfs-archive`):
  zip, 7z, rar, cab, lha, iso9660 and the compressed tar archives are opened with
  it (any other format libarchive reads can be entered as `file/uarchive://`),
  read directly, without unpacking them to a temporary directory.
  * Encrypted archives: the password is asked for (and asked again if it is wrong).
  * Entries libarchive cannot read are left out instead of failing the whole archive.
  * The start of the last read file is kept in memory, so viewing a file in a
    compressed archive again doesn't decompress the archive again.
  * Compressed tar archives (`.tar.gz`, `.tar.xz`, `.tar.bz2`, …) are opened with
    it too, so a big backup can be browsed without filling `/tmp`.
* **Copying out of an archive in the order the files are stored**: a solid or
  compressed archive is read in one pass instead of from its start again for
  every file (200 marked files of a `.tar.xz`: 45 s → 0.8 s).
* **Read-only btrfs image file system** (`ubtrfs://`, `--enable-vfs-btrfs`).

### Shell link (`sh://`)

* **Copying many files with one command**: the files are sent one after the
  other by a single remote `perl`, instead of one command and one round trip per
  file (10,000 small files: 7–8 minutes → 8 seconds). Needs perl on the host.
* **Uploading without a local temporary copy**: a written file is sent in 4 MiB
  pieces while it is written; Skip and Abort stop the upload.
* Error handling of the remote send, append and unlink scripts, and fixes of the
  `dd` based scripts (upstream PRs #5157, #5159 and fixes on top of them).

### SFTP and FTP

* sftp: directory cache like the other network file systems (upstream PR #5180),
  default key files and rsa-sha2 host keys, no failure on a write not yet
  acknowledged.
* ftp: reconnect after the idle timeout of the server, remembering the password.

### Viewer (mcview)

* **Filter (F6)**: show only the lines containing a string; Shift-F6 turns it off.
* **Follow mode (like `tail -f`)**: at the end of a growing file the view
  follows the new data.
* Fast search for plain strings, also backwards and in data from a pipe, with
  progress when the size is unknown (upstream PRs #5205, #5206, #5215).
* "Ignore accents" option for normal searches (upstream PR #5216).
* The status line shows the first and the last offset on the screen; F7 at the
  start of a file searches forward.

### Diff viewer

* Lines with UTF-8 text are drawn in screen columns: no leftovers of the panels at
  the right edge, tab stops and horizontal scrolling line up after accented and
  wide (CJK) characters (upstream PR #5220).

### File manager

* `cd` on the command line expands shell wildcards: `cd fo*`, `cd s*/i*2`
  (upstream PR #5210).
* The size of the current file is shown above the mini status line; the mini
  status has its own time format (`timeformat_ministatus`).
* The hint bar shows no random hints.
* ESC + Shift + digit means F11..F20 (for terminals without Shift-F keys).
* "Ignore all" on read errors doesn't freeze the copy (upstream PR #5170).

### Editor

* Fixes of upstream PRs #5070 (mcedit hanging on a macro) and #5071 (block file
  change detection).
