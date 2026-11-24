#!/bin/sh
# build-packages.sh - builds one package from an installed tree.
#
#   sh packaging/build-packages.sh VERSION FORMAT STAGE.tar.gz [OUTDIR]
#       VERSION  x.y.z or x.y.z-beta
#       FORMAT   deb | rpm | archlinux
#       STAGE    tarball of the tree installed with DESTDIR (usr/lib/..., usr/share/...)
#       OUTDIR   where the package goes (default: dist)
#
# Uses `nfpm` if it is installed, otherwise its official Docker image.
set -eu
cd "$(dirname "$0")/.."

VERSION=${1:?version} FORMAT=${2:?format deb|rpm|archlinux} STAGE=${3:?stage tarball}
OUT=${4:-dist}
case $FORMAT in deb|rpm|archlinux) ;; *) echo "unknown format: $FORMAT" >&2; exit 1 ;; esac

rm -rf build/pkgroot
mkdir -p build/pkgroot "$OUT"
tar -xzf "$STAGE" -C build/pkgroot
export NF_VERSION="$VERSION"

# The contents list: SoundRoot's own folders (so removing the package removes
# them), then every file. System folders are left to the distribution.
CONFIG=build/nfpm.yaml
{
    cat packaging/nfpm.yaml
    echo "contents:"
    (cd build/pkgroot && find . -type d -name '*soundroot*' | sort) | sed 's|^\./||' | while read -r d; do
        (cd build/pkgroot && find "$d" -type d | sort)
    done | while read -r d; do
        printf '  - dst: /%s\n    type: dir\n    file_info:\n      mode: 0755\n' "$d"
    done
    (cd build/pkgroot && find . \( -type f -o -type l \) | sort) | sed 's|^\./||' | while read -r f; do
        case $f in *.so) mode=0755 ;; *) mode=0644 ;; esac   # not the modes of the build machine
        printf '  - src: build/pkgroot/%s\n    dst: /%s\n    file_info:\n      mode: %s\n' "$f" "$f" "$mode"
    done
} > "$CONFIG"
grep -q 'src: build/pkgroot/.*org.kde.plasma.soundroot.so' "$CONFIG" ||
    { echo "the plugin is not in $STAGE" >&2; exit 1; }

if command -v nfpm >/dev/null 2>&1; then
    nfpm package --config "$CONFIG" --packager "$FORMAT" --target "$OUT/"
elif command -v docker >/dev/null 2>&1; then
    docker run --rm -v "$PWD:/w" -w /w -e NF_VERSION \
        goreleaser/nfpm:v2.48.0 package --config "$CONFIG" --packager "$FORMAT" --target "$OUT/"
else
    echo "need nfpm or docker (https://nfpm.goreleaser.com)" >&2
    exit 1
fi
ls -l "$OUT"
