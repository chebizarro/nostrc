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
    build-essential cmake ninja-build pkg-config clang \
    # groundhog-ci.yml "Install build and display dependencies"
    libgtk-4-dev libadwaita-1-dev libglib2.0-dev libsecret-1-dev \
    libjansson-dev libsecp256k1-dev libwebsockets-dev libsodium-dev \
    libssl-dev libcurl4-openssl-dev libsoup-3.0-dev libjson-glib-dev glib-networking \
    libgit2-dev libsqlite3-dev libnsync-dev libsqlcipher-dev \
    desktop-file-utils appstream xvfb xauth dbus-bin at-spi2-core python3-gi \
    blueprint-compiler gnome-keyring adwaita-icon-theme librsvg2-common \
    # gnostr-appimage.yml (Gnostr, gnostr-signer, nostr-gtk)
    libpeas-2-dev libqrencode-dev librsvg2-dev libgdk-pixbuf-2.0-dev \
    # signet-ci.yml
    libmicrohttpd-dev libcbor-dev \
    # local tooling: sources, debugging, the gate's tree sync and lock
    git python3 ca-certificates gdb rsync util-linux \
 && rm -rf /var/lib/apt/lists/*
# /gate-lock: scripts/linux-gate.sh mounts a volume there whose lock lets one
# gate test run (smoke or sanitizer) at a time.
RUN useradd -m -u 1001 ci && install -d -o ci -g ci /build /work /gate-lock
USER ci
