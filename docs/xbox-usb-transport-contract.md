# Xbox USB transport audit (retail kernel 17559)

## Result and scope

The current worker-thread submission model lacks the serialization used by the
kernel USB stack. Direct submission from `AudioWorker` is not a supported
assumption: open, queue and close routines manipulate shared host-controller
state, while completions run in a kernel DPC. A plugin-private mutex does not
serialize against those kernel routines. This is an identified correctness
gap and a plausible freeze mechanism, **not a proven explanation of the
observed freeze**; no crash dump or failing instruction was recovered.

This audit used read-only inspection of the running kernel, its export table,
live controller objects, the plugin source, and the references below. No
kernel modifications, deployments, device requests or resets were performed.
Addresses below identify evidence for this kernel only, not portable APIs.
No kernel binary or disassembly capture is included in this repository.

## Evidence ledger

| Requirement | Observed evidence | Consequence |
| --- | --- | --- |
| Serialize USB mutations with its DPC execution domain | Ordinal 895 at `0x800d82c0` tail-calls OHCI queue `0x800dc9a8`. Queue pops global ITDs at `0x801a7f88`, updates endpoint tail and outstanding count without a surrounding lock. Completion `0x800dcce0` returns ITDs to the same list and decrements the same count. | Do not call these routines independently from an arbitrary worker. The memory-management helper's internal lock does not span these mutations. |
| USB completion context | OHCI ISR `0x800dd518` queues the DPC at controller+0x14 with `KeInsertQueueDpc`. DPC `0x800dd470` calls done-list processing `0x800dcd90`, which calls `0x800dcce0`. | Callback execution inherits the USB DPC context. Keeping a worker on one CPU at normal IRQL does not itself exclude that DPC. |
| Controller affinity | Initialization `0x800dd850` calls `KeInitializeInterrupt` with affinity mask 4. The initializer stores the selected processor at interrupt+0x13. All four live controller objects have processor 2 there. Their DPC target byte is zero, so ISR queuing uses the current processor. | The inspected stack's normal completion domain is logical processor/hardware thread 2, not an arbitrary free core. Validate this at runtime rather than assuming it on every kernel. |
| Supported dispatch mechanism | Export 886 (`KeCallAndWaitForDpcRoutine`) resolves to `0x80071d00`; 885 to `0x800719f0`. USB initialization itself calls 886 with processor argument 2. Export 757's RVA is zero on this kernel despite its name in external export lists. | Resolve actual exports, never trust the name alone. A passive worker can marshal a short operation through 886; do not use unavailable 757 or call a blocking dispatcher from a DPC. |
| First-frame scheduling | `0x800dca90..0x800dcabc` uses the endpoint's next frame only if another transfer is pending and that frame is still in the future. Otherwise it chooses the current OHCI frame. | The API supplies no explicit future start-frame argument. A lone submission can have its first frame expire before the controller accesses it. This is consistent with, but does not uniquely prove the cause of, the observed first-packet `not accessed` result. |
| Isochronous interval | OHCI endpoint open `0x800dc310` forces interval 1 for transfer type 1 at `0x800dc3b8`. | Changing the passed interval to 2 did not change this path's interval. Limit support accordingly instead of claiming arbitrary descriptor intervals. |
| Completion data lifetime | `0x800dcce0` builds lengths/status arrays on its stack and invokes the callback through `0x800d5d20`. | Copy completion metadata before returning. Never queue those array pointers to a worker. Normal OHCI ITD completion encodes at most eight packets. |
| Control actual length | Control queue initializes TRB+0x1c; completion `0x800dcb48` accumulates completed data bytes there. Requested length remains at +0x18. | Validate actual length, not allocation/request length. Current compile-time layout checks cover +0x1c actual and +0x20 setup. |
| Close is asynchronous | Ordinal 750 at `0x800d8238` routes to `0x800dc590`, links a close request onto controller+0x3c, unlinks the endpoint, marks it skipped, and enables deferred processing. `0x800dd078` processes it later. | Queue-close return is not a drain barrier. Use a dedicated close-request object and wait for its callback. |
| Cancellation precedes close callback in inspected path | Deferred processing gathers isoch TRBs via `0x800dcf88`, calls cancellation completion through `0x800d5d68`, then returns endpoint storage and invokes close callbacks at `0x800dd354`. | Count every normal/cancelled transfer completion and close completion before reuse. Cancellation uses the isoch callback shape, with cancelled statuses and zero lengths. |
| Queue resource assumptions | Isoch queue consumes global ITD entries without a graceful failure return or adequate null handling after free-list allocation. | Use a small bounded queue. Never interpret the void queue call as allocation success or retry blindly. Global resource exhaustion remains a kernel-level limitation. |

