// SPDX-License-Identifier: GPL-3.0-or-later
#include <xtl.h>
#include <string.h>

#include "audio.h"
#include "diagnostics.h"
#include "guide_ui.h"
#include "device_claim_gate.h"
#include "playback_profile.h"
#include "uac_descriptors.h"
#include "xbox_usb_transport.h"
#include "detour.h"

extern "C" LONG XexGetProcedureAddress(HANDLE module, DWORD ordinal,
                                        PVOID address);
extern "C" volatile DWORD UsbAudioDiagnostic[64];
extern "C" DWORD NTAPI ExCreateThread(
    PHANDLE handle, DWORD stack_size, LPDWORD thread_id,
    PVOID api_thread_startup, LPTHREAD_START_ROUTINE start_address,
    LPVOID parameter, DWORD creation_flags);
extern "C" VOID XapiThreadStartup(VOID (__cdecl* start_routine)(VOID*),
                                    PVOID start_context, DWORD exit_code);

namespace {

const WORD kSupportedKernel = 17559;
const DWORD kUsbDeviceStateTable = 0x801A8220;
const DWORD kUsbHcdTable = 0x801A8230;
const DWORD kGetInterfaceDescriptor = 0x800D8500;
const DWORD kResetRootHubPort = 0x800D7D20;
const DWORD kNodePoolBytes = 0x1000;
const DWORD kVoiceBindingCategory = 1;
const DWORD kVirtualVoiceDeviceId = 0xA7554D49;
// XVoiced context family 4 is the standalone voice-device path. The low
// 28 bits select its player/device slot, so player zero uses slot zero.
const DWORD kVirtualStandaloneVoiceContext = 0x40000000;
const DWORD kRetailBindWrapper21256 = 0x816D9060;
const DWORD kRetailBindingTablePointer21256 = 0x81A82FF4;
const DWORD kVoiceBindingRetryMs = 2000;

struct XboxKernelVersion {
  WORD major;
  WORD minor;
  WORD build;
  WORD qfe;
};

struct DeviceHandle;

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

struct __declspec(align(2)) DriverExtension {
  DeviceHandle* device_handle;
  UsbTrb interrupt_trb;
  BYTE interface_number;
  BYTE gap20[3];
  UsbControlTrb control_trb;
  BYTE gap4c[4];
  DWORD cleanup_handler;
  BYTE gap54[24];
  DWORD queue;
  BYTE always_one;
  BYTE always_one_two;
  BYTE unknown_flag;
  BYTE always_zero;
  BYTE cleanup_done;
  BYTE init_transfer_pending;
  BYTE always_zero_two;
  BYTE device_type;
  BYTE always_zero_three;
  BYTE always_zero_four;
};

struct DeviceHandle {
  DriverExtension* driver;
};

// Retail XAM 21256 keeps one 0x40-byte device-binding record per player.
// Category zero remains the controller association; category one is the
// active chat route switched by an original wireless headset's quadrant
// button. This layout is used only after ResolveVoiceBinding validates the
// corresponding retail binding routine instruction-for-instruction.
struct VoiceBindingRecord {
  DWORD primary_device_id;
  DWORD secondary_device_id;
  struct {
    DWORD active;
    DWORD context;
  } category[7];
};

struct UsbDeviceDescriptor {
  BYTE length;
  BYTE descriptor_type;
  WORD usb_version;
  BYTE device_class;
  BYTE device_subclass;
  BYTE device_protocol;
  BYTE max_packet_size0;
  WORD vendor_id;
  WORD product_id;
  WORD device_version;
  BYTE manufacturer_index;
  BYTE product_index;
  BYTE serial_index;
  BYTE configuration_count;
};

struct UsbInterfaceDescriptor {
  BYTE length;
  BYTE descriptor_type;
  BYTE interface_number;
  BYTE alternate_setting;
  BYTE endpoint_count;
  BYTE interface_class;
  BYTE interface_subclass;
  BYTE interface_protocol;
  BYTE interface_index;
};

typedef UsbDeviceDescriptor* (*GetDeviceDescriptorFn)(DeviceHandle*);
typedef UsbInterfaceDescriptor* (*GetInterfaceDescriptorFn)(DeviceHandle*);
typedef int (*AddDeviceCompleteFn)(DeviceHandle*, int);
typedef LONG (*RemoveDeviceCompleteFn)(DeviceHandle*);
typedef DeviceHandle* (*GetRootHubDeviceNodeFn)(DWORD);
typedef DeviceHandle* (*GetPortDeviceNodeFn)(DeviceHandle*, DWORD);
typedef VOID (*ResetRootHubPortFn)(DeviceHandle*, DWORD);
typedef PVOID (*XexPcToFileHeaderFn)(PVOID, PVOID*);
typedef BOOL (*XamVoiceHeadsetPresentFn)(void*);
typedef LONG (*XamVoiceSubmitPacketFn)(void*, DWORD, void*);
typedef LONG (*XamVoiceGetBatteryStatusFn)(DWORD, DWORD*);
typedef int (*XamUserBindDeviceCallbackFn)(DWORD, DWORD, BYTE, BOOL, BYTE*);
typedef LONG (*XamUserGetDeviceContextFn)(DWORD, DWORD, DWORD*);

static GetDeviceDescriptorFn g_get_device_descriptor = 0;
static GetInterfaceDescriptorFn g_get_interface_descriptor = 0;
static AddDeviceCompleteFn g_add_device_complete = 0;
static RemoveDeviceCompleteFn g_remove_device_complete = 0;
static GetRootHubDeviceNodeFn g_get_root_hub_device_node = 0;
static GetPortDeviceNodeFn g_get_port_device_node = 0;
static PowerPcDetour g_add_detour;
static PowerPcDetour g_remove_detour;
static PowerPcDetour g_voice_present_detour;
static PowerPcDetour g_voice_submit_detour;
static PowerPcDetour g_voice_battery_detour;
static XamUserBindDeviceCallbackFn g_bind_device = 0;
static XamUserGetDeviceContextFn g_get_device_context = 0;
static volatile LONG g_voice_binding_active = 0;
static BYTE g_voice_binding_user = 0xFF;
static DWORD g_voice_binding_retry_at = 0;
static DWORD g_displaced_voice_context = 0;
static DWORD g_displaced_voice_device_id = 0;
static DriverExtension g_extension;
static volatile DeviceHandle* g_playback_handle = 0;
static usb_transport::DeviceClaimGate g_device_gate;
static AudioHostApi g_audio_api;

static BOOL XamVoiceHeadsetPresentHook(void* handle) {
  XamVoiceHeadsetPresentFn original =
      g_voice_present_detour.Original<XamVoiceHeadsetPresentFn>();
  if (original && original(handle)) return TRUE;
  return AudioMicrophoneAvailable();
}

static LONG XamVoiceSubmitPacketHook(void* handle, DWORD direction,
                                     void* packet) {
  XamVoiceHeadsetPresentFn present =
      g_voice_present_detour.Original<XamVoiceHeadsetPresentFn>();
  XamVoiceSubmitPacketFn submit =
      g_voice_submit_detour.Original<XamVoiceSubmitPacketFn>();
  // A category-1 virtual binding makes the original presence query succeed,
  // but it has no radio hardware from which XVoiced can obtain packets. Feed
  // that binding from USB; before it is active, preserve native arbitration.
  if (direction == 1 &&
      (g_voice_binding_active || !present || !present(handle)) &&
      AudioSubmitMicrophonePacket(packet))
    return 0;
  return submit ? submit(handle, direction, packet) : (LONG)0x80004005;
}

static LONG XamVoiceGetBatteryStatusHook(DWORD user, DWORD* status) {
  // The virtual headset is USB-powered and therefore always available at
  // full charge. XAM/XVoiced use the same four-level value as XInput, where
  // 3 is full. Do not alter battery reporting for any native voice device.
  if (status && g_voice_binding_active && user == g_voice_binding_user) {
    *status = 3;
    return 0;
  }
  XamVoiceGetBatteryStatusFn original =
      g_voice_battery_detour.Original<XamVoiceGetBatteryStatusFn>();
  return original ? original(user, status) : (LONG)0x80004005;
}

static bool ResolveVoiceBinding() {
  HANDLE xam = GetModuleHandleA("xam.xex");
  if (!xam) return false;
  XexGetProcedureAddress(xam, 520, &g_get_device_context);

  static const DWORD expected[] = {
      0x7C8B2378, 0x7CA42B78, 0x54CA063F, 0x41820010,
      0x7CE53B78, 0x7D635B78, 0x4BFFFED8, 0x7C852378,
      0x7CE63B78, 0x7D645B78, 0x4BFFFC50,
  };
  volatile const DWORD* code =
      (volatile const DWORD*)kRetailBindWrapper21256;
  for (DWORD i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
    if (code[i] != expected[i]) {
      return false;
    }
  }
  g_bind_device =
      (XamUserBindDeviceCallbackFn)kRetailBindWrapper21256;
  return true;
}

static VoiceBindingRecord* VoiceBindingRecords() {
  volatile VoiceBindingRecord* const* pointer =
      (volatile VoiceBindingRecord* const*)kRetailBindingTablePointer21256;
  VoiceBindingRecord* records = (VoiceBindingRecord*)*pointer;
  const DWORD address = (DWORD)records;
  if (!records || (address & 3) || address < 0x80000000 ||
      address >= 0xA0000000) {
    return 0;
  }
  return records;
}

static void RestoreDisplacedVoiceRoute() {
  if (!g_displaced_voice_context || !g_bind_device) return;
  BYTE requested_user = 0;
  g_bind_device(g_displaced_voice_device_id, g_displaced_voice_context,
                (BYTE)kVoiceBindingCategory, FALSE, &requested_user);
  g_displaced_voice_context = 0;
  g_displaced_voice_device_id = 0;
}

static bool MakePlayerZeroVoiceSlotAvailable() {
  DWORD context = 0;
  if (!g_get_device_context ||
      g_get_device_context(0, kVoiceBindingCategory, &context) < 0 ||
      !context || context == kVirtualStandaloneVoiceContext) {
    return true;
  }

  VoiceBindingRecord* records = VoiceBindingRecords();
  if (!records || !records[0].category[kVoiceBindingCategory].active ||
      records[0].category[kVoiceBindingCategory].context != context) {
    return false;
  }

  const DWORD family = context & 0xF0000000;
  g_displaced_voice_context = context;
  g_displaced_voice_device_id = family == 0x40000000 ||
      family == 0x50000000
      ? records[0].secondary_device_id
      : records[0].primary_device_id;

  BYTE removed_user = 0xFF;
  const int result = g_bind_device(
      g_displaced_voice_device_id, context, (BYTE)kVoiceBindingCategory,
      TRUE, &removed_user);
  if (result < 0 || removed_user != 0) {
    g_displaced_voice_context = 0;
    g_displaced_voice_device_id = 0;
    return false;
  }
  return true;
}

static void UnbindVirtualVoiceHeadset() {
  if (!g_voice_binding_active || !g_bind_device) return;
  BYTE removed_user = 0xFF;
  g_bind_device(
      kVirtualVoiceDeviceId, kVirtualStandaloneVoiceContext,
      (BYTE)kVoiceBindingCategory, TRUE, &removed_user);
  InterlockedExchange(&g_voice_binding_active, 0);
  g_voice_binding_user = 0xFF;
  RestoreDisplacedVoiceRoute();
}

static void VoiceBindingTick() {
  const bool microphone_available = AudioMicrophoneAvailable() != FALSE;
  if (!microphone_available) {
    UnbindVirtualVoiceHeadset();
    return;
  }
  if (g_voice_binding_active || !g_bind_device) return;
  if (XUserGetSigninState(0) == eXUserSigninState_NotSignedIn) return;

  const DWORD now = GetTickCount();
  if ((LONG)(now - g_voice_binding_retry_at) < 0) return;
  g_voice_binding_retry_at = now + kVoiceBindingRetryMs;

  // XAM has a single active chat route per player. This is the same category
  // an original standalone wireless headset replaces, while the controller
  // remains associated independently in category zero. Save the exact prior
  // route so disconnect and every failure path are reversible.
  if (!MakePlayerZeroVoiceSlotAvailable()) return;

  BYTE requested_user = 0;
  int result = g_bind_device(
      kVirtualVoiceDeviceId, kVirtualStandaloneVoiceContext,
      (BYTE)kVoiceBindingCategory, FALSE, &requested_user);
  if (result < 0) {
    RestoreDisplacedVoiceRoute();
    return;
  }

  // This first implementation deliberately emulates a headset pinned to the
  // first quadrant. Silently accepting XAM's fallback slot would associate
  // voice with the wrong profile.
  if (requested_user != 0 ||
      XUserGetSigninState(0) == eXUserSigninState_NotSignedIn) {
    BYTE removed_user = 0xFF;
    g_bind_device(
        kVirtualVoiceDeviceId, kVirtualStandaloneVoiceContext,
        (BYTE)kVoiceBindingCategory, TRUE, &removed_user);
    RestoreDisplacedVoiceRoute();
    return;
  }

  g_voice_binding_user = requested_user;
  InterlockedExchange(&g_voice_binding_active, 1);
}

template <typename T>
static bool Resolve(HANDLE module, DWORD ordinal, T* result) {
  *result = 0;
  return XexGetProcedureAddress(module, ordinal, result) >= 0 && *result != 0;
}

static WORD ReadWord(const BYTE* value) {
  return (WORD)value[0] | ((WORD)value[1] << 8);
}

static bool InNodePool(const BYTE* pool, const void* address) {
  const BYTE* value = (const BYTE*)address;
  return pool && value >= pool && value < pool + kNodePoolBytes &&
         (((DWORD)(value - pool) & 0x0F) == 0);
}

static bool HasSupportedPlayback(const BYTE* configuration) {
  if (!configuration || configuration[0] < 9 ||
      configuration[1] != 2) {
    return false;
  }
  const WORD total = ReadWord(configuration + 2);
  if (total < configuration[0] || total > 0x400) return false;

  uac::Format formats[32];
  size_t count = 0;
  if (uac::Discover(configuration, total, formats, 32, &count) != uac::kOk)
    return false;
  for (size_t index = 0; index < count; ++index) {
    if (uac::SupportedFullSpeedPlayback(formats[index])) return true;
  }
  return false;
}

static bool ValidateResetRoutine() {
  const volatile DWORD* code = (const volatile DWORD*)kResetRootHubPort;
  return code[0] == 0x7D8802A6 && code[1] == 0x9181FFF8 &&
         code[2] == 0xFBE1FFF0 && code[3] == 0x9421FFA0;
}

static bool PoolContainsOnlyTarget(BYTE* target_pool,
                                   DeviceHandle* target) {
  if (!target_pool || !target) return false;
  for (DWORD controller = 0; controller < 4; ++controller) {
    BYTE* hcd = ((BYTE**)kUsbHcdTable)[controller];
    if (!hcd || *(BYTE**)(hcd + 0x40) != target_pool) continue;
    DeviceHandle* root = g_get_root_hub_device_node(controller);
    if (!root) return false;
    for (DWORD port = 0; port < 8; ++port) {
      DeviceHandle* device = g_get_port_device_node(root, port);
      if (device && device != target) return false;
    }
  }
  return true;
}

// DashLaunch can load the plugin after the kernel has already rejected the
// DAC's AudioStreaming interface. Re-enumerate exactly one verified root-port
// device so the installed add hook gets another chance to claim it. This is
// intentionally stricter than hotplug: ambiguous mappings or a controller
// pool shared with any other physical device fail closed.
static bool ReenumerateBootDevice() {
  if (g_playback_handle || !g_device_gate.available() ||
      !ValidateResetRoutine()) {
    return false;
  }

  DeviceHandle* selected = 0;
  DeviceHandle* selected_root = 0;
  BYTE* selected_hcd = 0;
  DWORD selected_port = 0;
  DWORD matches = 0;

  for (DWORD controller = 0; controller < 4; ++controller) {
    BYTE* hcd = ((BYTE**)kUsbHcdTable)[controller];
    BYTE* context = ((BYTE**)kUsbDeviceStateTable)[controller];
    DeviceHandle* root = g_get_root_hub_device_node(controller);
    if (!hcd || !(hcd[0x96] & 2) || !context || !root ||
        *(BYTE**)context != hcd ||
        *(DeviceHandle**)(hcd + 0x48) != root ||
        *(DWORD*)(context + 4) != 0 || *(DWORD*)(context + 8) != 0 ||
        context[0x27A] != controller || context[0x27E] != 0 ||
        !HasSupportedPlayback(context + 0x60)) {
      continue;
    }

    UsbDeviceDescriptor* cached = (UsbDeviceDescriptor*)(context + 0x4C);
    for (DWORD port = 0; port < 8; ++port) {
      DeviceHandle* device = g_get_port_device_node(root, port);
      UsbDeviceDescriptor* descriptor = device
          ? g_get_device_descriptor(device) : 0;
      if (!descriptor || descriptor->vendor_id != cached->vendor_id ||
          descriptor->product_id != cached->product_id) {
        continue;
      }
      ++matches;
      selected = device;
      selected_root = root;
      selected_hcd = hcd;
      selected_port = port;
    }
  }

  BYTE* pool = selected_hcd ? *(BYTE**)(selected_hcd + 0x40) : 0;
  if (matches != 1 || !selected || !selected_root ||
      !InNodePool(pool, selected) || !InNodePool(pool, selected_root) ||
      g_get_port_device_node(selected_root, selected_port) != selected ||
      !PoolContainsOnlyTarget(pool, selected)) {
    return false;
  }

  ((ResetRootHubPortFn)kResetRootHubPort)(selected_root, selected_port);
  return true;
}

static bool ProfileForHandle(DeviceHandle* handle, AudioProfile* profile,
                             BYTE* rejection) {
  if (rejection) *rejection = 0;
  if (!handle || !profile || !g_get_interface_descriptor) {
    if (rejection) *rejection = 1;
    return false;
  }
  UsbInterfaceDescriptor* interface_descriptor =
      g_get_interface_descriptor(handle);
  if (!interface_descriptor) {
    if (rejection) *rejection = 2;
    return false;
  }
  if (interface_descriptor->alternate_setting != 0) {
    if (rejection) *rejection = 3;
    return false;
  }
  if (interface_descriptor->interface_class != 1) {
    if (rejection) *rejection = 4;
    return false;
  }
  if (interface_descriptor->interface_subclass != 2) {
    if (rejection) *rejection = 5;
    return false;
  }
  BYTE protocol = interface_descriptor->interface_protocol;
  if (protocol != 0 && protocol != 0x20) {
    if (rejection) *rejection = 6;
    return false;
  }
  // First test supports only the OHCI path whose control-length contract was
  // inspected. Do not assume high-speed/EHCI endpoint or completion semantics.
  DWORD controller = *((BYTE*)handle + 8) & 3;
  BYTE* hcd = ((BYTE**)kUsbHcdTable)[controller];
  if (!hcd) {
    if (rejection) *rejection = 7;
    return false;
  }
  if (!(hcd[0x96] & 2)) {
    if (rejection) *rejection = 8;
    return false;
  }
  memset(profile, 0, sizeof(*profile));
  profile->audio_class_version = protocol == 0x20 ? 2 : 1;
  profile->interface_number = interface_descriptor->interface_number;
  return true;
}

static void RecordDevice(DeviceHandle* handle, int status,
                         UsbAudioDeviceDecision decision, BYTE rejection) {
  UsbInterfaceDescriptor* interface_descriptor =
      handle && g_get_interface_descriptor
          ? g_get_interface_descriptor(handle) : 0;
  // Only AudioStreaming interfaces are relevant. Avoid filling the bounded
  // queue with unrelated failed USB interfaces on composite devices.
  if (!interface_descriptor || interface_descriptor->interface_class != 1 ||
      interface_descriptor->interface_subclass != 2) return;
  UsbDeviceDescriptor* device_descriptor =
      handle && g_get_device_descriptor ? g_get_device_descriptor(handle) : 0;
  UsbAudioDeviceObservation observation;
  memset(&observation, 0, sizeof(observation));
  if (device_descriptor) {
    observation.vendor_id = device_descriptor->vendor_id;
    observation.product_id = device_descriptor->product_id;
    observation.usb_version = device_descriptor->usb_version;
    observation.device_version = device_descriptor->device_version;
    observation.device_class = device_descriptor->device_class;
    observation.device_subclass = device_descriptor->device_subclass;
    observation.device_protocol = device_descriptor->device_protocol;
    observation.configuration_count = device_descriptor->configuration_count;
  }
  observation.interface_number = interface_descriptor->interface_number;
  observation.alternate_setting = interface_descriptor->alternate_setting;
  observation.interface_class = interface_descriptor->interface_class;
  observation.interface_subclass = interface_descriptor->interface_subclass;
  observation.interface_protocol = interface_descriptor->interface_protocol;
  observation.endpoint_count = interface_descriptor->endpoint_count;
  observation.controller = handle ? (*((BYTE*)handle + 8) & 3) : 0xff;
  observation.rejection = rejection;
  observation.add_status = status;
  observation.decision = decision;
  DiagnosticsObserveDevice(observation);
}

static int AddDeviceCompleteHook(DeviceHandle* handle, int status) {
  // The first compatible playback interface owns the only plugin slot. A
  // secondary DAC/interface is left entirely to the kernel's normal path.
  if (status == 0)
    return g_add_detour.Original<AddDeviceCompleteFn>()(handle, status);

  AudioProfile profile;
  BYTE rejection = 0;
  if (!ProfileForHandle(handle, &profile, &rejection)) {
    RecordDevice(handle, status, kDeviceRejectedProfile, rejection);
    return g_add_detour.Original<AddDeviceCompleteFn>()(handle, status);
  }
  if (!g_device_gate.available()) {
    RecordDevice(handle, status, kDeviceSlotBusy, 0);
    return g_add_detour.Original<AddDeviceCompleteFn>()(handle, status);
  }
  if (!g_device_gate.Activate(handle)) {
    RecordDevice(handle, status, kDeviceSlotBusy, 0);
    return g_add_detour.Original<AddDeviceCompleteFn>()(handle, status);
  }
  if (!UsbTransportAttach(handle)) {
    RecordDevice(handle, status, kDeviceTransportRejected, 0);
    g_device_gate.CancelActivation(handle);
    return g_add_detour.Original<AddDeviceCompleteFn>()(handle, status);
  }
  RecordDevice(handle, status, kDeviceClaimed, 0);
  UsbAudioDiagnostic[0] = profile.audio_class_version;
  memset(&g_extension, 0, sizeof(g_extension));
  g_extension.device_handle = handle;
  g_extension.interface_number = profile.interface_number;
  g_extension.interrupt_trb.flags = 1;
  handle->driver = &g_extension;
  UsbDeviceDescriptor* device_descriptor = g_get_device_descriptor(handle);
  // USB descriptors are little-endian; the PPC native WORD view is swapped.
  const BYTE* device_bytes = (const BYTE*)device_descriptor;
  g_audio_api.vendor_id = device_bytes ? device_bytes[8] | (device_bytes[9] << 8) : 0;
  g_audio_api.product_id = device_bytes ? device_bytes[10] | (device_bytes[11] << 8) : 0;
  g_audio_api.usb_version = device_bytes ? device_bytes[2] | (device_bytes[3] << 8) : 0;
  g_audio_api.device_version = device_bytes ? device_bytes[12] | (device_bytes[13] << 8) : 0;
  g_audio_api.manufacturer_index = device_bytes ? device_bytes[14] : 0;
  g_audio_api.product_index = device_bytes ? device_bytes[15] : 0;
  g_audio_api.profile = profile;
  InterlockedExchange((volatile LONG*)&g_playback_handle, (LONG)handle);
  int result = g_add_detour.Original<AddDeviceCompleteFn>()(handle, 0);
  // This internal completion routine does not expose a documented
  // NTSTATUS-style success contract. The established working path claims the
  // interface after forcing the completion status to zero and preserves its
  // return value only for the caller.
  return result;
}

static void DeviceRearmComplete() {
  // Invoked only after transport ownership has drained and reset, in the same
  // USB DPC domain that serializes add/remove callbacks.
  g_device_gate.CompleteRemoval();
}

static LONG RemoveDeviceCompleteHook(DeviceHandle* handle) {
  if (handle && g_device_gate.BeginRemoval(handle)) {
    UsbRemoveCompleteRoutine remove_complete =
        (UsbRemoveCompleteRoutine)
            g_remove_detour.Original<RemoveDeviceCompleteFn>();
    UsbTransportDetach(handle, remove_complete, DeviceRearmComplete);
    InterlockedExchange((volatile LONG*)&g_playback_handle, 0);
    AudioDeviceRemoved();
    return 0;
  }
  return g_remove_detour.Original<RemoveDeviceCompleteFn>()(handle);
}

static DWORD WINAPI AudioWorker(void*) {
  Sleep(2000);
  g_audio_api.playback_handle = (void*)g_playback_handle;
  if (!AudioInitialize(&g_audio_api)) return 0;

  // Give normal enumeration time to reach the hook before recovering a DAC
  // that the kernel rejected before DashLaunch loaded this plugin.
  for (DWORD settle = 0; settle < 500 && !g_playback_handle; ++settle)
    Sleep(10);
  if (!g_playback_handle) ReenumerateBootDevice();

  for (;;) {
    g_audio_api.playback_handle = (void*)g_playback_handle;
    AudioTick(&g_audio_api);
    Sleep(g_playback_handle ? 0 : 10);
  }
}

static DWORD WINAPI NotificationWorker(void*) {
  for (;;) {
    GuideUiTick();
    AudioNotificationTick();
    VoiceBindingTick();
    DiagnosticsTick();
    Sleep(100);
  }
}

static bool StartWorker(LPTHREAD_START_ROUTINE entry, int priority,
                        bool pin_notification) {
  HANDLE thread = 0;
  const DWORD kSystemThreadFlags = 0x18000427;
  DWORD status = ExCreateThread(&thread, 0, 0, (PVOID)XapiThreadStartup,
                                entry, 0, kSystemThreadFlags);
  if (status != 0 || !thread) return false;
  if (pin_notification) XSetThreadProcessor(thread, 5);
  SetThreadPriority(thread, priority);
  bool started = ResumeThread(thread) != (DWORD)-1;
  CloseHandle(thread);
  return started;
}

}  // namespace

