# Portable USB Audio core: implementation status

For the current comparison against the user-confirmed working UAC1 baseline,
see [UAC2 differential audit](uac2-baseline-audit.md). The staged-build entries
below are chronological history, not all descriptions of the current build.

## Continuous UAC1/UAC2 integration

Production marker `0x55414344` selects continuous captured-system-PCM output
with physical-removal reattachment;
debug marker `0x55414342` retains the remotely commanded bounded stage-5
harness. Both use the same descriptor discovery, UAC2 clock graph, activation
readbacks, PCM writer and Xbox transport adapter.

After the active alternate is verified, endpoint opens retain the validated
250 ms nonblocking separation. The release build registers render capture,
buffers 768 stereo frames, then primes two four-packet OUT slots and, for an
asynchronous profile, two explicit-feedback IN slots together in the USB DPC
domain. Completion owns refill and resubmission. The worker no longer polls
free TRBs, and it never moves the PCM read index after streaming starts. This
avoids racing the completion-side consumer; a full ring causes the capture
producer to drop a render block rather than overwrite unread storage.

Synchronous/adaptive formats send fixed 48-frame packets. Asynchronous formats
start at nominal 48 frames/ms and retain the latest validated feedback rate.
The continuous pacer uses a persistent fractional accumulator and bounded
Linux-style shift normalization for the full-speed feedback encodings already
accepted by the parser. Packet sizes remain limited to 47..49 frames and the
advertised endpoint capacity. UAC1 endpoint rate control and UAC2 clock-source
CUR/RANGE behavior remain class-specific setup paths; neither is hard-coded by
device ID.

Unknown OUT/feedback completion errors stop new submissions and retain all
storage. Current-frame `NotAccessed` startup packets are recorded and tolerated;
OHCI short feedback-IN completion is accepted only with bounded actual length.
Physical removal stops the active transport generation in the USB DPC domain.
The worker waits one second, then re-arms only if every submitted control, OUT,
and feedback transfer has produced a normal or cancellation completion. Static
TRB and endpoint fields are cleared only after that drain, and the next device
uses the incremented generation. A context mismatch or incomplete drain fails
closed for the remainder of the boot. Re-arm checks run at most every 250 ms
and stop after a five-second removal deadline. No controller or port reset is
used.

Portable tests now include continuous fixed/feedback pacing as well as the
descriptor-to-clock-to-PCM pipeline. XDK release/debug builds succeed. Apple
AirPods Max USB Audio (`05ac:110c`) has passed audible generated-tone and
continuous captured Xbox system-audio validation through this setup, format,
and feedback stack.

See the [Xbox transport contract audit](xbox-usb-transport-contract.md) before
expanding transport support. Completion-driven refill is integrated and has
passed continuous playback validation. Reattachment after physical removal now
has a bounded fail-closed drain/re-arm path. Both UAC1-to-UAC2 and UAC2-to-UAC1
switching passed repeated hardware validation without a reboot.

This is development documentation, not a claim of additional hardware support.
Discovery and clock setup are now connected to an experimental Xbox test
runtime. No console binary is changed by running host-side tests alone.

## Current Xbox test build

Following an unresponsive-console report during the integrated playback test,
the setup-only build completed discovery and read back 48 kHz on hardware while
the debug connection remained responsive. That test submitted no streaming
transfers and kept AudioStreaming alternate zero.

The preceding **activation/open-only** build (`USB_AUDIO360_TEST_STAGE=2` in
audio.cpp). After clock verification it selects the discovered active alternate
and opens OUT and feedback IN endpoints, with 250 ms nonblocking gaps between
steps to expose progress to the monitor. It does not capture console audio,
write feature controls, or submit any audio/feedback transfers. No sound or
connected notification is expected. The marker is `0x55414333`; diagnostic 55
records progress (1=activating, 2=activated, 3=opening OUT, 4=OUT opened,
5=opening feedback, 6=opens complete, 7=test finished), and 57 becomes 1 on
success. Diagnostic 54 retains the last clock frequency response. Stage 1
selects the earlier setup-only test; stage 0 enables the unvalidated playback
path described below. The build marker must be updated when changing test mode.