## DPC helper ABI established by inspection

Export 886 accepts five arguments: routine, deferred context, target processor,
system argument 1, system argument 2. It creates a DPC and completion event,
sets the target to processor+1 in the DPC object, queues it, and waits for the
wrapper to signal that the routine has returned. The wrapper at `0x80071960`
replaces argument 2 with the supplied context and preserves the normal four-
argument DPC callback shape:

```
routine(dpc, context, system_argument_1, system_argument_2)
```

Its return is the wait result, **not the operation result**. Store the operation
result in adapter-owned context. It waits for dispatch completion, not USB I/O
completion. Do not sleep, wait for USB completion, notify UI, perform file I/O,
or call this blocking helper recursively inside the dispatched routine.

For sustained playback an owned `KeInitializeDpc`/`KeInsertQueueDpc` work item
can avoid a blocking dispatch per batch. That is a later optimization: preserve
the same processor domain and lifecycle rules. Do not patch the kernel's DPC
object or hardware frame counter to get a different schedule.

## Concrete adapter design

1. **Capability check:** resolve ordinals and validate supported kernel, OHCI
   routing, and the common controller/DPC processor domain. Fail closed if the
   expected context cannot be established.
2. **Preparation worker:** produce PCM and immutable request descriptions away
   from USB execution. It must not call raw open/queue/close functions.
3. **USB dispatch:** marshal open, control submission, isoch submission and close
   to the verified USB DPC domain. Validate the attachment generation and stop
   flag again inside that domain, immediately before dereferencing a handle.
   Perform only bounded, nonblocking work there.
4. **Transfer ownership:** `free -> prepared -> submitted -> completed -> free`.
   Mark submitted before the host call. Once submitted, buffer contents, TRB and
   callback identity cannot change. Callbacks copy results and publish completion.
   Submission return never frees a slot. Give close requests separate storage.
5. **Stop:** prohibit new submissions in the USB domain, initiate asynchronous
   close, process cancellation callbacks, and wait outside DPC context. Reclaim
   only after all transfer and close completions. A deadline reports failure;
   it does not authorize freeing hardware-owned storage.
6. **Removal:** invoke the original remove completion exactly once, in the USB
   domain, only after transfer ownership and every endpoint-close request have
   drained. A generation tag alone cannot make a stale device pointer or DMA
   buffer safe. A timeout remains stopped and retains all storage.

Keep audio algorithm state separate from this adapter. Descriptor parsing,
clock selection, feedback decoding, sample packing and packet timing should
remain portable and testable. `snd-usb-audio` provides that model; it does not
replace the Xbox host-controller contract.

## What is still unproved

- The original freeze's exact instruction and cause.
- Shutdown ordering on kernels or host-controller paths outside the validated
  retail-17559 full-speed OHCI scope.
- Whether all shutdown/error paths preserve the normal common-processor domain.
- Buffer residency/coherency and physical layout requirements for every memory
  allocation class. The helper at `0x8007fc90` must not be casually described as
  a universal cache flush: its inspected high-address path returns without
  iterating the buffer. Cache-line separation alone is not proof of DMA safety.
- Safe global ITD capacity under other active USB devices. Bounded plugin use
  helps, but cannot promise an infallible host controller.
