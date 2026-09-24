# Testing

Offline logic tests — no cross-compile, no LVGL/SDL2 needed — run with:

```sh
make test
```

Covers `test-z-compensate-status`, `test-z-offset-persistence`, `test-contract-fixture`,
`test-integration-harness`, `test-subscription-baseline-ordering`, and
`test-config-theme-parse-safety` (`tests/*.cpp`).

`test_z_compensate_status.cpp` covers the structured-status handoff the Recalibration Wizard reads
from `NebulaOS-klipper`'s `z_compensate` module — see
[NebulaOS-klipper's Z Compensation page](https://github.com/coreflake1/NebulaOS-klipper/wiki/Z-Compensation).

`test_config_theme_parse_safety.cpp` covers the `Config::init()`/`ThemeConfig::init()` path fixed in
`b15ad7f` — see [Config and Theme](Config-and-Theme).

For interactive/visual work, build the SDL simulator target (see
[Development and Simulator](Development-and-Simulator)) instead of round-tripping to real hardware
for every UI tweak. Real-hardware qualification is still the actual gate before anything ships —
see [Integration with OpenKE](Integration-with-OpenKE).
