# Cross-compile toolchain

The legacy `docker/k1-bash-build` directory and `k1-sysroot-min.tar.gz` (used for older glibc-based pre-Buildroot third-party binary experiments) have been retired and removed. OpenKE now builds all system dependencies natively via Buildroot.

## GuppyScreen's own toolchain (this is what a normal OpenKE build actually uses)

GuppyScreen itself (the touchscreen binary) builds with a different toolchain — musl, fully static.
Both the normal OpenKE build and this repo's own CI cross-compile it inside
`OpenKE`'s unified build image — see the primary [`OpenKE`](https://github.com/OpenKlipperEdition/OpenKE) repository.

Before that, this used a separate, standalone image, `ghcr.io/coreflake1/guppydev` (built from
[`docker/Dockerfile`](../docker/Dockerfile), top-level, not under this directory). That image's
retired now — if you see a doc anywhere still describing it as the current CI toolchain, that's
out of date. The unified image bundles the exact same Bootlin `mips32el--musl` toolchain `guppydev`
provided, on its own `PATH` entry instead of a separate container.
