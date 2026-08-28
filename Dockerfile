# The PocketBook toolchain ships as x86_64 Linux binaries, so this image is
# amd64 even on Apple Silicon (Docker runs it under emulation).
FROM --platform=linux/amd64 debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
        make \
        build-essential \
        file \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work
