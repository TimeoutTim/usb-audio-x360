# Native Guide volume control (experimental)

The plugin adds a native XUI slider to the Guide Home page instead of polling
Back/D-pad during gameplay. This implementation is under hardware validation;
successful compilation and layout tests do not prove focus or rendering on
the console.

## Integration

- The notification worker installs a chained `XuiSceneCreate` hook after a
  startup delay, allowing Nova to install its hook first when present. It does
  not manipulate controls from that worker or from the audio worker.
- After native stock or Nova Home scene creation returns, the hook locates
  the Home action and its next native navigation target. It checks geometry,
  available space, sibling obstacles and the existing bidirectional links.
  Unrecognized layouts are left unchanged.
- A custom `XuiScene` owns a native `XuiText` label and `XuiSlider`. Objects,
  properties and messages all use the running XAM runtime. The public XDK's
  `xui2bin` emits XUR format 5/schema 000c, but the inspected retail Guide
  loader requires format 8/schema 000e; no compiled XUR is shipped or loaded.
- The complete subtree is linked with `XuiElementAddChild`, then initialized
  children-first using `XM_INIT`. `XM_SKIN_CHANGED` resolves the native
  `Slider_Volume` visual from the active Guide skin. The slider must have a
  visual before insertion continues.
- The row is inserted immediately below Home. Existing action rows move down
  one row. Explicit Up/Down navigation links include the slider, and all four
  links must resolve to the intended objects. A failed insertion restores
  the original layout and links before destroying the new subtree.
- Native slider input produces `XN_VALUE_CHANGED`. Its handler clamps and
  atomically updates the existing software output-volume value, including
  while no DAC is playing. Audio work
  remains on the existing audio worker; UI code does not submit USB transfers.
- Each created Home scene owns its own control state and frees it during
  destruction. The hook does not retain control handles between scenes or
  repeatedly force controller focus. Zero volume displays `USB Muted` while
  the Guide is open; there is no gameplay overlay.

## Validation

`tools/test_uac.sh` includes portable layout tests for available space,
collisions, malformed geometry and row limits. The normal and debug XEX builds
use the same control implementation. To retain ordinary system audio while
exposing read-only Guide counters, build with
`USB_AUDIO360_GUIDE_DIAGNOSTICS=1 bash tools/build_plugin.sh`; this writes
`bin/guide-debug/usb_audio360.xex`. The separate `USB_AUDIO360_DEBUG_API=1`
mode intentionally replaces normal playback with bounded USB smoke tests and
must not be used to validate normal volume behavior. Release builds have
neither diagnostic interface enabled.

Hardware has confirmed native row creation, focus and value-change events on
Nova Home, with all four navigation links resolving correctly. Earlier volume
snapback was traced to the setter refusing changes in the idle USB smoke-test
build, not to missing slider input.

Hardware checks still required before treating the feature as complete:

1. Open Home and confirm a visible, non-overlapping row, including with Nova.
2. Navigate down into the slider and out again in both directions; verify
   Left/Right changes volume without changing Guide tabs or stealing focus.
3. Close and reopen the Guide repeatedly; check signed-in and signed-out Home,
   different controller slots, and launching/returning from a game.
4. Confirm connected-device audio, mute, reconnect and microphone behavior
   remain intact, without stutter while the Guide is being used.
5. Check stock Guide without Nova. The integration intentionally refuses
   unfamiliar layouts rather than replacing native pages or controls.
