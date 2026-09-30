// SPDX-License-Identifier: GPL-3.0-or-later
#include <xtl.h>
#include <string.h>

#include "audio.h"
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

static GetDeviceDescriptorFn g_get_device_descriptor = 0;
static GetInterfaceDescriptorFn g_get_interface_descriptor = 0;
static AddDeviceCompleteFn g_add_device_complete = 0;
static RemoveDeviceCompleteFn g_remove_device_complete = 0;
static GetRootHubDeviceNodeFn g_get_root_hub_device_node = 0;
static GetPortDeviceNodeFn g_get_port_device_node = 0;
static PowerPcDetour g_add_detour;
static PowerPcDetour g_remove_detour;
static DriverExtension g_extension;
static volatile DeviceHandle* g_playback_handle = 0;
static usb_transport::DeviceClaimGate g_device_gate;
static AudioHostApi g_audio_api;

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

static bool ProfileForHandle(DeviceHandle* handle, AudioProfile* profile) {
  if (!handle || !profile || !g_get_interface_descriptor) return false;
  UsbInterfaceDescriptor* interface_descriptor =
      g_get_interface_descriptor(handle);
  if (!interface_descriptor ||
      interface_descriptor->alternate_setting != 0 ||
      interface_descriptor->interface_class != 1 ||
      interface_descriptor->interface_subclass != 2) {
    return false;
  }
  BYTE protocol = interface_descriptor->interface_protocol;
  if (protocol != 0 && protocol != 0x20) return false;
  // First test supports only the OHCI path whose control-length contract was
  // inspected. Do not assume high-speed/EHCI endpoint or completion semantics.
  DWORD controller = *((BYTE*)handle + 8) & 3;
  BYTE* hcd = ((BYTE**)kUsbHcdTable)[controller];
  if (!hcd || !(hcd[0x96] & 2)) return false;
  memset(profile, 0, sizeof(*profile));
  profile->audio_class_version = protocol == 0x20 ? 2 : 1;
  profile->interface_number = interface_descriptor->interface_number;
  return true;
}

static int AddDeviceCompleteHook(DeviceHandle* handle, int status) {
  // The first compatible playback interface owns the only plugin slot. A
  // secondary DAC/interface is left entirely to the kernel's normal path.
  if (status == 0 || !g_device_gate.available())
    return g_add_detour.Original<AddDeviceCompleteFn>()(handle, status);

  AudioProfile profile;
  if (ProfileForHandle(handle, &profile)) {
    if (!g_device_gate.Activate(handle))
      return g_add_detour.Original<AddDeviceCompleteFn>()(handle, status);
    if (!UsbTransportAttach(handle)) {
      g_device_gate.CancelActivation(handle);
      return g_add_detour.Original<AddDeviceCompleteFn>()(handle, status);
    }
    UsbAudioDiagnostic[0] = profile.audio_class_version;
    memset(&g_extension, 0, sizeof(g_extension));
    g_extension.device_handle = handle;
    g_extension.interface_number = profile.interface_number;
    g_extension.interrupt_trb.flags = 1;
    handle->driver = &g_extension;
    g_audio_api.profile = profile;
    InterlockedExchange((volatile LONG*)&g_playback_handle, (LONG)handle);
    int result = g_add_detour.Original<AddDeviceCompleteFn>()(handle, 0);
    // This internal completion routine does not expose a documented
    // NTSTATUS-style success contract. The established working path claims the
    // interface after forcing the completion status to zero and preserves its
    // return value only for the caller.
    return result;
  }
  return g_add_detour.Original<AddDeviceCompleteFn>()(handle, status);
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

static void PollVolumeChord() {
  static WORD previous_buttons[XUSER_MAX_COUNT] = {0};
  for (DWORD user = 0; user < XUSER_MAX_COUNT; ++user) {
    XINPUT_STATE state;
    memset(&state, 0, sizeof(state));
    if (XInputGetState(user, &state) != ERROR_SUCCESS) {
      previous_buttons[user] = 0;
      continue;
    }

    WORD buttons = state.Gamepad.wButtons;
    WORD pressed = buttons & ~previous_buttons[user];
    previous_buttons[user] = buttons;
    if (!(buttons & XINPUT_GAMEPAD_BACK)) continue;

    bool up = (pressed & XINPUT_GAMEPAD_DPAD_UP) != 0;
    bool down = (pressed & XINPUT_GAMEPAD_DPAD_DOWN) != 0;
    if (up != down) AudioAdjustVolume(up ? 5 : -5);
  }
}

static DWORD WINAPI NotificationWorker(void*) {
  for (;;) {
    PollVolumeChord();
    AudioNotificationTick();
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

extern "C" BOOL APIENTRY DllMain(HANDLE, DWORD reason, LPVOID) {
  if (reason != DLL_PROCESS_ATTACH) return TRUE;

  HANDLE kernel = GetModuleHandleA("xboxkrnl.exe");
  XboxKernelVersion* version = 0;
  if (!kernel || !Resolve(kernel, 344, &version) || !version ||
      version->build != kSupportedKernel) {
    return TRUE;
  }

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
