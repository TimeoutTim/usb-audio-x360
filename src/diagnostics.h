// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <xtl.h>

#include "audio.h"
#include "uac_descriptors.h"

enum UsbAudioDeviceDecision {
  kDeviceRejectedProfile = 1,
  kDeviceSlotBusy = 2,
  kDeviceTransportRejected = 3,
  kDeviceClaimed = 4,
};

struct UsbAudioDeviceObservation {
  WORD vendor_id;
  WORD product_id;
  WORD usb_version;
  WORD device_version;
  BYTE device_class;
  BYTE device_subclass;
  BYTE device_protocol;
  BYTE configuration_count;
  BYTE interface_number;
  BYTE alternate_setting;
  BYTE interface_class;
  BYTE interface_subclass;
  BYTE interface_protocol;
  BYTE endpoint_count;
  BYTE controller;
  BYTE rejection;
  LONG add_status;
  UsbAudioDeviceDecision decision;
};

// Producers only copy into a bounded in-memory queue. DiagnosticsTick performs
// all filesystem and formatting work from the low-priority notification thread.
void DiagnosticsInitialize(HANDLE module, WORD kernel_build);
void DiagnosticsObserveDevice(const UsbAudioDeviceObservation& observation);
void DiagnosticsConfiguration(const BYTE* descriptor, DWORD length);
void DiagnosticsSelected(const AudioProfile& profile,
                         const uac::Format& format);
void DiagnosticsFailure(DWORD error, DWORD stage, DWORD request,
                        DWORD index, LONG status, DWORD received,
                        DWORD clock_error, const AudioProfile& profile,
                        const uac::Format& format);
void DiagnosticsStreaming();
void DiagnosticsDisconnected();
void DiagnosticsTick();
