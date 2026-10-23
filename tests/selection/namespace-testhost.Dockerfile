# Dedicated regression host, NOT an authored native selector/profile backend.
FROM ubuntu:24.04@sha256:68ee22004a9109b44973313b02ff7b1de250f1de0f5ecb628d73f579aaff33ac
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build git pkg-config python3 python3-gi \
    libglib2.0-dev libjson-glib-dev libsecret-1-dev libsecret-tools \
    dbus-bin dbus-daemon xvfb xauth gnome-keyring blueprint-compiler \
    libxml2-utils ca-certificates gettext gir1.2-gtk-4.0 \
    # The policy suite configures the real homed unit graph
    # (gnome/nostr-homed/tests/unit): jansson, SQLite3 and OpenSSL::Crypto.
    libjansson-dev libsqlite3-dev libssl-dev \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --gid 1001 namespace-test \
    && useradd --uid 1001 --gid 1001 --create-home namespace-test \
    # Custody mounts over this canonical root-owned socket directory privately.
    # There is no ambient display server; /tmp must not hide this setup layer.
    && install -d -o root -g root -m 1777 /tmp/.X11-unix
USER 1001:1001
ENV XPC_SERVICE_NAME=0 PYTHONDONTWRITEBYTECODE=1 PYTHONNOUSERSITE=1
WORKDIR /source
CMD ["/usr/bin/python3", "-m", "unittest", "discover", "-s", "tests/selection", "-v"]
