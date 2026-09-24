# Vendored dependencies

Two genuinely different things share the word "vendored" here — worth keeping them straight.

## GuppyScreen's own build submodules

`lvgl` (LVGL v8), `lv_drivers`, `libhv`, `spdlog` — real git submodules, fetched by
`git clone --recurse-submodules`. `wpa_supplicant` is vendored in-tree. These are used by every
build, standalone or as part of OpenKE — see
[Building from Source](https://github.com/OpenKlipperEdition/OpenKE/wiki/Build-From-Source).

## The `docker/k1-bash-build/` toolchain reconstruction and `scripts/build-*-mipsel.sh`

You may spot `docker/k1-bash-build/`, the `scripts/build-{nginx,pillow,streaming-form-data,curl,
openrc,ft2font}-mipsel.sh` scripts, and `docs/VENDORING.md`'s vendored-Klipper-mods system
(`k1/k1_mods/`, `scripts/installer.sh`) in this repo's tree. These are all inherited from this
repo's OpenKE lineage — they support OpenKE's own SSH installer on stock Creality firmware,
rebuilding pieces of its toolchain and dependency chain from scratch if an upstream image ever
disappears. Real, credit-worthy work — see [`docker/README.md`](../docker/README.md) for the
Pellcorp lineage behind the toolchain piece specifically.

**A normal OpenKE build or this repo's own CI never touches any of this.** These are rare,
one-off maintenance/re-vendoring scripts carried over in the tree, not part of OpenKE's current
build or deployment path. See `docker/README.md` and `docs/VENDORING.md` for the full detail if
you're specifically maintaining OpenKE's own installer chain.

See also: [OpenKE Relationship](OpenKE-Relationship), [CI](CI).
