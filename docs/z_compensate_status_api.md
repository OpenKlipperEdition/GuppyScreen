# `z_compensate` structured status contract (v1)

> This is a synced copy of the authoritative version in the `ke-mainline-klipper` repo
> (`docs/z_compensate_status_api.md`), which owns the backend object this describes. Kept
> in sync manually, same as `tests/fixtures/` - see `tests/fixtures/README.md`. If this
> contract ever changes, update the authoritative copy first.

This document describes the `[z_compensate]` object's Klipper status contract - the
structured, typed replacement for scanning `Z_OFFSET_CALIBRATION`'s human-readable
`gcmd.respond_info()` text. It is consumed today by the `openke-guppyscreen` fork's
recalibration wizard via Moonraker's `printer.objects.subscribe`.

This is a **project-specific object**, not an upstream Klipper object - `z_compensate.py`
lives in `klippy_extras/` in this repo (see `DESIGN.md`), and this contract is this
project's own addition to it. It has no relationship to any object of the same or similar
name in upstream Klipper or in Creality's stock firmware.

## How to get it

`ZCompensate` implements `get_status(eventtime)`. Any Klipper object with this method is
automatically queryable under its config section name (`z_compensate`) - no separate
registration. Two ways to read it:

- **Moonraker `printer.objects.subscribe`**, requesting `{"z_compensate": null}` (null =
  all fields). Moonraker's webhooks layer polls `get_status()` on every internal update
  cycle, diffs it against the last value sent to each subscriber, and pushes only the
  fields that changed (see "Partial update handling" below).
- **Moonraker `printer.objects.query`**, same section name, for a one-shot read.

GuppyScreen's `init_panel.cpp` already subscribes to every loaded Klipper object with a
null field-list, so `z_compensate` is included automatically with zero subscription-setup
changes on the frontend.

## Fields

| Field                  | Type            | Meaning                                                                 |
|-------------------------|-----------------|--------------------------------------------------------------------------|
| `calibration_id`        | integer         | Increments by 1 on every new `Z_OFFSET_CALIBRATION` invocation. `0` before the first ever call in this Klipper session. |
| `calibration_state`     | string enum     | One of `"idle"`, `"running"`, `"complete"`, `"error"`.                   |
| `calibration_z_offset`  | float or `null` | The **final, applied** Z offset. Only meaningful when `calibration_state == "complete"`; `null` in every other state. |
| `calibration_error`     | string or `null`| A short, human-readable failure description. Only meaningful when `calibration_state == "error"`; `null` in every other state. |

### `calibration_z_offset` is the final value, never a raw intermediate measurement

The published offset is the same value already applied live via `SET_GCODE_OFFSET` -
`tri_expand_mm`'s correction has already been added, and (per `_probe_overrides()`) this
section's own `tri_min_hold`/`tri_max_hold`/`speed` tuning was already in effect during the
measurement. A subscriber never needs to re-derive or re-correct this number; it is exactly
what a human reading `Z_OFFSET_CALIBRATION`'s own console response would compute by hand,
and exactly what should be written to `printer.cfg` if the caller chooses to persist it.

## State machine

```
        Z_OFFSET_CALIBRATION invoked
                    |
                    v
   idle ────────> running ──────> complete
                    |
                    └──────────> error
```

- **`idle`**: the initial state (`calibration_id = 0`) before `Z_OFFSET_CALIBRATION` has
  ever been called in this Klipper session, and the state a caller should treat any
  `calibration_id` at or below its own recorded baseline as still meaning.
- **`running`**: set synchronously at the very start of `cmd_z_offset_calibration`, before
  any motion or probing happens - `calibration_id` has already incremented, and
  `calibration_z_offset`/`calibration_error` are both reset to `null` at the same instant, so
  a subscriber can never observe a stale result from a previous attempt alongside a new id.
- **`complete`**: the measurement succeeded, was applied as this print's live Z offset, and
  `calibration_z_offset` now holds that exact value. Published *before* the optional
  `persist_offset` block runs (see "Persistence ownership" below).
- **`error`**: any exception during motion, probing, or the finite-value check.
  `calibration_z_offset` is `null` and `calibration_error` holds a short description.
  `Z_OFFSET_CALIBRATION` also still raises normally (existing behavior, unchanged) - the
  structured state and the gcode-level error are two independent signals of the same
  failure, not a replacement for one another (see "Command-error handling" below).

There is no direct `running -> idle` or `complete -> running` transition without an
intervening state change: every new attempt goes through the synchronous `running` reset
described above, so a subscriber correlating on `calibration_id` alone is sufficient (see
next section) - it never needs to separately watch for a state reset.

## ID correlation

A caller that wants to track *one specific invocation* it just triggered should:

1. Read the current `calibration_id` **before** sending `Z_OFFSET_CALIBRATION` - this is
   the baseline.
2. After sending the command, ignore every observed snapshot whose `calibration_id` is not
   **strictly greater than** the baseline - such a snapshot belongs to a prior invocation
   (or is the pre-command `idle`/leftover state) and must not be misread as progress on the
   new one.
