// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio.h"

#include <string.h>

extern "C" LONG XexGetProcedureAddress(HANDLE module, DWORD ordinal,
                                        PVOID address);

namespace {

const int kIsochRingDepth = 2;
const int kIsochPacketCount = 4;
const int kFramesPerUsbPacket = 48;
const int kBytesPerFrame = 4;
const int kPacketBytes = kFramesPerUsbPacket * kBytesPerFrame;
const int kBatchBytes = kPacketBytes * kIsochPacketCount;
const int kPcmRingFrames = 4096;
const int kPcmTargetFrames = 768;
const int kPcmHighWaterFrames = 1280;
const int kVolumeFeedbackSamples = 1440;
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
  BYTE pad[4];
  UsbPacket packet;
};

struct RenderMecClient {
  DWORD callback;
  DWORD context;
};

struct __declspec(align(128)) RenderCaptureBuffer {
  float samples[6 * 256];
};

typedef LONG (*OpenDefaultFn)(void*, DWORD*);
typedef LONG (*OpenEndpointFn)(void*, int, int, int, int, DWORD*);
typedef LONG (*QueueTransferFn)(void*, void*);
typedef VOID (*QueueIsochTransferFn)(void*, void*, WORD*);
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
static volatile LONG g_volume_percent = 100;
static volatile LONG g_volume_feedback_pending = 0;
static volatile LONG g_isoch_slot_busy[kIsochRingDepth];
static volatile LONG g_pcm_write = 0;
static volatile LONG g_pcm_read = 0;

static UsbControlTrb g_control;
static UsbIsochTrb g_isoch[kIsochRingDepth];
static WORD g_packet_lengths[kIsochPacketCount];
static __declspec(align(128)) BYTE g_usb_packets[kIsochRingDepth][kBatchBytes];
static __declspec(align(128)) BYTE g_pcm_ring[kPcmRingFrames][kBytesPerFrame];
static BYTE g_sample_rate[3] = {0x80, 0xBB, 0x00};
static RenderMecClient g_mec_client;
static RenderCaptureBuffer g_render_frame;
static void* volatile g_mec_handle = 0;
static CaptureRenderFrameFn g_capture_render_frame = 0;
static NotifyQueueUiFn g_notify_queue_ui = 0;
static LONG g_volume_feedback_remaining = 0;
static LONG g_volume_feedback_phase = 0;

static WORD Swap16(WORD value) {
  return (WORD)((value >> 8) | (value << 8));
}