- Actual feedback content/format on Xbox and long-run queue continuity.

These uncertainties must be represented as scope limits, not hidden by sleeps,
exceptions, host resets, or device-specific conditionals.

## Feedback-IN short-packet audit

The stage-4 single-feedback-batch test (marker `0x55414336`) completed once
without losing debug responsiveness. Attach, dispatch and completion again
reported processor 2 / IRQL 2. Packet statuses were `0xc005100e`, then three
`0xc0050003`; actual lengths were 0, 0, 0, 3. No OUT transfer was submitted.

Read-only inspection established that OHCI completion at `0x800dcce0` extracts
the packet status word's top nibble and indexes a DWORD table at `0x800447f8`.
Entry 9 is `0xc0050003`: this is **OHCI DataUnderrun**, not condition code 3
(DataToggleMismatch, whose table value is `0xc0051003`). Entry 14 maps to
`0xc005100e`. The routine passes the low 11 bits as length, except that it
forces length zero for condition 14. It does not normalize short IN packets.

The submitted feedback TRB requested 16 bytes, four packets of four bytes.
The completed buffer's last packet slot contained `00 00 0c 00`; with actual
length 3, its payload is `00 00 0c`, integer `0x0c0000`. Interpreted as Q10.14
samples per full-speed frame, that is exactly 48 samples/frame. This is
consistent with the verified 48 kHz clock; it does not prove sustained feedback
or audible playback. The preceding zero-length packets provide no rate sample.

Linux `ohci-q.c:td_done` explicitly converts DataUnderrun to success for
isochronous IN, preserving the actual length. Our feedback callback currently
requires raw status zero and therefore discards this otherwise usable short
packet. This is an established host-status adaptation gap, not a reason to
change descriptor packet sizes, introduce device IDs, or reset the controller.

Next implementation gate: add a tested transport-level interpretation for
isochronous IN short packets only. Preserve raw diagnostics, validate actual
length against requested capacity, ignore empty feedback, and never accept
CRC/NotAccessed/other errors. Repeat the same bounded test before progressing
to a bounded playback/feedback queue. No kernel writes or further transfers
were needed for this audit.

## Implementation and validation gates

### First audible-test investigation

The bounded tone build completed 501 batches per direction with only the first
packet missed, no other packet errors, successful setup including unmute, and
no outstanding transfers. The user did not clearly hear a tone. This is not
verified audible playback.

Read-only inspection of the selected descriptors confirmed stereo Type I PCM,
four-byte subslots and 24 valid bits. The compiled sample writer emits
little-endian, left-aligned samples with unused low bits cleared, consistent
with Linux format.c selecting S32_LE for four-byte PCM subslots. There is no
evidence here supporting a switch to right-aligned 24-bit packing.

The linked feature unit advertises readable/writable master mute and volume;
individual channel controls are absent. Setup writes master mute but does not
read back mute or query current volume. A successful write is not proof that
output attenuation permits an audible signal. Next instrumentation should use
descriptor-gated GET_CUR reads without changing volume.

Both retained OUT buffers were zero after the test. This is expected because
the final part of the test deliberately sends silence and recycles the slots;
it provides no evidence of what the controller saw during the earlier tone.
No nonzero-payload snapshot or physical/DMA visibility comparison was retained.
The next bounded test should retain a small pre-submission tone sample and
nonzero/peak counters without keeping a full audio capture.

The raw queue's helper at `0x8007fc90` skips page-accounting for the plugin's
high-address buffers and does not perform a data-cache flush on that path.
That absence alone does not prove incoherency: the mapping attributes and USB
DMA coherency contract remain to be established. Do not add cache operations,
change memory aliases, or claim stale silence as the root cause without that
evidence. No requests, volume writes, resets or kernel changes were performed
during this inspection.

### Activation timeout inspection

