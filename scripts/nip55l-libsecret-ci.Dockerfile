# Dedicated NIP-55L contract lane. Noble's libsecret 0.21.4 cannot exercise
# the synchronous OpenSession failure bug fixed by the signer's async path.
FROM ubuntu:24.04@sha256:534baea6a22c03a63003dbc8dbe78fe34bc0d7e595d9a9dc9834884ff530eb55

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build meson pkg-config python3 \
    ca-certificates curl xz-utils gettext dbus-daemon \
    libglib2.0-dev libgcrypt20-dev libsecp256k1-dev libjansson-dev \
    libwebsockets-dev libsodium-dev libssl-dev libcurl4-openssl-dev \
    libnsync-dev \
 && rm -rf /var/lib/apt/lists/*

# GNOME's published SHA-256: libsecret-0.21.8.sha256sum. Keep the source and
# its private prefix pinned; never fall back to Noble's 0.21.4 at build time.
ARG LIBSECRET_VERSION=0.21.8
ARG LIBSECRET_SHA256=70e7279ccc7e8cce1bb0e1c57a3785e2c79aea6d1dd9a538abda3886f11cdf72
RUN set -eux; \
    archive="libsecret-${LIBSECRET_VERSION}.tar.xz"; \
    curl --fail --location --retry 3 \
      "https://download.gnome.org/sources/libsecret/0.21/${archive}" \
      --output "/tmp/${archive}"; \
    echo "${LIBSECRET_SHA256}  /tmp/${archive}" | sha256sum --check -; \
    tar -xf "/tmp/${archive}" -C /tmp; \
    meson setup /tmp/libsecret-build "/tmp/libsecret-${LIBSECRET_VERSION}" \
      --prefix="/opt/libsecret-${LIBSECRET_VERSION}" --libdir=lib \
      -Dmanpage=false -Dgtk_doc=false -Dintrospection=false -Dvapi=false \
      -Dtest_setup=disabled -Dbash_completion=disabled; \
    meson compile -C /tmp/libsecret-build; \
    meson install -C /tmp/libsecret-build; \
    rm -rf "/tmp/${archive}" "/tmp/libsecret-${LIBSECRET_VERSION}" /tmp/libsecret-build

ENV PKG_CONFIG_PATH=/opt/libsecret-0.21.8/lib/pkgconfig
ENV LD_LIBRARY_PATH=/opt/libsecret-0.21.8/lib
RUN test "$(pkg-config --modversion libsecret-1)" = "0.21.8" \
 && test "$(pkg-config --variable=libdir libsecret-1)" = "/opt/libsecret-0.21.8/lib"

WORKDIR /src
