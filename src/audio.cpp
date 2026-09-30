// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio.h"
#include "diagnostics.h"
#include "xbox_usb_transport.h"
#include "isoch_result.h"
#include "stream_test_budget.h"
#include "feedback_pacer.h"
#include "control_deadline.h"
#include "test_tone.h"
#include "pcm_packet.h"
#include "playback_pacer.h"
#include "playback_profile.h"
#include "uac_clock.h"
#include "uac_descriptors.h"
#include "uac_setup_policy.h"

#include <string.h>
#if USB_AUDIO360_DEBUG_API
#include "debug_command.h"
#endif

// Release builds stream captured system PCM. Debug builds open the same
// descriptor-selected endpoints, then wait for bounded mailbox smoke tests.
#ifndef USB_AUDIO360_TEST_STAGE
#if USB_AUDIO360_DEBUG_API
#define USB_AUDIO360_TEST_STAGE 5
#else
#define USB_AUDIO360_TEST_STAGE 0
#endif
#endif

extern "C" LONG XexGetProcedureAddress(HANDLE module, DWORD ordinal,
                                        PVOID address);
extern "C" volatile DWORD UsbAudioDiagnostic[64] = {0};
extern "C" volatile DWORD UsbAudioControlDiagnostic[16] = {0};
extern "C" volatile DWORD UsbAudioToneDiagnostic[16] = {0};
extern "C" volatile DWORD UsbAudioActivationDiagnostic[16] = {0};
extern "C" volatile DWORD UsbAudioCleanupDiagnostic[16] = {0};
// First 32 setup requests, eight words each; never wraps or owns USB storage.
extern "C" volatile DWORD UsbAudioSetupTrace[256] = {0};
#if USB_AUDIO360_DEBUG_API
// ABI: magic, version, build, command, acknowledgement, result, run, stopped,
// followed by pointers to main/control/tone/activation/trace diagnostics.
extern "C" volatile DWORD UsbAudioDebugMailbox[16] = {0};
#endif