The crash report did not yield a dump, so streaming remains a hypothesis to
isolate, not an established cause. The playback path is currently gated off.

That activation/open-only test completed successfully on hardware: both endpoint
opens returned success and the debug connection remained responsive. The
current build selects **stage 3: one silent OUT batch**, marker `0x55414334`.
After the same setup/open sequence it submits exactly one four-packet batch of
48 stereo frames per packet, all zero samples. No feedback transfer, unmute,
render capture, retry or continuous submission is enabled. The storage stays
allocated until reboot even on timeout. A one-second completion deadline marks
failure without cancelling or reusing the hardware-owned transfer.

Diagnostic 58 is 1 before submission and 2 after the void queue routine returns;
59 is 1 after the completion is observed; 60 counts nonzero packet statuses.
Individual OUT statuses/lengths remain at 23..26/28..31. Setup error `0xe005`
means no completion arrived before the deadline. Completion alone does not
mean every packet succeeded, or that the device rendered audio. No audible
sound or connected notification is expected from this silent test.

The stage-3 DPC-adapter build marker is `0x55414335`: it keeps the exact same
single silent batch but routes USB opens and submissions through the DPC-domain
adapter. Diagnostics 56/61/62 report observed attach/dispatch/completion CPU and
IRQL. See the transport audit for requirements and outstanding lifecycle limits.

That test completed on hardware with attach, submission and completion all on
hardware thread 2 at IRQL 2. The first OUT packet reported Not Accessed, while
the other three reported success. This verifies the execution context, not
sustained playback or the cause of previous freezes.

The initial stage-4 build selected **one feedback IN batch**, marker
`0x55414336`. It keeps the same setup and endpoint opens but submits no OUT
packets. It requests four descriptor-sized feedback packets exactly once,
through the same adapter, with the same one-second deadline and no retries.
Diagnostic 13 counts feedback callbacks; 40..43 contain packet statuses and
44..47 actual lengths. Only successful packets with lengths of 3 or 4 bytes
within the requested size are read. In this stage, 48..51 contain their raw
little-endian feedback integers, **not normalized rates**, and 52 counts such
packets. Diagnostics 58/59/60 now describe the feedback submission, completion,
and nonzero status count. Error `0xe006` rejects a missing or unsupported-size
feedback endpoint. No feedback value is applied to playback timing. No sound
or connected notification is expected. This test does not establish sustained
feedback scheduling or valid feedback without an active playback stream.

The current stage-4 marker is `0x55414337`. The same single batch now uses
`isoch_result.h` to accept the verified OHCI short-IN status `0xc0050003`, as
well as status zero, while enforcing actual length <= requested capacity.
Feedback decoding accepts only 3/4-byte payloads; empty/partial responses and
all other error statuses are ignored. Raw statuses remain unchanged in
40..43 and in diagnostic 60's nonzero count: a short packet may therefore be
accepted even though its raw status is nonzero. Diagnostic 52 counts decoded
payloads; 48..51 remain raw values in this stage. No OUT, retries, feature
writes or continuous playback have been added. Host tests cover short reads,
length bounds, invalid statuses, empty responses and exact-size buffers under
ASan/UBSan. Hardware validation accepted two three-byte feedback values of
`0x0c0000` (48 samples/frame in Q10.14), ignored empty/not-accessed packets,
and completed without losing debug responsiveness.

The current build selects **stage 5**, marker `0x55414338`: a 250 ms bounded
silent OUT plus feedback-IN test. Each endpoint has two independent four-packet
slots. The worker submits only completed/free slots through the DPC adapter;
callbacks never resubmit. There is also a hard cap of 128 submissions per
direction. OUT contains exactly 48 stereo silent frames per packet; feedback
is recorded but not applied to packet sizing in this transport-only test.
No unmute, render capture, notification, controller reset or endpoint close
is enabled. Stop means cease submissions, then observe completion accounting;
it is not a claim of endpoint closure or safe module unload.

