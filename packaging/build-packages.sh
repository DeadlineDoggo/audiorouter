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

if command -v nfpm >/dev/null 2>&1; then
    nfpm package --config packaging/nfpm.yaml --packager "$FORMAT" --target "$OUT/"
elif command -v docker >/dev/null 2>&1; then
    docker run --rm -v "$PWD:/w" -w /w -e NF_VERSION \
        goreleaser/nfpm:v2.48.0 package --config packaging/nfpm.yaml --packager "$FORMAT" --target "$OUT/"
else
    echo "need nfpm or docker (https://nfpm.goreleaser.com)" >&2
    exit 1
fi
ls -l "$OUT"