namespace {

const int kIsochRingDepth = 2;
const int kIsochPacketCount = 4;
const int kFramesPerUsbPacket = 48;
const int kChannels = 2;
const int kMaxBytesPerSample = 4;
const int kMaxFramesPerUsbPacket = 49;
const int kMaxPacketBytes =
    kMaxFramesPerUsbPacket * kChannels * kMaxBytesPerSample;
const int kMaxBatchBytes = kMaxPacketBytes * kIsochPacketCount;
const int kIsochBufferStride = (kMaxBatchBytes + 127) & ~127;
// Descriptor-sized feedback reads; two independent batches keep the endpoint
// queued while the audio worker processes completions.
const int kFeedbackTransferBytes = 4;
const int kFeedbackPacketCount = 4;
const int kFeedbackRingDepth = 2;
const int kPcmRingFrames = 4096;
const int kPcmTargetFrames = 768;
const int kVolumeFeedbackSamples = 1440;
const DWORD kRemovalSettleMs = 1000;
const DWORD kRearmRetryMs = 250;
const DWORD kRearmDeadlineMs = 5000;
const DWORD kNotifyTypeCustom = 80;

const SHORT kVolumeFeedbackWave[48] = {
     0,  157,  311,  459,  600,  731,  849,  952,
  1039, 1109, 1159, 1190, 1200, 1190, 1159, 1109,
  1039,  952,  849,  731,  600,  459,  311,  157,
     0, -157, -311, -459, -600, -731, -849, -952,
 -1039,-1109,-1159,-1190,-1200,-1190,-1159,-1109,
 -1039, -952, -849, -731, -600, -459, -311, -157
};

enum NotificationEvent {
  kNotificationNone = 0,
  kNotificationConnected = 1,
  kNotificationDisconnected = 2,
};

struct UsbTrb {
  DWORD endpoint;
  DWORD callback;
  DWORD saved_endpoint;
  BYTE padding[4];
  BYTE flags;
  BYTE controller_index;
  BYTE pad2;
  BYTE endpoint_index;
  void* buffer;
  DWORD length;
};

struct UsbIsochTrb {
  DWORD endpoint;
  DWORD callback;
  DWORD saved_endpoint;
  BYTE padding[4];
  void* buffer;
  DWORD length;
  BYTE packet_count;
  BYTE controller_index;
  BYTE pad2;
  BYTE endpoint_index;
};

struct UsbPacket {
  BYTE request_type;
  BYTE request;
  WORD value;
  WORD index;
  WORD length;
};

struct UsbControlTrb {
  UsbTrb trb;
  DWORD actual_length;  // OHCI writes completed data bytes at TRB + 0x1c.
  UsbPacket packet;
};
typedef char ControlActualOffset[offsetof(UsbControlTrb, actual_length) == 0x1c ? 1 : -1];
typedef char ControlSetupOffset[offsetof(UsbControlTrb, packet) == 0x20 ? 1 : -1];
typedef char IsochBufferOffset[offsetof(UsbIsochTrb, buffer) == 0x10 ? 1 : -1];

struct RenderMecClient {
  DWORD callback;
  DWORD context;
};

struct __declspec(align(128)) RenderCaptureBuffer {
  float samples[6 * 256];
};

typedef LONG (*RegisterMecFn)(RenderMecClient*, void**);
typedef LONG (*CaptureRenderFrameFn)(void*, float*);
typedef VOID (NTAPI *NotifyQueueUiFn)(DWORD, DWORD, ULONGLONG, PWCHAR, PVOID);

static const AudioHostApi* g_api = 0;
static void* g_last_handle = 0;
static volatile LONG g_started = 0;
static volatile LONG g_control_pending = 0;
static volatile LONG g_streaming = 0;
static volatile LONG g_stopping = 0;
static volatile LONG g_bridge_started = 0;
static volatile LONG g_notification_event = kNotificationNone;
static volatile LONG g_notification_shown = 0;
static volatile LONG g_diagnostic_streaming = 0;
static volatile LONG g_volume_percent = 100;
static volatile LONG g_volume_feedback_pending = 0;
static volatile LONG g_rearm_pending = 0;
static DWORD g_removed_at = 0;
static DWORD g_rearm_last_attempt = 0;
static volatile LONG g_isoch_slot_busy[kIsochRingDepth];
static volatile LONG g_pcm_write = 0;
static volatile LONG g_pcm_read = 0;
static volatile LONG g_feedback_busy[kFeedbackRingDepth];
static uac::PlaybackPacer g_playback_pacer;
static void* g_io_handle = 0;
static uac::Format g_format;
static uac::ClockSetup g_clock;
static uac::ClockRequest g_clock_request;
static volatile LONG g_control_done = 0;
static LONG g_control_status = 0;
static DWORD g_control_issued = 0;
static DWORD g_control_deadline = 1000;
enum SetupStage { kHeader, kDescriptor, kConfiguration, kInactive,
                  kClock, kUac1Rate, kUnmute, kActive, kFinished, kOpening,
                  kReadMute, kReadVolume, kVerifyInterface, kVerifyClock,
                  kVerifyMute, kVerifyVolume, kUac1FallbackInactive,
                  kUac1FallbackRate };
static BYTE g_read_feature_id = 0;
static BYTE g_read_feature_controls = 0;
static bool g_uac1_rate_programmed = false;
static bool g_uac1_rate_fallback = false;
static SetupStage g_setup_stage = kHeader;
static DWORD g_endpoint_phase_time = 0;
static int g_endpoint_phase = 0;
const DWORD kStreamTestDurationMs = 2000;
static usb_transport::StreamTestBudget g_test_budget(kStreamTestDurationMs, 1024);
static uac::FeedbackPacer g_test_pacer;
static unsigned g_test_tone_frame = 0;
static DWORD g_test_started = 0;
static DWORD g_test_drain_started = 0;
static DWORD g_test_duration = kStreamTestDurationMs;
static volatile LONG g_test_cancelled = 0;
#if USB_AUDIO360_DEBUG_API
static volatile LONG g_debug_mailbox = 0; // 0=free, 1=writer, 2=ready, 3=worker.
static usb_debug::Request g_debug_request;
static volatile LONG g_debug_result = 0; // 0=none, 1=accepted, 2=started, -1=rejected.
static volatile LONG g_debug_run = 0;
static bool g_debug_silent = false;
static bool g_debug_controls = false;
#endif
static bool SubmitBoundedSlot(unsigned direction, int slot);
static bool SubmitContinuousSlot(unsigned direction, int slot);

// DPC-owned diagnostic: 1-based completion ordinal and 0-based packet index.
// Zero means no miss. Retain first/last locations, not just aggregate counts.
static void RecordMiss(unsigned direction, unsigned packet) {
  DWORD batch = UsbAudioDiagnostic[direction ? 13 : 15];
  DWORD location = (batch << 3) | packet;
  if (!UsbAudioDiagnostic[16 + direction])
    UsbAudioDiagnostic[16 + direction] = location;
  UsbAudioDiagnostic[18 + direction] = location;
}

static UsbControlTrb g_control;
static UsbIsochTrb g_isoch[kIsochRingDepth];
static UsbIsochTrb g_feedback_isoch[kFeedbackRingDepth];
static WORD g_packet_lengths[kIsochRingDepth][kIsochPacketCount];
static WORD g_feedback_packet_lengths[kFeedbackRingDepth][kFeedbackPacketCount];
static __declspec(align(128)) BYTE
    g_usb_packets[kIsochRingDepth][kIsochBufferStride];
static __declspec(align(128)) BYTE
    g_feedback_packets[kFeedbackRingDepth][128];
static __declspec(align(128)) LONG g_pcm_ring[kPcmRingFrames][kChannels];
static __declspec(align(128)) BYTE g_configuration_descriptor[0x400];
static __declspec(align(128)) BYTE g_control_data[256];
static BYTE g_sample_rate[3] = {0x80, 0xBB, 0x00};
static BYTE g_unmuted = 0;
static AudioProfile g_profile;
static RenderMecClient g_mec_client;
static RenderCaptureBuffer g_render_frame;
static void* volatile g_mec_handle = 0;
static CaptureRenderFrameFn g_capture_render_frame = 0;
static NotifyQueueUiFn g_notify_queue_ui = 0;
static LONG g_volume_feedback_remaining = 0;
static LONG g_volume_feedback_phase = 0;

static void ResetDeviceState() {
  DWORD marker = UsbAudioDiagnostic[63];
  DWORD rearm_attempts = UsbAudioDiagnostic[58];
  DWORD rearm_pending = UsbAudioDiagnostic[59];
  DWORD rearm_result = UsbAudioDiagnostic[60];
  memset((void*)UsbAudioDiagnostic, 0, sizeof(UsbAudioDiagnostic));
  memset((void*)UsbAudioControlDiagnostic, 0,
         sizeof(UsbAudioControlDiagnostic));
  memset((void*)UsbAudioToneDiagnostic, 0, sizeof(UsbAudioToneDiagnostic));
  memset((void*)UsbAudioActivationDiagnostic, 0,
         sizeof(UsbAudioActivationDiagnostic));
  memset((void*)UsbAudioSetupTrace, 0, sizeof(UsbAudioSetupTrace));
  UsbAudioDiagnostic[1] = 1;
  UsbAudioDiagnostic[58] = rearm_attempts;
  UsbAudioDiagnostic[59] = rearm_pending;
  UsbAudioDiagnostic[60] = rearm_result;
  UsbAudioDiagnostic[63] = marker;
  g_clock.Cancel();
  g_last_handle = 0;
  g_io_handle = 0;
  memset(&g_profile, 0, sizeof(g_profile));
  memset(&g_format, 0, sizeof(g_format));
  memset(&g_clock_request, 0, sizeof(g_clock_request));
  memset(&g_control, 0, sizeof(g_control));
  memset(g_isoch, 0, sizeof(g_isoch));
  memset(g_feedback_isoch, 0, sizeof(g_feedback_isoch));
  memset(g_packet_lengths, 0, sizeof(g_packet_lengths));
  memset(g_feedback_packet_lengths, 0, sizeof(g_feedback_packet_lengths));
  memset(g_configuration_descriptor, 0, sizeof(g_configuration_descriptor));
  memset(g_control_data, 0, sizeof(g_control_data));
  g_sample_rate[0] = 0x80;
  g_sample_rate[1] = 0xbb;
  g_sample_rate[2] = 0;
  g_unmuted = 0;
  g_read_feature_id = 0;
  g_read_feature_controls = 0;
  g_uac1_rate_programmed = false;
  g_uac1_rate_fallback = false;
  g_setup_stage = kHeader;
  g_endpoint_phase = 0;
  g_endpoint_phase_time = 0;
  g_control_status = 0;
  g_control_issued = 0;
  g_control_deadline = 1000;
  g_playback_pacer = uac::PlaybackPacer();
  g_test_pacer = uac::FeedbackPacer();
  g_test_budget = usb_transport::StreamTestBudget(kStreamTestDurationMs, 1024);
  g_test_tone_frame = 0;
  g_test_started = 0;
  g_test_drain_started = 0;
  g_test_duration = kStreamTestDurationMs;
  g_volume_feedback_remaining = 0;
  g_volume_feedback_phase = 0;
  for (int slot = 0; slot < kIsochRingDepth; ++slot)
    InterlockedExchange(&g_isoch_slot_busy[slot], 0);
  for (int slot = 0; slot < kFeedbackRingDepth; ++slot)
    InterlockedExchange(&g_feedback_busy[slot], 0);
  InterlockedExchange(&g_control_pending, 0);
  InterlockedExchange(&g_control_done, 0);
  InterlockedExchange(&g_streaming, 0);
  InterlockedExchange(&g_stopping, 0);
  InterlockedExchange(&g_bridge_started, 0);
  InterlockedExchange(&g_diagnostic_streaming, 0);
  InterlockedExchange(&g_volume_feedback_pending, 0);
  InterlockedExchange(&g_test_cancelled, 0);
#if USB_AUDIO360_DEBUG_API
  InterlockedExchange(&g_debug_mailbox, 0);
  InterlockedExchange(&g_debug_result, 0);
  memset(&g_debug_request, 0, sizeof(g_debug_request));
  g_debug_silent = false;
  g_debug_controls = false;
#endif
  InterlockedExchange(&g_pcm_read, g_pcm_write);
  g_rearm_last_attempt = 0;
  InterlockedExchange(&g_rearm_pending, 0);
  InterlockedExchange(&g_started, 0);
}

static WORD Swap16(WORD value) {
  return (WORD)((value >> 8) | (value << 8));
}

static WORD Read16(const BYTE* value) {
  return (WORD)value[0] | ((WORD)value[1] << 8);
}

static DWORD Read32(const BYTE* value) {
  return (DWORD)value[0] | ((DWORD)value[1] << 8) |
         ((DWORD)value[2] << 16) | ((DWORD)value[3] << 24);
}

static bool FillUsbPacket(int slot) {
  uac::PcmPacketBuilder builder(g_usb_packets[slot], sizeof(g_usb_packets[slot]),
      g_profile.bytes_per_sample, g_format.valid_bits,
      g_profile.endpoint_packet_size);
  const int bytes_per_frame = g_profile.bytes_per_sample * kChannels;
  int frame_count = 0;
  for (int packet = 0; packet < kIsochPacketCount; ++packet) {
    unsigned frames = 0;
    if (!g_playback_pacer.Next(&frames)) return false;
    if (!builder.Append(frames, &g_packet_lengths[slot][packet])) return false;
    frame_count += frames;
  }
  LONG read = g_pcm_read;
  LONG write = g_pcm_write;
  LONG available = write - read;
  int copied = available < frame_count ? (int)available : frame_count;

  int byte_offset = 0;
  for (int frame = 0; frame < copied; ++frame) {
    LONG* source = g_pcm_ring[(read + frame) & (kPcmRingFrames - 1)];
    BYTE* destination = &g_usb_packets[slot][byte_offset];
    builder.Stereo(destination, (unsigned)source[0], (unsigned)source[1]);
    byte_offset += bytes_per_frame;
  }
  if (copied < frame_count) {
    memset(&g_usb_packets[slot][byte_offset], 0,
           (frame_count - copied) * bytes_per_frame);
  }
  g_isoch[slot].length = builder.total();
  InterlockedExchange(&g_pcm_read, read + copied);
  return true;
}

static void __cdecl RenderCaptureCallback(void*) {
  void* handle = g_mec_handle;
  CaptureRenderFrameFn capture = g_capture_render_frame;
  if (!handle || !capture || capture(handle, g_render_frame.samples) < 0)
    return;

  LONG write = g_pcm_write;
  LONG read = g_pcm_read;
  if ((DWORD)(write - read) > kPcmRingFrames - 256) return;
  float gain = (float)g_volume_percent / 100.0f;
  if (InterlockedExchange(&g_volume_feedback_pending, 0)) {
    g_volume_feedback_remaining = kVolumeFeedbackSamples;
    g_volume_feedback_phase = 0;
  }

  for (int sample = 0; sample < 256; ++sample) {
    float left = g_render_frame.samples[sample] * gain;
    float right = g_render_frame.samples[256 + sample] * gain;
    if (g_volume_feedback_remaining > 0) {
      LONG elapsed = kVolumeFeedbackSamples - g_volume_feedback_remaining;
      float envelope = 1.0f;
      if (elapsed < 96) envelope = (float)elapsed / 96.0f;
      if (g_volume_feedback_remaining < 240)
        envelope = (float)g_volume_feedback_remaining / 240.0f;
      float feedback =
          ((float)kVolumeFeedbackWave[g_volume_feedback_phase] * 4.0f /
           32767.0f) * gain * envelope;
      left += feedback;
      right += feedback;
      g_volume_feedback_phase = (g_volume_feedback_phase + 1) % 48;
      --g_volume_feedback_remaining;
    }
    if (left > 1.0f) left = 1.0f;
    if (left < -1.0f) left = -1.0f;
    if (right > 1.0f) right = 1.0f;
    if (right < -1.0f) right = -1.0f;
    LONG* destination = g_pcm_ring[(write + sample) & (kPcmRingFrames - 1)];
    destination[0] = (LONG)(left * 2147483647.0f);
    destination[1] = (LONG)(right * 2147483647.0f);
  }
  InterlockedExchange(&g_pcm_write, write + 256);
}

static bool StartRenderCapture() {
  HANDLE kernel = GetModuleHandleA("xboxkrnl.exe");
  RegisterMecFn register_mec = 0;
  if (!kernel || XexGetProcedureAddress(kernel, 800, &register_mec) < 0 ||
      XexGetProcedureAddress(kernel, 802, &g_capture_render_frame) < 0 ||
      !register_mec || !g_capture_render_frame) {
    return false;
  }

  memset(&g_mec_client, 0, sizeof(g_mec_client));
  g_mec_client.callback = (DWORD)RenderCaptureCallback;
  void* handle = 0;
  if (register_mec(&g_mec_client, &handle) < 0 || !handle) return false;
  g_mec_handle = handle;
  return true;
}

// Callbacks only publish completion. All setup and resubmission runs on the
// audio worker; no callback chains and no reuse following removal or timeout.
static LONG __cdecl ControlComplete(DWORD transfer, LONG status) {
  if (transfer != (DWORD)&g_control || !UsbTransportComplete((void*)transfer)) return 0;
  DWORD completed = GetTickCount();
  UsbAudioControlDiagnostic[6] = completed;
  UsbAudioControlDiagnostic[7] = completed - g_control_issued;
  UsbAudioControlDiagnostic[8] = (DWORD)status;
  UsbAudioControlDiagnostic[9] = g_control.actual_length;
  ++UsbAudioControlDiagnostic[10];
  UsbAudioControlDiagnostic[15] = g_stopping ? 1 : 0;
  DWORD sequence = UsbAudioControlDiagnostic[0];
  if (sequence && sequence <= 32) {
    volatile DWORD* row = UsbAudioSetupTrace + (sequence - 1) * 8;
    row[4] = completed - g_control_issued;
    row[5] = (DWORD)status;
    row[6] = g_control.actual_length;
    if ((g_control.packet.request_type & 0x80) && g_control.trb.buffer) {
      DWORD n = g_control.actual_length;
      if (n > g_control.trb.length) n = g_control.trb.length;
      if (n > 4) n = 4;
      const BYTE* data = (const BYTE*)g_control.trb.buffer;
      for (DWORD i = 0; i < n; ++i) row[7] |= (DWORD)data[i] << (8 * i);
    }
  }
  g_control_status = status;
  InterlockedExchange(&g_control_done, 1);
  return 0;
}

static void QueueControl(BYTE type, BYTE request, WORD value, WORD index,
                         WORD length, void* buffer, SetupStage stage) {
  if (g_stopping || !g_io_handle || g_control_pending) return;
  g_setup_stage = stage;
  g_control.packet.request_type = type;
  g_control.packet.request = request;
  g_control.packet.value = Swap16(value);
  g_control.packet.index = Swap16(index);
  g_control.packet.length = Swap16(length);
  g_control.trb.buffer = buffer;
  g_control.trb.length = length;
  g_control.actual_length = 0;
  g_control.trb.flags = 1;  // Accept short IN; validate actual length ourselves.
  g_control.trb.callback = (DWORD)ControlComplete;
  g_control.trb.saved_endpoint = g_control.trb.endpoint;
  g_control_issued = GetTickCount();
  g_control_deadline = usb_transport::ControlDeadline(type);
  ++UsbAudioControlDiagnostic[0];
  if (UsbAudioControlDiagnostic[0] <= 32) {
    volatile DWORD* row = UsbAudioSetupTrace + (UsbAudioControlDiagnostic[0] - 1) * 8;
    row[0] = stage;
    row[1] = (type << 24) | (request << 16) | value;
    row[2] = index; row[3] = length;
    row[5] = 0xffffffff; // No completion yet.
  }
  UsbAudioControlDiagnostic[1] = stage;
  UsbAudioControlDiagnostic[2] = (type << 24) | (request << 16) | value;
  UsbAudioControlDiagnostic[3] = index;
  UsbAudioControlDiagnostic[4] = g_control_issued;
  for (int i = 5; i <= 9; ++i) UsbAudioControlDiagnostic[i] = 0;
  UsbAudioControlDiagnostic[11] = 0;
  UsbAudioControlDiagnostic[12] = 0;
  UsbAudioControlDiagnostic[13] = g_control_deadline;
  UsbAudioControlDiagnostic[15] = 0;
  UsbAudioDiagnostic[32] = stage;
  UsbAudioDiagnostic[33] = (type << 24) | (request << 16) | value;
  UsbAudioDiagnostic[34] = index;
  InterlockedExchange(&g_control_done, 0);
  InterlockedExchange(&g_control_pending, 1);
  LONG submitted = UsbTransportControl(g_io_handle, &g_control);
  UsbAudioControlDiagnostic[5] = GetTickCount();
  if (submitted < 0) {
    g_control_status = submitted;
    InterlockedExchange(&g_control_done, 1);
  }
}

static void SetupFailed(DWORD error) {
  UsbAudioDiagnostic[38] = error;
  if (InterlockedCompareExchange(&g_stopping, 1, 0) == 0)
    DiagnosticsFailure(error, g_setup_stage, UsbAudioDiagnostic[33],
                       UsbAudioDiagnostic[34], g_control_status,
                       g_control.actual_length, g_clock.error(),
                       g_profile, g_format);
  g_clock.Cancel();
  // Outstanding TRBs/buffers stay allocated and are never recycled this boot.
}

static bool SelectProfile(DWORD received) {
  // The bounded 1 KiB configuration can contain many alternates. Discovery
  // retains unsupported candidates so selection can rank compatible profiles.
  uac::Format formats[32];
  size_t count = 0;
  DiagnosticsConfiguration(g_configuration_descriptor, received);
  if (received < 9 || Read16(g_configuration_descriptor + 2) != received)
    return false;
  if (uac::Discover(g_configuration_descriptor, received, formats, 32,
                    &count) != uac::kOk) return false;
  size_t selected = 0;
  if (!uac::SelectFullSpeedPlayback(
          formats, count, g_profile.interface_number, &selected)) return false;
  {
    const uac::Format& f = formats[selected];
    g_format = f;
    memset(&g_profile, 0, sizeof(g_profile));
    g_profile.configuration = f.configuration;
    g_profile.interface_number = f.interface_number;
    g_profile.alternate_setting = f.alternate;
    g_profile.audio_class_version = f.version;
    g_profile.bytes_per_sample = f.sample_bytes;
    g_profile.endpoint_address = f.data.address;
    g_profile.endpoint_packet_size = f.data.max_packet_bytes;
    g_profile.feedback_endpoint_address = f.feedback.address;
    g_profile.feedback_endpoint_packet_size = f.feedback.max_packet_bytes;
    g_profile.control_interface_number = f.control_interface;
    g_profile.clock_source_id = f.clock;
    // Optional writable master mute on a directly linked UAC2 feature unit.
    // Do not issue unsupported controls or assume a fixed entity ID.
    bool in_control = false;
    for (DWORD o = g_configuration_descriptor[0]; o < received;
         o += g_configuration_descriptor[o]) {
      const BYTE* d = g_configuration_descriptor + o;
      if (d[1] == 4)
        in_control = d[2] == f.control_interface && d[3] == 0;
      if (f.version == 2 && in_control && d[1] == 0x24 && d[0] >= 10 &&
          d[2] == 6 && d[4] == f.terminal) {
        if (g_read_feature_id) return false; // Ambiguous direct feature path.
        g_read_feature_id = d[3];
        g_read_feature_controls = (BYTE)Read32(d + 5);
        UsbAudioToneDiagnostic[0] = d[3];
        UsbAudioToneDiagnostic[1] = Read32(d + 5);
        if (uac::ControlWritable(g_read_feature_controls, 1))
          g_profile.feature_unit_id = d[3];
      }
    }
    UsbAudioDiagnostic[39] = (f.interface_number << 24) | (f.alternate << 16) |
                             (f.sample_bytes << 8) | f.valid_bits;
    DiagnosticsSelected(g_profile, g_format);
    return true;
  }
  return false;
}

static LONG __cdecl FeedbackComplete(DWORD transfer, DWORD* statuses,
                                     WORD* lengths) {
  int slot = -1;
  for (int i = 0; i < kFeedbackRingDepth; ++i)
    if (transfer == (DWORD)&g_feedback_isoch[i]) slot = i;
  if (slot < 0 || !UsbTransportComplete((void*)transfer)) return 0;
  ++UsbAudioDiagnostic[13];
  for (int packet = 0; packet < kFeedbackPacketCount; ++packet) {
    UsbAudioDiagnostic[40 + packet] = statuses ? statuses[packet] : 0xffffffff;
    UsbAudioDiagnostic[44 + packet] = lengths ? lengths[packet] : 0xffffffff;
#if USB_AUDIO360_TEST_STAGE == 0 || USB_AUDIO360_TEST_STAGE == 5
    if (statuses && lengths && usb_transport::IsochInSucceeded(
          statuses[packet], lengths[packet], g_format.feedback.max_packet_bytes))
      ++UsbAudioDiagnostic[7];
    else if (statuses && statuses[packet] == 0xc005100e) {
      ++UsbAudioDiagnostic[9];
      RecordMiss(1, packet);
    }
    else {
      ++UsbAudioDiagnostic[14];
#if USB_AUDIO360_TEST_STAGE == 0
      if (!g_stopping) SetupFailed(0xe010);
#endif
    }
#endif
    DWORD raw = 0;
    if (!g_stopping && statuses && lengths &&
        usb_transport::ReadFeedbackPacket(statuses[packet], lengths[packet],
          g_format.feedback.max_packet_bytes,
          g_feedback_packets[slot] + packet * g_format.feedback.max_packet_bytes,
          &raw)) {
#if USB_AUDIO360_TEST_STAGE == 4 || USB_AUDIO360_TEST_STAGE == 5
      // Preserve the wire value; this transport test does not infer a rate.
      UsbAudioDiagnostic[48 + packet] = raw;
      ++UsbAudioDiagnostic[52];
#if USB_AUDIO360_TEST_STAGE == 5
      if (g_test_pacer.Update(raw, lengths[packet], GetTickCount())) {
        UsbAudioDiagnostic[22] = g_test_pacer.rate();
        ++UsbAudioDiagnostic[30];
      } else ++UsbAudioDiagnostic[29];
#endif
#else
      if (g_playback_pacer.Update(raw)) {
        UsbAudioDiagnostic[48 + packet] = g_playback_pacer.rate();
        ++UsbAudioDiagnostic[52];
      }
#endif
    }
  }
#if USB_AUDIO360_TEST_STAGE == 5
  if (SubmitBoundedSlot(1, slot)) return 0;
#elif USB_AUDIO360_TEST_STAGE == 0
  if (SubmitContinuousSlot(1, slot)) return 0;
#endif
  InterlockedExchange(&g_feedback_busy[slot], 0);
  return 0;
}

static LONG __cdecl IsochComplete(DWORD transfer, DWORD* statuses,
                                  WORD* lengths) {
  ++UsbAudioDiagnostic[15];
#if USB_AUDIO360_TEST_STAGE != 5
  UsbAudioDiagnostic[22] = statuses ? 1 : 0;
#endif
  UsbAudioDiagnostic[23] = statuses ? statuses[0] : 0xFFFFFFFF;
  UsbAudioDiagnostic[24] = statuses ? statuses[1] : 0xFFFFFFFF;
  UsbAudioDiagnostic[25] = statuses ? statuses[2] : 0xFFFFFFFF;
  UsbAudioDiagnostic[26] = statuses ? statuses[3] : 0xFFFFFFFF;
#if USB_AUDIO360_TEST_STAGE != 5
  UsbAudioDiagnostic[27] = lengths ? 1 : 0;
  UsbAudioDiagnostic[28] = lengths ? lengths[0] : 0xFFFFFFFF;
  UsbAudioDiagnostic[29] = lengths ? lengths[1] : 0xFFFFFFFF;
  UsbAudioDiagnostic[30] = lengths ? lengths[2] : 0xFFFFFFFF;
  UsbAudioDiagnostic[31] = lengths ? lengths[3] : 0xFFFFFFFF;
#endif
  int slot = -1;
  for (int index = 0; index < kIsochRingDepth; ++index) {
    if ((DWORD)&g_isoch[index] == transfer) {
      slot = index;
      break;
    }
  }
  if (slot < 0 || !UsbTransportComplete((void*)transfer)) return 0;

#if USB_AUDIO360_TEST_STAGE == 0 || USB_AUDIO360_TEST_STAGE == 5
  for (int packet = 0; packet < kIsochPacketCount; ++packet) {
    if (statuses && statuses[packet] == 0) ++UsbAudioDiagnostic[6];
    else if (statuses && statuses[packet] == 0xc005100e) {
      ++UsbAudioDiagnostic[8];
      RecordMiss(0, packet);
    }
    else {
      ++UsbAudioDiagnostic[10];
#if USB_AUDIO360_TEST_STAGE == 0
      if (!g_stopping) SetupFailed(0xe00f);
#endif
    }
  }
#endif

  bool successful = statuses != 0;
  for (int packet = 0; successful && packet < kIsochPacketCount; ++packet)
    successful = statuses[packet] == 0;
  bool active = !g_stopping && g_streaming && g_api &&
                g_api->playback_handle;
  if (successful && active && !g_notification_shown)
    InterlockedCompareExchange(&g_notification_event,
                               kNotificationConnected,
                               kNotificationNone);
  if (successful && active &&
      InterlockedCompareExchange(&g_diagnostic_streaming, 1, 0) == 0)
    DiagnosticsStreaming();

#if USB_AUDIO360_TEST_STAGE == 5
  if (SubmitBoundedSlot(0, slot)) return 0;
#elif USB_AUDIO360_TEST_STAGE == 0
  if (SubmitContinuousSlot(0, slot)) return 0;
#endif
  InterlockedExchange(&g_isoch_slot_busy[slot], 0);
  return 0;
}

static void StartIsoch() {
#if USB_AUDIO360_TEST_STAGE == 0
  if (g_stopping) return;
  unsigned maximum = g_profile.endpoint_packet_size /
      (kChannels * g_profile.bytes_per_sample);
  if (maximum > kMaxFramesPerUsbPacket) maximum = kMaxFramesPerUsbPacket;
  if (!g_playback_pacer.Begin(g_format.sync == uac::kAsynchronous, maximum)) {
    SetupFailed(0xe00a);
    return;
  }
  if (g_format.feedback.address) {
    for (int slot = 0; slot < kFeedbackRingDepth; ++slot) {
      g_feedback_isoch[slot].endpoint = g_feedback_isoch[0].endpoint;
      g_feedback_isoch[slot].saved_endpoint = g_feedback_isoch[0].endpoint;
      g_feedback_isoch[slot].buffer = g_feedback_packets[slot];
      g_feedback_isoch[slot].length = g_format.feedback.max_packet_bytes * kFeedbackPacketCount;
      g_feedback_isoch[slot].packet_count = kFeedbackPacketCount;
      g_feedback_isoch[slot].callback = (DWORD)FeedbackComplete;
      for (int packet = 0; packet < kFeedbackPacketCount; ++packet)
        g_feedback_packet_lengths[slot][packet] = g_format.feedback.max_packet_bytes;
    }
  }
  for (int slot = 0; slot < kIsochRingDepth; ++slot) {
    g_isoch[slot].endpoint = g_isoch[0].endpoint;
    g_isoch[slot].saved_endpoint = g_isoch[0].endpoint;
    g_isoch[slot].buffer = g_usb_packets[slot];
    g_isoch[slot].packet_count = kIsochPacketCount;
    g_isoch[slot].callback = (DWORD)IsochComplete;
  }
  g_setup_stage = kFinished;
  UsbAudioDiagnostic[32] = kFinished;
  UsbAudioDiagnostic[53] = 1;
  InterlockedExchange(&g_streaming, 1);
#endif
}

static void SetupOnlyReady() {
  g_setup_stage = kFinished;
  UsbAudioDiagnostic[32] = kFinished;
  UsbAudioDiagnostic[53] = 1;
}

// Called only in the USB DPC domain, after copying completion metadata.
// Keep the slot busy across resubmission; no worker can reuse its storage.
static bool SubmitBoundedSlot(unsigned direction, int slot) {
#if USB_AUDIO360_TEST_STAGE == 5
  if (g_stopping || !g_test_budget.Take(direction, GetTickCount())) return false;
  volatile LONG* busy = direction ? &g_feedback_busy[slot] : &g_isoch_slot_busy[slot];
  UsbIsochTrb* trb = direction ? &g_feedback_isoch[slot] : &g_isoch[slot];
  WORD* lengths = direction ? g_feedback_packet_lengths[slot] : g_packet_lengths[slot];
  if (!direction) {
    uac::PcmPacketBuilder builder(g_usb_packets[slot], sizeof(g_usb_packets[slot]),
        g_profile.bytes_per_sample, g_format.valid_bits, g_profile.endpoint_packet_size);
    bool nonzero_batch = false;
    for (int packet = 0; packet < kIsochPacketCount; ++packet) {
      unsigned frames;
      DWORD now = GetTickCount();
      UsbAudioDiagnostic[31] = g_test_pacer.age(now);
      if (!g_test_pacer.Next(now, &frames)) {
        UsbAudioDiagnostic[38] = 0xe009; // Missing/stale feedback or invalid pacing.
        InterlockedExchange(&g_stopping, 1);
        return false;
      }
      BYTE* packet_data = builder.Append(frames, &lengths[packet]);
      if (!packet_data) {
        UsbAudioDiagnostic[38] = 0xe00a;
        InterlockedExchange(&g_stopping, 1);
        return false;
      }
      for (unsigned frame = 0; frame < frames; ++frame) {
        // Multiply rather than left-shifting a negative signed value.
        unsigned frame_index = g_test_tone_frame++;
        int tone = uac::TestToneSample(frame_index);
#if USB_AUDIO360_DEBUG_API
        tone = g_debug_silent ? 0 : uac::TestToneSample(frame_index % 96000);
#endif
        LONG sample = (LONG)tone * 65536;
        BYTE* destination = packet_data +
            frame * kChannels * g_profile.bytes_per_sample;
        builder.Stereo(destination, (unsigned)sample, (unsigned)sample);
        ++UsbAudioToneDiagnostic[6];
        unsigned peak = (unsigned)(tone < 0 ? -tone : tone);
        if (peak) { ++UsbAudioToneDiagnostic[7]; nonzero_batch = true; }
        if (peak > UsbAudioToneDiagnostic[8]) UsbAudioToneDiagnostic[8] = peak;
        if (peak >= 800 && !UsbAudioToneDiagnostic[15]) {
          DWORD left = 0, right = 0;
          for (unsigned b = 0; b < g_profile.bytes_per_sample; ++b) {
            left |= (DWORD)destination[b] << (b * 8);
            right |= (DWORD)destination[g_profile.bytes_per_sample + b] << (b * 8);
          }
          UsbAudioToneDiagnostic[9] = frame_index;
          UsbAudioToneDiagnostic[10] = left;
          UsbAudioToneDiagnostic[11] = right;
          UsbAudioToneDiagnostic[12] = slot;
          UsbAudioToneDiagnostic[13] = UsbAudioDiagnostic[4] + 1;
          UsbAudioToneDiagnostic[15] = 1; // CPU snapshot, not DMA visibility proof.
        }
      }
      if (!UsbAudioDiagnostic[27] || frames < UsbAudioDiagnostic[27])
        UsbAudioDiagnostic[27] = frames;
      if (frames > UsbAudioDiagnostic[28]) UsbAudioDiagnostic[28] = frames;
    }
    trb->length = builder.total(); // Only completed/unsubmitted slots reach here.
    if (nonzero_batch) ++UsbAudioToneDiagnostic[14];
  }
  InterlockedExchange(busy, 1);
  ++UsbAudioDiagnostic[4 + direction];
  LONG status = UsbTransportIsochInDomain(g_io_handle, trb, lengths);
  if (status < 0) {
    --UsbAudioDiagnostic[4 + direction];
    UsbAudioDiagnostic[38] = (DWORD)status;
    InterlockedExchange(&g_stopping, 1);
    return false;  // Adapter rejected this submission before the raw host call.
  }
  return true;
#else
  return false;
#endif
}

// Called only from the USB DPC domain. Completed slot storage is refilled and
// resubmitted there; the render callback only publishes frames to the PCM ring.
static bool SubmitContinuousSlot(unsigned direction, int slot) {
#if USB_AUDIO360_TEST_STAGE == 0
  if (g_stopping || !g_streaming || slot < 0 || slot >= 2) return false;
  volatile LONG* busy = direction ? &g_feedback_busy[slot] : &g_isoch_slot_busy[slot];
  UsbIsochTrb* trb = direction ? &g_feedback_isoch[slot] : &g_isoch[slot];
  WORD* lengths = direction ? g_feedback_packet_lengths[slot] : g_packet_lengths[slot];
  if (!direction && !FillUsbPacket(slot)) {
    SetupFailed(0xe00a);
    return false;
  }
  InterlockedExchange(busy, 1);
  ++UsbAudioDiagnostic[4 + direction];
  LONG status = UsbTransportIsochInDomain(g_io_handle, trb, lengths);
  if (status < 0) {
    --UsbAudioDiagnostic[4 + direction];
    SetupFailed((DWORD)status);
    InterlockedExchange(busy, 0);
    return false;
  }
  return true;
#else
  return false;
#endif
}

static void PrimeContinuousStream(void*) {
#if USB_AUDIO360_TEST_STAGE == 0
  if (g_stopping || !UsbTransportIdleInDomain()) {
    SetupFailed(0xe00e);
    return;
  }
  for (int slot = 0; slot < 2; ++slot) {
    if (!SubmitContinuousSlot(0, slot)) return;
    if (g_format.feedback.address && !SubmitContinuousSlot(1, slot)) return;
  }
#endif
}

static void PrimeBoundedStream(void*) {
#if USB_AUDIO360_TEST_STAGE == 5
  g_test_started = GetTickCount();
  unsigned maximum = g_profile.endpoint_packet_size / (kChannels * g_profile.bytes_per_sample);
  if (maximum > kMaxFramesPerUsbPacket) maximum = kMaxFramesPerUsbPacket;
  if (!g_test_pacer.Begin(g_test_started, maximum)) {
    UsbAudioDiagnostic[38] = 0xe00a;
    InterlockedExchange(&g_stopping, 1);
    return;
  }
  g_test_budget.Begin(g_test_started);
  for (int slot = 0; slot < 2; ++slot) {
    SubmitBoundedSlot(0, slot);
    SubmitBoundedSlot(1, slot);
  }
#endif
}

static void BoundedStreamTick() {
#if USB_AUDIO360_TEST_STAGE == 5
  if (g_stopping) return;
  DWORD now = GetTickCount();
  UsbAudioDiagnostic[21] = now - g_test_started;
  if (g_endpoint_phase == 7) {
    DWORD pending = 0;
    for (int i = 0; i < 2; ++i)
      pending += (g_isoch_slot_busy[i] != 0) + (g_feedback_busy[i] != 0);
    UsbAudioDiagnostic[20] = pending;
    if (!pending) {
      UsbAudioDiagnostic[60] =
          (UsbAudioDiagnostic[4] != UsbAudioDiagnostic[15]) +
          (UsbAudioDiagnostic[5] != UsbAudioDiagnostic[13]);
      if (UsbAudioDiagnostic[60]) { SetupFailed(0xe008); return; }
      UsbAudioDiagnostic[59] = 1;
      UsbAudioDiagnostic[58] = 4;  // All submitted transfers completed.
      SetupOnlyReady();
    } else if (now - g_test_drain_started >= 1000) SetupFailed(0xe007);
    return;
  }
  if (g_test_cancelled || now - g_test_started >= g_test_duration) {
    g_endpoint_phase = 7;
    g_test_drain_started = now;
    UsbAudioDiagnostic[58] = 3;  // Stop submitting; retain all storage.
    return;
  }
  // Worker observes only; replenishment and budget accounting are DPC-owned.
#endif
}

static void PrepareBoundedStream(void*) {
#if USB_AUDIO360_TEST_STAGE == 5
  if (g_stopping || !UsbTransportIdleInDomain()) { SetupFailed(0xe00e); return; }
  const DWORD feedback_bytes = g_format.feedback.max_packet_bytes;
  if (!g_format.feedback.address || (feedback_bytes != 3 && feedback_bytes != 4)) {
    SetupFailed(0xe006); return;
  }
  for (int slot = 0; slot < 2; ++slot) {
    g_isoch[slot].endpoint = g_isoch[0].endpoint;
    g_isoch[slot].saved_endpoint = g_isoch[0].endpoint;
    g_isoch[slot].buffer = g_usb_packets[slot];
    g_isoch[slot].length = 4 * 48 * 2 * g_profile.bytes_per_sample;
    g_isoch[slot].packet_count = 4;
    g_isoch[slot].callback = (DWORD)IsochComplete;
    memset(g_usb_packets[slot], 0, sizeof(g_usb_packets[slot]));
    g_feedback_isoch[slot].endpoint = g_feedback_isoch[0].endpoint;
    g_feedback_isoch[slot].saved_endpoint = g_feedback_isoch[0].endpoint;
    g_feedback_isoch[slot].buffer = g_feedback_packets[slot];
    g_feedback_isoch[slot].length = 4 * feedback_bytes;
    g_feedback_isoch[slot].packet_count = 4;
    g_feedback_isoch[slot].callback = (DWORD)FeedbackComplete;
    for (int i = 0; i < 4; ++i) {
      g_packet_lengths[slot][i] = (WORD)(48 * 2 * g_profile.bytes_per_sample);
      g_feedback_packet_lengths[slot][i] = (WORD)feedback_bytes;
    }
  }
  g_endpoint_phase = 6;
  UsbAudioDiagnostic[58] = 2;
  PrimeBoundedStream(0);
#endif
}

static void BeginBoundedStream() {
  LONG status = UsbTransportRun(PrepareBoundedStream, 0);
  if (status < 0) SetupFailed(status);
}

// Nonblocking gaps allow the debug monitor to observe each boundary. Endpoint
// objects remain owned by this single attachment until reboot; never recycled.
static void EndpointOpenTick() {
#if USB_AUDIO360_TEST_STAGE == 0 || (USB_AUDIO360_TEST_STAGE >= 2 && USB_AUDIO360_TEST_STAGE <= 5)
  if (g_endpoint_phase >= 6) { BoundedStreamTick(); return; }
  if (g_endpoint_phase == 5) {
    if (g_stopping) return;
#if USB_AUDIO360_TEST_STAGE == 4
    const bool completed = UsbAudioDiagnostic[13] && !g_feedback_busy[0];
    const int status_base = 40;
#else
    const bool completed = UsbAudioDiagnostic[15] && !g_isoch_slot_busy[0];
    const int status_base = 23;
#endif
    if (completed) {
      UsbAudioDiagnostic[59] = 1;  // Completion received, not proof of audio.
      UsbAudioDiagnostic[60] = 0;
      for (int i = 0; i < kIsochPacketCount; ++i)
        if (UsbAudioDiagnostic[status_base + i]) ++UsbAudioDiagnostic[60];
      SetupOnlyReady();
    } else if (GetTickCount() - g_endpoint_phase_time >= 1000) {
      SetupFailed(0xe005);  // Timeout: retain storage, never retry/reuse.
    }
    return;
  }
  if (g_stopping || GetTickCount() - g_endpoint_phase_time < 250) return;
  LONG status = 0;
  if (g_endpoint_phase == 1) {
    UsbAudioDiagnostic[55] = 3;  // Before opening OUT.
    status = UsbTransportOpen(
        g_io_handle, g_profile.endpoint_address,
        g_profile.endpoint_packet_size, 1, (DWORD*)&g_isoch[0]);
    UsbAudioDiagnostic[11] = status;
    if (status < 0) { SetupFailed(status); return; }
    UsbAudioDiagnostic[55] = 4;  // OUT opened, no transfer queued.
    g_endpoint_phase = 2;
  } else if (g_endpoint_phase == 2) {
    if (g_format.feedback.address) {
      UsbAudioDiagnostic[55] = 5;  // Before opening feedback IN.
      status = UsbTransportOpen(
          g_io_handle, g_profile.feedback_endpoint_address,
          g_profile.feedback_endpoint_packet_size, 1, (DWORD*)&g_feedback_isoch[0]);
      UsbAudioDiagnostic[12] = status;
      if (status < 0) { SetupFailed(status); return; }
    }
    UsbAudioDiagnostic[55] = 6;  // Endpoint opens completed.
    g_endpoint_phase = 3;
  } else if (g_endpoint_phase == 3) {
    UsbAudioDiagnostic[55] = 7;
    UsbAudioDiagnostic[57] = 1;  // Activation/open-only test passed.
#if USB_AUDIO360_TEST_STAGE == 0
    StartIsoch();
#elif USB_AUDIO360_TEST_STAGE >= 3 && USB_AUDIO360_TEST_STAGE <= 5
    g_endpoint_phase = 4;
#else
    SetupOnlyReady();
#endif
  } else if (g_endpoint_phase == 4) {
#if USB_AUDIO360_TEST_STAGE == 5
#if USB_AUDIO360_DEBUG_API
    // Open once, then wait for a remotely requested bounded test.
    g_endpoint_phase = 7;
    SetupOnlyReady();
#else
    BeginBoundedStream();
#endif
#endif
#if USB_AUDIO360_TEST_STAGE == 4
    const DWORD packet_bytes = g_format.feedback.max_packet_bytes;
    if (!g_format.feedback.address || (packet_bytes != 3 && packet_bytes != 4)) {
      SetupFailed(0xe006); return;
    }
    UsbAudioDiagnostic[58] = 1;
    g_feedback_isoch[0].saved_endpoint = g_feedback_isoch[0].endpoint;
    g_feedback_isoch[0].buffer = g_feedback_packets[0];
    g_feedback_isoch[0].length = kFeedbackPacketCount * packet_bytes;
    g_feedback_isoch[0].packet_count = kFeedbackPacketCount;
    g_feedback_isoch[0].callback = (DWORD)FeedbackComplete;
    memset(g_feedback_packets[0], 0, sizeof(g_feedback_packets[0]));
    for (int i = 0; i < kFeedbackPacketCount; ++i)
      g_feedback_packet_lengths[0][i] = (WORD)packet_bytes;
    g_endpoint_phase = 5;
    InterlockedExchange(&g_feedback_busy[0], 1);
    LONG submitted = UsbTransportIsoch(g_io_handle, &g_feedback_isoch[0],
                                       g_feedback_packet_lengths[0]);
    if (submitted < 0) { SetupFailed(submitted); return; }
    UsbAudioDiagnostic[58] = 2;
#endif
#if USB_AUDIO360_TEST_STAGE == 3
    UsbAudioDiagnostic[58] = 1;  // Before the single submission.
    g_isoch[0].saved_endpoint = g_isoch[0].endpoint;
    g_isoch[0].buffer = g_usb_packets[0];
    g_isoch[0].length = kIsochPacketCount * kFramesPerUsbPacket *
                        kChannels * g_profile.bytes_per_sample;
    g_isoch[0].packet_count = kIsochPacketCount;
    g_isoch[0].callback = (DWORD)IsochComplete;
    memset(g_usb_packets[0], 0, sizeof(g_usb_packets[0]));
    for (int i = 0; i < kIsochPacketCount; ++i)
      g_packet_lengths[0][i] = kFramesPerUsbPacket * kChannels *
                              g_profile.bytes_per_sample;
    // Commit the phase before calling the host: completion may be immediate.
    g_endpoint_phase = 5;
    InterlockedExchange(&g_isoch_slot_busy[0], 1);
    LONG submitted = UsbTransportIsoch(g_io_handle, &g_isoch[0], g_packet_lengths[0]);
    if (submitted < 0) { SetupFailed(submitted); return; }
    UsbAudioDiagnostic[58] = 2;  // Queue routine returned (void, not a status).
#endif
  }
  g_endpoint_phase_time = GetTickCount();
#endif
}

static void ActivateInterface() {
#if USB_AUDIO360_TEST_STAGE == 1
  SetupOnlyReady();
#else
  UsbAudioDiagnostic[55] = 1;  // Before SET_INTERFACE active alternate.
  QueueControl(1, 11, g_profile.alternate_setting, g_profile.interface_number,
               0, 0, kActive);
#endif
}

static void VerifyInterface() {
  QueueControl(0x81, 10, 0, g_profile.interface_number, 1,
               g_control_data, kVerifyInterface);
}

static void ReadVolumeOrActivate() {
  if (g_read_feature_id && uac::ControlReadable(g_read_feature_controls, 2))
    QueueControl(0xa1, 1, 0x0200,
        ((WORD)g_read_feature_id << 8) | g_profile.control_interface_number,
        2, g_control_data, kReadVolume);
  else ActivateInterface();
}

static void ReadMuteOrVolume() {
  if (g_read_feature_id && uac::ControlReadable(g_read_feature_controls, 1))
    QueueControl(0xa1, 1, 0x0100,
        ((WORD)g_read_feature_id << 8) | g_profile.control_interface_number,
        1, g_control_data, kReadMute);
  else ReadVolumeOrActivate();
}

static void ClockReady() {
#if USB_AUDIO360_TEST_STAGE == 1
  SetupOnlyReady();
#elif USB_AUDIO360_TEST_STAGE >= 2 && USB_AUDIO360_TEST_STAGE <= 4
  ActivateInterface();  // Isolate activation/open; no feature-unit writes yet.
#else
  if (g_profile.feature_unit_id)
    QueueControl(0x21, 1, 0x0100,
        ((WORD)g_profile.feature_unit_id << 8) | g_profile.control_interface_number,
        1, &g_unmuted, kUnmute);
  else ReadMuteOrVolume();
#endif
}

static void ActivationVerified() {
  UsbAudioActivationDiagnostic[0] = 1;
#if USB_AUDIO360_DEBUG_API
  if (g_debug_controls) {
    g_debug_controls = false;
    SetupOnlyReady();
    return;
  }
#endif
#if USB_AUDIO360_TEST_STAGE != 1
  g_setup_stage = kOpening;
  UsbAudioDiagnostic[32] = kOpening;
  g_endpoint_phase = 1;
  g_endpoint_phase_time = GetTickCount();
#else
  SetupOnlyReady();
#endif
}

static void VerifyVolume() {
  if (g_read_feature_id && uac::ControlReadable(g_read_feature_controls, 2))
    QueueControl(0xa1, 1, 0x0200,
        ((WORD)g_read_feature_id << 8) | g_profile.control_interface_number,
        2, g_control_data, kVerifyVolume);
  else ActivationVerified();
}

static void VerifyMute() {
  if (g_read_feature_id && uac::ControlReadable(g_read_feature_controls, 1))
    QueueControl(0xa1, 1, 0x0100,
        ((WORD)g_read_feature_id << 8) | g_profile.control_interface_number,
        1, g_control_data, kVerifyMute);
  else VerifyVolume();
}

static void SetupTick() {
  if (g_stopping) { g_clock.Cancel(); return; }
  if (g_control_pending && g_control_done) {
    const LONG status = g_control_status;
    const DWORD received = g_control.actual_length;
    InterlockedExchange(&g_control_pending, 0);
    InterlockedExchange(&g_control_done, 0);
    UsbAudioDiagnostic[35] = status;
    UsbAudioDiagnostic[36] = received;
    if (status != 0) {
      // Most UAC1 devices accept endpoint-rate programming after their active
      // alternate is selected. A bounded fallback supports firmware that only
      // exposes the control while inactive: deactivate once, program, then
      // reactivate. Never retry another failed operation or loop indefinitely.
      if (g_setup_stage == kUac1Rate &&
          uac::AllowRateBeforeInterfaceFallback(
              g_profile.audio_class_version, g_format.fixed_48000,
              g_uac1_rate_fallback)) {
        g_uac1_rate_fallback = true;
        UsbAudioActivationDiagnostic[10] = 1;
        QueueControl(1, 11, 0, g_profile.interface_number, 0, 0,
                     kUac1FallbackInactive);
        return;
      }
      // Some otherwise conforming UAC1 devices accept SET_INTERFACE but stall
      // the standard GET_INTERFACE readback. The active SET already completed
      // successfully before this request was issued, so continue without
      // readback only for UAC1. UAC2 retains strict interface/clock checks.
      if (g_setup_stage == kVerifyInterface &&
          g_profile.audio_class_version == 1) {
        UsbAudioActivationDiagnostic[1] = 2;  // Active, readback unsupported.
        UsbAudioActivationDiagnostic[2] = g_profile.alternate_setting;
        ActivationVerified();
        return;
      }
      SetupFailed(status);
      return;
    }
    if (g_setup_stage != kClock && received != g_control.trb.length) {
      SetupFailed(0xe001); return;
    }
    switch (g_setup_stage) {
      case kHeader: {
        DWORD length = Read16(g_configuration_descriptor + 2);
        if (g_configuration_descriptor[0] < 9 || g_configuration_descriptor[1] != 2 ||
            length < 9 || length > sizeof(g_configuration_descriptor)) {
          SetupFailed(0xe002); return;
        }
        QueueControl(0x80, 6, 0x0200, 0, (WORD)length,
                     g_configuration_descriptor, kDescriptor);
        break;
      }
      case kDescriptor:
        if (!SelectProfile(received)) { SetupFailed(0xe003); return; }
        QueueControl(0, 9, g_profile.configuration, 0, 0, 0, kConfiguration);
        break;
      case kConfiguration:
        QueueControl(1, 11, 0, g_profile.interface_number, 0, 0, kInactive);
        break;
      case kInactive:
        if (uac::ActionAfterInactive(g_profile.audio_class_version) ==
            uac::kBeginClockSetup) {
          g_setup_stage = kClock;
          g_clock.Begin(g_configuration_descriptor,
              Read16(g_configuration_descriptor + 2), g_profile.control_interface_number,
              g_profile.clock_source_id, GetTickCount());
        } else {
          ActivateInterface();
        }
        break;
      case kClock:
        if (g_clock_request.request_type == 0xa1 &&
            g_clock_request.value == 0x0100 && received == 4)
          UsbAudioDiagnostic[54] = Read32(g_control_data);
        g_clock.Complete(g_clock_request.token, true, g_control_data,
                         received, GetTickCount());
        break;
      case kUac1Rate:
        g_uac1_rate_programmed = true;
        VerifyInterface();
        break;
      case kUac1FallbackInactive:
        QueueControl(0x22, 1, 0x0100, g_profile.endpoint_address, 3,
                     g_sample_rate, kUac1FallbackRate);
        break;
      case kUac1FallbackRate:
        g_uac1_rate_programmed = true;
        UsbAudioActivationDiagnostic[11] = 1;
        ActivateInterface();
        break;
      case kUnmute: ReadMuteOrVolume(); break;
      case kReadMute:
        UsbAudioToneDiagnostic[3] = g_control_data[0];
        UsbAudioToneDiagnostic[2] = 1;
        ReadVolumeOrActivate();
        break;
      case kReadVolume:
        UsbAudioToneDiagnostic[5] = Read16(g_control_data); // Raw signed 8.8 dB.
        UsbAudioToneDiagnostic[4] = 1;
        ActivateInterface();
        break;
      case kActive:
        UsbAudioDiagnostic[55] = 2;  // SET_INTERFACE completed successfully.
        if (uac::ActionAfterActive(g_profile.audio_class_version,
                                   g_format.endpoint_rate_control,
                                   g_format.fixed_48000,
                                   g_uac1_rate_programmed) ==
            uac::kProgramEndpointRate)
          QueueControl(0x22, 1, 0x0100, g_profile.endpoint_address, 3,
                       g_sample_rate, kUac1Rate);
        else VerifyInterface();
        break;
      case kVerifyInterface:
        UsbAudioActivationDiagnostic[1] = 1;
        UsbAudioActivationDiagnostic[2] = g_control_data[0];
        if (g_control_data[0] != g_profile.alternate_setting) {
          SetupFailed(0xe00b); return;
        }
        if (g_profile.audio_class_version == 2) {
          BYTE source = g_clock.source();
          UsbAudioActivationDiagnostic[3] = source;
          if (!source) { SetupFailed(0xe00c); return; }
          QueueControl(0xa1, 1, 0x0100,
              ((WORD)source << 8) | g_profile.control_interface_number,
              4, g_control_data, kVerifyClock);
        } else ActivationVerified();
        break;
      case kVerifyClock:
        UsbAudioActivationDiagnostic[4] = 1;
        UsbAudioActivationDiagnostic[5] = Read32(g_control_data);
        if (Read32(g_control_data) != 48000) { SetupFailed(0xe00c); return; }
        VerifyMute();
        break;
      case kVerifyMute:
        UsbAudioActivationDiagnostic[6] = 1;
        UsbAudioActivationDiagnostic[7] = g_control_data[0];
        if (g_control_data[0] != 0) { SetupFailed(0xe00d); return; }
        VerifyVolume();
        break;
      case kVerifyVolume:
        UsbAudioActivationDiagnostic[8] = 1;
        UsbAudioActivationDiagnostic[9] = Read16(g_control_data);
        ActivationVerified();
        break;
      default: break;
    }
  }
  if (g_stopping) return;
  if (g_setup_stage == kOpening) { EndpointOpenTick(); return; }
  if (g_setup_stage == kClock) {
    g_clock.Tick(GetTickCount());
    UsbAudioDiagnostic[37] = g_clock.error();
    if (g_clock.state() == uac::ClockSetup::kFailed) {
      SetupFailed(0xe100 | g_clock.error()); return;
    }
    if (!g_control_pending && g_clock.state() == uac::ClockSetup::kReady) ClockReady();
    else if (!g_control_pending && g_clock.Pending(&g_clock_request)) {
      memset(g_control_data, 0, sizeof(g_control_data));
      if (!(g_clock_request.request_type & 0x80))
        memcpy(g_control_data, g_clock_request.output, g_clock_request.length);
      QueueControl(g_clock_request.request_type, g_clock_request.request,
          g_clock_request.value, g_clock_request.index, g_clock_request.length,
          g_control_data, kClock);
    }
  } else if (g_control_pending) {
    DWORD now = GetTickCount();
    if (usb_transport::ControlExpired(g_control_issued, now, g_control_deadline,
                                      g_control_done != 0)) {
      UsbAudioControlDiagnostic[11] = now;
      UsbAudioControlDiagnostic[12] = now - g_control_issued;
      ++UsbAudioControlDiagnostic[14];
      SetupFailed(0xe004);
    }
  }
}

static void StartDevice() {
  UsbAudioDiagnostic[2] = 1;
  if (!g_api || !g_api->playback_handle || g_stopping) return;
  g_io_handle = g_api->playback_handle;
  g_profile = g_api->profile;
  memset(&g_control, 0, sizeof(g_control));
  LONG status = UsbTransportOpenDefault(
      g_io_handle, (DWORD*)&g_control);
  UsbAudioDiagnostic[3] = status;
  if (status < 0) { SetupFailed(status); return; }
  QueueControl(0x80, 6, 0x0200, 0, 9, g_configuration_descriptor, kHeader);
}

static bool QueueNotification(NotificationEvent event) {
  if (!g_notify_queue_ui) {
    HANDLE xam = GetModuleHandleA("xam.xex");
    if (xam) XexGetProcedureAddress(xam, 656, &g_notify_queue_ui);
  }
  if (!g_notify_queue_ui) return false;

  WCHAR connected[] = L"USB Audio Connected";
  WCHAR disconnected[] = L"USB Audio Disconnected";
  PWCHAR message = event == kNotificationConnected
      ? connected : disconnected;
  g_notify_queue_ui(kNotifyTypeCustom, XUSER_INDEX_ANY, 1, message, 0);
  return true;
}

#if USB_AUDIO360_DEBUG_API
static bool DebugIdle() {
  if (g_stopping || !g_io_handle || !g_api || g_api->playback_handle != g_io_handle ||
      g_control_pending || g_setup_stage != kFinished ||
      g_endpoint_phase != 7 || !UsbAudioActivationDiagnostic[0] ||
      !g_isoch[0].endpoint || !g_feedback_isoch[0].endpoint ||
      UsbAudioDiagnostic[10] || UsbAudioDiagnostic[14]) return false;
  for (int i = 0; i < 2; ++i)
    if (g_isoch_slot_busy[i] || g_feedback_busy[i]) return false;
  return UsbAudioDiagnostic[4] == UsbAudioDiagnostic[15] &&
         UsbAudioDiagnostic[5] == UsbAudioDiagnostic[13];
}

static void DebugStart(void*) {
  // Recheck in the USB domain, serialized against removal and completion.
  if (!DebugIdle() || !UsbTransportIdleInDomain()) {
    InterlockedExchange(&g_debug_result, -1); return;
  }
  for (int i = 4; i <= 31; ++i)
    if (i != 11 && i != 12) UsbAudioDiagnostic[i] = 0;
  for (int i = 40; i <= 52; ++i) UsbAudioDiagnostic[i] = 0;
  for (int i = 58; i <= 60; ++i) UsbAudioDiagnostic[i] = 0;
  for (int i = 6; i < 16; ++i) UsbAudioToneDiagnostic[i] = 0;
  g_test_tone_frame = 0;
  g_test_cancelled = 0;
  g_test_duration = g_debug_request.duration;
  g_debug_silent = g_debug_request.command == usb_debug::kSilence;
  g_test_budget = usb_transport::StreamTestBudget(g_test_duration, 4096);
  g_setup_stage = kOpening;
  UsbAudioDiagnostic[32] = kOpening;
  UsbAudioDiagnostic[53] = 0;
  InterlockedIncrement(&g_debug_run);
  InterlockedExchange(&g_debug_result, 2);
  PrepareBoundedStream(0);
}

static void DebugStop(void*) {
  g_test_budget.Stop(); // Completion retains ownership until each callback.
  InterlockedExchange(&g_test_cancelled, 1);
}

static void DebugTick() {
  if (InterlockedCompareExchange(&g_debug_mailbox, 3, 2) != 2) return;
  const usb_debug::Command command = g_debug_request.command;
  if (command == usb_debug::kStop) {
    if (!g_stopping && UsbTransportRun(DebugStop, 0) >= 0)
      InterlockedExchange(&g_debug_result, 2);
    else InterlockedExchange(&g_debug_result, -1);
  } else if (!DebugIdle()) InterlockedExchange(&g_debug_result, -1);
  else if (command == usb_debug::kControls) {
    g_debug_controls = true;
    memset((void*)UsbAudioActivationDiagnostic, 0, sizeof(UsbAudioActivationDiagnostic));
    QueueControl(0x81, 10, 0, g_profile.interface_number, 1,
                 g_control_data, kVerifyInterface);
    InterlockedExchange(&g_debug_result, 2);
  } else if (UsbTransportRun(DebugStart, 0) < 0)
    InterlockedExchange(&g_debug_result, -1);
  InterlockedExchange(&g_debug_mailbox, 0);
}

static void DebugMailboxTick() {
  // Same single-word, atomic-consume mechanism as legacy PluginCommand.
  DWORD encoded = (DWORD)InterlockedExchange(
      (volatile LONG*)&UsbAudioDebugMailbox[3], 0);
  if (encoded) {
    usb_debug::Request request = usb_debug::Decode(encoded);
    if (request.command == usb_debug::kInvalid) {
      InterlockedExchange(&g_debug_result, -1);
    } else {
      g_debug_request = request;
      InterlockedExchange(&g_debug_result, 1);
      InterlockedExchange(&g_debug_mailbox, 2);
      DebugTick();
    }
    UsbAudioDebugMailbox[5] = (DWORD)g_debug_result;
    InterlockedIncrement((volatile LONG*)&UsbAudioDebugMailbox[4]);
  }
  UsbAudioDebugMailbox[5] = (DWORD)g_debug_result;
  UsbAudioDebugMailbox[6] = (DWORD)g_debug_run;
  UsbAudioDebugMailbox[7] = (DWORD)g_stopping;
}

static void DebugInitialize() {
  UsbAudioDebugMailbox[1] = 1;
  UsbAudioDebugMailbox[2] = 0x55414342;
  UsbAudioDebugMailbox[8] = (DWORD)UsbAudioDiagnostic;
  UsbAudioDebugMailbox[9] = (DWORD)UsbAudioControlDiagnostic;
  UsbAudioDebugMailbox[10] = (DWORD)UsbAudioToneDiagnostic;
  UsbAudioDebugMailbox[11] = (DWORD)UsbAudioActivationDiagnostic;
  UsbAudioDebugMailbox[12] = (DWORD)UsbAudioSetupTrace;
  InterlockedExchange((volatile LONG*)&UsbAudioDebugMailbox[0], 0x55414d42);
}

#endif

}  // namespace

