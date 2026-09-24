# CI

`.github/workflows/build.yml` cross-compiles the Ender-3 V3 KE asset (material theme, small screen)
on pushes to `main`/`develop`/`ke-next` and on tags.

Since the Final Closure mission (2026-08-15), it runs inside the same digest-pinned
`ghcr.io/openklipperedition/openke-build` image `OpenKE`'s own `build.sh` uses — not the
retired standalone `ghcr.io/coreflake1/guppydev:latest`. We confirmed both toolchain prefixes the
workflow's matrix expects actually resolve to real binaries in the new image before switching.
Two real gotchas came up during that migration, both explained right in the workflow file's own
comments:
- An arbitrary container UID has no `/etc/passwd` entry in this image, so `HOME` defaults to `/`
  (unwritable) — we set it to `/tmp` explicitly.
- The image deliberately doesn't put the Bootlin toolchain on the global `PATH` (its bundled
  `autoreconf`/`automake` would shadow the system ones a v4l-utils-style build needs) — `guppydev`
  used to put it directly on `PATH`, so this tripped us up briefly.

There's also `.github/workflows/wiki.yml`, which syncs the `wiki/` folder to this repo's own
GitHub Wiki on every push to `main` that touches `wiki/**`. See [Wiki Publishing](Wiki-Publishing)
for how that works — and a real failure it hit (2026-08-14, fixed the next day) because the wiki
had never had a first page yet.

See also: [Integration with OpenKE](Integration-with-OpenKE), and
[OpenKE's Build Environment doc](https://github.com/OpenKlipperEdition/OpenKE)
for what's actually in that shared image.