The first feedback-paced test (`0x5541433b`) reported plugin timeout `0xe004`
while SET_INTERFACE requested alternate 1 on interface 2. No isoch transfers
were submitted. Subsequent read-only inspection found the control completion
mailbox set (`done=1`, status zero), actual length zero as expected for this
request, and endpoint head equal to tail with neither halted nor skipped set.
The TRB's setup bytes matched the intended request. Debug access remained live.

Thus the control request did complete successfully, but the setup worker had
already stopped and does not consume completion after its stop flag is set.
The current diagnostics record worker-consumed results, not late completions;
they misleadingly retain the preceding clock read's status/length. No callback
timestamp was recorded, so exact completion delay and a deadline-boundary race
cannot be distinguished from this snapshot. The device-versus-host contribution
to the delay remains unproved. An earlier activation returned genuine
device-not-responding (`0xc0051005`); this finding does not explain that result.

Linux's `include/linux/usb.h` defines 5000 ms control GET/SET timeouts, whereas
this plugin's non-clock setup requests have a 1000 ms deadline. The next scoped
change should timestamp issue/completion/timeout independently and allow a
bounded standard-request deadline consistent with that reference. This is not
a pre-request sleep, retry, reset, or permission to reuse outstanding storage.
Keep clock-state-machine deadlines separate. Never resume this timed-out
attachment by clearing stop flags: lifecycle recovery remains unverified.

### Completion-driven bounded test

Further read-only inspection of `0x800dcd90` confirms that the done-list walk
saves the next descriptor at `0x800dcddc` before calling isoch completion.
Completion decrements endpoint pending count, copies packet results to stack,
returns the descriptor to the free list, and invokes the plugin callback.
After the callback, neither completion nor its wrapper accesses the completed
TRB/descriptor again. This supports immediate isoch requeue in the same DPC
domain on this kernel; removal/close ordering is still a separate gate.

Marker `0x55414339` retains stage 5's duration, packet sizes and queue depth,
but primes all four slots in one DPC invocation and replenishes directly from
completion after consuming packet results and releasing adapter ownership.
The new domain-only submission entry validates context, stop state and ownership
without calling the blocking dispatcher. The worker only observes completion
accounting. Budget and submission counters have one DPC-domain owner; a slot
stays busy across resubmission. Deadline/cap expiry prevents resubmission, and
buffers remain allocated. No PCM generation, allocation, UI or waits occur
inside these callbacks. Host tests model callback-side ownership reuse and
budget-expiry drain; they cannot validate kernel execution or timing.

The preceding worker-replenished test completed 48 batches in each direction,
with 10 OUT and 8 IN not-accessed packets and no other packet errors. All
submissions completed, but continuity was not established. The new test must
compare those counters without increasing buffering. Hardware validation is
pending; startup's current-frame scheduling behavior is unchanged.

**Implemented, awaiting hardware validation:** `xbox_usb_transport.cpp` marshals
open/default-open, control submission and isoch submission through export 886.
It validates the common live controller affinity and default DPC targeting, and
requires the observed processor/IRQL to match that domain at attach, dispatch
and completion. The worker is required to be at passive IRQL before dispatch.
All raw worker open/queue calls in audio.cpp now use the adapter. No continuous
playback or additional test transfers have been enabled.

`transfer_ownership.h` supplies bounded DPC-owned accounting. Submission records
ownership before entering the host; completion clears it, and removal stops new
work without inventing completions. For physical removal, the replacement path
stops submissions, queues dedicated close requests for every opened data
endpoint and the default control endpoint, then waits for all normal/cancellation
and close completions. After a one-second settle interval it calls the original
remove completion exactly once in the USB domain, clears static request state,
and re-arms with the incremented generation. Failure to drain remains stopped
and retains all request storage.
The bookkeeping assumes the host completes each submission once: a duplicated
callback after the same TRB has already been resubmitted cannot be distinguished
using the pointer alone. The current single-batch test never resubmits its TRB.

The dispatched request context is stack-owned until export 886 returns. That
export has no caller timeout; it must never be called from DPC context. USB
request deadlines still do not free submitted storage. This is not a promise
that a stalled kernel/DPC queue can be recovered by the plugin.

