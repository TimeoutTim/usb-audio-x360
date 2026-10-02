// SPDX-License-Identifier: GPL-3.0-or-later
#include "guide_ui.h"
#include <xui.h>
#include <stdlib.h>
#include <stdio.h>
#include <wchar.h>
#include <math.h>
#include "audio.h"
#include "detour.h"
#include "guide_icons.h"
#include "guide_layout.h"

extern "C" LONG XexGetProcedureAddress(HANDLE, DWORD, PVOID);

namespace {
typedef HRESULT (APIENTRY* SceneCreateFn)(LPCWSTR, LPCWSTR, void*, HXUIOBJ*);
typedef DWORD (NTAPI* BuildResourceLocatorFn)(HANDLE, LPCWSTR, LPCWSTR,
                                               LPWSTR, DWORD);
static PowerPcDetour g_create_detour;
static volatile LONG g_install_state = 0;
static XUIClass g_volume_class;
static HXUICLASS g_volume_class_handle = 0;
static XUIClass g_settings_class;
static HXUICLASS g_settings_class_handle = 0;
static bool g_register_attempted = false;
static HXUIOBJ g_guide_main_scene = 0;
static WCHAR g_volume_icon[128];
static WCHAR g_microphone_icon[128];
static WCHAR g_microphone_muted_icon[128];
static WCHAR g_options_icon[128];

struct XuiApi {
  HRESULT (APIENTRY* create)(LPCWSTR, HXUIOBJ*);
  HRESULT (APIENTRY* destroy)(HXUIOBJ);
  HRESULT (APIENTRY* add_child)(HXUIOBJ, HXUIOBJ);
  HRESULT (APIENTRY* parent)(HXUIOBJ, HXUIOBJ*);
  HRESULT (APIENTRY* first)(HXUIOBJ, HXUIOBJ*);
  HRESULT (APIENTRY* next)(HXUIOBJ, HXUIOBJ*);
  HRESULT (APIENTRY* id)(HXUIOBJ, LPCWSTR*);
  HRESULT (APIENTRY* position)(HXUIOBJ, D3DXVECTOR3*);
  HRESULT (APIENTRY* set_position)(HXUIOBJ, const D3DXVECTOR3*);
  HRESULT (APIENTRY* get_property_id)(HXUIOBJ, LPCWSTR, DWORD*);
  HRESULT (APIENTRY* get_property)(HXUIOBJ, DWORD, DWORD, XUIElementPropVal*);
  HRESULT (APIENTRY* set_property)(HXUIOBJ, DWORD, DWORD, const XUIElementPropVal*);
  void (APIENTRY* construct_value)(XUIElementPropVal*);
  void (APIENTRY* destroy_value)(XUIElementPropVal*);
  HRESULT (APIENTRY* string_value)(XUIElementPropVal*, LPCWSTR);
  void (APIENTRY* float_value)(XUIElementPropVal*, float);
  void (APIENTRY* int_value)(XUIElementPropVal*, int);
  void (APIENTRY* uint_value)(XUIElementPropVal*, unsigned int);
  void (APIENTRY* color_value)(XUIElementPropVal*, unsigned int);
  void (APIENTRY* bool_value)(XUIElementPropVal*, BOOL);
  HRESULT (APIENTRY* register_class)(const XUIClass*, HXUICLASS*);
  HXUIOBJ (APIENTRY* cast)(HXUIOBJ, HXUICLASS);
  HRESULT (APIENTRY* instance)(HXUIOBJ, void**);
  HXUIOBJ (APIENTRY* navigation)(HXUIOBJ, XUI_CONTROL_NAVIGATE, BOOL, BOOL);
  HRESULT (APIENTRY* set_text)(HXUIOBJ, LPCWSTR);
  HRESULT (APIENTRY* set_slider)(HXUIOBJ, int);
  HRESULT (APIENTRY* get_slider)(HXUIOBJ, int*);
  BYTE (APIENTRY* focus_user)(HXUIOBJ);
  HRESULT (APIENTRY* send)(HXUIOBJ, XUIMessage*);
  HRESULT (APIENTRY* broadcast)(HXUIOBJ, XUIMessage*);
  HRESULT (APIENTRY* visual)(HXUIOBJ, HXUIOBJ*);
  HRESULT (APIENTRY* set_user_focus)(HXUIOBJ, BYTE);
  HRESULT (APIENTRY* navigate_forward)(HXUIOBJ, BOOL, HXUIOBJ, BYTE);
  HRESULT (APIENTRY* navigate_back)(HXUIOBJ, HXUIOBJ, BYTE);
  BOOL (APIENTRY* valid)(HXUIOBJ);
  BOOL (APIENTRY* in_transition)(HXUIOBJ);
  HRESULT (APIENTRY* set_timer)(HXUIOBJ, DWORD, DWORD);
  HRESULT (APIENTRY* kill_timer)(HXUIOBJ, DWORD);
  BOOL (APIENTRY* tree_has_focus)(HXUIOBJ);
  HXUIOBJ (APIENTRY* tree_focus)(HXUIOBJ);
  HRESULT (APIENTRY* figure_shape)(HXUIOBJ, const XUIFigurePoint*, int);
  HRESULT (APIENTRY* figure_fill)(HXUIOBJ, XUI_FILL_TYPE, DWORD,
      XUIGradientStop*, int, float, const D3DXVECTOR2*, const D3DXVECTOR2*);
  HRESULT (APIENTRY* set_scale)(HXUIOBJ, const D3DXVECTOR3*);
} g_xui = {};

template <typename T>
bool Resolve(HANDLE module, DWORD ordinal, T* result) {
  PVOID address = 0;
  if (XexGetProcedureAddress(module, ordinal, &address) < 0 || !address) return false;
  *result = (T)address;
  return true;
}

// Property storage is owned by XAM, never a plugin-local XUI runtime.
class PropertyValue {
 public:
  PropertyValue() { g_xui.construct_value(get()); }
  ~PropertyValue() { g_xui.destroy_value(get()); }
  XUIElementPropVal* get() { return (XUIElementPropVal*)storage_; }
 private:
  PropertyValue(const PropertyValue&);
  PropertyValue& operator=(const PropertyValue&);
  __declspec(align(16)) BYTE storage_[sizeof(XUIElementPropVal)];
};

bool GetProperty(HXUIOBJ object, LPCWSTR name, PropertyValue* value) {
  DWORD id = 0;
  return g_xui.get_property_id(object, name, &id) >= 0 &&
      g_xui.get_property(object, id, 0, value->get()) >= 0;
}

bool SetProperty(HXUIOBJ object, LPCWSTR name, PropertyValue* value) {
  DWORD id = 0;
  return g_xui.get_property_id(object, name, &id) >= 0 &&
      g_xui.set_property(object, id, 0, value->get()) >= 0;
}

bool SetString(HXUIOBJ object, LPCWSTR name, LPCWSTR text) {
  PropertyValue value;
  return g_xui.string_value(value.get(), text) >= 0 && SetProperty(object, name, &value);
}

bool SetFloat(HXUIOBJ object, LPCWSTR name, float number) {
  PropertyValue value;
  g_xui.float_value(value.get(), number);
  return SetProperty(object, name, &value);
}

bool SetInt(HXUIOBJ object, LPCWSTR name, int number) {
  PropertyValue value;
  g_xui.int_value(value.get(), number);
  return SetProperty(object, name, &value);
}

bool SetUint(HXUIOBJ object, LPCWSTR name, unsigned int number, bool color) {
  PropertyValue value;
  if (color) g_xui.color_value(value.get(), number);
  else g_xui.uint_value(value.get(), number);
  return SetProperty(object, name, &value);
}

bool SetBool(HXUIOBJ object, LPCWSTR name, BOOL enabled) {
  PropertyValue value;
  g_xui.bool_value(value.get(), enabled);
  return SetProperty(object, name, &value);
}

bool MemoryLocator(WCHAR* output, DWORD capacity, const BYTE* data,
                   DWORD size) {
  return output && data && size &&
      _snwprintf_s(output, capacity, _TRUNCATE, L"memory://%08X,%X",
                   (DWORD)data, size) >= 0;
}

bool SetCheck(HXUIOBJ object, BOOL checked) {
  XUIMessage message;
  XUIMessageSetCheckState state;
  XuiMessageSetCheckstate(&message, &state, checked);
  return g_xui.send(object, &message) >= 0;
}

bool ReadRect(HXUIOBJ object, guide_layout::Rect* rect) {
  D3DXVECTOR3 position;
  PropertyValue width, height;
  if (g_xui.position(object, &position) < 0 ||
      !GetProperty(object, L"Width", &width) || !GetProperty(object, L"Height", &height) ||
      width.get()->type != XUI_EPT_FLOAT || height.get()->type != XUI_EPT_FLOAT) return false;
  rect->x = position.x; rect->y = position.y;
  rect->width = width.get()->fVal; rect->height = height.get()->fVal;
  return true;
}

HXUIOBJ Find(HXUIOBJ root, LPCWSTR wanted, DWORD depth, DWORD* remaining) {
  if (!root || !*remaining || depth > 16) return 0;
  --*remaining;
  LPCWSTR id = 0;
  if (g_xui.id(root, &id) >= 0 && id && wcscmp(id, wanted) == 0) return root;
  HXUIOBJ child = 0;
  if (g_xui.first(root, &child) < 0) return 0;
  while (child && *remaining) {
    HXUIOBJ found = Find(child, wanted, depth + 1, remaining);
    if (found) return found;
    HXUIOBJ next = 0;
    if (g_xui.next(child, &next) < 0) return 0;
    child = next;
  }
  return 0;
}

HXUIOBJ Find(HXUIOBJ root, LPCWSTR id) {
  DWORD remaining = 256;
  return Find(root, id, 0, &remaining);
}

struct VolumeScene {
  HXUIOBJ object;
  HXUIOBJ volume_button;
  HXUIOBJ confirm_button;
  HXUIOBJ microphone_button;
  HXUIOBJ options_button;
  HXUIOBJ slider;
  HXUIOBJ dismiss_button;
  HXUIOBJ title;
  HXUIOBJ host_scene;
  HXUIOBJ above_button, next_button;
  WCHAR old_down[96], old_up[96];
  bool layout_ready, visible;
  bool expanded;
  bool synchronizing;
};

struct SettingsScene {
  HXUIOBJ object;
  HXUIOBJ back_button;
  HXUIOBJ gain_slider;
  HXUIOBJ gain_value;
  HXUIOBJ device_info;
  HXUIOBJ test_button, test_status;
  HXUIOBJ volume_slider, volume_label, output_mute, mic_mute;
  HXUIOBJ meter, meter_label;
  HXUIOBJ properties_button;
  bool properties;
  WCHAR device_text[1024];
  LONG gain, volume;
  DWORD clipped_at;
  bool clip_latched;
  float meter_db;
  BYTE user;
  bool focus_pending, testing, returning;
  bool synchronizing;
};
const DWORD kSettingsRefreshTimer = 0x55534253;
const DWORD kVolumeRefreshTimer = 0x55534256;

bool SetExpanded(VolumeScene* state, bool expanded, BYTE user, bool restore_focus = true);
bool OpenSettings(VolumeScene* state, BYTE user);
bool RegisterSettingsClass();
bool OpenProperties(SettingsScene* state, BYTE user);
void RefreshDeviceInfo(SettingsScene* state);

bool DacConnected() {
  AudioDeviceInfo info;
  return AudioGetDeviceInfo(&info) && info.connected;
}

void RefreshRowVisibility(VolumeScene* state) {
  if (!state || !state->layout_ready) return;
  AudioDeviceInfo info;
  if (!AudioGetDeviceInfo(&info)) return;
  const bool visible = info.connected != FALSE;
  if (visible == state->visible) return;
  // All handles belong to this scene, and this timer runs on its UI thread.
  if (!visible) {
    HXUIOBJ focus = g_xui.tree_focus(state->object);
    if (focus) g_xui.set_user_focus(state->above_button, g_xui.focus_user(focus));
  }
  bool ok = SetString(state->above_button, L"NavDown", visible
      ? L"UsbAudioVolume\\VolumeButton" : state->old_down) &&
      SetString(state->next_button, L"NavUp", visible
      ? L"UsbAudioVolume\\VolumeButton" : state->old_up);
  if (!SetBool(state->object, L"Show", visible)) ok = false;
  if (!SetBool(state->object, L"Enabled", visible)) ok = false;
  if (ok) state->visible = visible;
}

void Synchronize(VolumeScene* state) {
  if (!state || !state->slider || state->synchronizing) return;
  state->synchronizing = true;
  LONG volume = AudioGetVolume();
  g_xui.set_slider(state->slider, volume);
  SetString(state->microphone_button, L"ImagePath",
            AudioIsMicrophoneMuted()
                ? g_microphone_muted_icon : g_microphone_icon);
  SetBool(state->microphone_button, L"Enabled", AudioMicrophoneAvailable());
  state->synchronizing = false;
}

HRESULT APIENTRY CreateVolume(HXUIOBJ, void** instance) {
  if (!instance) return E_INVALIDARG;
  *instance = calloc(1, sizeof(VolumeScene));
  return *instance ? S_OK : E_OUTOFMEMORY;
}

HRESULT APIENTRY CreateSettings(HXUIOBJ, void** instance) {
  if (!instance) return E_INVALIDARG;
  *instance = calloc(1, sizeof(SettingsScene));
  return *instance ? S_OK : E_OUTOFMEMORY;
}

HRESULT APIENTRY DestroyState(void* instance) {
  free(instance);
  return S_OK;
}

BYTE PressUser(const XUINotify* notify) {
  if (notify->pvData && notify->cbData >= sizeof(XUINotifyPress))
    return ((const XUINotifyPress*)notify->pvData)->UserIndex;
  return XUSER_INDEX_FOCUS;
}

bool MeterRectangle(HXUIOBJ object, float width, float height, DWORD color) {
  if (width < 0.1f) width = 0.1f;
  XUIFigurePoint points[4];
  memset(points, 0, sizeof(points));
  points[0].point = D3DXVECTOR2(0, 0);
  points[1].point = D3DXVECTOR2(width, 0);
  points[2].point = D3DXVECTOR2(width, height);
  points[3].point = D3DXVECTOR2(0, height);
  for (unsigned i = 0; i < 4; ++i)
    points[i].ptCtl1 = points[i].ptCtl2 = points[i].point;
  D3DXVECTOR2 scale(1, 1), translation(0, 0);
  return SetBool(object, L"Closed", TRUE) &&
      g_xui.figure_shape(object, points, 4) >= 0 &&
      SetFloat(object, L"Width", width) && SetFloat(object, L"Height", height) &&
      g_xui.figure_fill(object, XUI_FILL_SOLID, color, 0, 0, 0, &scale, &translation) >= 0;
}

void RefreshSettings(SettingsScene* state) {
  if (!state || state->synchronizing) return;
  RefreshDeviceInfo(state);
  if (state->properties) return;
  state->synchronizing = true;
  const bool microphone = AudioMicrophoneAvailable();
  if (!microphone) {
    HXUIOBJ focus = g_xui.tree_focus(state->object);
    if (focus == state->gain_slider || focus == state->mic_mute || focus == state->test_button)
      g_xui.set_user_focus(state->properties_button, g_xui.focus_user(focus));
  }
  SetBool(state->gain_slider, L"Enabled", microphone);
  SetBool(state->test_button, L"Enabled", microphone && !AudioIsMicrophoneMuted());
  LONG peak = 0;
  BOOL clipped = FALSE;
  AudioGetMicrophonePeak(&peak, &clipped);
  LONG test = AudioGetMicrophoneTestStatus(0);
  if (state->testing && (!microphone || AudioIsMicrophoneMuted() ||
      !g_xui.tree_has_focus(state->object))) {
    AudioStopMicrophoneTest();
    state->testing = false;
  }
  if (state->testing) {
    AudioKeepMicrophoneTestAlive();
    if (test == 0) state->testing = false;
  }
  WCHAR status[96];
  if (!microphone) wcscpy_s(status, L"No supported recording input is available.");
  else if (AudioIsMicrophoneMuted()) wcscpy_s(status, L"Microphone muted. Level shown for adjustment.");
  else wcscpy_s(status, state->testing
      ? L"Use headphones. Game sound is temporarily replaced."
      : L"Speak normally to check the level.");
  g_xui.set_text(state->test_status, status);
  SetCheck(state->test_button, state->testing);
  DWORD now = GetTickCount();
  if (clipped) { state->clipped_at = now; state->clip_latched = true; }
  if (!microphone || now - state->clipped_at >= 2000) state->clip_latched = false;
  float db = peak > 0 ? 20.0f * log10f((float)peak / 32768.0f) : -60.0f;
  if (db < -60) db = -60;
  if (db > 0) db = 0;
  state->meter_db -= 3; // 30 dB/s release; instantaneous rise from interval peak.
  if (db > state->meter_db) state->meter_db = db;
  DWORD color = state->clip_latched ? 0xffc52b22 :
      (state->meter_db >= -12 ? 0xffc58a16 : 0xff70a900);
  MeterRectangle(state->meter, (state->meter_db + 60) * 4.0f, 10, color);
  wcscpy_s(status, state->clip_latched ? L"Recording level: Too high" : L"Recording level");
  if (state->clip_latched && microphone && !AudioIsMicrophoneMuted())
    g_xui.set_text(state->test_status, L"Too high. Lower the microphone level.");
  g_xui.set_text(state->meter_label, status);
  SetUint(state->meter_label, L"TextColor", state->clip_latched ? 0xffc52b22 : 0xff333333, true);
  LONG volume = AudioGetVolume();
  if (volume != state->volume) {
    state->volume = volume;
    g_xui.set_slider(state->volume_slider, volume);
  }
  SetCheck(state->output_mute, AudioIsOutputMuted());
  SetCheck(state->mic_mute, AudioIsMicrophoneMuted());
  SetBool(state->mic_mute, L"Enabled", microphone);
  const HXUIOBJ recording[] = {state->gain_slider, state->gain_value, state->mic_mute,
      state->meter, state->meter_label, state->test_button, Find(state->object, L"MicPeakBackground")};
  for (unsigned i = 0; i < _countof(recording); ++i) SetBool(recording[i], L"Show", microphone);
  SetString(state->output_mute, L"NavDown", microphone ? L"MicrophoneGainSlider" : L"DevicePropertiesButton");
  SetString(state->properties_button, L"NavUp", microphone ? L"MicrophoneTestButton" : L"SettingsOutputMute");
  LONG gain = AudioGetMicrophoneGain();
  if (gain != state->gain) {
    state->gain = gain;
    g_xui.set_slider(state->gain_slider, gain);
    WCHAR value[48];
    _snwprintf_s(value, _countof(value), _TRUNCATE, L"Microphone level: %ld%%", gain);
    g_xui.set_text(state->gain_value, value);
  }
  state->synchronizing = false;
}

void RefreshDeviceInfo(SettingsScene* state) {
  AudioDeviceInfo info;
  WCHAR text[1024];
  if (AudioGetDeviceInfo(&info)) {
    if (!info.connected) wcscpy_s(text, L"No USB audio device connected");
    else if (!state->properties) {
      _snwprintf_s(text, _countof(text), _TRUNCATE, L"%.32s%s%.32s",
          info.product_name[0] ? info.product_name : L"USB Audio Device",
          info.manufacturer_name[0] ? L" - " : L"", info.manufacturer_name);
    } else {
      const LPCWSTR sync[] = {L"none", L"asynchronous", L"adaptive", L"synchronous"};
      WCHAR capture[64];
      if (info.capture_channels)
        _snwprintf_s(capture, _countof(capture), _TRUNCATE,
            L"48 kHz / %u-bit / %u ch; voice 16 kHz", info.capture_valid_bits, info.capture_channels);
      else wcscpy_s(capture, L"No compatible capture format selected");
      _snwprintf_s(text, _countof(text), _TRUNCATE,
          L"Device: %.32s\nManufacturer: %.32s\nHardware ID: %04X:%04X\nUSB %x.%02x   Device revision: %x.%02x\n"
          L"USB Audio Class %u   PCM formats: Playback %u / Recording %u\nAdvertised bit depth: Playback %u / Recording %u\n"
          L"Playback: 48 kHz, stereo, %u-bit\nInterface %u / Alternate %u / Endpoint %02X / Packet %u bytes\n"
          L"Synchronization: %s / Feedback %02X / Clock %u\n\n"
          L"Recording: %s\n%s\nInterface %u / Alternate %u / Endpoint %02X\n"
          L"Driver status: %s   Last error: %08X\nDriver build: %S %S   Console software: 17559",
          info.product_name[0] ? info.product_name :
              (info.name_status ? L"Product name unavailable" : L"Reading device name..."),
          info.manufacturer_name[0] ? info.manufacturer_name : L"Manufacturer not provided",
          info.vendor_id, info.product_id, info.usb_version >> 8, info.usb_version & 255,
          info.device_version >> 8, info.device_version & 255, info.audio_class_version,
          info.advertised_outputs, info.advertised_inputs, info.max_output_bits, info.max_input_bits,
          info.playback_valid_bits, info.output_interface, info.output_alternate,
          info.output_endpoint, info.output_packet_bytes,
          sync[info.sync_type < 4 ? info.sync_type : 0], info.feedback_endpoint, info.clock_id,
          info.microphone_available ? L"active" : (info.capture_channels ? L"inactive" : L"not supported"),
          capture, info.capture_interface,
          info.capture_alternate, info.capture_endpoint, info.streaming ? L"running" : L"stopped",
          info.last_error, __DATE__, __TIME__);
    }
    if (wcscmp(text, state->device_text) != 0) {
      wcscpy_s(state->device_text, text);
      g_xui.set_text(state->device_info, text);
    }
  }
}

bool SettingsBack(HXUIOBJ object, BYTE user) {
  if (g_xui.in_transition(object)) return false;
  XUIMessage message;
  XUIMessageGetBackScene data = {};
  XuiMessageGetBackScene(&message, &data);
  if (g_xui.send(object, &message) < 0 || !data.hBackScene ||
      !g_xui.valid(data.hBackScene)) return false;
  HRESULT result = g_xui.navigate_back(object, data.hBackScene, user);
  return SUCCEEDED(result);
}

HRESULT APIENTRY SettingsObjectProc(HXUIOBJ object, XUIMessage* message,
                                    void* instance) {
  SettingsScene* state = (SettingsScene*)instance;
  if (!state || !message) return S_OK;
  if (message->dwMessage == XM_INIT) {
    state->object = object;
    state->back_button = Find(object, L"SettingsBackButton");
    state->gain_slider = Find(object, L"MicrophoneGainSlider");
    state->gain_value = Find(object, L"MicrophoneGainValue");
    state->device_info = Find(object, L"DeviceInformationValue");
    state->properties_button = Find(object, L"DevicePropertiesButton");
    state->properties = Find(object, L"PropertiesTitle") != 0;
    if (state->properties) {
      if (!state->back_button || !state->device_info) return E_FAIL;
      RefreshDeviceInfo(state);
      return g_xui.set_timer(object, kSettingsRefreshTimer, 100);
    }
    state->test_button = Find(object, L"MicrophoneTestButton");
    state->test_status = Find(object, L"MicrophoneTestStatus");
    state->volume_slider = Find(object, L"SettingsVolumeSlider");
    state->volume_label = Find(object, L"SettingsVolumeLabel");
    state->output_mute = Find(object, L"SettingsOutputMute");
    state->mic_mute = Find(object, L"SettingsMicMute");
    state->meter = Find(object, L"MicPeakFill");
    state->meter_label = Find(object, L"MicPeakLabel");
    if (!state->back_button || !state->gain_slider || !state->gain_value ||
        !state->device_info || !state->test_button || !state->test_status ||
        !state->volume_slider || !state->volume_label || !state->output_mute ||
        !state->mic_mute || !state->meter || !state->meter_label || !state->properties_button) return E_FAIL;
    state->gain = state->volume = -1;
    state->meter_db = -60;
    RefreshSettings(state);
    return g_xui.set_timer(object, kSettingsRefreshTimer, 100);
  } else if (message->dwMessage == XM_TIMER && message->pvData &&
      message->cbData >= sizeof(XUIMessageTimer) &&
      ((XUIMessageTimer*)message->pvData)->nId == kSettingsRefreshTimer) {
    if (state->focus_pending && !g_xui.in_transition(object)) {
      g_xui.set_user_focus(state->properties ? state->back_button :
          (state->returning ? state->properties_button : state->volume_slider), state->user);
      state->focus_pending = false;
    }
    AudioDeviceInfo info;
    if (AudioGetDeviceInfo(&info) && !info.connected && g_xui.tree_has_focus(object) && !g_xui.in_transition(object)) {
      AudioStopMicrophoneTest();
      SettingsBack(object, state->user);
      return S_OK;
    }
    RefreshSettings(state);
  } else if (message->dwMessage == XM_TRANSITION_END && message->pvData &&
      message->cbData >= sizeof(XUIMessageTransition)) {
    DWORD transition = ((XUIMessageTransition*)message->pvData)->dwTransType;

    if (transition == XUI_TRANSITION_TO || transition == XUI_TRANSITION_BACKTO) {
      state->returning = transition == XUI_TRANSITION_BACKTO;
      state->focus_pending = true;
    }
  } else if (message->dwMessage == XM_TRANSITION_START && message->pvData &&
      message->cbData >= sizeof(XUIMessageTransition)) {
    DWORD transition = ((XUIMessageTransition*)message->pvData)->dwTransType;
    if (transition == XUI_TRANSITION_FROM || transition == XUI_TRANSITION_BACKFROM) {
      state->focus_pending = false;
      state->testing = false;
      AudioStopMicrophoneTest();
    }
  } else if (message->dwMessage == XM_DESTROY) {
    g_xui.kill_timer(object, kSettingsRefreshTimer);
    AudioStopMicrophoneTest();
  } else if (message->dwMessage == XM_NOTIFY && message->pvData &&
           message->cbData >= sizeof(XUINotify)) {
    XUINotify* notify = (XUINotify*)message->pvData;
    if (notify->dwNotify == XN_VALUE_CHANGED &&
        notify->hObjSource == state->gain_slider && !state->synchronizing) {
      int gain = 0;
      if (g_xui.get_slider(state->gain_slider, &gain) >= 0) {
        AudioSetMicrophoneGain(gain);
        RefreshSettings(state);

      }
      message->bHandled = TRUE;
    } else if (notify->dwNotify == XN_VALUE_CHANGED &&
        notify->hObjSource == state->volume_slider && !state->synchronizing) {
      int volume = 0;
      if (g_xui.get_slider(state->volume_slider, &volume) >= 0) AudioSetVolume(volume);
      RefreshSettings(state);
      message->bHandled = TRUE;
    } else if (notify->dwNotify == XN_PRESS &&
        (notify->hObjSource == state->output_mute || notify->hObjSource == state->mic_mute)) {
      if (notify->hObjSource == state->output_mute) AudioSetOutputMuted(!AudioIsOutputMuted());
      else AudioSetMicrophoneMuted(!AudioIsMicrophoneMuted());
      RefreshSettings(state);
      message->bHandled = TRUE;
    } else if (notify->dwNotify == XN_PRESS && notify->hObjSource == state->properties_button) {
      OpenProperties(state, PressUser(notify));
      message->bHandled = TRUE;
    } else if (notify->dwNotify == XN_PRESS &&
        notify->hObjSource == state->test_button) {
      if (state->testing) { AudioStopMicrophoneTest(); state->testing = false; }
      else state->testing = AudioStartMicrophoneTest() != FALSE;
      RefreshSettings(state);
      message->bHandled = TRUE;
    } else if (notify->dwNotify == XN_PRESS &&
        notify->hObjSource == state->back_button) {
      SettingsBack(object, PressUser(notify));
      message->bHandled = TRUE;
    }
  }
  return S_OK;
}

HRESULT APIENTRY VolumeObjectProc(HXUIOBJ object, XUIMessage* message, void* instance) {
  VolumeScene* state = (VolumeScene*)instance;
  if (!state || !message) return S_OK;
  if (message->dwMessage == XM_INIT) {
    state->object = object;
    state->volume_button = Find(object, L"VolumeButton");
    state->confirm_button = Find(object, L"VolumeConfirmButton");
    state->microphone_button = Find(object, L"MicrophoneMuteButton");
    state->options_button = Find(object, L"OptionsButton");
    state->slider = Find(object, L"VolumeSlider");
    state->dismiss_button = Find(object, L"VolumeDismissButton");
    state->title = Find(object, L"UsbAudioLabel");
    Synchronize(state);
  } else if (message->dwMessage == XM_TIMER && message->pvData &&
      message->cbData >= sizeof(XUIMessageTimer) &&
      ((XUIMessageTimer*)message->pvData)->nId == kVolumeRefreshTimer) {
    RefreshRowVisibility(state);
    // External focus changes (including closing/switching the Guide) must
    // not leave a stale expanded row or steal focus back from the new view.
    if (state->expanded && !g_xui.tree_has_focus(object))
      SetExpanded(state, false, XUSER_INDEX_FOCUS, false);
    if (state->visible) Synchronize(state);
  } else if (message->dwMessage == XM_DESTROY) {
    g_xui.kill_timer(object, kVolumeRefreshTimer);
  } else if (message->dwMessage == XM_NOTIFY && message->pvData &&
             message->cbData >= sizeof(XUINotify)) {
    XUINotify* notify = (XUINotify*)message->pvData;
    HXUIOBJ source = notify->hObjSource;
    if (notify->dwNotify == XN_SET_FOCUS) {
      Synchronize(state);
    } else if (notify->dwNotify == XN_KILL_FOCUS && source == state->slider && state->expanded) {
      // Up/Down accepts the live value and keeps native focus navigation.
      // Clear first because hiding the slider can generate another notification.
      state->expanded = false;
      SetExpanded(state, false, XUSER_INDEX_FOCUS, false);
    } else if (notify->dwNotify == XN_VALUE_CHANGED && source == state->slider) {
      if (!state->synchronizing) {
        int value = 0;
        if (g_xui.get_slider(state->slider, &value) >= 0) {
          AudioSetVolume(value);
          Synchronize(state);
        }
      }
      message->bHandled = TRUE;
    } else if (notify->dwNotify == XN_PRESS) {
      BYTE user = PressUser(notify);
      if (source == state->volume_button)
        SetExpanded(state, !state->expanded, user);
      else if (source == state->dismiss_button || source == state->confirm_button)
        SetExpanded(state, false, user);
      else if (source == state->microphone_button && AudioMicrophoneAvailable()) {
        AudioSetMicrophoneMuted(!AudioIsMicrophoneMuted());
        Synchronize(state);
      } else if (source == state->options_button) {
        OpenSettings(state, user);
      }
      else return S_OK;
      message->bHandled = TRUE;
    }
  }
  return S_OK;
}

bool RegisterVolumeClass() {
  if (g_register_attempted) return g_volume_class_handle != 0;
  g_register_attempted = true;
  memset(&g_volume_class, 0, sizeof(g_volume_class));
  g_volume_class.szClassName = L"UsbAudioVolumeScene";
  g_volume_class.szBaseClassName = XUI_CLASS_SCENE;
  g_volume_class.Methods.CreateInstance = CreateVolume;
  g_volume_class.Methods.DestroyInstance = DestroyState;
  g_volume_class.Methods.ObjectProc = VolumeObjectProc;
  HRESULT result = g_xui.register_class(&g_volume_class, &g_volume_class_handle);
  return SUCCEEDED(result) && g_volume_class_handle;
}

bool RegisterSettingsClass() {
  if (g_settings_class_handle) return true;
  memset(&g_settings_class, 0, sizeof(g_settings_class));
  g_settings_class.szClassName = L"UsbAudioSettingsScene";
  // HUDScene is the Guide shell's expandable scene class.  Retail and Nova's
  // file-browser resources both use it for the wide blade rather than an
  // ordinary XuiScene.
  g_settings_class.szBaseClassName = L"HUDScene";
  g_settings_class.Methods.CreateInstance = CreateSettings;
  g_settings_class.Methods.DestroyInstance = DestroyState;
  g_settings_class.Methods.ObjectProc = SettingsObjectProc;
  HRESULT result = g_xui.register_class(&g_settings_class,
                                        &g_settings_class_handle);
  return SUCCEEDED(result) && g_settings_class_handle;
}

bool InitializeObject(HXUIOBJ object) {
  XUIMessage message;
  XUIMessageInit data;
  XuiMessageInit(&message, &data, 0);
  HRESULT result = g_xui.send(object, &message);
  return SUCCEEDED(result);
}

bool ConfigureElement(HXUIOBJ object, LPCWSTR id, float width, float height,
                      float x, float y) {
  D3DXVECTOR3 position(x, y, 0);
  return SetString(object, L"Id", id) && SetFloat(object, L"Width", width) &&
      SetFloat(object, L"Height", height) && g_xui.set_position(object, &position) >= 0;
}

bool ConfigureButton(HXUIOBJ button, LPCWSTR id, float x, LPCWSTR image) {
  // Match MiniMediaPlayer.xur: 52-unit cells on a 323-unit Guide row.
  return ConfigureElement(button, id, 52, 28, x, 0) &&
      SetString(button, L"Visual", L"xuiButtonImageCenteredMusic") &&
      SetString(button, L"ImagePath", image) && SetString(button, L"Text", L"");
}

bool SetExpanded(VolumeScene* state, bool expanded, BYTE user, bool restore_focus) {
  if (!state || !state->volume_button || !state->slider) return false;
  D3DXVECTOR3 position(expanded ? 109.0f : 161.0f, 0, 0);
  bool ok = g_xui.set_position(state->volume_button, &position) >= 0 &&
      SetBool(state->microphone_button, L"Show", !expanded) &&
      SetBool(state->options_button, L"Show", !expanded) &&
      SetBool(state->slider, L"Show", expanded) &&
      SetBool(state->dismiss_button, L"Enabled", expanded) &&
      SetBool(state->confirm_button, L"Enabled", expanded) &&
      SetString(state->volume_button, L"NavRight",
                expanded ? L"VolumeSlider" : L"MicrophoneMuteButton");
  if (!ok) return false;
  state->expanded = expanded;
  Synchronize(state);
  HXUIOBJ focus = expanded ? state->slider : state->volume_button;
  return !restore_focus || g_xui.set_user_focus(focus, user) >= 0;
}

HXUIOBJ CreateSettingsPanel(bool properties = false) {
  struct Element {
    LPCWSTR type, id;
    float x, y, width, height;
    LPCWSTR text, visual;
    float font;
  };
  const Element controls[] = {
    {XUI_CLASS_LABEL, L"SettingsTitle", 156, 36, 272, 25, L"USB Audio", L"Label_Head", 0},
    {XUI_CLASS_TEXT, L"SettingsVolumeLabel", 154, 145, 240, 22, L"Volume", 0, 12},
    {XUI_CLASS_SLIDER, L"SettingsVolumeSlider", 154, 174, 240, 22, 0, L"Slider_Volume", 0},
    {XUI_CLASS_CHECKBOX, L"SettingsOutputMute", 144, 210, 260, 28, L"Mute", L"XuiCheckbox", 12},
    {XUI_CLASS_TEXT, L"MicrophoneGainValue", 454, 145, 240, 22, L"Microphone level", 0, 12},
    {XUI_CLASS_SLIDER, L"MicrophoneGainSlider", 454, 174, 240, 22, 0, L"Slider_Volume", 0},
    {XUI_CLASS_CHECKBOX, L"SettingsMicMute", 444, 210, 260, 28, L"Mute microphone", L"XuiCheckbox", 12},
    {XUI_CLASS_TEXT, L"MicPeakLabel", 454, 248, 240, 22, L"Recording level", 0, 11},
    {XUI_CLASS_FIGURE, L"MicPeakBackground", 454, 275, 240, 10, 0, 0, 0},
    {XUI_CLASS_FIGURE, L"MicPeakFill", 454, 275, 240, 10, 0, 0, 0},
    {XUI_CLASS_TEXT, L"PlaybackHeading", 154, 109, 240, 24, L"Playback", 0, 14},
    {XUI_CLASS_CHECKBOX, L"MicrophoneTestButton", 444, 307, 260, 28, L"Listen to microphone", L"XuiCheckbox", 12},
    {XUI_CLASS_TEXT, L"MicrophoneTestStatus", 454, 345, 240, 52, L"Speak normally to check the level.", 0, 10},
    {XUI_CLASS_TEXT, L"RecordingHeading", 454, 109, 240, 24, L"Recording", 0, 14},
    {XUI_CLASS_TEXT, L"DeviceInformationValue", 154, 76, 540, 22, L"USB Audio Device", 0, 11},
    {XUI_CLASS_BUTTON, L"SettingsBackButton", 0, 0, 0, 0, 0, 0, 0},
    {XUI_CLASS_FIGURE, L"SettingsDivider", 424, 110, 1, 279, 0, 0, 0},
    {XUI_CLASS_BUTTON, L"DevicePropertiesButton", 144, 352, 260, 28, L"Device properties", L"XuiButtonGuide", 12}
  };
  const Element details[] = {
    {XUI_CLASS_LABEL, L"PropertiesTitle", 156, 36, 400, 25, L"Device Properties", L"Label_Head", 0},
    {XUI_CLASS_TEXT, L"DeviceInformationValue", 154, 84, 540, 300, L"Reading device information...", 0, 10},
    // Input-only B handler; the Guide shell supplies the visible Back legend.
    {XUI_CLASS_BUTTON, L"SettingsBackButton", 0, 0, 0, 0, 0, 0, 0}
  };
  const Element* elements = properties ? details : controls;
  const unsigned count = properties ? _countof(details) : _countof(controls);
  HXUIOBJ scene = 0, children[_countof(controls)] = {0};
  const unsigned focus[] = {2, 3, 5, 6, 11, 17};
  unsigned owned = 0;
  if (g_xui.create(L"UsbAudioSettingsScene", &scene) < 0 || !scene) goto fail;
  if (!ConfigureElement(scene, L"UsbAudioSettings", 852, 480, 0, 0) ||
      !SetString(scene, L"DefaultFocus", properties ? L"SettingsBackButton" : L"SettingsVolumeSlider") ||
      !SetString(scene, L"LegendB", L"Back") ||
      !SetString(scene, L"TransTo", L"FadeIn")) goto fail;
  for (unsigned i = 0; i < count; ++i) {
    const Element& e = elements[i];
    if (g_xui.create(e.type, &children[i]) < 0 || !children[i] ||
        !ConfigureElement(children[i], e.id, e.width, e.height, e.x, e.y)) goto fail;
    if (e.text && !SetString(children[i], L"Text", e.text)) goto fail;
    if (e.visual && !SetString(children[i], L"Visual", e.visual)) goto fail;
    if (e.font && !SetFloat(children[i], L"PointSize", e.font)) goto fail;
    if (wcscmp(e.type, XUI_CLASS_TEXT) == 0) {
      if (!SetUint(children[i], L"TextColor", 0xff333333, true) ||
          !SetUint(children[i], L"TextStyle", 0, false)) goto fail;
    }
  }
  if (!properties) {
    for (unsigned i = 0; i < _countof(focus); ++i) {
      unsigned previous = focus[i ? i - 1 : i];
      unsigned next = focus[i + 1 < _countof(focus) ? i + 1 : i];
      if (!SetString(children[focus[i]], L"NavUp", elements[previous].id) ||
          !SetString(children[focus[i]], L"NavDown", elements[next].id)) goto fail;
    }
    for (unsigned i = 2; i <= 5; i += 3) {
      if (!SetInt(children[i], L"RangeMin", 0) ||
          !SetInt(children[i], L"RangeMax", i == 2 ? 100 : 200) ||
          !SetInt(children[i], L"Value", 100) ||
          !SetInt(children[i], L"Step", i == 2 ? 5 : 10) ||
          !SetInt(children[i], L"AccelInc", 0) ||
          !SetString(children[i], L"NavLeft", elements[i].id) ||
          !SetString(children[i], L"NavRight", elements[i].id)) goto fail;
    }
    if (!MeterRectangle(children[8], 240, 10, 0xffb4babd) ||
        !MeterRectangle(children[9], 0, 10, 0xff70a900) ||
        !MeterRectangle(children[16], 1, 279, 0xffb4babd) ||
        !SetUint(children[15], L"PressKey", VK_PAD_B_OR_BACK, false) ||
        !SetBool(children[15], L"UnfocusedInput", TRUE)) goto fail;
  } else if (!SetUint(children[2], L"PressKey", VK_PAD_B_OR_BACK, false) ||
      !SetBool(children[2], L"UnfocusedInput", TRUE)) goto fail;
  for (unsigned i = 0; i < count; ++i) {
    if (g_xui.add_child(scene, children[i]) < 0) goto fail;
    ++owned;
  }
  for (unsigned i = 0; i < count; ++i)
    if (!InitializeObject(children[i])) goto fail;
  if (!InitializeObject(scene)) goto fail;
  {
    XUIMessage message;
    XuiMessage(&message, XM_SKIN_CHANGED);
    if (g_xui.broadcast(scene, &message) < 0) goto fail;
    if (!properties) {
      // Slider_Volume's authored visual is 270 units wide even when its
      // control Width is smaller. Scale the complete native control so its
      // track, thumb, and focus animation all fit the 240-unit column.
      D3DXVECTOR3 scale(240.0f / 270.0f, 1, 1);
      for (unsigned i = 2; i <= 5; i += 3)
        if (!SetFloat(children[i], L"Width", 270) ||
            g_xui.set_scale(children[i], &scale) < 0) goto fail;
    }
  }
  return scene;
fail:
  for (unsigned i = owned; i < count; ++i)
    if (children[i]) g_xui.destroy(children[i]);
  if (scene) g_xui.destroy(scene);
  return 0;
}

bool OpenProperties(SettingsScene* state, BYTE user) {
  if (!state || state->properties || !DacConnected() ||
      g_xui.in_transition(state->object)) return false;
  HXUIOBJ scene = CreateSettingsPanel(true);
  if (!scene) return false;
  void* raw = 0;
  HXUIOBJ implementation = g_xui.cast(scene, g_settings_class_handle);
  if (!implementation || g_xui.instance(implementation, &raw) < 0 || !raw ||
      !SetUint(scene, L"OpenType", 2, false)) {
    g_xui.destroy(scene);
    return false;
  }
  ((SettingsScene*)raw)->user = user;
  AudioStopMicrophoneTest();
  state->testing = false;
  if (FAILED(g_xui.navigate_forward(state->object, FALSE, scene, user))) {
    g_xui.destroy(scene);
    return false;
  }
  return true;
}

bool OpenSettings(VolumeScene* state, BYTE user) {
  if (!state || !state->host_scene || !DacConnected() || !RegisterSettingsClass()) return false;
  // Expanded pages navigate from the outer HUDScene rather than its Media
  // tab. SceneCreateHook records the actual object
  // returned for GuideMain.xur; validate the observed native ID before use.
  HXUIOBJ guide_scene = g_guide_main_scene;
  LPCWSTR guide_id = 0;
  if (!guide_scene || !g_xui.valid(guide_scene) ||
      g_xui.in_transition(guide_scene) ||
      g_xui.id(guide_scene, &guide_id) < 0 || !guide_id ||
      wcscmp(guide_id, L"HUDScene") != 0) return false;
  PropertyValue previous_open_type, open_type;
  if (!GetProperty(guide_scene, L"OpenType", &previous_open_type) ||
      previous_open_type.get()->type != XUI_EPT_UNSIGNED) return false;

  HXUIOBJ settings = CreateSettingsPanel();
  if (!settings) return false;
  void* raw = 0;
  HXUIOBJ implementation = g_xui.cast(settings, g_settings_class_handle);
  if (!implementation || g_xui.instance(implementation, &raw) < 0 || !raw) {
    g_xui.destroy(settings);
    return false;
  }
  ((SettingsScene*)raw)->user = user;

  // HUDScene defaults to Closed (0). Transition-in applies this property to
  // the shared Guide shell; Full (2) is required even if navigation succeeds.
  if (!SetUint(settings, L"OpenType", 2, false) ||
      !GetProperty(settings, L"OpenType", &open_type) ||
      open_type.get()->type != XUI_EPT_UNSIGNED || open_type.get()->uVal != 2) {
    SetProperty(guide_scene, L"OpenType", &previous_open_type);
    g_xui.destroy(settings);
    return false;
  }

  HRESULT result = g_xui.navigate_forward(guide_scene, FALSE, settings, user);

  if (FAILED(result)) {
    SetProperty(guide_scene, L"OpenType", &previous_open_type);
    g_xui.destroy(settings);
  }
  return SUCCEEDED(result);
}

HXUIOBJ CreateVolumePanel(HXUIOBJ parent, float x, float y) {
  // XAM's Guide loader requires XUR 8; the public XDK compiler emits XUR 5.
  // Construct native objects instead, without a second/private XUI runtime.
  HXUIOBJ panel = 0, volume = 0, confirm = 0, microphone = 0, options = 0;
  HXUIOBJ slider = 0, dismiss = 0, title = 0;
  HXUIOBJ children[7] = {0};
  const LPCWSTR classes[7] = {XUI_CLASS_BUTTON, XUI_CLASS_BUTTON,
      XUI_CLASS_BUTTON, XUI_CLASS_BUTTON, XUI_CLASS_SLIDER,
      XUI_CLASS_BUTTON, XUI_CLASS_TEXT};
  unsigned owned = 0;

  if (g_xui.create(L"UsbAudioVolumeScene", &panel) < 0 || !panel) goto fail;
  for (unsigned i = 0; i < 7; ++i)
    if (g_xui.create(classes[i], &children[i]) < 0 || !children[i]) goto fail;
  volume = children[0]; confirm = children[1]; microphone = children[2];
  options = children[3]; slider = children[4]; dismiss = children[5];
  title = children[6];

  if (!ConfigureElement(panel, L"UsbAudioVolume", 323, 28, x, y) ||
      !ConfigureElement(title, L"UsbAudioLabel", 95, 28, 10, 2) ||
      !ConfigureButton(volume, L"VolumeButton", 161, g_volume_icon) ||
      !ConfigureElement(confirm, L"VolumeConfirmButton", 0, 0, 0, 0) ||
      !ConfigureButton(microphone, L"MicrophoneMuteButton", 213,
                       g_microphone_icon) ||
      !ConfigureButton(options, L"OptionsButton", 265, g_options_icon) ||
      !ConfigureElement(slider, L"VolumeSlider", 270, 22, 165, 3) ||
      !ConfigureElement(dismiss, L"VolumeDismissButton", 0, 0, 0, 0)) goto fail;

  if (!SetString(title, L"Text", L"USB Audio") ||
      !SetFloat(title, L"PointSize", 12) ||
      !SetUint(title, L"TextColor", 0xff333333, true) ||
      !SetUint(title, L"TextStyle", 16, false) ||
      !SetString(slider, L"Visual", L"Slider_Volume") ||
      !SetString(slider, L"NavLeft", L"VolumeSlider") ||
      !SetString(slider, L"NavRight", L"VolumeSlider") ||
      !SetInt(slider, L"RangeMin", 0) || !SetInt(slider, L"RangeMax", 100) ||
      !SetInt(slider, L"Value", 100) || !SetInt(slider, L"Step", 5) ||
      !SetInt(slider, L"AccelInc", 0) ||
      !SetBool(slider, L"Show", FALSE) ||
      !SetUint(dismiss, L"PressKey", VK_PAD_B_OR_BACK, false) ||
      !SetBool(dismiss, L"UnfocusedInput", TRUE) ||
      !SetBool(dismiss, L"Enabled", FALSE) ||
      !SetUint(confirm, L"PressKey", VK_PAD_A, false) ||
      !SetBool(confirm, L"UnfocusedInput", TRUE) ||
      !SetBool(confirm, L"Enabled", FALSE) ||
      !SetString(volume, L"NavLeft", L"") ||
      !SetString(volume, L"NavRight", L"MicrophoneMuteButton") ||
      !SetString(microphone, L"NavLeft", L"VolumeButton") ||
      !SetString(microphone, L"NavRight", L"OptionsButton") ||
      !SetString(options, L"NavLeft", L"MicrophoneMuteButton") ||
      !SetString(options, L"NavRight", L"")) goto fail;

  for (unsigned i = 0; i < 7; ++i) {
    if (g_xui.add_child(panel, children[i]) < 0) goto fail;
    ++owned;
  }
  if (g_xui.add_child(parent, panel) < 0) goto fail;

  for (unsigned i = 0; i < 7; ++i)
    if (!InitializeObject(children[i])) goto fail;
  if (!InitializeObject(panel)) goto fail;
  {
    XUIMessage message;
    XuiMessage(&message, XM_SKIN_CHANGED);
    HRESULT result = g_xui.broadcast(panel, &message);

    HXUIOBJ visual = 0;
    if (FAILED(result) || g_xui.visual(volume, &visual) < 0 || !visual ||
        g_xui.visual(microphone, &visual) < 0 || !visual ||
        g_xui.visual(options, &visual) < 0 || !visual ||
        g_xui.visual(slider, &visual) < 0 || !visual) goto fail;

    // Width alone does not resize Slider_Volume's 270-unit skin geometry.
    D3DXVECTOR3 scale(152.0f / 270.0f, 1, 1);
    if (g_xui.set_scale(slider, &scale) < 0) goto fail;
  }
  return panel;
fail:
  for (unsigned i = owned; i < 7; ++i)
    if (children[i]) g_xui.destroy(children[i]);
  if (panel) g_xui.destroy(panel);
  return 0;
}

bool ParentPath(HXUIOBJ object, WCHAR* output, DWORD capacity) {
  LPCWSTR id = 0;
  if (g_xui.id(object, &id) < 0 || !id || !*id || wcslen(id) + 4 > capacity ||
      wcschr(id, L'\\') || wcschr(id, L'/')) return false;
  return _snwprintf_s(output, capacity, _TRUNCATE, L"..\\%s", id) >= 0;
}

bool InjectMedia(HXUIOBJ scene) {

  if (Find(scene, L"UsbAudioVolume")) return true;
  HXUIOBJ above = Find(scene, L"btnMCX");
  HXUIOBJ parent = 0, next_parent = 0;
  if (!above || g_xui.parent(above, &parent) < 0 || !parent) return false;
  // Select Music belongs to the native mini-player's scene. Link to that
  // scene, preserving its own controls/default focus rather than stealing them.
  HXUIOBJ next = Find(parent, L"scnMusic");
  if (!next || next == above || g_xui.parent(next, &next_parent) < 0 ||
      next_parent != parent ||
      g_xui.navigation(next, XUI_CONTROL_NAVIGATE_UP, FALSE, FALSE) != above) return false;

  guide_layout::Rect above_rect, next_rect, parent_rect, obstacles[32];
  unsigned obstacle_count = 0;
  if (!ReadRect(above, &above_rect) || !ReadRect(parent, &parent_rect) ||
      !ReadRect(next, &next_rect) ||
      above_rect.width != 323 || above_rect.height != 28) return false;
  HXUIOBJ child = 0;
  if (g_xui.first(parent, &child) < 0) return false;
  unsigned children = 0;
  while (child) {
    if (++children > 32) return false;
    if (child != above && child != next) {
      guide_layout::Rect rect;
      if (!ReadRect(child, &rect)) return false;
      if (obstacle_count == 32) return false;
      obstacles[obstacle_count++] = rect;
    }
    HXUIOBJ sibling = 0;
    if (g_xui.next(child, &sibling) < 0) return false;
    child = sibling;
  }
  float insertion_y = 0;
  if (!guide_layout::PlanGap(above_rect, next_rect, parent_rect.height,
                            obstacles, obstacle_count, 28, &insertion_y)) return false;

  WCHAR up_path[96], down_path[96];
  PropertyValue old_down, old_up;
  if (!ParentPath(above, up_path, _countof(up_path)) ||
      !ParentPath(next, down_path, _countof(down_path)) ||
      !GetProperty(above, L"NavDown", &old_down) || !GetProperty(next, L"NavUp", &old_up) ||
      !RegisterVolumeClass()) return false;
  if (old_down.get()->type != XUI_EPT_STRING || old_up.get()->type != XUI_EPT_STRING ||
      (old_down.get()->szVal && wcslen(old_down.get()->szVal) >= 96) ||
      (old_up.get()->szVal && wcslen(old_up.get()->szVal) >= 96)) return false;

  HXUIOBJ panel = CreateVolumePanel(parent, above_rect.x, insertion_y);
  if (!panel) return false;
  HXUIOBJ volume_button = Find(panel, L"VolumeButton");
  HXUIOBJ microphone_button = Find(panel, L"MicrophoneMuteButton");
  HXUIOBJ options_button = Find(panel, L"OptionsButton");
  HXUIOBJ slider = Find(panel, L"VolumeSlider");
  VolumeScene* state = 0;
  HXUIOBJ implementation = g_xui.cast(panel, g_volume_class_handle);
  if (!volume_button || !microphone_button || !options_button ||
      !slider || !implementation ||
      g_xui.instance(implementation, (void**)&state) < 0 || !state ||
      state->slider != slider || !state->dismiss_button || !state->confirm_button || !state->title ||
      state->volume_button != volume_button) {
    g_xui.destroy(panel);
    return false;
  }
  state->host_scene = scene;
  state->above_button = above;
  state->next_button = next;
  wcscpy_s(state->old_down, old_down.get()->szVal ? old_down.get()->szVal : L"");
  wcscpy_s(state->old_up, old_up.get()->szVal ? old_up.get()->szVal : L"");

  // Commit both navigation directions together, or restore them. Existing
  // Media rows and the native music controls never change position.
  bool attempted_above = false, attempted_next = false;
  HXUIOBJ controls[4] = {volume_button, microphone_button, options_button, slider};
  bool success = true;
  for (unsigned i = 0; success && i < _countof(controls); ++i)
    success = SetString(controls[i], L"NavUp", up_path) &&
        SetString(controls[i], L"NavDown", down_path);
  if (success) {
    attempted_above = true;
    success = SetString(above, L"NavDown", L"UsbAudioVolume\\VolumeButton");
  }
  if (success) {
    attempted_next = true;
    success = SetString(next, L"NavUp", L"UsbAudioVolume\\VolumeButton");
  }
  DWORD links = 0;
  if (success) {
    if (g_xui.navigation(above, XUI_CONTROL_NAVIGATE_DOWN, FALSE, FALSE) == volume_button) links |= 1;
    if (g_xui.navigation(volume_button, XUI_CONTROL_NAVIGATE_UP, FALSE, FALSE) == above) links |= 2;
    if (g_xui.navigation(volume_button, XUI_CONTROL_NAVIGATE_DOWN, FALSE, FALSE) == next) links |= 4;
    if (g_xui.navigation(next, XUI_CONTROL_NAVIGATE_UP, FALSE, FALSE) == volume_button) links |= 8;
    success = links == 15;
  }
  if (success) success = SUCCEEDED(g_xui.set_timer(panel, kVolumeRefreshTimer, 100));

  if (!success) {
    bool restored = true;
    if (attempted_above && !SetProperty(above, L"NavDown", &old_down)) restored = false;
    if (attempted_next && !SetProperty(next, L"NavUp", &old_up)) restored = false;
    // If XUI cannot restore a property (e.g. allocation failure), preserve
    // the parent-owned child rather than destroy a navigation target.
    if (!restored) return false;
    g_xui.destroy(panel);
    return false;
  }

  state->layout_ready = true;
  state->visible = true;
  RefreshRowVisibility(state);
  return true;
}

bool IsGuideMainScene(LPCWSTR path) {
  if (!path) return false;
  LPCWSTR name = path;
  for (LPCWSTR p = path; *p; ++p)
    if (*p == L'/' || *p == L'\\' || *p == L'#') name = p + 1;
  return wcscmp(name, L"GuideMain.xur") == 0;
}

HRESULT APIENTRY SceneCreateHook(LPCWSTR base, LPCWSTR path, void* data, HXUIOBJ* scene) {
  SceneCreateFn original = g_create_detour.Original<SceneCreateFn>();
  HRESULT result = original(base, path, data, scene);
  if (SUCCEEDED(result) && scene && *scene) {
    LPCWSTR id = 0;
    // XuiElementGetId reports "HUDScene" for the outer object; the
    // GuideMainScene string in GuideMain.xur is its implementation class.
    if (IsGuideMainScene(path) && g_xui.id(*scene, &id) >= 0 && id &&
        wcscmp(id, L"HUDScene") == 0) {
      g_guide_main_scene = *scene;
      // All object work is on the Guide thread; the audio worker never touches XUI.
      HXUIOBJ media = Find(*scene, L"Tab3");
      if (media) InjectMedia(media);
    }
  }
  return result;
}

bool Install() {
  HANDLE xam = GetModuleHandleA("xam.xex");
  HANDLE hud = GetModuleHandleA("hud.xex");
  SceneCreateFn scene_create = 0;
  BuildResourceLocatorFn build_locator = 0;
  if (!MemoryLocator(g_microphone_icon, _countof(g_microphone_icon),
                     kGuideMicrophonePng, sizeof(kGuideMicrophonePng)) ||
      !MemoryLocator(g_microphone_muted_icon,
                     _countof(g_microphone_muted_icon),
                     kGuideMicrophoneMutedPng,
                     sizeof(kGuideMicrophoneMutedPng)) ||
      !MemoryLocator(g_options_icon, _countof(g_options_icon),
                     kGuideAudioSettingsPng,
                     sizeof(kGuideAudioSettingsPng)) ||
      !xam || !hud || !Resolve(xam, 795, &build_locator) ||
      build_locator(hud, L"hud", L"ico_32x_volume.png", g_volume_icon,
                    _countof(g_volume_icon)) != 0 ||
      !Resolve(xam, 855, &scene_create) ||
      !Resolve(xam, 805, &g_xui.create) ||
      !Resolve(xam, 806, &g_xui.destroy) || !Resolve(xam, 808, &g_xui.add_child) ||
      !Resolve(xam, 817, &g_xui.parent) || !Resolve(xam, 811, &g_xui.first) ||
      !Resolve(xam, 816, &g_xui.next) || !Resolve(xam, 814, &g_xui.id) ||
      !Resolve(xam, 991, &g_xui.position) || !Resolve(xam, 945, &g_xui.set_position) ||
      !Resolve(xam, 840, &g_xui.get_property_id) || !Resolve(xam, 839, &g_xui.get_property) ||
      !Resolve(xam, 892, &g_xui.set_property) || !Resolve(xam, 889, &g_xui.construct_value) ||
      !Resolve(xam, 890, &g_xui.destroy_value) || !Resolve(xam, 891, &g_xui.string_value) ||
      !Resolve(xam, 2093, &g_xui.float_value) || !Resolve(xam, 2092, &g_xui.int_value) ||
      !Resolve(xam, 922, &g_xui.uint_value) || !Resolve(xam, 909, &g_xui.color_value) ||
      !Resolve(xam, 920, &g_xui.bool_value) ||
      !Resolve(xam, 842, &g_xui.register_class) || !Resolve(xam, 807, &g_xui.cast) ||
      !Resolve(xam, 838, &g_xui.instance) || !Resolve(xam, 918, &g_xui.navigation) ||
      !Resolve(xam, 867, &g_xui.set_text) || !Resolve(xam, 897, &g_xui.set_slider) ||
      !Resolve(xam, 898, &g_xui.get_slider) || !Resolve(xam, 813, &g_xui.focus_user) ||
      !Resolve(xam, 863, &g_xui.send) || !Resolve(xam, 887, &g_xui.broadcast) ||
      !Resolve(xam, 917, &g_xui.visual) ||
      !Resolve(xam, 824, &g_xui.set_user_focus) ||
      !Resolve(xam, 0x81a, &g_xui.set_scale) ||
      !Resolve(xam, 858, &g_xui.navigate_forward) ||
      !Resolve(xam, 856, &g_xui.navigate_back) ||
      !Resolve(xam, 872, &g_xui.valid) ||
      !Resolve(xam, 1319, &g_xui.in_transition) ||
      !Resolve(xam, 868, &g_xui.set_timer) ||
      !Resolve(xam, 881, &g_xui.kill_timer) ||
      !Resolve(xam, 994, &g_xui.tree_has_focus) ||
      !Resolve(xam, 825, &g_xui.tree_focus) ||
      !Resolve(xam, 910, &g_xui.figure_fill) ||
      !Resolve(xam, 1017, &g_xui.figure_shape)) return false;

  g_create_detour = PowerPcDetour((void*)scene_create, (void*)SceneCreateHook);
  return g_create_detour.Install();
}
}  // namespace

VOID GuideUiTick() {
  // Chain after Nova when present. The worker only installs the detour.
  static DWORD ticks = 0;
  if (g_install_state) return;
  ++ticks;

  if (ticks < 150) return;
  HANDLE nova = GetModuleHandleA("Nova.xex");

  if (!nova && ticks < 600) return;
  LONG state = Install() ? 1 : -1;
  InterlockedExchange(&g_install_state, state);

}
