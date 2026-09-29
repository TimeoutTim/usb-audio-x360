// SPDX-License-Identifier: GPL-3.0-or-later
#include <xtl.h>
#include <string.h>

#include "audio.h"
#include "detour.h"

extern "C" LONG XexGetProcedureAddress(HANDLE module, DWORD ordinal,
                                        PVOID address);
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
static AudioHostApi g_audio_api;

static WORD ReadWord(const BYTE* value) {
  return (WORD)value[0] | ((WORD)value[1] << 8);
}

static DWORD ReadRate(const BYTE* value) {
  return (DWORD)value[0] | ((DWORD)value[1] << 8) |
         ((DWORD)value[2] << 16);
}

template <typename T>
static bool Resolve(HANDLE module, DWORD ordinal, T* result) {
  *result = 0;
  return XexGetProcedureAddress(module, ordinal, result) >= 0 && *result != 0;
}

static bool InNodePool(const BYTE* pool, const void* address) {
  const BYTE* value = (const BYTE*)address;
  return pool && value >= pool && value < pool + kNodePoolBytes &&
         (((DWORD)(value - pool) & 0x0F) == 0);
}

static bool Supports48Khz(const BYTE* descriptor, BYTE length) {
  if (length < 8 || descriptor[3] != 1 || descriptor[4] != 2 ||
      descriptor[5] != 2 || descriptor[6] != 16) {
    return false;
  }
  BYTE count = descriptor[7];
  if (count == 0) {
    return length >= 14 && ReadRate(descriptor + 8) <= 48000 &&
           ReadRate(descriptor + 11) >= 48000;
  }
  for (BYTE index = 0; index < count; ++index) {
    DWORD offset = 8 + index * 3;
    if (offset + 3 <= length && ReadRate(descriptor + offset) == 48000)
      return true;
  }
  return false;
}

static bool HasSupportedProfile(const BYTE* configuration) {
  if (!configuration || configuration[0] < 9 ||
      configuration[1] != 2 || configuration[5] != 1) {
    return false;
  }
  DWORD total = ReadWord(configuration + 2);
  if (total < 9 || total > 0x400) return false;

  for (DWORD start = configuration[0]; start + 9 <= total;) {
    BYTE length = configuration[start];
    if (length < 2 || start + length > total) return false;
    if (configuration[start + 1] != 4 || length < 9 ||
        configuration[start + 2] != 1 ||
        configuration[start + 3] != 1 ||
        configuration[start + 5] != 1 ||
        configuration[start + 6] != 2 ||
        configuration[start + 7] != 0) {
      start += length;
      continue;
    }

    bool pcm = false;
    bool format = false;
    bool sample_rate_control = false;
    bool output_endpoint = false;
    bool feedback_endpoint = false;
    DWORD offset = start + length;
    while (offset + 2 <= total) {
      BYTE item_length = configuration[offset];
      BYTE type = configuration[offset + 1];
      if (item_length < 2 || offset + item_length > total) return false;
      if (type == 4) break;
      const BYTE* item = configuration + offset;
      if (type == 0x24 && item_length >= 3) {
        if (item[2] == 1 && item_length >= 7)
          pcm = ReadWord(item + 5) == 1;
        else if (item[2] == 2)
          format = Supports48Khz(item, item_length);
      } else if (type == 5 && item_length >= 7) {
        BYTE attributes = item[3];
        bool isochronous = (attributes & 3) == 1;
        bool adaptive = ((attributes >> 2) & 3) == 2;
        if (item[2] == 1 && isochronous && adaptive &&
            ReadWord(item + 4) == 200 && item[6] == 1) {
          output_endpoint = true;
          if (item_length >= 9 && item[8] != 0) feedback_endpoint = true;
        } else if (isochronous && (item[2] & 0x80)) {
          feedback_endpoint = true;
        }
      } else if (type == 0x25 && item_length >= 4 && item[2] == 1) {
        sample_rate_control = (item[3] & 1) != 0;
      }
      offset += item_length;
    }
    return pcm && format && sample_rate_control && output_endpoint &&
           !feedback_endpoint;
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

static bool ReenumerateBootDevice() {
  if (g_playback_handle || !ValidateResetRoutine()) return false;

  DeviceHandle* selected = 0;
  DeviceHandle* selected_root = 0;
  BYTE* selected_hcd = 0;
  DWORD selected_port = 0;
  DWORD matches = 0;

  for (DWORD controller = 0; controller < 4; ++controller) {
    BYTE* hcd = ((BYTE**)kUsbHcdTable)[controller];
    BYTE* context = ((BYTE**)kUsbDeviceStateTable)[controller];
    DeviceHandle* root = g_get_root_hub_device_node(controller);
    if (!hcd || !context || !root || *(BYTE**)context != hcd ||
        *(DeviceHandle**)(hcd + 0x48) != root ||
        *(DWORD*)(context + 4) != 0 || *(DWORD*)(context + 8) != 0 ||
        context[0x27A] != controller || context[0x27E] != 0 ||
        !HasSupportedProfile(context + 0x60)) {
      continue;
    }

    UsbDeviceDescriptor* cached = (UsbDeviceDescriptor*)(context + 0x4C);
    if (cached->length < 18 || cached->descriptor_type != 1) continue;
    for (DWORD port = 0; port < 8; ++port) {
      DeviceHandle* device = g_get_port_device_node(root, port);
      UsbDeviceDescriptor* descriptor = device
          ? g_get_device_descriptor(device) : 0;
      if (!descriptor || descriptor->length < 18 ||
          descriptor->descriptor_type != 1 ||
          descriptor->vendor_id != cached->vendor_id ||
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
  if (!interface_descriptor || interface_descriptor->interface_number != 1 ||
      interface_descriptor->alternate_setting != 0 ||
      interface_descriptor->interface_class != 1 ||
      interface_descriptor->interface_subclass != 2 ||
      interface_descriptor->interface_protocol != 0) {
    return false;
  }
  profile->configuration = 1;
  profile->interface_number = 1;
  profile->alternate_setting = 1;
  profile->endpoint_address = 1;
  profile->endpoint_packet_size = 200;
  return true;
}

static int AddDeviceCompleteHook(DeviceHandle* handle, int status) {
  AudioProfile profile;
  if (status != 0 && !g_playback_handle &&
      ProfileForHandle(handle, &profile)) {
    memset(&g_extension, 0, sizeof(g_extension));
    g_extension.device_handle = handle;
    g_extension.interface_number = profile.interface_number;
    g_extension.interrupt_trb.flags = 1;
    handle->driver = &g_extension;
    int result = g_add_detour.Original<AddDeviceCompleteFn>()(handle, 0);
    g_audio_api.profile = profile;
    g_playback_handle = handle;
    return result;
  }
  return g_add_detour.Original<AddDeviceCompleteFn>()(handle, status);
}

static LONG RemoveDeviceCompleteHook(DeviceHandle* handle) {
  if (handle && handle == g_playback_handle) {
    g_playback_handle = 0;
    g_extension.device_handle = 0;
    AudioDeviceRemoved();
    return 0;
  }
  return g_remove_detour.Original<RemoveDeviceCompleteFn>()(handle);
}

static DWORD WINAPI AudioWorker(void*) {
  Sleep(2000);
  g_audio_api.playback_handle = (void*)g_playback_handle;
  if (!AudioInitialize(&g_audio_api)) return 0;

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