BOOL AudioInitialize(const AudioHostApi* api) {
  UsbAudioDiagnostic[63] = 0x5541434d;  // Single-owner device admission.
#if USB_AUDIO360_DEBUG_API
  UsbAudioDiagnostic[63] = 0x55414342;
#endif
  UsbAudioDiagnostic[1] = 1;
  if (!api) return FALSE;
#if USB_AUDIO360_TEST_STAGE == 0
  if (!StartRenderCapture()) return FALSE;
#endif
  g_api = api;
#if USB_AUDIO360_DEBUG_API
  DebugInitialize();
#endif
  g_last_handle = api->playback_handle;
  if (g_last_handle && !InterlockedExchange(&g_started, 1)) StartDevice();
  return TRUE;
}

VOID AudioTick(const AudioHostApi* api) {
  if (!api) return;
  if (g_rearm_pending) {
    DWORD now = GetTickCount();
    DWORD elapsed = now - g_removed_at;
    if (elapsed < kRemovalSettleMs) return;
    if (elapsed >= kRearmDeadlineMs) {
      UsbAudioDiagnostic[60] = 0xe201;  // Drain/re-arm deadline; fail closed.
      InterlockedExchange(&g_rearm_pending, 0);
      return;
    }
    if (now - g_rearm_last_attempt < kRearmRetryMs) return;
    g_rearm_last_attempt = now;
    if (!UsbTransportRearm()) return;
    ResetDeviceState();
  }
  if (api->playback_handle != g_last_handle) {
    g_last_handle = api->playback_handle;
    if (g_last_handle && !InterlockedExchange(&g_started, 1)) StartDevice();
  }

  SetupTick();
#if USB_AUDIO360_DEBUG_API
  DebugMailboxTick();
#endif
#if USB_AUDIO360_TEST_STAGE != 0
  return;
#else
  if (!g_last_handle || !g_streaming || g_stopping) return;
  LONG available = g_pcm_write - g_pcm_read;
  if (!g_bridge_started) {
    if (available < kPcmTargetFrames) return;
    if (available > kPcmTargetFrames)
      InterlockedExchange(&g_pcm_read, g_pcm_write - kPcmTargetFrames);
    if (InterlockedCompareExchange(&g_bridge_started, 1, 0) == 0) {
      LONG status = UsbTransportRun(PrimeContinuousStream, 0);
      if (status < 0) SetupFailed(status);
    }
  }
#endif
}

