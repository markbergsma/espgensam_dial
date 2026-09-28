# espgensam_dial

**espgensam_dial** is [ESPHome](https://esphome.io/) firmware that turns an **M5Stack Dial v1.1**
into a physical remote control — primarily a volume knob — for a Genelec SAM monitor system driven
by [`espgensam`](https://github.com/markbergsma/espgensam).

Turning the knob moves the on-screen volume instantly; the monitors follow within about 200 ms. The
round display shows the level in dB, the equivalent percentage, and the condition of the system at
a glance.

---

## Why this exists

`espgensam` already exposes a Genelec SAM system to Home Assistant over the GLM RS-485 bus. But the
only ways to change volume were a Home Assistant slider and Genelec's own GLM software, and neither
fits the thing you do most often at a desk: reach out and turn a knob.

---

## Features

- **Accelerated volume knob** — 0.5 dB per detent when turning slowly for fine trim, scaling to
  3 dB per detent on a fast spin, so the full −80…−25 dB range is under three revolutions instead
  of seven.
- **Zero-latency display** — the arc and readout update from local state on the detent itself, not
  after a network round trip.
- **Self-limiting writes** — coalesced and rate-capped to protect espgensam's unthrottled RS-485
  path (see [Why the dial rate-limits itself](#why-the-dial-rate-limits-itself)).
- **Honest state** — GLM bus lockout, standby, wake-up progress and loss of the Home Assistant
  link are each shown distinctly, using the same colour vocabulary as espgensam's own status LED.
- **Runtime bounds discovery** — volume min/max/step are read from Home Assistant, so nothing is
  hardcoded and espgensam's configuration can change freely.

---

## Controls

| Input | Gesture | Action |
|---|---|---|
| Knob | turn | volume, with acceleration |
| Front button | short press | mute / unmute |
| Front button | long press (≥ 800 ms) | monitors on / off |
| Screen | tap | wake the backlight only — never changes audio |

Tapping is deliberately inert beyond waking the screen, so picking the device up or brushing it
can never change monitor level.

The buzzer is **silent on ordinary detents** by default — an 80 dB click per detent in a studio is
intolerable, and this is a device whose whole job is not making unwanted noise. It sounds only when
you reach the end of the volume range or when a write is refused. Enable detent clicks with the
"Click feedback" switch in Home Assistant if you want them.

---

## Display

```
      ╭───────────────╮        outer ring   volume, 270° sweep
    ╭─┤    standby    ├─╮      badge        state, blank when plainly active
    │ │     -42.5     │ │      large        volume in dB
    │ │       dB      │ │
    ╰─┤      58%      ├─╯      small        equivalent percentage
      │     MUTE      │        red          only when muted
      ╰───────────────╯
```

Ring colours are borrowed from espgensam's status LED, with one simplification: espgensam shows
discovering and configuring as separate colours, while the dial shows a single "waking" cyan.
Note that the Waveshare controller has no RGB LED at all, so on that board the dial is the only
status indicator rather than a second opinion.

| Colour | Meaning | Knob |
|---|---|---|
| green | active | works |
| dim green | standby — level is stored, applied on power-up | works |
| cyan | waking (discovering / configuring) | works |
| amber | Genelec's GLM adapter owns the bus | **refused** |
| dim white | powered on, but no monitors found | works, inaudible |
| dark grey | no Home Assistant link | **refused** |

After 20 s idle the backlight dims but never blanks, so the level stays readable across the room.

---

## Supported hardware

### M5Stack Dial v1.1 (`packages/board_m5stack_dial.yaml`)

| Item | Detail |
|---|---|
| **MCU** | M5Stamp-S3A — ESP32-S3FN8, 8 MB flash, 512 KB SRAM, **no PSRAM** |
| **Display** | GC9A01A 240×240 round TFT, SPI |
| **Touch** | FT3267 (ESPHome `ft5x06` driver), I²C `0x38` |
| **Encoder** | 16 detents / 64 pulses per revolution |
| **RTC** | PCF8563, I²C `0x51` |
---

## Architecture

```
espgensam_dial.yaml            identity, network, and every tunable as substitutions
packages/
  board_m5stack_dial.yaml      HARDWARE ONLY — swap this file for a different dial controller
  target_espgensam_ha.yaml     TRANSPORT — the only file that knows Home Assistant exists
  control.yaml                 input, optimistic state, coalescing, reconciliation
  ui_volume.yaml               LVGL fonts, page, repaint
include/dial_state.h           the DialState vocabulary and its precedence rules
```

Two seams, no more. `control.yaml` and `ui_volume.yaml` never name a Home Assistant entity; they
reach the controlled system only through three scripts (`target_set_volume`, `target_toggle_mute`,
`target_set_power`) and read its condition only from globals the transport package writes. See
[AGENTS.md](AGENTS.md) for the full contract.

That is what makes a planned UDP-direct transport a one-file change, and a different dial
controller a one-file change in the other direction.

### Why the dial rate-limits itself

espgensam has **no throttling on its volume path**: each write synchronously transmits an RS-485
frame from the ESPHome loop, and each send calls `parser_.clear()`, discarding partially-received
monitor telemetry. Four consecutive missed poll cycles marks a monitor offline. A fast spin issuing
one write per detent would therefore knock monitors offline.

So the dial coalesces. Local state updates on every detent — that is what you see and feel — while
a flusher writes at most every 200 ms (about one frame per espgensam poll cycle), plus an immediate
flush once the knob has been still for 90 ms so the exact final value always lands.

A reconciler then has to tell three things apart: the echo of the dial's own write, a stale value
arriving while the user is still turning, and a genuine external change (a Home Assistant slider,
Genelec's GLM software, or espgensam rejecting a write and snapping back). It does this with a
600 ms grace period keyed on the last local change and the last write.

---

## Getting started

### 1. Configure secrets

```bash
cp secrets.yaml.example secrets.yaml
```

Fill in Wi-Fi credentials and an API key. A dedicated `api_key` per device is recommended over
reusing espgensam's.

### 2. Point it at your espgensam

**Do this before the first flash.** `espgensam_dial.yaml` ships the entity IDs that espgensam's
*committed* configuration produces:

```yaml
ent_volume:     "number.genelec_sam_controller_volume_db"
ent_media:      "media_player.genelec_sam_system"
ent_bus_status: "sensor.genelec_sam_controller_bus_status"
```

Yours will very likely differ. ESPHome derives entity IDs from the device friendly name plus the
entity name, and either may have been renamed in Home Assistant afterwards — renames stick, so the
ID can end up looking nothing like the YAML. Check all three in **Developer Tools → States**. A
wrong one makes that value read `unknown` forever, with no error.

### 3. Build and flash

```bash
esphome compile espgensam_dial-local.yaml    # or espgensam_dial.yaml if you edited it directly
```

then flash the resulting firmware.

### 4. Allow the dial to perform Home Assistant actions

**Required, and easy to miss.** In Home Assistant: Settings → Devices & Services → ESPHome →
the dial's entry → **Configure** → enable **"Allow the device to perform Home Assistant
actions"**.

Reading state needs no permission, but *every* write — volume, mute and power alike — goes through
a Home Assistant action and is gated by this single option. With it off the dial looks like it is
working: the screen tracks the knob perfectly and reads espgensam's state correctly, but nothing
ever reaches the monitors.

How to recognise it:

| Where | What you see |
|---|---|
| `esphome logs espgensam_dial.yaml` | `Home Assistant action 'number.set_value' dropped; client has not subscribed to actions (yet)` |
| Home Assistant log | `... rejected; If you trust this device and want to allow access for it to make Home Assistant service calls, you can enable this functionality in the options flow` |

Either message means this option. The dial also logs one `write: ...` line per outbound change, so
the device log shows plainly whether a write was attempted and whether it left the device.

---

## Notes

- **0% means −80 dB, not silence.** espgensam treats only a slider value ≤ 0.001 as true silence
  (−130 dB), and that case is unreachable when driving it in dB.
- **Volume bounds are discovered at runtime** from the Home Assistant `number` entity's `min`,
  `max` and `step` attributes, a few seconds after the API connects. Until then the readout shows
  `--%` and the arc stays empty. This is why espgensam's committed `-30.0` maximum and the
  `-25.0` in its `espgensam-atoms3-rs485-iso-local.yaml` both work with no change here.
- **`--.-` means the volume itself is unknown**, not zero — the dial has had no usable value from
  Home Assistant yet, or the entity has gone unavailable. The knob is refused in that state on
  purpose: a detent is a *relative* change, so without a known starting point it would jump the
  system to an arbitrary level.
- **If an entity id is wrong, nothing errors.** Home Assistant simply sends no state, so the value
  sits empty forever. About 30 s after boot the dial logs a warning naming any of the three that
  never reported — worth checking for on a first flash.
- **The dial depends on Home Assistant.** espgensam itself is autonomous, but an HA-mediated remote
  is not; the dark grey "no link" state makes that visible rather than pretending. A UDP transport
  straight to espgensam is the planned fix.

---

## License

[GNU General Public License v3.0](LICENSE), matching `espgensam`.