After the budget expires, wait at most one second for all four slots to become
idle. Storage is retained even on failure. Error `0xe007` means drain timeout;
`0xe008` means submission/completion counts did not match. Single attachment
per boot still applies. Stage-5 diagnostics:

- 4/5: submitted OUT/IN batches; 15/13: completed OUT/IN batches.
- 6/7: successful OUT/IN packets (IN includes valid short/empty packets).
- 8/9: not-accessed OUT/IN packets; 10/14: other OUT/IN errors.
- 20: pending slots during drain; 21: elapsed test milliseconds.
- 52: decoded nonempty feedback payload count; 48..51: last decoded raw values.
- 58: 2=submitting, 3=waiting for completions, 4=accounting complete.
- 59: accounting complete; 60: mismatched direction count.

Completion accounting alone does not certify packet success or uninterrupted
streaming. Cumulative not-accessed/error counters provide continuity evidence;
the first-frame scheduling limitation remains.

The current marker is `0x55414339`: stage 5 now primes both endpoints together
in the USB DPC domain and replenishes directly from completions, not the worker.
The same 250 ms / 128-per-direction caps and two four-packet slots apply.
See the transport audit for the inspected requeue contract. All diagnostic
meanings above remain unchanged. Hardware validation of this version is pending.

The 250 ms completion-driven test completed 64 batches per direction with all
submissions accounted for, 253 decoded feedback payloads, one not-accessed
packet per direction, and no other packet errors. Debug access remained
responsive. The totals alone did not locate those misses.

The current stage-5 marker is `0x5541433a`: the same queue and submission path
now run for 30 seconds with an 8192-submission cap per direction. Diagnostics
16/17 record the first OUT/IN not-accessed location; 18/19 record the last.
Location encoding is `(one_based_completion_ordinal << 3) | packet_index`;
zero means none. Thus 8 means packet zero of the first completed batch.
These identify startup versus later misses without per-packet logging in DPC.
The one-second completion-drain deadline and retained-storage policy remain.
The duration/cap are tested, including timer wrap. Hardware validation is pending.

This remains a transport continuity test with fixed 48-frame silent packets.
It neither applies feedback to playback sizing nor proves the device's internal
audio buffer remains balanced. Feedback pacing, audible output, and testing
under varied dashboard/game load are subsequent gates, not inferred successes.

The 30-second fixed-packet retry completed 7501 batches per direction, with
30001 decoded feedback payloads. Its only not-accessed packets were packet zero
of the first completion on each endpoint; no later misses or other packet errors
were observed. All submissions completed and debug access remained responsive.
An earlier attempt failed SET_INTERFACE with device-not-responding before any
streaming submission; the intermittent activation issue remains unresolved.

Current marker `0x5541433b` retains the 30-second limit and same queue depth,
but applies feedback using `feedback_pacer.h`. It supports three-byte Q10.14
and four-byte Q16.16 samples per full-speed frame, without arbitrary rescaling
or device-specific quirks. Values outside 47..49 samples/frame or the endpoint's
capacity are rejected. This deliberately narrower policy is not Linux's full
format autodetection/quirk handling and does not claim every feedback encoding.
The rate starts at nominal 48; a single DPC-owned fractional accumulator spans
both OUT slots. Only completed/free slot lengths change, and zero-filled buffers
are already large enough for the maximum permitted packet. No PCM capture,
unmute, mixing or resampling is enabled.

A one-second initial-feedback/stale-feedback deadline is a conservative test
policy, not a USB protocol requirement. Invalid/empty packets do not refresh it.
Error `0xe009` stops submissions on expiry; `0xe00a` means a capacity/pacing
configuration failure. Storage remains allocated and outstanding completions
can return. Hardware validation is pending.

