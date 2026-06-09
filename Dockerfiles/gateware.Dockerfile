# syntax=docker/dockerfile:1
ARG TARGETPLATFORM
FROM ubuntu:22.04

ARG TARGETPLATFORM
ARG RADIANT_VERSION=2024.2
# Bump this ARG when Lattice ships a new 2024.2.x build; do not edit RUN steps.
# Pinned against electronics-infra/install_radiant.sh (RADIANT_VERSION=2024.2.0.3.4_Radiant_lin)
# and verified via HTTP 200 from
# https://files.latticesemi.com/Radiant/2024.2/2024.2.0.3.4_Radiant_lin.zip
ARG RADIANT_BUILD=2024.2.0.3.4
ARG HOST_UID=1000

ENV DEBIAN_FRONTEND=noninteractive \
    TZ=America/Los_Angeles \
    LANG=en_US.UTF-8 \
    LANGUAGE=en_US:en \
    LC_ALL=en_US.UTF-8 \
    RADIANT_DIR=/opt/lattice/radiant/2024.2
# LM_LICENSE_FILE is intentionally NOT baked in: the Radiant license is
# supplied from outside at runtime (synapsectl forwards the host's
# LM_LICENSE_FILE via -e, and bind-mounts a file license to
# /opt/lattice/license.dat). Hardcoding it here would mask the "no license
# provided" case and break the SDK's license detection.
ENV PATH="/opt/axon-peripheral-sdk/bin:${RADIANT_DIR}/bin/lin64:${PATH}"

# Base toolchain + locale + Radiant runtime deps + Verilator/iverilog build deps
# in a single layer. Radiant runtime deps mirror electronics-infra/playbook.yml
# "Install lattice radiant dependencies"; Verilator deps mirror
# "Install Verilator & Iverilog dependencies". Omitted by design (out of scope
# per spec Non-Goals): GtkWave, OSS CAD Suite, PDM, direnv.
RUN set -eux; \
    apt-get update; \
    apt-get install -y --no-install-recommends \
        ca-certificates curl unzip gpg git locales gosu sudo tzdata \
        make autoconf flex bison gperf help2man perl perl-doc \
        python3.10 python3.10-venv python3-pip \
        iverilog mold numactl ccache libfl2 libfl-dev \
        zlib1g zlib1g-dev libgoogle-perftools-dev g++ \
        at-spi2-core libpangocairo-1.0-0 libcairo2-dev pulseaudio libc6 \
        libjpeg-dev libieee1284-3 libusb-0.1-4 lsb-base libnss3 libice6 \
        libgl1-mesa-glx libsm6 libxt6 libxext6 libxrender1 libxi6 libxft2 \
        libxslt1.1 libxrandr2 libxfixes3 libxdamage1 libxcursor1 \
        libxcomposite1 libxinerama1 libxss1 libxcb-image0 libxcb-keysyms1 \
        libxcb-render-util0 libxcb-xkb1 libxcb-xinput0 libxcb-xinerama0 \
        libxcb-cursor0 libxkbcommon0 libxkbcommon-x11-0 libxcb-icccm4 \
        libx11-6 libgl1-mesa-dri libopengl0 qtbase5-dev qtchooser qt5-qmake \
        qtbase5-dev-tools libxkbfile1; \
    locale-gen en_US.UTF-8; \
    apt-get clean; \
    rm -rf /var/lib/apt/lists/*

# Lattice Radiant install. Install path is ${RADIANT_DIR}
# (=/opt/lattice/radiant/${RADIANT_VERSION}) so the version stamp lives in the
# path — matches axon-peripheral-sdk's expected layout.
RUN set -eux; \
    curl -fL "https://files.latticesemi.com/Radiant/${RADIANT_VERSION}/${RADIANT_BUILD}_Radiant_lin.zip" -o /tmp/radiant.zip; \
    unzip -q /tmp/radiant.zip -d /tmp/radiant; \
    mkdir -p "${RADIANT_DIR%/*}"; \
    /tmp/radiant/${RADIANT_BUILD}_Radiant_lin.run --verbose --console --prefix "${RADIANT_DIR}"; \
    rm -rf /tmp/radiant /tmp/radiant.zip; \
    rm -rf "${RADIANT_DIR}/ispfpga/ap"*; \
    rm -rf "${RADIANT_DIR}/ispfpga/sa6t00"

# Verilator v5.034 from source. Ubuntu 22.04's packaged Verilator is too old
# for current axon-peripheral-sdk testbenches.
RUN set -eux; \
    git clone --depth 1 --branch v5.034 https://github.com/verilator/verilator /tmp/verilator; \
    cd /tmp/verilator; \
    unset VERILATOR_ROOT; \
    autoconf; \
    ./configure; \
    make -j"$(nproc)"; \
    make install; \
    cd /; \
    rm -rf /tmp/verilator

# axon-peripheral-sdk install from the Science apt repo (jammy channel),
# with sdk/*.deb local override for unreleased SDKs (same pattern as
# driver.Dockerfile). When sdk/ has a .deb the apt repo isn't touched, so
# unpublished or pre-release builds don't require a working repo.
ARG AXON_SDK_VERSION=0.1.0
COPY keys/science-repo-public.asc /usr/share/keyrings/scifi-repo-science-public.asc
COPY sdk/ /tmp/sdk-staging/
USER root
RUN set -eux; \
    apt-get update; \
    apt-get install -y --no-install-recommends ca-certificates; \
    if ls /tmp/sdk-staging/axon-peripheral-sdk*.deb >/dev/null 2>&1; then \
        echo "==> Using local SDK .deb from sdk/"; \
        apt-get install -y --no-install-recommends /tmp/sdk-staging/axon-peripheral-sdk*.deb; \
    else \
        echo "==> Installing axon-peripheral-sdk=${AXON_SDK_VERSION} from Science apt repo (jammy)"; \
        echo "deb [signed-by=/usr/share/keyrings/scifi-repo-science-public.asc] https://pub-879bfa29e67b4cd6b0c78b0d4cc3aa59.r2.dev/scifi jammy main" > /etc/apt/sources.list.d/repo-science.list; \
        apt-get update; \
        apt-get install -y --no-install-recommends axon-peripheral-sdk="${AXON_SDK_VERSION}"; \
    fi; \
    rm -rf /var/lib/apt/lists/* /tmp/sdk-staging

# Non-root dev user. synapsectl peripherals build passes --build-arg
# HOST_UID=$(id -u) so bind-mounted writes (e.g. src/gateware/build/bitstreams/)
# come back host-user-owned. Groups mirror electronics-infra/playbook.yml's
# "Add user" task (users, sudo, dialout).
RUN set -eux; \
    groupadd -f --gid "${HOST_UID}" dev || true; \
    useradd -m -u "${HOST_UID}" -g dev -s /bin/bash -G sudo,dialout dev; \
    echo "dev ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/dev; \
    chmod 0440 /etc/sudoers.d/dev

WORKDIR /home/workspace
USER dev
CMD ["/bin/bash"]
