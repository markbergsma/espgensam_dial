#pragma once

#include <string>
#include <vector>

/**
 * @file dial_state.h
 * @brief System state and picker vocabulary shared between the transport, control and UI layers.
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

/**
 * @brief What the knob and button currently mean. Unlike DialState this is the dial's own
 *        condition, not the controlled system's, so it is owned by `packages/control.yaml`.
 *
 * The knob means volume except while the picker is open. The picker is entered only by a
 * deliberate tap and leaves by itself after `picker_timeout`, so a dial left alone always drifts
 * back to being a volume knob. Nothing is applied until a button press confirms, which is why a
 * stray tap can at worst open a picker that then closes unused.
 *
 * The picker is one list of two kinds of row: Home Assistant scenes first, then espgensam's group
 * presets. They confirm differently, on purpose. A scene means "set the room to this", and the
 * room drifts as soon as anything in it is touched, so confirming the last applied scene applies it
 * again; that is the remedy for drift. A group is a state espgensam holds and reports, so
 * confirming the group already in use does nothing; a re-push would only duck the volume.
 *
 * Scene rows stay usable while GLM owns the bus, group rows do not. A scene is mostly about things
 * other than the monitors (and may itself power the system on), whereas a group write is refused
 * by espgensam outright in that state.
 */
enum UiMode : int {
  /// Knob: volume. Short press: mute. Long press: power.
  UI_VOLUME = 0,

  /// Knob: move the highlight. Short press: apply the highlighted scene or group. Long press or
  /// tap: cancel. Power is deliberately unreachable from here.
  UI_PICKER = 1,
};

/**
 * @brief Identity of picker row @p i: `"s:<scene key>"` or `"g:<group name>"`.
 *
 * Rows are the scenes in `scene_keys` order, then the groups in `group_names` order. The picker
 * remembers its highlight by this key rather than by index, because the scene list is re-queried
 * while the picker is open, and a scene added or removed above the highlight would otherwise
 * silently move it onto a different item.
 *
 * @param scene_keys  the transport's opaque scene keys
 * @param group_names the transport's group names
 * @param i           row index
 * @return the row's key, or `""` when @p i is out of range
 */
inline std::string picker_row_key(const std::vector<std::string> &scene_keys,
                                  const std::vector<std::string> &group_names, int i) {
  const int scenes = (int) scene_keys.size();
  if (i < 0) {
    return "";
  }
  if (i < scenes) {
    return "s:" + scene_keys[i];
  }
  if (i - scenes < (int) group_names.size()) {
    return "g:" + group_names[i - scenes];
  }
  return "";
}

/**
 * @brief Row index holding @p key, the inverse of picker_row_key().
 * @return the index, or -1 when no row has that key (or @p key is empty)
 */
inline int picker_find_row(const std::vector<std::string> &scene_keys,
                           const std::vector<std::string> &group_names, const std::string &key) {
  const int rows = (int) (scene_keys.size() + group_names.size());
  for (int i = 0; !key.empty() && i < rows; i++) {
    if (picker_row_key(scene_keys, group_names, i) == key) {
      return i;
    }
  }
  return -1;
}

}  // namespace espgensam_dial

using espgensam_dial::DialState;
using espgensam_dial::picker_find_row;
using espgensam_dial::picker_row_key;
using espgensam_dial::STATE_ACTIVE;
using espgensam_dial::STATE_DISCONNECTED;
using espgensam_dial::STATE_GLM_LOCKED;
using espgensam_dial::STATE_OFFLINE;
using espgensam_dial::STATE_STANDBY;
using espgensam_dial::STATE_WAKING;
using espgensam_dial::UiMode;
using espgensam_dial::UI_PICKER;
using espgensam_dial::UI_VOLUME;
