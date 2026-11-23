#!/bin/sh
# Build a Debian/Ubuntu package of mc from a distribution archive (made by "make dist-gzip"),
# with libarchive built in statically (a newer one than the distribution's, with 7-Zip AES).
#
# usage: build-deb.sh mc-VERSION.tar.gz OUTDIR [WORKDIR]
#
# Installs the build dependencies with apt-get (as root, or with sudo). The package replaces
# the mc and mc-data packages of the distribution. Used by .github/workflows/fork-dist.yml.

set -eu

LIBARCHIVE_VERSION=3.8.11
LIBARCHIVE_SHA256=900fe9fc7237c2b04f090f3c3648de8fe597525771f5035016537ce2d667d12b

if [ $# -lt 2 ]; then
    echo "usage: $0 mc-VERSION.tar.gz OUTDIR [WORKDIR]" >&2
    exit 1
fi

tarball=$(realpath "$1")
mkdir -p "$2"
outdir=$(realpath "$2")
workdir=${3:-$outdir/build}
patchdir=$(realpath "$(dirname "$0")/../patches")
jobs=$(nproc)

sudo=
[ "$(id -u)" -eq 0 ] || sudo=sudo

rm -rf "$workdir"
mkdir -p "$workdir"
cd "$workdir"

echo "=== build dependencies"
$sudo apt-get update -q
DEBIAN_FRONTEND=noninteractive $sudo apt-get install -y -q --no-install-recommends \
    build-essential pkg-config curl ca-certificates gettext dpkg-dev file \
    libglib2.0-dev libslang2-dev libssh2-1-dev \
    zlib1g-dev libbz2-dev liblzma-dev libzstd-dev liblz4-dev libssl-dev

echo "=== libarchive $LIBARCHIVE_VERSION"
curl -fsSL -o libarchive.tar.gz \
    "https://github.com/libarchive/libarchive/releases/download/v$LIBARCHIVE_VERSION/libarchive-$LIBARCHIVE_VERSION.tar.gz"
echo "$LIBARCHIVE_SHA256  libarchive.tar.gz" | sha256sum -c -
tar xzf libarchive.tar.gz
(
    cd "libarchive-$LIBARCHIVE_VERSION"
    patch -p1 < "$patchdir/libarchive-7zip-aes.patch"
    ./configure --prefix="$workdir/libarchive" --disable-shared --enable-static \
        --disable-bsdtar --disable-bsdcpio --disable-bsdcat --disable-bsdunzip \
        --disable-acl --disable-xattr --without-xml2 --without-expat --without-lzo2 \
        --without-libb2 --without-nettle --without-mbedtls --with-openssl
    make -j"$jobs"
    make install
)
la_pc="$workdir/libarchive/lib/pkgconfig"

echo "=== mc"
tar xzf "$tarball"
srcdir=$(find . -maxdepth 1 -type d -name 'mc-*' | head -n 1)
version=$(sed -n 's/^#define MC_CURRENT_VERSION "\(.*\)"$/\1/p' "$srcdir/mc-version.h")
[ -n "$version" ]
(
    cd "$srcdir"
    ./configure --prefix=/usr --sysconfdir=/etc --libexecdir=/usr/lib \
        --enable-vfs-sftp --enable-vfs-archive --disable-vfs-btrfs \
        --without-gpm-mouse --disable-ext2fs-attr --disable-tests \
        LIBARCHIVE_CFLAGS="-I$workdir/libarchive/include" \
        LIBARCHIVE_LIBS="$(PKG_CONFIG_PATH=$la_pc pkg-config --static --libs libarchive)"
    make -j"$jobs"
    make install-strip DESTDIR="$workdir/root"
)

# no libarchive library may be needed at run time
if ldd "$workdir/root/usr/bin/mc" | grep -q libarchive; then
    echo "mc is linked to a shared libarchive" >&2
    exit 1
fi
TERM=dumb "$workdir/root/usr/bin/mc" -V | grep -q archivefs
TERM=dumb "$workdir/root/usr/bin/mc" -V | grep -q sftpfs

echo "=== package"
# 4.8.33-969-20261122 -> 3:4.8.33+fork969.20261122: above the 3:4.8.x of the distribution
debversion="3:$(echo "$version" | sed 's/-/+fork/; s/-/./g')"
arch=$(dpkg --print-architecture)
deb="mc_$(echo "$debversion" | sed 's/^[0-9]*://')_$arch.deb"

mkdir -p "$workdir/debian" "$workdir/root/DEBIAN"
printf 'Source: mc\n\nPackage: mc\nArchitecture: any\n' > "$workdir/debian/control"
depends=$(cd "$workdir" && dpkg-shlibdeps -O -e root/usr/bin/mc -e root/usr/lib/mc/cons.saver \
    | sed -n 's/^shlibs:Depends=//p')

{
    echo "Package: mc"
    echo "Version: $debversion"
    echo "Architecture: $arch"
    echo "Maintainer: A'rpi <arpi.git@interoot.hu>"
    echo "Installed-Size: $(du -sk --exclude=DEBIAN "$workdir/root" | cut -f1)"
    echo "Depends: $depends"
    echo "Recommends: mailcap, perl, unzip, sensible-utils"
    echo "Suggests: bzip2, xz-utils, zstd, zip, file, python3"
    echo "Conflicts: mc-data"
    echo "Replaces: mc, mc-data"
    echo "Provides: mc-data"
    echo "Section: utils"
    echo "Priority: optional"
    echo "Homepage: https://github.com/gereoffy/mc"
    echo "Description: Midnight Commander - a powerful file manager (gereoffy's fork)"
    echo " GNU Midnight Commander is a text-mode full-screen file manager."
    echo " ."
    echo " This build of https://github.com/gereoffy/mc ($version) adds an archive"
    echo " file system based on a built-in libarchive $LIBARCHIVE_VERSION (7z, rar,"
    echo " zip with passwords, compressed tar without temporary files), fast copying"
    echo " of many files over the shell link, and the other changes listed in its"
    echo " README.md. It replaces the mc and mc-data packages of the distribution."
} > "$workdir/root/DEBIAN/control"

(cd "$workdir/root" && find etc -type f | sed 's|^|/|' | sort) > "$workdir/root/DEBIAN/conffiles"

# cons.saver (the panels of the Linux console) needs the tty group, as in the mc package
cat > "$workdir/root/DEBIAN/postinst" << 'EOF'
#!/bin/sh
set -e
if [ "$1" = configure ] && [ -f /usr/lib/mc/cons.saver ]; then
    chown root:tty /usr/lib/mc/cons.saver
    chmod 2755 /usr/lib/mc/cons.saver
fi
EOF
chmod 755 "$workdir/root/DEBIAN/postinst"

dpkg-deb --root-owner-group --build "$workdir/root" "$outdir/$deb"
echo "=== $outdir/$deb"
dpkg-deb --info "$outdir/$deb" | sed -n '/Package:/,/Homepage:/p'
