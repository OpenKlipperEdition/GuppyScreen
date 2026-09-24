# Integration with OpenKE & NebulaOS Extensions

OpenKE is a direct fork of NebulaOS that integrates GuppyScreen directly into the system image and continues to use all of NebulaOS's companion Klipper extensions (`NebulaOS-klipper-extensions`). The [`OpenKE`](https://github.com/OpenKlipperEdition/OpenKE) build pipeline pins an exact commit of this repo in `manifests/dependencies.conf` and its build pipeline fetches, cross-compiles, and installs it automatically as part of the full OS image. You do not need to build this repo separately when building the complete OpenKE firmware.

Every real OpenKE build records this repo's exact commit in `build-manifest.txt`.

Also worth knowing: config and theme persist across boots on OpenKE's read-only-squashfs setup —
see [Config and Theme](Config-and-Theme) for details.

See also: [CI](CI), [Vendored Dependencies](Vendored-Dependencies), [OpenKE Relationship](OpenKE-Relationship).
