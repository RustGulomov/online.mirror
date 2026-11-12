FROM alpine:3.21

RUN apk add --no-cache \
    build-base autoconf automake libtool pkgconf git wget ccache \
    gperf nasm libxslt flex bison zip rsync curl bash coreutils findutils tar \
    meson ninja perl python3 \
    zlib-dev util-linux-dev \
    libx11-dev libxrender-dev libxext-dev libxrandr-dev libxinerama-dev \
    fontconfig-dev freetype-dev \
    libpng-dev openssl-dev zstd-dev libcap-dev linux-pam-dev cppunit-dev \
    linux-headers \
    py3-lxml py3-polib \
    nodejs npm

RUN mkdir -p /tmp/builder-home && chmod 1777 /tmp/builder-home