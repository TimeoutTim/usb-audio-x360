# UAC2 differential audit against working UAC1

Baseline: `b7053918370b6afc4cba63294f5a61f155813894` (user-confirmed
working UAC1 playback). Audited development implementation: `b83e7e4`.
USB audio output on the Xbox is established. The task is to extend that
implementation to descriptor-driven UAC2, not re-prove USB DMA in general.

## Comparison

| Area | Working baseline | Current bounded UAC2 test | Audit result |
| --- | --- | --- | --- |
| OUT storage | Static, 128-byte-aligned byte arrays | Same allocation/alignment strategy; larger slot stride | No evidence that this strategy itself causes silence. Do not replace it based only on generic Linux DMA restrictions. |
| Host submission | Xbox isoch queue, two slots, four packets each, callback refill | Same underlying queue and depth, serialized through USB DPC domain | Preserve the serialization fixes; protocol compatibility does not require reverting them. |
| Sample rate | Three-byte endpoint SET_CUR | Four-byte AudioControl clock-source CUR, optional RANGE and SET/readback | Request addressing matches Linux's UAC2 path. Skipping SET when CUR already reports 48 kHz also matches its normal path. |
| Format | Stereo S16_LE, four bytes/frame | Descriptor-selected four-byte subslots, 24 valid bits, eight bytes/frame for the test device | Linux maps four-byte PCM subslots to S32_LE. Current MSB-aligned, little-endian writer agrees; do not switch to right-aligned 24-bit without evidence. |
| Packet sizing | Fixed 48 frames / 192 bytes | Feedback-paced 47/48/49 frames / 376/384/392 bytes | Persistent fractional accumulator agrees with Linux's basic explicit-feedback algorithm. Feedback is endpoint-dependent, not exclusive to UAC2. |
| Feedback | None | Explicit IN endpoint; observed three-byte Q10.14 despite four-byte capacity | Decode actual length, not advertised capacity. Current bounded path does this and accepts the verified OHCI short-IN completion. |
| Audio source | Continuous captured system PCM | Two-second test with only one second of nonzero tone | Current test is not system-audio playback and has a narrower listening window. No evidence yet establishes device startup latency as the cause. |
| Feature controls | No feature setup in audio.cpp | Descriptor-gated direct master unmute and mute/volume reads before active alternate | Readback reports mute off and -5 dB. State after alternate activation has not been read back. |

## Variable-length host contract

Read-only inspection of the current retail kernel's isoch queue confirmed:

- It reads each packet length as a native-endian WORD and accumulates offsets.
- Packet data must be contiguous, without maximum-packet-size padding between
  OUT packets. `SubmitBoundedSlot` uses this layout.
- The transfer's total length supplies the final buffer-end address. The
  current code sets it to the sum of all packet lengths before submission.
- Initial offsets start at the buffer's offset within a 4 KiB page. The live
  OUT slots observed in this build do not cross a page even at maximum size.

Thus the source-level variable-length submission is consistent with the
inspected host implementation. This is not a capture of transmitted payload.

## Evidence and limits

The latest run completed 501 batches in each direction, with one initial
not-accessed packet per direction and no other packet errors. CPU diagnostics
recorded nonzero tone generation. The user heard no tone. Neither completion
nor a connection notification establishes audible playback.

All eight portable host-test suites pass, including descriptor, clock,
feedback, ownership and bounded-tone tests. They do not exercise the compiled
Xbox sample writer or the complete setup-to-packet path as one integration
test. No missing mandatory UAC2 request or definitive cause of silence was
identified in this audit.

Stage 5 currently requires an explicit feedback endpoint. Consequently a
SABRENT run through that test is not a valid shared-path regression test until
the harness also supports descriptor-selected non-feedback playback. Do not
interpret that deliberate harness restriction as a UAC1 hardware regression.
Stage 0 still contains the older feedback normalization/playback path; simply
enabling it would bypass the validated bounded-test implementation.

## Next implementation, before another listening test

