# OpenKE GuppyScreen

This is OpenKE's touchscreen UI — a KE-focused fork of
[GuppyScreen](https://github.com/ballaswag/guppyscreen). Full print control, an interactive 3D bed
mesh, an on-screen calibration suite, running right on the printer's display with no X11, Wayland,
or display server involved.

Originally developed as the display interface for NebulaOS, this project serves as the touchscreen UI for [OpenKE](https://github.com/OpenKlipperEdition/OpenKE), an open-source firmware distribution forked from NebulaOS that continues to use all of NebulaOS's companion Klipper extensions.

## Features

- 🖨️ **Print control & status** — temps, fans, LED, movement/homing, file browser (incl. USB sticks), Spoolman
- 🟦 **Interactive 3D bed mesh** — rotate / zoom / pan colour height map (plus a table view)
- 🎯 **Guided Calibration hub** — a single numbered menu: Axis Twist, a combined Z-offset + bed mesh
  Recalibration Wizard, Input Shaper, E-Steps Calibration, Skew Correction, TMC Autotune
- 🎚️ **Fine-tune mid-print** — speed, flow, Z-offset, pressure advance (firmware retraction is its own panel)
- 📷 **Camera** — persistent image tuning (contrast/saturation)
- 🔔 **Buzzer beeps & songs** — real-pitch `M300`, `PLAY_TUNE` jingles (editable `songs.conf`), soft touchscreen click
- 🔒 **Print-state safety locks** — anything that could ruin a running job is blocked or asks first
- 📐 Tuned **480×272** layout

## Building

Two different workflows depending on what you're doing.

**If you're just trying to build the whole OS** — use
[`OpenKE`](https://github.com/OpenKlipperEdition/OpenKE) instead. It pins an exact
commit of this repo and cross-compiles + installs it automatically as part of the full image. You
don't need to clone this repo directly for that.

**If you're actually developing GuppyScreen itself:**

```bash
git clone --recurse-submodules https://github.com/OpenKlipperEdition/GuppyScreen.git
cd GuppyScreen
```

Submodules: `lvgl` (LVGL v8), `lv_drivers`, `libhv`, `spdlog`. `wpa_supplicant` is vendored in-tree.
Full prerequisites, the SDL simulator target, and the MIPS cross-build are covered in
**[Building from Source](wiki/Building-from-Source.md)**.

Offline logic tests (no cross-compile, no LVGL/SDL2 needed) run with `make test`.

## Compatibility

| | |
|---|---|
| **Hardware Platform** | Creality Nebula Pad / Nebula Smart Kit ecosystem |
| **Reference Target** | Creality Ender-3 V3 KE (primary tested & qualified baseline) |
| **Printer Roadmap** | All printers supported by Creality Nebula Smart Kit (Ender-3 V3 SE, V2 Neo, S1, V2, Pro, CR-10 SE) |
| **SoC / arch** | Ingenic XBurst2 X2000 — MIPS (mipsel) |
| **Display** | 480×272 |

## Config and theme persistence

Commit `b15ad7f` fixed a real bug where `Config::init()`/`ThemeConfig::init()` silently fell back to
in-memory defaults on every boot on read-only-squashfs setups, instead of reading your
actual saved `config.json`/`theme.json`. No crash, just settings that quietly never stuck. We've
since tested this on real hardware — config and theme survive a real flash now. See
[`OpenKE`'s `manifests/dependencies.conf`](https://github.com/OpenKlipperEdition/OpenKE/blob/main/manifests/dependencies.conf)
for the pin history.

## Documentation & Heritage

Developer docs live in [`wiki/`](wiki/) and [`DEVELOPMENT.md`](DEVELOPMENT.md).

OpenKE GuppyScreen traces its lineage through the NebulaOS project and early stock-firmware modding experiments, building upon upstream GuppyScreen. For full OS build and installation instructions, refer to the primary [`OpenKE`](https://github.com/OpenKlipperEdition/OpenKE) repository.

## License & credits

**GPL-3.0** — see [LICENSE](./LICENSE). Builds on
[ballaswag/guppyscreen](https://github.com/ballaswag/guppyscreen),
[probielodan/guppyscreen](https://github.com/probielodan/guppyscreen), and
[pellcorp/grumpyscreen](https://github.com/pellcorp/grumpyscreen), with the 3D bed mesh from
[prestonbrown/guppyscreen](https://github.com/prestonbrown/guppyscreen). Vendored Klipper mods keep
their own upstream licenses and credits.