VOID AudioDeviceRemoved() {
  InterlockedExchange(&g_stopping, 1);
  bool was_connected = InterlockedExchange(&g_streaming, 0) != 0 ||
                       g_notification_shown;
  InterlockedExchange(&g_notification_shown, 0);
  InterlockedExchange(&g_bridge_started, 0);
  InterlockedExchange(&g_pcm_read, g_pcm_write);
  g_removed_at = GetTickCount();
  g_rearm_last_attempt = g_removed_at;
  InterlockedExchange(&g_rearm_pending, 1);
  DiagnosticsDisconnected();
  InterlockedExchange(&g_notification_event, was_connected
      ? kNotificationDisconnected : kNotificationNone);
}

VOID AudioNotificationTick() {
  NotificationEvent event = (NotificationEvent)InterlockedExchange(
      &g_notification_event, kNotificationNone);
  if (event == kNotificationNone) return;
  if (event == kNotificationConnected && g_notification_shown) return;
  if (event == kNotificationConnected &&
      (g_stopping || !g_streaming || !g_api || !g_api->playback_handle)) {
    return;
  }
  if (!QueueNotification(event)) {
    InterlockedExchange(&g_notification_event, event);
    return;
  }
  if (event == kNotificationConnected)
    InterlockedExchange(&g_notification_shown, 1);
}

BOOL AudioAdjustVolume(LONG delta_percent) {
  if (!g_streaming || g_stopping || !g_api || !g_api->playback_handle)
    return FALSE;

  LONG current = g_volume_percent;
  for (;;) {
    LONG next = current + delta_percent;
    if (next < 0) next = 0;
    if (next > 100) next = 100;
    if (next == current) return FALSE;
    LONG observed = InterlockedCompareExchange(&g_volume_percent,
                                               next, current);
    if (observed == current) {
      InterlockedExchange(&g_volume_feedback_pending, 1);
      return TRUE;
    }
    current = observed;
  }
}