1. Add an integrated host test for descriptor selection, clock request/response
   sequence, sample packing and feedback-sized packet construction. Exercise
   both the baseline S16_LE case and 24-valid-in-32 PCM, using the same portable
   packet builder as the Xbox. Check byte vectors, per-packet offsets, lengths
   and total length. Keep the working buffer and host queue design.
2. Add bounded post-activation verification: standard GET_INTERFACE, clock CUR,
   and descriptor-supported mute/volume readback. Retain a bounded setup trace,
   rather than only the last request, to distinguish accepted requests from
   final device state. These are diagnostics, not claimed mandatory UAC2 setup
   steps or an established fix. No speculative SETs, resets or volume increases.
3. If those checks pass, run a bounded listening test with a clearly announced
   listening window and quiet repeated tone. Change timing separately from
   protocol behavior. Then restore captured PCM through the same tested packet
   path, not a second implementation.

Generic compatibility remains descriptor-driven. No Apple IDs or vendor
requests are warranted by the evidence. Broader clock topologies, indirect
feature paths, non-1-ms intervals, high-speed transport and safe repeated
attachment remain separate coverage/lifecycle work, not proven explanations
for this device's silence.

## Reference implementations

- [Linux clock.c](https://github.com/torvalds/linux/blob/master/sound/usb/clock.c): `set_sample_rate_v2v3`, clock entity addressing and rate readback.
- [Linux endpoint.c](https://github.com/torvalds/linux/blob/master/sound/usb/endpoint.c): `synced_next_packet_size`, feedback decoding and packed packet offsets.
- [Linux v6.12 format.c](https://github.com/torvalds/linux/blob/v6.12/sound/usb/format.c): `parse_audio_format_i_type`, PCM container selection.

This audit changed no runtime code, console binary or console state.

## Follow-up build: activation verification (marker `0x5541433f`)

Implemented the shared `pcm_packet.h` writer/batch builder in stage 5. A ninth
host suite feeds synthetic UAC1/UAC2 descriptors through discovery, verifies
the UAC2 clock request/response sequence, and constructs fixed/feedback-paced
packets with byte-exact stereo expectations. It covers S16_LE, 24-valid-in-32,
packed 24-bit, signed endpoints, packet offsets, total lengths and rejection
bounds under ASan/UBSan. This is not an emulation of the Xbox control transport.

After active SET_INTERFACE completes, setup now reads standard GET_INTERFACE,
then UAC2 CUR on the resolved clock source, followed by descriptor-supported
master mute and volume. Exact lengths and existing deadlines still apply.
Wrong alternate (`0xe00b`), wrong/missing clock (`0xe00c`), or nonzero mute
(`0xe00d`) stops setup before endpoint opens; no corrective write is attempted.
Volume is recorded, never increased. These checks precede the unchanged
endpoint-open gaps and two-second quiet-tone test.

`UsbAudioActivationDiagnostic[16]` contains:

| Index | Meaning |
| --- | --- |
| 0 | Verification finished; not a claim of audible sound |
| 1 / 2 | GET_INTERFACE valid / returned alternate |
| 3 | Resolved clock source ID |
| 4 / 5 | Clock CUR valid / returned Hz |
| 6 / 7 | Mute read valid / value |
| 8 / 9 | Volume read valid / raw signed 8.8 dB |
| 10..15 | Reserved |

`UsbAudioSetupTrace[256]` retains the first 32 control requests as eight-word
rows: stage, packed type/request/value, index, requested bytes, elapsed ms,
completion status, actual bytes, and up to four response bytes decoded LE.
Status `0xffffffff` means no callback observed, not a USB failure code. The
existing total request count indicates truncation beyond 32 rows. Entries do
not wrap, retain full captures or own transfer storage. Stage IDs 12..15 are
GET_INTERFACE, clock, mute and volume verification respectively.

All nine suites pass and the XDK build succeeds. Hardware validation is pending.
Allocation, queue depth, pacing, tone amplitude/duration and the single-attach
lifecycle are unchanged. This build is diagnostic, not a claimed silence fix.