Stage-5 diagnostics now use 22=last accepted Q16.16 rate, 27/28=min/max OUT
frames per packet, 29=rejected decoded feedback values, 30=accepted rate updates,
31=feedback age in milliseconds at the last OUT preparation. These replace the
older OUT status-pointer/length diagnostics; OUT raw statuses remain at 23..26.
The other counters and missed-packet locations are unchanged. Host tests cover
both encodings, exact accumulated frame totals, rate updates without phase reset,
capacity rejection, invalid payloads, stale feedback and timer wrap.

Current marker `0x5541433c` keeps the same feedback-paced silent test but gives
standard USB setup requests a 5000 ms deadline. Non-clock class requests retain
1000 ms; the UAC2 clock state machine's own deadlines are unchanged. There is
no pre-request sleep, retry, USB reset, or recovery by clearing stop flags.
Host tests cover request classification, deadline boundaries, completed-request
precedence and timer wrap. Hardware validation is pending.

A separate 16-DWORD `UsbAudioControlDiagnostic` symbol records the most recent
request independently of setup-worker consumption. Locate it using the matching
linker map; do not assume adjacency to the existing diagnostic array:

0=request sequence; 1=setup stage; 2=packed request; 3=index;
4=worker issue tick; 5=dispatch-return tick; 6=callback tick;
7=callback elapsed ms; 8=raw callback status; 9=actual length;
10=cumulative callback count; 11=non-clock timeout tick;
12=timeout elapsed ms; 13=request deadline ms;
14=cumulative non-clock timeout count; 15=callback observed stop flag.

Ticks are unsigned 32-bit GetTickCount values. Callback fields are published
even after setup has stopped; completion count distinguishes a zero status from
no callback. This is diagnostic state, not a synchronization API: snapshots may
span updates. Exact deadline-boundary races remain possible, but both timeout
and completion observations are now retained if a request times out. Outstanding
storage is never recycled as a consequence of a deadline.

The control-timing/feedback-paced build completed a 30-second hardware test:
activation took 579 ms, 7501 batches per direction completed, 30001 feedback
updates were accepted without rate rejections, and OUT packets varied from
47 to 49 frames. Only packet zero of the first batch was not accessed on each
endpoint; no later misses/errors or outstanding transfers were observed. This
does not establish that intermittent setup failures have been eliminated.

Current marker `0x5541433d` enables the first bounded audible test. Stage 5 now
runs for two seconds (1024 submissions maximum per direction), using the same
feedback pacing and queue depth. Samples contain 100 ms initial silence, a
one-second 1 kHz stereo tone at approximately -31 dBFS peak with 5 ms fades,
then silence. The sequence advances in generated PCM frames across both slots;
actual wall-clock tone duration follows the DAC's feedback-paced sample clock.
Host tests cover amplitude bounds, silence, fade endpoints and periodicity.

After clock verification, a directly linked UAC2 feature unit's master mute is
cleared only if its descriptor advertises read/write access. No hardware volume
is written; missing mute controls are skipped. An unmute failure stops setup.
The class request retains its one-second deadline, while standard SET_INTERFACE
retains five seconds. System capture and connection notifications remain off.
Tone packing uses the selected PCM subslot and valid-bit alignment. Hardware
audibility is pending; a USB completion alone is not proof of rendered sound.

The first tone run completed all 501 batches per direction with only the first
packet missed, but the user did not clearly hear it. Current marker `0x5541433e`
keeps the same tone level, duration, packing and cache behavior. After optional
unmute, setup reads current master mute and volume only when the directly linked
UAC2 feature unit advertises readable controls. No volume write is performed.
Read requests use the existing serialized control path and require exact actual
lengths; failures stop setup. Complex feature topologies remain unsupported.

New `UsbAudioToneDiagnostic[16]` retains small diagnostic values, not an audio
capture. Locate its address with the matching map:

0=feature ID; 1=master controls; 2=mute read valid; 3=mute value;
4=volume read valid; 5=raw signed 16-bit 8.8 dB volume (0x8000 is silence);
6=generated stereo-frame count; 7=nonzero generated frame count;
8=peak magnitude on signed-16 scale; 9=snapshot frame index;
10/11=packed left/right sample interpreted little-endian;
12=slot; 13=one-based upcoming OUT batch; 14=nonzero prepared batch count;
15=snapshot valid. Snapshot is the first sample of magnitude at least 800.

