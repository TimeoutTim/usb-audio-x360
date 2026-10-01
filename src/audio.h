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
  AudioProfile profile;
};

BOOL AudioInitialize(const AudioHostApi* api);
VOID AudioTick(const AudioHostApi* api);
VOID AudioDeviceRemoved();
VOID AudioNotificationTick();
BOOL AudioSetVolume(LONG percent);
LONG AudioGetVolume();
BOOL AudioMicrophoneAvailable();
BOOL AudioSubmitMicrophonePacket(void* packet);
