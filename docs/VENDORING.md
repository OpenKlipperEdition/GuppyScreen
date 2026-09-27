# Vendored Dependencies (Retired)

All legacy vendored sources, wheels, and standalone build scripts previously located under `scripts/vendor/` and `scripts/build-*-mipsel.sh` have been removed.

OpenKE now builds all system dependencies (Python, Moonraker, Nginx, ustreamer, etc.) natively via Buildroot. GuppyScreen is cross-compiled directly as a static binary via `scripts/build-mips.sh` inside OpenKE's unified build environment.