extern "C" BOOL APIENTRY DllMain(HANDLE module, DWORD reason, LPVOID) {
  if (reason != DLL_PROCESS_ATTACH) return TRUE;

  HANDLE kernel = GetModuleHandleA("xboxkrnl.exe");
  XboxKernelVersion* version = 0;
  if (!kernel || !Resolve(kernel, 344, &version) || !version ||
      version->build != kSupportedKernel) {
    return TRUE;
  }
  XexPcToFileHeaderFn pc_to_file_header = 0;
  PVOID loader_entry = module;
  if (Resolve(kernel, 412, &pc_to_file_header))
    pc_to_file_header((PVOID)DiagnosticsInitialize, &loader_entry);
  DiagnosticsInitialize(loader_entry, version->build);

  if (!Resolve(kernel, 759, &g_get_device_descriptor) ||
      !Resolve(kernel, 740, &g_add_device_complete) ||
      !Resolve(kernel, 751, &g_remove_device_complete) ||
      !Resolve(kernel, 880, &g_get_root_hub_device_node) ||
      !Resolve(kernel, 881, &g_get_port_device_node)) {
    return TRUE;
  }

  memset(&g_audio_api, 0, sizeof(g_audio_api));
  Resolve(kernel, 746, &g_audio_api.usbd_open_default_endpoint);
  Resolve(kernel, 747, &g_audio_api.usbd_open_endpoint);
  Resolve(kernel, 748, &g_audio_api.usbd_queue_async_transfer);
  Resolve(kernel, 895, &g_audio_api.usbd_queue_isoch_transfer);
  Resolve(kernel, 749, &g_audio_api.usbd_close_default_endpoint);
  Resolve(kernel, 750, &g_audio_api.usbd_close_endpoint);
  if (!g_audio_api.usbd_open_default_endpoint ||
      !g_audio_api.usbd_open_endpoint ||
      !g_audio_api.usbd_queue_async_transfer ||
      !g_audio_api.usbd_queue_isoch_transfer) {
    return TRUE;
  }

  g_get_interface_descriptor =
      (GetInterfaceDescriptorFn)kGetInterfaceDescriptor;
  if (!UsbTransportInitialize(&g_audio_api)) return TRUE;
  g_add_detour = PowerPcDetour((void*)g_add_device_complete,
                               (const void*)AddDeviceCompleteHook);
  g_remove_detour = PowerPcDetour((void*)g_remove_device_complete,
                                  (const void*)RemoveDeviceCompleteHook);
  if (!g_remove_detour.Install() || !g_add_detour.Install()) {
    g_add_detour.Remove();
    g_remove_detour.Remove();
    return TRUE;
  }

  // XHV2 is statically linked into titles and reaches the persistent XAM
  // voice service through these exports. Hooking only presence and microphone
  // packet submission preserves native voice behavior and avoids title hooks.
  HANDLE xam = GetModuleHandleA("xam.xex");
  XamVoiceHeadsetPresentFn voice_present = 0;
  XamVoiceSubmitPacketFn voice_submit = 0;
  XamVoiceGetBatteryStatusFn voice_battery = 0;
  if (xam && Resolve(xam, 0x30D, &voice_present) &&
      Resolve(xam, 0x30E, &voice_submit) &&
      Resolve(xam, 0x310, &voice_battery)) {
    ResolveVoiceBinding();
    g_voice_present_detour = PowerPcDetour(
        (void*)voice_present, (const void*)XamVoiceHeadsetPresentHook);
    g_voice_submit_detour = PowerPcDetour(
        (void*)voice_submit, (const void*)XamVoiceSubmitPacketHook);
    g_voice_battery_detour = PowerPcDetour(
        (void*)voice_battery, (const void*)XamVoiceGetBatteryStatusHook);
    if (!g_voice_present_detour.Install() ||
        !g_voice_submit_detour.Install() ||
        !g_voice_battery_detour.Install()) {
      g_voice_battery_detour.Remove();
      g_voice_submit_detour.Remove();
      g_voice_present_detour.Remove();
    }
  }

  if (!StartWorker((LPTHREAD_START_ROUTINE)AudioWorker,
                   THREAD_PRIORITY_ABOVE_NORMAL, false)) {
    g_add_detour.Remove();
    g_remove_detour.Remove();
    return TRUE;
  }
  StartWorker((LPTHREAD_START_ROUTINE)NotificationWorker,
              THREAD_PRIORITY_BELOW_NORMAL, true);
  return TRUE;
}
