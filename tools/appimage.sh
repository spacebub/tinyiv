#!/usr/bin/env bash
# Usage: tools/appimage.sh
# Builds a release in a Debian 13 container and packs it as an AppImage in build-appimage/.
# Needs Docker. The compile flags are the project defaults.
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
IMAGE=tinyiv-appimage
TREE=$ROOT/build-appimage

if [ "${1:-}" = "--inside" ]; then
    cmake -S /src -B /work/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DTIV_RELEASE=ON \
        -DCMAKE_INSTALL_PREFIX=/usr -DTIV_DOWNLOAD_CACHE=/work/download-cache
    cmake --build /work/build

    rm -rf /work/AppDir
    DESTDIR=/work/AppDir cmake --install /work/build

    # libheif opens its HEIC and AVIF decoders as plugins at run time, which linuxdeploy cannot see.
    PLUGINS=/work/AppDir/usr/lib/libheif/plugins
    mkdir -p "$PLUGINS"
    cp -L /usr/lib/x86_64-linux-gnu/libheif/plugins/libheif-libde265.so \
        /usr/lib/x86_64-linux-gnu/libheif/plugins/libheif-dav1d.so "$PLUGINS/"

    linuxdeploy --appdir /work/AppDir --executable /work/AppDir/usr/bin/tinyiv --deploy-deps-only "$PLUGINS" \
        --desktop-file /work/AppDir/usr/share/applications/tinyiv.desktop \
        --icon-file /src/assets/tinyiv-256.png --icon-filename tinyiv \
        --custom-apprun /src/packaging/linux/AppRun

    # dlopen ignores the runpath of the binary, so each plugin needs its own to find the bundled libraries.
    patchelf --set-rpath '$ORIGIN/../..' "$PLUGINS"/*.so

    # linuxdeploy leaves the compiler runtime out, but the binary needs the one of gcc 14. AppRun
    # uses it only when the host libstdc++ lacks a version named in needed.
    COMPAT=/work/AppDir/usr/lib/compat
    mkdir -p "$COMPAT"
    cp -L /usr/lib/x86_64-linux-gnu/libstdc++.so.6 /usr/lib/x86_64-linux-gnu/libgcc_s.so.1 "$COMPAT/"
    objdump -T /work/AppDir/usr/bin/tinyiv /work/AppDir/usr/lib/*.so* "$PLUGINS"/*.so \
        | grep -o 'GLIBCXX_[0-9.]*' | sort -uV | tail -n 1 > "$COMPAT/needed"

    ARCH=x86_64 appimagetool --runtime-file /opt/runtime-x86_64 /work/AppDir "/work/tinyiv-$TIV_VERSION-x86_64.AppImage"
    exit 0
fi

command -v docker >/dev/null || { echo "Docker is needed" >&2; exit 1; }

VERSION=$(sed -n 's/^[[:space:]]*VERSION \([0-9]*\.[0-9]*\.[0-9]*\)$/\1/p' "$ROOT/CMakeLists.txt" | head -n 1)

mkdir -p "$TREE"
docker build -q --platform linux/amd64 -t "$IMAGE" "$ROOT/packaging/linux" >/dev/null
docker run --rm --platform linux/amd64 --user "$(id -u):$(id -g)" -e HOME=/tmp -e TIV_VERSION="$VERSION" -v "$ROOT:/src:ro" -v "$TREE:/work" \
    "$IMAGE" /src/tools/appimage.sh --inside
ls -l "$TREE"/*.AppImage
