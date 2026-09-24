# Project Evolution & Lineage

This repository represents the touchscreen UI of **OpenKE** ([`OpenKlipperEdition`](https://github.com/OpenKlipperEdition)).

### Project Evolution:
1. **Upstream Roots**: Forked from [`ballaswag/guppyscreen`](https://github.com/ballaswag/guppyscreen) with improvements from the broader K1 and KE community.
2. **NebulaOS Origin**: Brought into the NebulaOS project to serve as the direct touchscreen UI for custom modern Linux on the Creality Nebula Pad hardware (with the Ender-3 V3 KE as the initial bring-up target).
3. **OpenKE Fork & Multi-Printer Scope**: OpenKE is a direct fork of NebulaOS that continues to utilize all of NebulaOS's companion Klipper extensions while providing native dual-slot SWUpdate upgrades, refined UI workflows, and a multi-printer architecture designed to support all printers compatible with Creality's Nebula Smart Kit.

Why this matters for the wiki: sections of this wiki inherited from earlier development phases describe the standalone or installer-based flows, whereas modern OpenKE integrates GuppyScreen natively into the A/B-upgradable OS image.

See also: [Integration with OpenKE](Integration-with-OpenKE) and [Vendored Dependencies](Vendored-Dependencies).
