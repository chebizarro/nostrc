# Shared Ubuntu 24.04 image for local Linux builds: the pre-push gate's Linux
# stage (scripts/linux-gate.sh) and scripts/groundhog-linux-ci.sh.
#
# The package list is the union of the apt lists in the hosted Linux workflows
# that build this tree, so a local build sees what CI sees.
# scripts/check-linux-ci-packages.py fails (in CI and in the gate) when a
# workflow installs a package this list lacks. Keep the groups below in step
# with the workflow each is named after.
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    # toolchain (all workflows; clang for nostrc-ci, signet-ci, libgo-ci)
    build-essential cmake ninja-build meson pkg-config clang \
    # groundhog-ci.yml "Install build and display dependencies"
    libxml2-dev libgtk-4-dev libadwaita-1-dev libglib2.0-dev libsecret-1-dev \
    libjansson-dev libsecp256k1-dev libwebsockets-dev libsodium-dev \
    libssl-dev libcurl4-openssl-dev libsoup-3.0-dev libjson-glib-dev glib-networking \
    libgit2-dev libsqlite3-dev libnsync-dev libsqlcipher-dev libqrencode-dev libzbar-dev \
    gettext locales desktop-file-utils appstream xvfb xauth dbus-bin at-spi2-core python3-gi \
    blueprint-compiler gnome-keyring gjs adwaita-icon-theme librsvg2-common \
    # W27 voice messages (groundhog-ci.yml sanitizer job)
    libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
    gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad gstreamer1.0-libav \
    # gnostr-appimage.yml (Gnostr, grotto, nostr-gtk)
    libpeas-2-dev librsvg2-dev libgdk-pixbuf-2.0-dev \
    # signet-ci.yml
    libmicrohttpd-dev libcbor-dev \
    # local tooling: sources, debugging, the gate's tree sync and lock
    git python3 ca-certificates curl gdb rsync util-linux \
 && rm -rf /var/lib/apt/lists/*
# Grotto Blueprint drift is checked with the same compiler on both supported
# distributions, independently of their distro package version. Official tarball.
RUN curl --fail --location --retry 3 \
      https://download.gnome.org/sources/blueprint-compiler/0.22/blueprint-compiler-0.22.2.tar.xz \
      --output /tmp/blueprint.tar.xz \
 && echo '231d72efbac931c235ac3e022fe94982095c20d88721d9a8dcf60152f2017e07  /tmp/blueprint.tar.xz' | sha256sum --check \
 && mkdir -p /opt/blueprint \
 && tar -xJf /tmp/blueprint.tar.xz --strip-components=1 -C /opt/blueprint \
 && meson setup /tmp/blueprint-build /opt/blueprint --prefix=/usr/local \
      -Dpython.purelibdir="$(python3 -c 'import sysconfig; print(sysconfig.get_path("purelib"))')" \
 && meson compile -C /tmp/blueprint-build \
 && meson install -C /tmp/blueprint-build \
 && blueprint-compiler --version | grep -x 0.22.2 \
 && rm /tmp/blueprint.tar.xz
# Ubuntu's stripped libsecret needs its matching dbgsym for LSan to match the
# narrow secret_collection_signal suppression (nostrc-eixk6).
RUN version="$(dpkg-query -W -f='${Version}' libsecret-1-0)" \
 && arch="$(dpkg --print-architecture)" \
 && curl --fail --location --retry 3 \
      "https://ddebs.ubuntu.com/ubuntu/pool/main/libs/libsecret/libsecret-1-0-dbgsym_${version}_${arch}.ddeb" \
      --output /tmp/libsecret-dbgsym.ddeb \
 && dpkg-deb --extract /tmp/libsecret-dbgsym.ddeb / \
 && rm /tmp/libsecret-dbgsym.ddeb
# A real (non-C) UTF-8 locale: gettext ignores LANGUAGE under C and C.UTF-8,
# so the en@pseudo catalog tests (nostrc-gofet.10) need one to run.
RUN localedef -i en_US -f UTF-8 en_US.UTF-8
# Fail the image build if partial runtime plugins would crash or disable voice.
RUN for element in appsrc appsink decodebin audioconvert audioresample \
    scaletempo autoaudiosrc autoaudiosink level opusenc opusdec oggmux \
    oggdemux qtdemux avdec_aac; do gst-inspect-1.0 "$element" >/dev/null; done
# Native Grotto package verification uses the same debhelper toolchain as CI.
RUN apt-get update && apt-get install -y --no-install-recommends debhelper dpkg-dev fakeroot \
 && rm -rf /var/lib/apt/lists/*
# Grotto's GUI-only LSan exceptions name static third-party functions, not modules
# (nostrc-ydtdf, nostrc-ojtu3). Install matching symbols, never replace runtime libs.
RUN arch="$(dpkg --print-architecture)" \
 && for spec in f/fontconfig:libfontconfig1 g/gtk4:libgtk-4-1 \
      g/glib2.0:libglib2.0-0t64 p/pango1.0:libpangoft2-1.0-0; do \
      source="${spec%%:*}"; package="${spec#*:}"; \
      version="$(dpkg-query -W -f='${Version}' "$package")"; \
      curl --fail --location --retry 3 \
        "https://ddebs.ubuntu.com/ubuntu/pool/main/$source/${package}-dbgsym_${version}_${arch}.ddeb" \
        --output /tmp/grotto-dbgsym.ddeb \
      && test "$(dpkg-deb -f /tmp/grotto-dbgsym.ddeb Package)" = "$package-dbgsym" \
      && test "$(dpkg-deb -f /tmp/grotto-dbgsym.ddeb Version)" = "$version" \
      && dpkg-deb --extract /tmp/grotto-dbgsym.ddeb / \
      && rm /tmp/grotto-dbgsym.ddeb || exit 1; \
    done
# /gate-lock: scripts/linux-gate.sh mounts a volume there whose lock lets one
# gate test run (smoke or sanitizer) at a time.
RUN useradd -m -u 1001 ci && install -d -o ci -g ci /build /work /gate-lock
USER ci