Build marker `0x55414335` identifies this adapter test. Diagnostics 56, 61 and
62 record attach, dispatch and completion context respectively, encoded as
`(processor << 16) | IRQL`. Expected on the inspected console: `0x00020002`.
These record observed values; hardware validation must check all three.

**Host tests:** exercise synchronous completion during submission, delayed
completion, duplicate completion, stop before dispatch, stale generation,
cancellation-before-close, and timeout with retained ownership. Assertions must
show no double submission, no early reuse, and no worker invocation of raw USB.

Host tests cover immediate/delayed completion accounting, duplicate completion
before reuse, capacity, invalid generations, stop-before-dispatch, retained
ownership after stop, out-of-order close callbacks, outstanding-transfer gating,
duplicate close completion, one-shot finalization, and zero-endpoint cleanup.
They do not execute Xbox DPCs; the ordering used by the adapter comes from the
kernel audit and the hardware results below.

**Hardware gates, in order:**

1. Repeat the existing one-batch test through the adapter; verify the recorded
   execution context and exactly one completion. Preserve per-packet results.
2. Submit one bounded feedback batch through the same adapter; verify actual
   lengths and statuses before interpreting data. Do not introduce retries.
3. Exercise a fixed-duration, bounded queue of silence with a documented stop
   policy. Validate completion accounting and queue continuity independently
   from feature controls and render capture.
4. Add capability-driven unmute, then captured system PCM, as separate changes.
5. Validate repeated physical hotplug across the known UAC1 and UAC2 devices,
   including both swap directions. Never reset the shared USB controller.

This final gate passed on hardware with repeated SABRENT UAC1 and AirPods Max
UAC2 swaps in both directions. Each removal re-armed on its first bounded check
with zero outstanding ownership; Aurora and XBDM remained responsive.

Later four-device testing exposed cumulative endpoint-open failure because the
old path did not close endpoints. Calling the original
`UsbdRemoveDeviceComplete` (ordinal 751) immediately from the claimed-handle
hook caused the console to become unresponsive on the first physical removal.
The replacement lifecycle therefore queues asynchronous closes first and defers
the original completion until transfer and close ownership have both drained.

That lifecycle passed three consecutive physical removals without rebooting:
two SPACETOUCH UAC1 removals each completed two closes (OUT and default), then
an AirPods Max UAC2 removal completed three (OUT, feedback IN, and default).
Every cycle reached zero transfer and close ownership, invoked original removal
once on processor 2 at IRQL 2, and reported no cleanup error. SPACETOUCH reopened
after the first removal, and AirPods Max streamed successfully after the two UAC1
cycles. Aurora and XBDM remained responsive. Shared-controller and port resets
remain prohibited.

The prior isolated successes are useful evidence, not a substitute for these
contracts. No further hardware variation is justified merely by changing a
packet count or delay until the corresponding requirement is understood.

## Prior art used

- [hiddriver360](https://github.com/EinTim23/hiddriver360), locally inspected
  revision `6159866ff0049d379238751d50a19409abf08e9d`: setup originates in its
  add hook and interrupt reads are requeued from completion callbacks. It does
  not establish that arbitrary worker-thread isoch calls are safe.
- [Xenia kernel export names](https://github.com/xenia-project/xenia/blob/master/src/xenia/kernel/xboxkrnl/xboxkrnl_table.inc): names/ordinals were cross-checked
  against the running kernel; a name is not evidence an export is present.
- [Linux endpoint.c](https://github.com/torvalds/linux/blob/master/sound/usb/endpoint.c): separation of data/sync endpoints and lifecycle management.
- [Linux OHCI queue implementation](https://github.com/torvalds/linux/blob/master/drivers/usb/host/ohci-q.c): host-controller responsibilities are separate from
  audio-class logic. Its locking and URB lifecycle cannot simply be assumed for
  an Xbox transfer routine with a different calling contract.
