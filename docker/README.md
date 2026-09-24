# Vendored cross-compile toolchain

You may still see `pellcorp/k1-bash-build` referenced in a few vendoring scripts here
(`scripts/build-{nginx,pillow,streaming-form-data,curl,openrc,ft2font}-mipsel.sh`). That's
intentional, not a stale leftover — these scripts are for rebuilding some old target-side
third-party libraries with the exact ABI they were originally built for, inherited from this
repo's OpenKE lineage. **The normal OpenKE build and this repo's own CI don't touch this
directory at all.** See
[Vendored Dependencies](https://github.com/OpenKlipperEdition/GuppyScreen/wiki/Vendored-Dependencies)
for the full picture of what's current vs. inherited-but-not-current.

Here's the actual problem these scripts solve: each `scripts/build-*-mipsel.sh` script pins its
Docker image by exact digest, which stays reproducible as long as that image is still on Docker
Hub — but if it ever disappears, the whole chain breaks. `docker/k1-bash-build/` lets you rebuild
that toolchain from scratch instead of depending on Docker Hub staying up forever, the same idea
as `scripts/vendor/nginx-src/`, `pillow-src/`, and friends.

Credit where it's due: this reconstructs [Pellcorp's](https://github.com/pellcorp)
`k1-bash-build` image. Pellcorp never published the Dockerfile itself anywhere we could find, so
this was pieced back together from `docker history --no-trunc` against the real pinned image —
since it was built with BuildKit, that actually preserves the exact `RUN`/`COPY`/`ENV` commands
used, byte for byte. We didn't just assume it was right, either — built it locally and confirmed
`build-nginx-mipsel.sh`/`build-pillow-mipsel.sh`/etc. behave identically against it.

- **`docker/k1-bash-build/`** — glibc, dynamically linked. Bundles `mips-gcc720-glibc229` (a real,
  versioned release from `ballaswag/k1-discovery` — the Dockerfile just downloads it, since it's
  already stably hosted) plus a small device-mirroring sysroot (`k1-sysroot-min.tar.gz`, ~11MB,
  glibc runtime only — see `scripts/build-pillow-mipsel.sh`'s own header comment for exactly what's
  in it).

## Building and using it

```sh
docker build -t openke-k1-bash-build:local docker/k1-bash-build/
# then edit the relevant scripts/build-*-mipsel.sh's K1_BASH_BUILD_IMAGE variable to
# "openke-k1-bash-build:local" instead of the pinned upstream digest, or just retag:
docker tag openke-k1-bash-build:local pellcorp/k1-bash-build@sha256:0b96d1d65175c5a2e3a83a64c3212d08dd774fef0900f991e0ebc570ba896c85
```

You'd only need to rebuild this if the pinned upstream image actually becomes unavailable — day to
day, the pinned digest is simpler and already reproducible on its own.

## GuppyScreen's own toolchain (this is what a normal OpenKE build actually uses)

GuppyScreen itself (the touchscreen binary) builds with a different toolchain — musl, fully static.
Both the normal OpenKE build and this repo's own CI cross-compile it inside
`OpenKE`'s unified build image — see the primary [`OpenKE`](https://github.com/OpenKlipperEdition/OpenKE) repository.

Before that, this used a separate, standalone image, `ghcr.io/coreflake1/guppydev` (built from
[`docker/Dockerfile`](../docker/Dockerfile), top-level, not under this directory). That image's
retired now — if you see a doc anywhere still describing it as the current CI toolchain, that's
out of date. The unified image bundles the exact same Bootlin `mips32el--musl` toolchain `guppydev`
provided, on its own `PATH` entry instead of a separate container.
