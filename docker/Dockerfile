# Build environment for the DoraCore GKI 6.6 kernel.
# Mirrors the "Build Environment" step of .github/workflows/GKI-6.6.yml.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# Same package set as the workflow, plus:
#   - clang/lld/llvm: only used when TOOLCHAIN=system
#   - tini: forwards Ctrl-C / `docker compose stop` to the build script
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      ca-certificates curl bison flex make binutils git perl gcc g++ python3 \
      python-is-python3 bc libssl-dev libelf-dev dwarves \
      cpio zip unzip ccache binutils-aarch64-linux-gnu \
      tar which wget xz-utils zstd tzdata tini \
      clang lld llvm \
 && rm -rf /var/lib/apt/lists/*

# The kernel tree is bind-mounted from the host and owned by another uid.
RUN git config --system --add safe.directory '*'

WORKDIR /work
ENTRYPOINT ["/usr/bin/tini", "--", "/bin/bash", "/work/build.sh"]