Preparation counters and a CPU-side sample do not prove DMA observed the bytes,
nor that each prepared batch was submitted successfully. Correlate with normal
transfer accounting. The test does not increase volume, alter cache mappings,
or bypass transfer ownership. Hardware validation is pending.

This is a single-attachment-per-boot diagnostic build, not a general release.
Boot with the DAC disconnected, wait for the dashboard, then attach it once.
After disconnect or setup failure, reboot before testing another attachment.
The driver deliberately never recycles its device extension or transfer storage
within that boot. Boot re-enumeration and all port/controller resets are disabled.

The initial transport scope is the inspected kernel's full-speed OHCI path,
48 kHz stereo Type I PCM in 2/3/4-byte subslots, a 1 ms data interval, and
adaptive/synchronous playback or asynchronous playback with explicit feedback.
Endpoint sizes are checked against the packet generator's 48/49-frame limits.
High-speed/EHCI, implicit feedback and non-unity clock multipliers are rejected.

Both class versions use shared discovery after fetching the configuration
header followed by its bounded total length. UAC2 clock requests run through
the portable state machine on the audio worker. USB callbacks publish completion
rather than chaining control requests. A directly linked writable UAC2 master
mute is cleared before activating the selected alternate. No hardware volume
setting is guessed. Isochronous submissions are made by the same audio worker;
feedback uses two independent batches and descriptor-sized packets. All DMA
buffers for independent transfers occupy separate cache lines.

Kernel inspection confirmed the OHCI control TRB layout: buffer at 0x14,
requested bytes at 0x18, actual completed data bytes at 0x1c, setup packet at
0x20. Compile-time offset checks enforce the layouts used here. The queue
routine is treated as void, not as a status-returning operation; callback
status and actual byte count determine success. No equivalent EHCI guarantee
is assumed.

This is not a proof of race-free removal or hardware playback. Transfer buffers
remain allocated on timeout/removal, and reattachment is blocked. Full hotplug
support still requires establishing the kernel's drain/cancellation contract.
Do not unload the module while transfers might remain outstanding.

The development diagnostic array can be located from the matching linker map:
32=setup stage, 33=last request type/request/value, 34=index, 35=completion
status, 36=actual bytes, 37=clock error, 38=setup error, 39=selected interface/
alternate/subslot/valid bits, 40..43=feedback packet statuses, 44..47=lengths,
48..51=normalized rates, 52=accepted feedback count, 63=build marker 0x55414331.
Stage/error evidence, not a connection notification alone, determines the next
diagnostic action. Existing counters 13/15 count feedback/playback callbacks.

## Implemented

`src/uac_descriptors.{h,cpp}` discovers independent stereo PCM candidates for
UAC1 and UAC2. It accepts an actual received byte count, validates descriptor
boundaries before walking them, uses caller-owned bounded output, and has no
Xbox dependencies, allocation, USB requests, or brand/VID/PID branches.

Discovery records configuration/interface/alternate numbers, sample container
width and valid bits, endpoint sizes/intervals, synchronization mode, explicit
feedback association, and the linked terminal/clock. UAC1 rate lists/ranges
are checked for 48 kHz; UAC2 rates remain explicitly unknown until queried.
AudioControl association uses UAC1's interface collection or UAC2's IAD, rather
than a guessed interface number. Missing/ambiguous associations are rejected.

Clock lookup is scoped to an AudioControl interface. Sources, selectors and
multipliers expose their descriptor capabilities and input IDs. Read-only
clocks are represented; selector choices are not guessed. This is entity
lookup, not graph resolution: the runtime must follow the active selector
with cycle detection and validate multiplier ratios before using a clock.

