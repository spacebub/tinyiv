FROM debian:13

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl git file patchelf desktop-file-utils \
        g++-14 cmake ninja-build pkg-config \
        libvips-dev libjpeg-dev libpng-dev libwebp-dev libjxl-dev libtiff-dev libheif-dev \
        zlib1g-dev libzstd-dev \
        libwayland-dev wayland-protocols libxkbcommon-dev libdecor-0-dev \
        libx11-dev libxext-dev libxcursor-dev libxfixes-dev libxi-dev libxrandr-dev libxss-dev libxtst-dev \
        libudev-dev libdbus-1-dev \
        libegl-dev libgl-dev libvulkan-dev libdrm-dev libgbm-dev \
    && rm -rf /var/lib/apt/lists/*

ENV CC=gcc-14 CXX=g++-14

ARG LINUXDEPLOY=https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20251107-1/linuxdeploy-x86_64.AppImage
ARG LINUXDEPLOY_SHA256=c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d
ARG APPIMAGETOOL=https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage
ARG APPIMAGETOOL_SHA256=ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0
ARG RUNTIME=https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64
ARG RUNTIME_SHA256=2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d

# appimagetool would download the latest runtime on every pack, unchecked.
RUN set -eux; \
    curl -fsSL -o /opt/runtime-x86_64 "$RUNTIME"; \
    echo "$RUNTIME_SHA256  /opt/runtime-x86_64" | sha256sum -c -

# The AppImages are unpacked, as the container has no FUSE.
RUN set -eux; \
    fetch() { curl -fsSL -o /tmp/tool.AppImage "$1"; echo "$2  /tmp/tool.AppImage" | sha256sum -c -; \
              chmod +x /tmp/tool.AppImage; (cd /tmp && ./tool.AppImage --appimage-extract >/dev/null); \
              mv /tmp/squashfs-root "$3"; rm /tmp/tool.AppImage; }; \
    fetch "$LINUXDEPLOY" "$LINUXDEPLOY_SHA256" /opt/linuxdeploy; \
    fetch "$APPIMAGETOOL" "$APPIMAGETOOL_SHA256" /opt/appimagetool; \
    ln -s /opt/linuxdeploy/AppRun /usr/local/bin/linuxdeploy; \
    ln -s /opt/appimagetool/AppRun /usr/local/bin/appimagetool
