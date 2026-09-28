#pragma once

/**
 * @file dial_state.h
 * @brief System state vocabulary shared between the transport, control and UI layers.
 *
 * ## Why this header exists
 *
 * The dial's behaviour depends on a small number of mutually exclusive conditions of the
 * *controlled system* (espgensam + its Genelec monitors), not of the dial itself. Those conditions
 * decide two things that must never disagree:
 *
 *   1. whether user input is acted on at all, and
 *   2. what the screen says.
 *
 * Encoding them as a single enum rather than a set of independent booleans is deliberate. The
 * conditions are genuinely exclusive and have a strict precedence, and a collection of booleans
 * would permit nonsense combinations (`glm_locked` *and* `standby`) that the UI would then have to
 * resolve arbitrarily. One value, one precedence order, one place to change it.
 *
 * ## Precedence
 *
 * `derive_state` (in `packages/target_espgensam_ha.yaml`, the transport package) evaluates these
 * top-down and takes the first match:
 *
 *   DISCONNECTED > GLM_LOCKED > STANDBY > WAKING > OFFLINE > ACTIVE
 *
 * The order encodes "what stops the user getting what they want, soonest". A dial with no link
 * cannot do anything, so that outranks everything; a bus owned by Genelec's own GLM adapter
 * refuses writes regardless of whether the amplifiers are awake; and so on down to ACTIVE, which
 * is the only state where a volume change both applies and is audible.
 *
 * STANDBY deliberately outranks every bus phase below GLM, matching espgensam's own status LED
 * (`components/gensam/status_led.cpp`, which promotes standby over any bus phase except GLM).
 * Two cases make this necessary rather than cosmetic:
 *
 *   - espgensam boots in standby and does *not* retry discovery while off, so a system whose
 *     monitors are unpowered sits at bus status "Offline" indefinitely. Ranking Offline higher
 *     would leave the dial showing "no monitors" with no way to switch the system on.
 *   - Discovery wakes monitors *silently* while the system stays off, so "Discovering" can occur
 *     with the system genuinely in standby. Reporting "waking" there would be wrong.
 *
 * Power intent must therefore never be inferred from this enum. It comes from the separate
 * `sys_power_off` global, which tracks the media_player state alone.
 *
 * Mute is intentionally *not* a state here. It is an orthogonal overlay — a system can be muted
 * while active, standby or waking — so it lives in the separate `is_muted` global and is rendered
 * on top of whichever state is current.
 *
 * ## Input policy per state
 *
 * REFUSED states (DISCONNECTED, GLM_LOCKED) reject knob input outright and pulse the ring amber.
 * This is an honesty choice: espgensam silently drops writes in both cases, so moving the arc and
 * then having it snap back would read as a bug in the dial rather than a condition of the system.
 *
 * ACCEPTED states include STANDBY, which may look surprising. espgensam's `set_volume_db()` stores
 * the value without transmitting while in standby, so presetting a level before powering the
 * monitors on genuinely works and is useful.
 */

namespace espgensam_dial {

/// Condition of the controlled system, in precedence order (see file comment).
enum DialState : int {
  /// No Home Assistant API connection, or the media_player entity reads unavailable/unknown
  /// (or has not reported yet). Knob input refused — there is nothing to send it to.
  STATE_DISCONNECTED = 0,

  /// Genelec's official GLM adapter owns the RS-485 bus (Bus Status == "GLM Active").
  /// espgensam refuses every write until 15 s after the last GLM frame. Knob input refused.
  STATE_GLM_LOCKED = 1,

  /// Monitors are coming up *and* the system is on: Bus Status is "Discovering" or
  /// "Configuring". Volume changes are accepted and will apply once the RACE pass completes.
  STATE_WAKING = 2,

  /// System is on but Bus Status is "Offline" — no monitors were discovered. Input accepted
  /// but inaudible.
  STATE_OFFLINE = 3,

  /// System is in standby (media_player state "off"). Volume changes are stored by espgensam
  /// but not transmitted, so presetting a level before power-on works.
  STATE_STANDBY = 4,

  /// Normal operation: monitors awake, bus ours, volume changes audible.
  STATE_ACTIVE = 5,
};

}  // namespace espgensam_dial

using espgensam_dial::DialState;
using espgensam_dial::STATE_ACTIVE;
using espgensam_dial::STATE_DISCONNECTED;
using espgensam_dial::STATE_GLM_LOCKED;
using espgensam_dial::STATE_OFFLINE;
using espgensam_dial::STATE_STANDBY;
using espgensam_dial::STATE_WAKING;