static void FillUsbPacket(int slot) {
  const int frame_count = kFramesPerUsbPacket * kIsochPacketCount;
  LONG read = g_pcm_read;
  LONG write = g_pcm_write;
  LONG available = write - read;
  int copied = available < frame_count ? (int)available : frame_count;

  for (int frame = 0; frame < copied; ++frame) {
    BYTE* source = g_pcm_ring[(read + frame) & (kPcmRingFrames - 1)];
    BYTE* destination = &g_usb_packets[slot][frame * kBytesPerFrame];
    memcpy(destination, source, kBytesPerFrame);
  }
  if (copied < frame_count) {
    memset(&g_usb_packets[slot][copied * kBytesPerFrame], 0,
           (frame_count - copied) * kBytesPerFrame);
  }
  InterlockedExchange(&g_pcm_read, read + copied);
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
    LONG left_sample = (LONG)(left * 32767.0f);
    LONG right_sample = (LONG)(right * 32767.0f);
    BYTE* destination = g_pcm_ring[(write + sample) & (kPcmRingFrames - 1)];
    destination[0] = (BYTE)(left_sample & 0xFF);
    destination[1] = (BYTE)((left_sample >> 8) & 0xFF);
    destination[2] = (BYTE)(right_sample & 0xFF);
    destination[3] = (BYTE)((right_sample >> 8) & 0xFF);
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

static LONG QueueControl(BYTE request_type, BYTE request, WORD value,
                         WORD index, WORD length, void* data,
                         DWORD callback) {
  g_control.packet.request_type = request_type;
  g_control.packet.request = request;
  g_control.packet.value = Swap16(value);
  g_control.packet.index = Swap16(index);
  g_control.packet.length = Swap16(length);
  g_control.trb.buffer = data;
  g_control.trb.length = length;
  g_control.trb.flags = 1;
  g_control.trb.callback = callback;
  g_control.trb.saved_endpoint = g_control.trb.endpoint;
  InterlockedExchange(&g_control_pending, 1);
  return ((QueueTransferFn)g_api->usbd_queue_async_transfer)(
      g_api->playback_handle, &g_control);
}

static LONG __cdecl IsochComplete(DWORD transfer, DWORD* statuses, WORD*) {
  int slot = -1;
  for (int index = 0; index < kIsochRingDepth; ++index) {
    if ((DWORD)&g_isoch[index] == transfer) {
      slot = index;
      break;
    }
  }
  if (slot < 0) return 0;

  bool successful = statuses != 0;
  for (int packet = 0; successful && packet < kIsochPacketCount; ++packet)
    successful = statuses[packet] == 0;
  bool active = !g_stopping && g_streaming && g_api &&
                g_api->playback_handle;
  if (successful && active && !g_notification_shown)
    InterlockedCompareExchange(&g_notification_event,
                               kNotificationConnected,
                               kNotificationNone);

  if (active) {
    FillUsbPacket(slot);
    ((QueueIsochTransferFn)g_api->usbd_queue_isoch_transfer)(
        g_api->playback_handle, &g_isoch[slot], g_packet_lengths);
  } else {
    InterlockedExchange(&g_isoch_slot_busy[slot], 0);
  }
  return 0;
}

static void StartIsoch() {
  const AudioProfile& profile = g_api->profile;
  memset(g_isoch, 0, sizeof(g_isoch));
  memset((void*)g_isoch_slot_busy, 0, sizeof(g_isoch_slot_busy));

  LONG status = ((OpenEndpointFn)g_api->usbd_open_endpoint)(
      g_api->playback_handle, 1, profile.endpoint_address,
      profile.endpoint_packet_size, 1, (DWORD*)&g_isoch[0]);
  if (status < 0) return;

  for (int slot = 0; slot < kIsochRingDepth; ++slot) {
    g_isoch[slot].endpoint = g_isoch[0].endpoint;
    g_isoch[slot].saved_endpoint = g_isoch[0].endpoint;
    g_isoch[slot].buffer = g_usb_packets[slot];
    g_isoch[slot].length = sizeof(g_usb_packets[slot]);
    g_isoch[slot].packet_count = kIsochPacketCount;
    g_isoch[slot].callback = (DWORD)IsochComplete;
  }
  for (int packet = 0; packet < kIsochPacketCount; ++packet)
    g_packet_lengths[packet] = kPacketBytes;
  InterlockedExchange(&g_streaming, 1);
}

static LONG __cdecl InterfaceComplete(DWORD, LONG status) {
  InterlockedExchange(&g_control_pending, 0);
  if (status == 0) StartIsoch();
  return status;
}

static LONG __cdecl SampleRateComplete(DWORD, LONG status) {
  if (status != 0) {
    InterlockedExchange(&g_control_pending, 0);
    return status;
  }
  return QueueControl(0x01, 0x0B, g_api->profile.alternate_setting,
                      g_api->profile.interface_number, 0, 0,
                      (DWORD)InterfaceComplete);
}

static LONG __cdecl ConfigurationComplete(DWORD, LONG status) {
  if (status != 0) {
    InterlockedExchange(&g_control_pending, 0);
    return status;
  }
  return QueueControl(0x22, 0x01, 0x0100,
                      g_api->profile.endpoint_address, 3, g_sample_rate,
                      (DWORD)SampleRateComplete);
}

static void StartDevice() {
  if (!g_api || !g_api->playback_handle ||
      !g_api->usbd_open_default_endpoint ||
      !g_api->usbd_open_endpoint ||
      !g_api->usbd_queue_async_transfer ||
      !g_api->usbd_queue_isoch_transfer) {
    return;
  }

  InterlockedExchange(&g_stopping, 0);
  memset(&g_control, 0, sizeof(g_control));
  if (((OpenDefaultFn)g_api->usbd_open_default_endpoint)(
          g_api->playback_handle, (DWORD*)&g_control) < 0) {
    return;
  }
  QueueControl(0x00, 0x09, g_api->profile.configuration, 0, 0, 0,
               (DWORD)ConfigurationComplete);
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

}  // namespace

BOOL AudioInitialize(const AudioHostApi* api) {
  if (!api || !StartRenderCapture()) return FALSE;
  g_api = api;
  g_last_handle = api->playback_handle;
  if (g_last_handle && !InterlockedExchange(&g_started, 1)) StartDevice();
  return TRUE;
}

VOID AudioTick(const AudioHostApi* api) {
  if (!api) return;
  if (api->playback_handle != g_last_handle) {
    g_last_handle = api->playback_handle;
    if (g_last_handle && !InterlockedExchange(&g_started, 1)) StartDevice();
  }

  if (!g_last_handle || !g_streaming || g_stopping) return;
  LONG available = g_pcm_write - g_pcm_read;
  if (!g_bridge_started) {
    if (available < kPcmTargetFrames) return;
    if (available > kPcmTargetFrames)
      InterlockedExchange(&g_pcm_read, g_pcm_write - kPcmTargetFrames);
    InterlockedExchange(&g_bridge_started, 1);
  } else if (available > kPcmHighWaterFrames) {
    InterlockedExchange(&g_pcm_read, g_pcm_write - kPcmTargetFrames);
  }

  for (int slot = 0; slot < kIsochRingDepth; ++slot) {
    if (InterlockedCompareExchange(&g_isoch_slot_busy[slot], 1, 0) != 0)
      continue;
    FillUsbPacket(slot);
    ((QueueIsochTransferFn)g_api->usbd_queue_isoch_transfer)(
        g_api->playback_handle, &g_isoch[slot], g_packet_lengths);
  }
}

VOID AudioDeviceRemoved() {
  InterlockedExchange(&g_stopping, 1);
  bool was_connected = InterlockedExchange(&g_streaming, 0) != 0 ||
                       g_notification_shown;
  InterlockedExchange(&g_notification_shown, 0);
  InterlockedExchange(&g_started, 0);
  InterlockedExchange(&g_bridge_started, 0);
  for (int slot = 0; slot < kIsochRingDepth; ++slot)
    InterlockedExchange(&g_isoch_slot_busy[slot], 0);
  InterlockedExchange(&g_pcm_read, g_pcm_write);
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
