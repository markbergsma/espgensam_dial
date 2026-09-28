# Agent Guidelines & Repository Rules

Architectural standards and constraints for AI coding assistants working in `espgensam_dial`.

This repository is a sibling of [`espgensam`](https://github.com/markbergsma/espgensam) and
inherits its conventions. Where this document is silent, `espgensam/AGENTS.md` applies — in
particular its documentation standards, hardware philosophy, and git policy.

The two repos now have closely parallel structures: espgensam has its own two-seam contract
(its AGENTS.md §3a, `packages/board_*.yaml` plus the `Rs485Profile` struct) for exactly the same
reason this one does. Read that section before reorganising anything here, and keep the two
descriptions consistent rather than restating one inside the other.

---

## 1. The seam contract — the rule that matters most

This project has exactly **two seams**, and they exist because two axes of change are expected:
other dial hardware, and other transports (a UDP link straight to espgensam) or other controlled
targets. Everything else stays concrete. Do not add a third abstraction.

```
packages/board_m5stack_dial.yaml    seam 1: hardware
packages/target_espgensam_ha.yaml   seam 2: transport
packages/control.yaml               ── behaviour, transport-agnostic
packages/ui_volume.yaml             ── presentation, transport-agnostic
```

### Rules

- **`control.yaml` and `ui_volume.yaml` must never name a Home Assistant entity**, call
  `homeassistant.action`, or reference any `homeassistant`-platform component. If you find
  yourself wanting to, the logic belongs in the transport package.
- **The board package declares peripherals only.** No volume logic, no HA entities, no LVGL
  widgets. Encoder and button *triggers* are bound in `control.yaml` via `!extend`, so that
  swapping the board cannot silently change what a detent means.
- **The transport package is the only file that knows how the outside world is reached.**

### The interface between them

A transport package must provide:

| Script | Contract |
|---|---|
| `target_set_volume(db: float)` | apply an absolute dB value |
| `target_toggle_mute()` | invert the current mute state |
| `target_set_power(power_on: bool)` | power the monitors on or off |

and must **define** `on_volume_reported(reported)` and `derive_state`, invoking them itself as
incoming state arrives. It owns these globals:

| Global | Meaning |
|---|---|
| `vol_db` | current volume in dB |
| `vol_min`, `vol_max`, `vol_step` | device bounds — discovered, never hardcoded |
| `bounds_known` | whether the real bounds have arrived; gates the arc and percentage |
| `volume_known` | whether `vol_db` reflects a value Home Assistant actually reported |
| `is_muted` | mute state |
| `mute_pending_until_ms` | optimistic mute awaiting confirmation |
| `sys_state` | a `DialState` value (see `include/dial_state.h`) |
| `sys_power_off` | media_player reports "off" — the **only** basis for power intent |

`sys_state` and `sys_power_off` answer different questions and must not be conflated. `sys_state`
is for display and for deciding whether input is accepted; power intent comes from
`sys_power_off` alone. Deriving power from `sys_state` is what once made a long press send
`turn_off` to an already-off system, with no way to switch it back on.

`control.yaml` and `ui_volume.yaml` own every other global. They are listed in `control.yaml`
under a comment saying so; keep that division visible.

---

## 2. Documentation standards

Inherited from `espgensam/AGENTS.md`, adapted for a YAML-dominated project:

- **`include/*.h` are the single source of truth** for any vocabulary or invariant shared between
  packages. Put the rationale — *why* this design, what breaks otherwise — at the top of the
  header, with Doxygen tags on the declarations. `include/dial_state.h` is the model.
- **YAML comments explain intent, not syntax.** A comment earns its place by recording something a
  reader cannot recover from the code: a hardware quirk, a timing justification, an ordering
  dependency. `# set the volume` on a line that sets the volume does not.
- **Every magic number gets a justification** where it is defined, not where it is used. The
  substitution block in `espgensam_dial.yaml` is where timings are explained.
- Document the traps. Three in this codebase have already cost time and are commented in place:
  the backlight booting off, the write-echo re-entering the reconciler, and the `on` parameter
  name colliding with a macro.

---

## 3. Constraints imposed by the controlled system

These are properties of `espgensam` as it exists, not preferences. Code that ignores them appears
to work on the bench and misbehaves in use.

1. **espgensam paces volume and mute writes itself.** Its setters only accept a request and
   publish it; `loop()` transmits it between telemetry polls, coalesced to at most one volume
   frame per `volume_tx_interval` (espgensam's hub.h §6). So the dial writes on every detent and
   must **not** grow its own rate limiter back — that would only add latency. This depends on
   espgensam `b734fdc` or later; against an older build a fast spin puts a frame on the bus per
   detent, landing on top of monitor replies.
2. **`last_write_ms` must be set before calling `target_set_volume`.** That script re-enters
   `on_volume_reported` synchronously, and the reconciler relies on the timestamp to recognise the
   echo as its own. Reordering those two statements causes the arc to fight the user.
3. **Writes are silently refused** while `Bus Status == "GLM Active"` (up to 15 s after the last
   GLM frame) and while disconnected. Refuse the input visibly; do not move state and let it
   revert.
4. **Volume bounds are configurable in espgensam** and differ between its committed config and the
   owner's local one. Never hardcode −80/−30/−25. They are discovered from Home Assistant.
5. **espgensam's entity schema still churns.** Depend on as few entities as possible — currently
   three — and always through substitutions. It has added, renamed and removed entities
   repeatedly (the hub-wide `Audio Source` select is gone; per-monitor `AES3 Channel` became
   `Input`; the crossover number became a select), but `Volume (dB)`, `Bus Status` and the
   `Genelec SAM System` media player have never changed. espgensam's `packages/gensam.yaml`
   declares `bus_status:` explicitly, naming this project as the reason.
6. **An unavailable entity arrives as NAN, and NAN defeats every comparison.** Home Assistant
   sends the literal string `unavailable`, which `HomeassistantNumber` cannot parse, so it
   publishes `NAN`. Every `<`, `>` and `==` against NAN is false, so ordinary guards fail *open*:
   the reconciler once adopted NAN into `vol_db`, the write path then saw a permanently dirty
   value, and the dial wrote `value: "nan"` to Home Assistant every 50 ms. `std::min`/`std::max`
   propagate NAN rather than clamping it away, so it reached the wire intact.

   Test `std::isfinite()` explicitly before trusting any value that came from Home Assistant, and
   never let "unknown" be represented by a plausible-looking default — that is what `volume_known`
   is for. The same reasoning applies to any future transport.
7. **Group pushes duck the volume without publishing state.** When espgensam has groups
   configured it applies one at every wake and rediscovery, ducking and restoring the master
   volume with broadcast frames only. The dial must keep showing the real target and must not
   try to follow the duck. See the note in `packages/target_espgensam_ha.yaml`.

---

## 4. Hardware constraints

- **Off-the-shelf only**, per espgensam's policy: unmodified M5Stack Dial, no soldering.
- **No PSRAM, 512 KB SRAM.** LVGL's `buffer_size` is set to 25%, the documented figure for
  PSRAM-less boards. If RAM gets tight, reduce it to 12% before removing anything visual. Check
  the figure reported by `esphome compile`, and free heap at runtime.
- **Subset every font** with `glyphs:`. Fonts are rasterised into the binary.
- **The touchscreen must declare `interrupt_pin: GPIO14`.** `Touchscreen::update()` forces a read
  every poll interval unless an interrupt is attached, so the driver reads `TD_STATUS` over I2C
  even when nothing has been touched; an idle FT3267 answers with a NAK, giving a recurring
  `Failed to read status` warning. With the pin set, `update()` calls `stop_poller()` and reads
  happen only on a touch. Published M5Dial configs frequently omit this, and one widely-linked
  one still claims ESPHome does not support the option — it does.
- **The encoder must not be routed through LVGL's `encoders:` input driver.** That driver is a
  focus-group navigator (turn changes focus, press enters edit mode), which is wrong for a
  dedicated knob — and it would mean the first detent after an idle timeout gets consumed waking
  the UI instead of changing volume. Use `rotary_encoder`'s direction triggers.

---

## 5. Verification

- **Compile before claiming anything works:**
  ```bash
  esphome compile espgensam_dial.yaml
  ```
  Zero errors and no warnings attributable to this project's YAML or headers. ESPHome's own
  `mipi_spi` component emits three upstream `-Wempty-body` warnings; those are not ours. Build
  `espgensam_dial-local.yaml` too if one exists — it is what actually gets flashed.

  `derive_state` reaches into `api::global_api_server`, an ESPHome internal. Re-check it on every
  ESPHome version bump; it is null-guarded, but the symbol itself is not a stable API.
- `esphome config espgensam_dial.yaml` validates substitutions and package merging without a full
  build — use it while iterating on YAML structure.
- There are no unit tests and no CI. The bring-up sequence in README.md is the functional test
  plan, and its order matters: each failure mode masks the next.
- **The spin acceptance test** cannot be skipped when touching the write path: spin the dial
  flat out for ten seconds while watching the *controller's* logs. Expect no more than ~20
  `Set system volume` lines per second, no monitor going offline, and the final value landing
  when the knob stops.

---

## 6. Git

- **Never commit without explicit approval from the user.** Suggest a message; let them edit it.
- Add an `Assisted-by: <model name>` trailer when you contributed to a change.
- **Never commit credentials.** `secrets.yaml` and `*-local.yaml` are gitignored; keep
  `secrets.yaml.example` sanitized and current.
