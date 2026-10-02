# Guide audio controls

The Media tab contains a USB Audio row between Windows Media Center and
Select Music. It uses the existing gap rather than moving native controls.
The row is hidden when no supported DAC is connected. No gameplay controller
polling or chord interception is used.

## Interaction

The three right-aligned icons open volume, toggle microphone mute, and open
settings. Outward navigation at the icon group's edges is left to the Guide.
While adjusting volume, Left/Right changes it in 5% steps and remains on the
slider at its endpoints. A/B accepts the value and restores the icon group.
Up/Down accepts it and moves to the native adjacent row; focus loss collapses
the slider without taking focus back. Changes apply immediately.

The expanded USB Audio page groups Playback and Recording controls. It includes
volume, output mute, microphone level/mute, a moving peak meter, and microphone
listening. Device properties is a separate page on the native back stack.
The Guide's B legend provides Back navigation; there is no extra visible button.
Recording controls are replaced with an explanation for output-only devices.

## Implementation boundaries

- A chained `XuiSceneCreate` hook recognizes `GuideMain.xur`. It checks the
  Media tab's controls, geometry, and navigation before inserting the row.
  Failed insertion restores the original navigation links. Unknown layouts
  are left untouched.
- Scene construction, navigation, focus, and 100 ms refresh timers use XAM's
  native XUI runtime on the Guide thread. Expanded pages derive from `HUDScene`
  with `OpenType=2`. Native slider visuals are scaled to their allotted space.
  The three custom icons are embedded PNG bytes with SVG sources in
  `assets/guide`; users install only the plugin XEX.
- The notification worker installs the hook after the existing Nova startup
  delay; it does not render UI. Audio callbacks do not wait for UI work or call
  XUI. Volume/mute/gain use atomic state; device details use a bounded snapshot.
- Output volume follows a 60 dB attenuation range, with zero explicitly silent.
  Microphone level is software gain from 0–200%, default 100%, with saturation.
  The peak meter measures post-gain input before mute, updates every 100 ms,
  releases at 30 dB/s, and holds clipping feedback for two seconds.
- Listen to microphone uses a separate bounded 16 kHz ring with a 20 ms prefill
  and 64 ms capacity, interpolated to 48 kHz. It replaces USB output only and
  respects output volume/mute. Game voice capture is not consumed by listening.
  Stop, page departure, disconnect, microphone mute, or a missing UI heartbeat
  stops listening. No recording is saved.
- Optional product/manufacturer string reads occur after streaming starts.
  Malformed or unavailable names do not prevent audio. Timed-out transfer
  storage remains alive until completion/removal; no USB controller reset or
  forced cancellation is introduced. USB serial-number strings are not queried.

## Validation

Run `bash tools/test_uac.sh` for Media-gap validation, gain saturation, continuous
monitor interpolation/buffering, peak measurement, USB string parsing, and the
existing transport/audio tests. `bash tools/build_plugin.sh` produces the normal
release XEX; there is no separate Guide diagnostic build or probe application.

The reference implementation was exercised on kernel 17559 with Aurora/Nova.
After changes, check Media-row visibility, outward tab navigation, slider
endpoints and A/B/Up/Down, settings/Device properties Back behavior, microphone
listening shutdown, and disconnect/reconnect. Build and host-test success do
not replace console input/rendering and audio validation.
