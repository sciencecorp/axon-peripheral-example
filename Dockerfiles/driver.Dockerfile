# syntax=docker/dockerfile:1
ARG TARGETPLATFORM
FROM ubuntu:20.04 AS base

ARG TARGETPLATFORM
ARG VCPKG_COMMIT=0f88ecb8528605f91980b90a2c5bad88e3cb565f

ENV DEBIAN_FRONTEND=noninteractive \
    VCPKG_ROOT=/vcpkg \
    PATH="${PATH}:/vcpkg"

RUN set -eux; \
    if [ "${TARGETPLATFORM}" = "linux/amd64" ]; then \
        dpkg --add-architecture arm64; \
        sed 's/^deb http/deb [arch=amd64] http/' -i /etc/apt/sources.list; \
        echo "deb [arch=arm64] http://ports.ubuntu.com/ focal main restricted" >  /etc/apt/sources.list.d/arm64-cross-compile.list; \
        echo "deb [arch=arm64] http://ports.ubuntu.com/ focal-updates main restricted" >> /etc/apt/sources.list.d/arm64-cross-compile.list; \
    fi

RUN set -eux; \
    apt-get update; \
    if [ "${TARGETPLATFORM}" = "linux/amd64" ]; then \
        apt-get install -y --no-install-recommends \
            autoconf autoconf-archive build-essential \
            gcc-10-aarch64-linux-gnu g++-10-aarch64-linux-gnu binutils-aarch64-linux-gnu \
            git curl wget ca-certificates gpg unzip tar pkg-config \
            libssl-dev libtool zip ninja-build gosu \
            python3 python3-setuptools python3-jinja2 python3-pip \
            qemu-user-static; \
    else \
        apt-get install -y --no-install-recommends \
            autoconf autoconf-archive build-essential \
            gcc-10 g++-10 \
            git curl wget ca-certificates gpg unzip tar pkg-config \
            libssl-dev libtool zip ninja-build gosu \
            python3 python3-setuptools python3-jinja2 python3-pip; \
    fi; \
    rm -rf /var/lib/apt/lists/*

RUN set -eux; \
    apt-get update; \
    apt-get install -y --no-install-recommends ruby-full ruby-dev rubygems build-essential; \
    rm -rf /var/lib/apt/lists/*

RUN set -eux; \
    if [ "${TARGETPLATFORM}" = "linux/amd64" ]; then \
        ln -s /usr/bin/aarch64-linux-gnu-gcc-10  /usr/bin/aarch64-linux-gnu-gcc; \
        ln -s /usr/bin/aarch64-linux-gnu-g++-10 /usr/bin/aarch64-linux-gnu-g++; \
    else \
        update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-10 100 --slave /usr/bin/g++ g++ /usr/bin/g++-10; \
    fi

RUN set -eux; \
    wget -O - https://apt.kitware.com/keys/kitware-archive-latest.asc 2>/dev/null | gpg --dearmor - | tee /usr/share/keyrings/kitware-archive-keyring.gpg >/dev/null; \
    echo 'deb [signed-by=/usr/share/keyrings/kitware-archive-keyring.gpg] https://apt.kitware.com/ubuntu/ focal main' > /etc/apt/sources.list.d/kitware.list; \
    apt-get update; \
    apt-get install -y --no-install-recommends kitware-archive-keyring \
        cmake=3.28.6-0kitware1ubuntu20.04.1 cmake-data=3.28.6-0kitware1ubuntu20.04.1; \
    rm -rf /var/lib/apt/lists/*

RUN gem install dotenv -v 2.8.1 && gem install --no-document fpm

ENV VCPKG_FORCE_SYSTEM_BINARIES=true
RUN git clone https://github.com/microsoft/vcpkg.git "${VCPKG_ROOT}" && \
    cd "${VCPKG_ROOT}" && \
    git checkout "${VCPKG_COMMIT}" && \
    ./bootstrap-vcpkg.sh -disableMetrics

COPY vcpkg.json "${VCPKG_ROOT}/vcpkg.json"
COPY external/sciencecorp/vcpkg "${VCPKG_ROOT}/external/sciencecorp/vcpkg"

RUN cd "${VCPKG_ROOT}" && \
    ./vcpkg install \
    --triplet arm64-linux-dynamic-release \
    --x-install-root "$PWD/build/host/vcpkg_installed" \
    --clean-after-build

# Install axon-peripheral-driver-sdk + scifi-headstage-shared-libraries from the Science apt repo.
# These supply libaxon-peripheral-driver-sdk.so + headers and the transitive runtime deps the
# plugin .so will pick up via -rpath-link.
ARG SDK_VERSION=0.2.0
ARG SHARED_LIBS_VERSION=1.3.0
COPY keys/science-repo-public.asc /usr/share/keyrings/scifi-repo-science-public.asc
# Drop a freshly-built axon-peripheral-driver-sdk_*.deb into sdk/ to test against an
# unreleased SDK; otherwise the apt repo version is pulled.
COPY sdk/ /tmp/sdk-staging/
RUN set -eux; \
    apt-get update && apt-get install -y --no-install-recommends ca-certificates; \
    echo "deb [signed-by=/usr/share/keyrings/scifi-repo-science-public.asc] https://pub-879bfa29e67b4cd6b0c78b0d4cc3aa59.r2.dev/scifi focal main" > /etc/apt/sources.list.d/repo-science.list; \
    apt-get update; \
    apt-get install -y scifi-headstage-shared-libraries="${SHARED_LIBS_VERSION}"; \
    if ls /tmp/sdk-staging/axon-peripheral-driver-sdk*.deb >/dev/null 2>&1; then \
        echo "==> Using local SDK .deb from sdk/"; \
        apt-get install -y /tmp/sdk-staging/axon-peripheral-driver-sdk*.deb; \
    else \
        echo "==> Installing axon-peripheral-driver-sdk from apt repo"; \
        apt-get install -y axon-peripheral-driver-sdk="${SDK_VERSION}"; \
    fi; \
    rm -rf /var/lib/apt/lists/* /tmp/sdk-staging

ENV VCPKG_INSTALLATION_ROOT="${VCPKG_ROOT}"
ENV CMAKE_TOOLCHAIN_FILE="${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
ENV VCPKG_INSTALLED_DIR="${VCPKG_ROOT}/build/host/vcpkg_installed"

WORKDIR /home/workspace
CMD ["/bin/bash"]