3. Once a snapshot with a newer id reaches `complete` or `error`, that is the terminal
   result for this invocation. Stop watching - any further update, even one that
   contradicts it, must be ignored. (Concretely: a late `error` arriving after a `complete`
   was already observed for the same id should never happen under this contract, but a
   subscriber must not action it if it somehow did.)

This is exactly what `openke-guppyscreen`'s `ZCompensateStatusTracker` implements
(`begin(baseline_id)` / `on_status()`, one terminal-latching boolean).

## Partial update handling

Moonraker subscription pushes only the fields that changed since the last notification to
that subscriber - a transition straight from `"running"` to `"complete"` between two poll
cycles may arrive as one combined update carrying only `calibration_state` and
`calibration_z_offset` (not `calibration_id`, if it didn't change on this particular push -
though in practice it always changes at the same instant `state` becomes `"running"`).
**A subscriber must maintain its own merged view of the full object** rather than assuming
every notification is self-contained. `openke-guppyscreen` already has a general-purpose
mechanism for this (`State::set_data()`, using `nlohmann::json::merge_patch` for every
subscribed object, not something built specifically for this contract) - `z_compensate`
relies on that existing merge, and any other subscriber to this object must do the
equivalent before calling anything like `parse_z_compensate_status()`.

## Command-error handling

`Z_OFFSET_CALIBRATION` still raises a normal Klipper `command_error` on failure - this
contract does not replace that. A JSON-RPC caller (e.g. Moonraker's
`printer.gcode.script`) may therefore observe a command failure via its own response
`error` field *before*, *after*, or *interleaved with* the structured `error` status
update, depending on timing. A subscriber must treat **whichever of these two signals it
observes first** as authoritative and ignore the other if it arrives later for the same
invocation - do not wait for both, and do not require both. This is the "first terminal
signal wins" rule `ZCompensateStatusTracker::on_status()` / `::fail_command()` implement.

## Timeout's role

This contract has no built-in timeout field or mechanism - `calibration_state` will sit at
`"running"` indefinitely if `Z_OFFSET_CALIBRATION` never returns (e.g. Klipper itself became
unresponsive, or the connection dropped). A subscriber that needs bounded wait behavior
(GuppyScreen's wizard uses a 4-minute UI-level timer) must implement it independently of
this contract; the timer firing is not a data point this contract can supply.

## Persistence ownership

This contract carries the offset; it does not persist it. `ZCompensate` itself never writes
`printer.cfg` as a side effect of the structured status update. Two independent, opt-in
persistence paths exist and neither is implied by the other:

- `[z_compensate]`'s own `persist_offset` config option (off by default) - a
  console/config-file concern, runs `Z_OFFSET_APPLY_PROBE` + `SAVE_CONFIG` (a real klippy
  restart) after `complete` is already published.
- GuppyScreen's own wizard, which reads `calibration_z_offset` from this contract and does
  its own `printer.cfg` patch + `FIRMWARE_RESTART` via `ZOffsetConfigPersistence`,
  completely independent of whether `persist_offset` is set.

## G-code response text is not part of this API

`Z_OFFSET_CALIBRATION`'s `gcmd.respond_info()` line (and any other console text this or
related commands emit) is for a human reading the console. It is explicitly **not** part of
this contract, carries no compatibility guarantee, and must never be parsed by any
subscriber. (This is the exact fragility this contract replaces - see `DESIGN.md`'s
"structured status contract" section for the history.) The stock-Klipper `PROBE_CALIBRATE`
text-response parsing GuppyScreen's wizard still does during its unrelated ZOFFSET stage is
scanning a *different, upstream* Klipper command's output and is unaffected by any of this.

## Versioning

This is contract **v1**. If a future change needs to alter field types, remove a field, or
change what a state value means, it must not silently redefine this document's existing
guarantees for existing subscribers - add a new field/state value (backward compatible) or
bump this document's version and coordinate the change with every subscriber (currently
only `openke-guppyscreen`). `calibration_id` incrementing forever within one Klipper
session is a deliberate design choice, not an implementation detail: it must remain
monotonic for ID correlation (see above) to stay valid.

## Worked examples

Generated directly from the real `ZCompensate` class (see
`klippy_extras/test_z_compensate_contract_fixture.py`, which writes these exact values to
`docs/z_compensate_status_contract_fixture.json` and is re-verified by
`klippy_extras/test_z_compensate_status.py`):

```json
{
  "idle":     {"calibration_id": 0, "calibration_state": "idle",     "calibration_z_offset": null,      "calibration_error": null},
  "running":  {"calibration_id": 1, "calibration_state": "running",  "calibration_z_offset": null,      "calibration_error": null},
  "complete": {"calibration_id": 1, "calibration_state": "complete", "calibration_z_offset": -0.12734,  "calibration_error": null},
  "error":    {"calibration_id": 1, "calibration_state": "error",    "calibration_z_offset": null,      "calibration_error": "Load-cell trigger not detected"}
}
```