`src/uac_clock.{h,cpp}` now supplies that bounded UAC2 clock state machine.
It reads the selected pin, detects cycles, reads multiplier ratios (only unity
ratios are supported for now), and reads the source's current frequency. A
48 kHz source is retained without writing it. Otherwise, only a writable source
whose queried ranges permit 48 kHz receives SET_CUR, followed by GET_CUR
verification. Clock validity is checked when readable. It neither changes
selector pins nor probes unsupported controls. Invalid clock reports fail
closed; delayed clock-settling retries remain future work.

The clock component returns value-copy control requests with unique tokens;
the caller feeds it completion status and actual transferred lengths. It has
1-second per-request and 5-second overall deadlines, driven by caller ticks.
Cancel and failure stop the sequence; late/duplicate completions cannot advance
it. It must be used on a serialized owner thread, not concurrently from USB
callbacks. Tokens are not recycled (exhaustion fails closed). It does not
select an alternate, unmute a feature unit, or start streaming.

Transport buffers must be independent of this object's request/response state.
Cancellation here is LOGICAL cancellation, not a claim that the USB host has
stopped DMA. A transport must retain each outstanding request and its buffers
until completion or a verified drain operation. The current Xbox experimental
runtime does not yet establish that contract; this test build instead prohibits
reuse for the rest of the boot.

The portable module currently discovers stereo Type I PCM in 2/3/4-byte
containers, adaptive/synchronous OUT or asynchronous OUT with explicit
feedback. It does not implement implicit feedback, UAC3, compressed formats,
resampling, or mixing. Discovery does not establish transport compatibility;
the runtime must check speed, bandwidth, supported packet sizes and intervals.

## Tests

Run `bash tools/test_uac.sh` (Clang with ASan and UBSan; override `CXX` if needed).
In a ptrace-based sandbox where LeakSanitizer cannot run, use
`ASAN_OPTIONS=detect_leaks=0 bash tools/test_uac.sh`. Address and undefined-behavior
checks remain enabled. The production module is also C++98-compatible for the
XDK compiler; tests use C++11 for synthetic descriptor fixture construction.

Fixtures test non-default interface/endpoint numbers, isolated alternate
settings, output capacity exhaustion, short buffers, bad descriptor lengths,
clock access capabilities, selectors/multipliers, AudioControl scoping,
duplicate clock IDs, invalid endpoint/format fields, and byte mutations.
These are synthetic fixtures, not device captures.
Clock tests also exercise read-only versus writable sources, request fields,
range-driven writes, readback mismatch, invalid clocks, selector pins and cycles,
unity/non-unity multipliers, malformed responses, cancellation/restart, duplicate
callbacks, and timeout across the timer wrap boundary.

## Remaining integration gates

1. Validate the integrated control sequence and real response lengths on Xbox.
   Extend feature-unit traversal beyond the directly linked master mute path
   only when needed by a descriptor-supported topology.
2. Transport: independently queued playback and feedback, descriptor-sized
   feedback reads, one playback timing accumulator, correct PCM packing, and
   explicit rejection of unsupported host-controller capabilities.
3. Lifecycle: prove cancellation/draining semantics of the Xbox API before
   recycling transfer objects; suppress old-generation callbacks and new
   submissions during teardown. No whole-controller resets.
4. Restore zero-touch boot and general hotplug only after lifecycle validation.
5. Validate known-working UAC1 playback, then UAC2, hotplug and boot. A successful
   USB completion alone does not prove audible output.

## Reference architecture

This module is original code, informed by the USB Audio descriptor model and
Linux's separation of format discovery, clock control, and endpoint streaming:

- [Linux stream.c](https://github.com/torvalds/linux/blob/master/sound/usb/stream.c)
- [Linux clock.c](https://github.com/torvalds/linux/blob/master/sound/usb/clock.c)
- [Linux endpoint.c](https://github.com/torvalds/linux/blob/master/sound/usb/endpoint.c)

Linux's host-controller implementation is not interchangeable with the Xbox
transport. Hardware validation remains necessary; device-specific protocol
captures are optional diagnostics, not the source of the generic design.
