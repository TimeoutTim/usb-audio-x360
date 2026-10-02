// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <xtl.h>

struct AudioProfile {
  BYTE configuration;
  BYTE interface_number;
  BYTE alternate_setting;
  BYTE endpoint_address;
  WORD endpoint_packet_size;
  BYTE audio_class_version;
  BYTE bytes_per_sample;
  BYTE feedback_endpoint_address;
  WORD feedback_endpoint_packet_size;
  BYTE control_interface_number;
  BYTE clock_source_id;
  BYTE feature_unit_id;
};

struct AudioHostApi {
  void* playback_handle;
  void* usbd_open_default_endpoint;
  void* usbd_open_endpoint;
  void* usbd_queue_async_transfer;
  void* usbd_queue_isoch_transfer;
  void* usbd_close_default_endpoint;
  void* usbd_close_endpoint;
  WORD vendor_id;
  WORD product_id;
  WORD usb_version, device_version;
  BYTE product_index, manufacturer_index;
  AudioProfile profile;
};

struct AudioDeviceInfo {
  BOOL connected;
  WORD vendor_id;
  WORD product_id;
  BYTE audio_class_version;
  BYTE playback_valid_bits;
  BYTE capture_channels;
  BYTE capture_valid_bits;
  BOOL microphone_available;
  WORD usb_version, device_version;
  WCHAR product_name[64], manufacturer_name[64];
  BYTE output_interface, output_alternate, output_endpoint;
  BYTE capture_interface, capture_alternate, capture_endpoint;
  BYTE feedback_endpoint, clock_id, sync_type;
  WORD output_packet_bytes;
  BYTE advertised_outputs, advertised_inputs;
  BYTE max_output_bits, max_input_bits;
  DWORD last_error;
  BOOL streaming;
  BYTE name_status; // 0 pending, 1 complete, 2 unavailable/failed.
};

BOOL AudioInitialize(const AudioHostApi* api);
VOID AudioTick(const AudioHostApi* api);
VOID AudioDeviceRemoved();
VOID AudioNotificationTick();
BOOL AudioSetVolume(LONG percent);
LONG AudioGetVolume();
VOID AudioSetOutputMuted(BOOL muted);
BOOL AudioIsOutputMuted();
VOID AudioSetMicrophoneMuted(BOOL muted);
BOOL AudioIsMicrophoneMuted();
BOOL AudioSetMicrophoneGain(LONG percent);
LONG AudioGetMicrophoneGain();
BOOL AudioGetDeviceInfo(AudioDeviceInfo* info);
// Loopback status: 0 off, 1 on. Independent bounded ring, no saved recording.
BOOL AudioStartMicrophoneTest();
VOID AudioStopMicrophoneTest();
VOID AudioKeepMicrophoneTestAlive();
LONG AudioGetMicrophoneTestStatus(LONG* peak);
VOID AudioGetMicrophonePeak(LONG* peak, BOOL* clipped);
BOOL AudioMicrophoneAvailable();
BOOL AudioSubmitMicrophonePacket(void* packet);
