// SPDX-License-Identifier: GPL-3.0-or-later
#include "guide_ui.h"
#include <xui.h>
#include <stdlib.h>
#include <stdio.h>
#include <wchar.h>
#include "audio.h"
#include "detour.h"
#include "guide_layout.h"

extern "C" LONG XexGetProcedureAddress(HANDLE, DWORD, PVOID);
#if USB_AUDIO360_DEBUG_API || USB_AUDIO360_GUIDE_DIAGNOSTICS
extern "C" __declspec(align(128)) volatile DWORD UsbAudioGuideDiagnostic[32] = {
    0x47554938};
#define UI_DIAG(i, value) (UsbAudioGuideDiagnostic[i] = (DWORD)(value))
#define UI_COUNT(i) (++UsbAudioGuideDiagnostic[i])
#else
#define UI_DIAG(i, value) ((void)0)
#define UI_COUNT(i) ((void)0)
#endif

namespace {
typedef HRESULT (APIENTRY* SceneCreateFn)(LPCWSTR, LPCWSTR, void*, HXUIOBJ*);
static PowerPcDetour g_create_detour;
static volatile LONG g_install_state = 0;
static XUIClass g_volume_class;
static HXUICLASS g_volume_class_handle = 0;
static bool g_register_attempted = false;

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
} g_xui = {};

template <typename T>
bool Resolve(HANDLE module, DWORD ordinal, T* result) {
  PVOID address = 0;
  UI_DIAG(4, ordinal);
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
  HXUIOBJ slider;
  HXUIOBJ label;
  bool synchronizing;
};

void Synchronize(VolumeScene* scene) {
  if (!scene || !scene->slider || !scene->label || scene->synchronizing) return;
  scene->synchronizing = true;
  LONG volume = AudioGetVolume();
  g_xui.set_slider(scene->slider, volume);
  WCHAR label[32];
  if (volume == 0) wcscpy_s(label, L"USB Muted");
  else _snwprintf_s(label, _countof(label), _TRUNCATE, L"USB %ld%%", volume);
  g_xui.set_text(scene->label, label);
  scene->synchronizing = false;
}

HRESULT APIENTRY CreateVolume(HXUIOBJ, void** instance) {
  if (!instance) return E_INVALIDARG;
  *instance = calloc(1, sizeof(VolumeScene));
  return *instance ? S_OK : E_OUTOFMEMORY;
}

HRESULT APIENTRY DestroyVolume(void* instance) {
  // Parent owns this subtree. Siblings may already be gone during teardown.
  free(instance);
  UI_COUNT(24);
  return S_OK;
}

HRESULT APIENTRY VolumeObjectProc(HXUIOBJ object, XUIMessage* message, void* instance) {
  VolumeScene* state = (VolumeScene*)instance;
  if (!state || !message) return S_OK;
  if (message->dwMessage == XM_INIT) {
    state->slider = Find(object, L"VolumeSlider");
    state->label = Find(object, L"VolumeLabel");
    Synchronize(state);
    UI_COUNT(15);
  } else if (message->dwMessage == XM_NOTIFY && message->pvData &&
             message->cbData >= sizeof(XUINotify)) {
    XUINotify* notify = (XUINotify*)message->pvData;
    if (notify->hObjSource != state->slider || !state->slider) return S_OK;
    if (notify->dwNotify == XN_SET_FOCUS) {
      Synchronize(state);
      UI_COUNT(16);
      UI_DIAG(19, g_xui.focus_user(state->slider));
    } else if (notify->dwNotify == XN_VALUE_CHANGED) {
      if (!state->synchronizing) {
        int value = 0;
        if (g_xui.get_slider(state->slider, &value) >= 0) {
          UI_DIAG(22, value);
          BOOL changed = AudioSetVolume(value);
          UI_DIAG(29, changed);
          UI_DIAG(18, AudioGetVolume());
          UI_COUNT(17);
          Synchronize(state);
        }
      }
      message->bHandled = TRUE;
    }
  }
  // Native base classes handle focus and input through the normal dispatcher.
  return S_OK;
}

bool RegisterVolumeClass() {
  if (g_register_attempted) return g_volume_class_handle != 0;
  g_register_attempted = true;
  memset(&g_volume_class, 0, sizeof(g_volume_class));
  g_volume_class.szClassName = L"UsbAudioVolumeScene";
  g_volume_class.szBaseClassName = XUI_CLASS_SCENE;
  g_volume_class.Methods.CreateInstance = CreateVolume;
  g_volume_class.Methods.DestroyInstance = DestroyVolume;
  g_volume_class.Methods.ObjectProc = VolumeObjectProc;
  HRESULT result = g_xui.register_class(&g_volume_class, &g_volume_class_handle);
  UI_DIAG(5, result);
  return SUCCEEDED(result) && g_volume_class_handle;
}

bool InitializeObject(HXUIOBJ object) {
  XUIMessage message;
  XUIMessageInit data;
  XuiMessageInit(&message, &data, 0);
  HRESULT result = g_xui.send(object, &message);
  UI_DIAG(21, result);
  return SUCCEEDED(result);
}

bool ConfigureElement(HXUIOBJ object, LPCWSTR id, float width, float height,
                      float x, float y) {
  D3DXVECTOR3 position(x, y, 0);
  return SetString(object, L"Id", id) && SetFloat(object, L"Width", width) &&
      SetFloat(object, L"Height", height) && g_xui.set_position(object, &position) >= 0;
}

HXUIOBJ CreateVolumePanel(HXUIOBJ parent, float x, float y) {
  // XAM's Guide loader requires XUR 8; the public XDK compiler emits XUR 5.
  // Construct native objects instead, without a second/private XUI runtime.
  HXUIOBJ panel = 0, label = 0, slider = 0;
  bool label_owned = false, slider_owned = false;
  UI_DIAG(26, 1);
  if (g_xui.create(L"UsbAudioVolumeScene", &panel) < 0 || !panel ||
      g_xui.create(XUI_CLASS_TEXT, &label) < 0 || !label ||
      g_xui.create(XUI_CLASS_SLIDER, &slider) < 0 || !slider) goto fail;
  UI_DIAG(26, 2);
  if (!ConfigureElement(panel, L"UsbAudioVolume", 323, 28, x, y) ||
      !ConfigureElement(label, L"VolumeLabel", 80, 28, 0, 0) ||
      !ConfigureElement(slider, L"VolumeSlider", 243, 22, 80, 3)) goto fail;
  UI_DIAG(26, 3);
  if (!SetString(label, L"Text", L"USB 100%") ||
      !SetFloat(label, L"PointSize", 12) ||
      !SetUint(label, L"TextColor", 0xff333333, true) ||
      !SetUint(label, L"TextStyle", 16, false) ||
      !SetString(slider, L"Visual", L"Slider_Volume") ||
      !SetString(slider, L"NavLeft", L"VolumeSlider") ||
      !SetString(slider, L"NavRight", L"VolumeSlider") ||
      !SetInt(slider, L"RangeMin", 0) || !SetInt(slider, L"RangeMax", 100) ||
      !SetInt(slider, L"Value", 100) || !SetInt(slider, L"Step", 5) ||
      !SetInt(slider, L"AccelInc", 0)) goto fail;
  UI_DIAG(26, 4);
  if (g_xui.add_child(panel, label) < 0) goto fail;
  label_owned = true;
  if (g_xui.add_child(panel, slider) < 0) goto fail;
  slider_owned = true;
  if (g_xui.add_child(parent, panel) < 0) goto fail;
  // AddChild supplies parent-change messages. Like the native scene loader,
  // initialize children before the owning scene, after assembling the tree.
  UI_DIAG(26, 5);
  if (!InitializeObject(label) || !InitializeObject(slider) ||
      !InitializeObject(panel)) goto fail;
  {
    XUIMessage message;
    XuiMessage(&message, XM_SKIN_CHANGED);
    HRESULT result = g_xui.broadcast(panel, &message);
    UI_DIAG(27, result);
    HXUIOBJ visual = 0;
    if (FAILED(result) || g_xui.visual(slider, &visual) < 0 || !visual) goto fail;
    UI_DIAG(28, visual);
  }
  UI_DIAG(26, 6);
  return panel;
fail:
  if (slider && !slider_owned) g_xui.destroy(slider);
  if (label && !label_owned) g_xui.destroy(label);
  if (panel) g_xui.destroy(panel);
  return 0;
}

bool ParentPath(HXUIOBJ object, WCHAR* output, DWORD capacity) {
  LPCWSTR id = 0;
  if (g_xui.id(object, &id) < 0 || !id || !*id || wcslen(id) + 4 > capacity ||
      wcschr(id, L'\\') || wcschr(id, L'/')) return false;
  return _snwprintf_s(output, capacity, _TRUNCATE, L"..\\%s", id) >= 0;
}

bool InjectHome(HXUIOBJ scene) {
  UI_COUNT(7);
  // Nova may recursively replace the stock Home scene through our hook.
  if (Find(scene, L"UsbAudioVolume")) return true;
  HXUIOBJ home = Find(scene, L"btnHome");
  if (!home) home = Find(scene, L"btnDashboard");
  HXUIOBJ parent = 0, next_parent = 0;
  if (!home || g_xui.parent(home, &parent) < 0 || !parent) return false;
  HXUIOBJ next = g_xui.navigation(home, XUI_CONTROL_NAVIGATE_DOWN, FALSE, FALSE);
  if (!next || next == home || g_xui.parent(next, &next_parent) < 0 ||
      next_parent != parent ||
      g_xui.navigation(next, XUI_CONTROL_NAVIGATE_UP, FALSE, FALSE) != home) return false;
  UI_DIAG(9, parent); UI_DIAG(10, home); UI_DIAG(11, next); UI_DIAG(8, 1);

  guide_layout::Rect home_rect, parent_rect, rows[16], obstacles[16];
  HXUIOBJ row_objects[16];
  D3DXVECTOR3 row_positions[16];
  unsigned row_count = 0, obstacle_count = 0;
  if (!ReadRect(home, &home_rect) || !ReadRect(parent, &parent_rect) ||
      home_rect.width != 323 || home_rect.height != 28) return false;
  HXUIOBJ child = 0;
  if (g_xui.first(parent, &child) < 0) return false;
  unsigned children = 0;
  while (child) {
    if (++children > 32) return false;
    if (child != home) {
      guide_layout::Rect rect;
      if (!ReadRect(child, &rect)) return false;
      if (rect.height == home_rect.height && rect.width == home_rect.width &&
          rect.x == home_rect.x) {
        if (row_count == 16) return false;
        row_objects[row_count] = child;
        rows[row_count] = rect;
        if (g_xui.position(child, &row_positions[row_count]) < 0) return false;
        ++row_count;
      } else {
        if (obstacle_count == 16) return false;
        obstacles[obstacle_count++] = rect;
      }
    }
    HXUIOBJ sibling = 0;
    if (g_xui.next(child, &sibling) < 0) return false;
    child = sibling;
  }
  float insertion_y = 0;
  if (!guide_layout::Plan(home_rect, parent_rect.height, rows, row_count,
                         obstacles, obstacle_count, 28, &insertion_y)) return false;
  UI_DIAG(8, 2);
  WCHAR up_path[96], down_path[96];
  PropertyValue old_down, old_up;
  if (!ParentPath(home, up_path, _countof(up_path)) ||
      !ParentPath(next, down_path, _countof(down_path)) ||
      !GetProperty(home, L"NavDown", &old_down) || !GetProperty(next, L"NavUp", &old_up) ||
      !RegisterVolumeClass()) return false;

  HXUIOBJ panel = CreateVolumePanel(parent, home_rect.x, insertion_y);
  if (!panel) return false;
  HXUIOBJ slider = Find(panel, L"VolumeSlider");
  VolumeScene* state = 0;
  HXUIOBJ implementation = g_xui.cast(panel, g_volume_class_handle);
  if (!slider || !implementation ||
      g_xui.instance(implementation, (void**)&state) < 0 || !state ||
      state->slider != slider || !state->label) {
    g_xui.destroy(panel);
    return false;
  }
  UI_DIAG(12, panel); UI_DIAG(13, slider); UI_DIAG(8, 3);

  // Commit layout and both navigation directions together, or restore all.
  unsigned moved = 0;
  bool attempted_home = false, attempted_next = false;
  bool success = SetString(slider, L"NavUp", up_path) && SetString(slider, L"NavDown", down_path);
  while (success && moved < row_count) {
    unsigned index = moved++;
    D3DXVECTOR3 shifted = row_positions[index];
    shifted.y += 28;
    success = g_xui.set_position(row_objects[index], &shifted) >= 0;
  }
  if (success) {
    attempted_home = true;
    success = SetString(home, L"NavDown", L"UsbAudioVolume\\VolumeSlider");
  }
  if (success) {
    attempted_next = true;
    success = SetString(next, L"NavUp", L"UsbAudioVolume\\VolumeSlider");
  }
  DWORD links = 0;
  if (success) {
    if (g_xui.navigation(home, XUI_CONTROL_NAVIGATE_DOWN, FALSE, FALSE) == slider) links |= 1;
    if (g_xui.navigation(slider, XUI_CONTROL_NAVIGATE_UP, FALSE, FALSE) == home) links |= 2;
    if (g_xui.navigation(slider, XUI_CONTROL_NAVIGATE_DOWN, FALSE, FALSE) == next) links |= 4;
    if (g_xui.navigation(next, XUI_CONTROL_NAVIGATE_UP, FALSE, FALSE) == slider) links |= 8;
    success = links == 15;
  }
  UI_DIAG(14, links);
  if (!success) {
    bool restored = true;
    if (attempted_home && !SetProperty(home, L"NavDown", &old_down)) restored = false;
    if (attempted_next && !SetProperty(next, L"NavUp", &old_up)) restored = false;
    // If XUI cannot restore a property (e.g. allocation failure), preserve
    // the parent-owned child rather than destroy a navigation target.
    if (!restored) { UI_DIAG(25, 1); return false; }
    while (moved) { --moved; g_xui.set_position(row_objects[moved], &row_positions[moved]); }
    g_xui.destroy(panel);
    return false;
  }
  UI_DIAG(8, 4);
  return true;
}

bool IsHomeScene(LPCWSTR path) {
  if (!path) return false;
  LPCWSTR name = path;
  for (LPCWSTR p = path; *p; ++p)
    if (*p == L'/' || *p == L'\\' || *p == L'#') name = p + 1;
  return wcscmp(name, L"HomeTabSignedIn.xur") == 0 ||
      wcscmp(name, L"HomeTabSignedOut.xur") == 0 ||
      wcscmp(name, L"novaHomeTabSignedIn.xur") == 0 ||
      wcscmp(name, L"novaHomeTabSignedOut.xur") == 0;
}

HRESULT APIENTRY SceneCreateHook(LPCWSTR base, LPCWSTR path, void* data, HXUIOBJ* scene) {
  SceneCreateFn original = g_create_detour.Original<SceneCreateFn>();
  HRESULT result = original(base, path, data, scene);
  if (SUCCEEDED(result) && scene && *scene && IsHomeScene(path)) {
    // All object work is on the Guide thread; the audio worker never touches XUI.
    if (!InjectHome(*scene)) UI_COUNT(23);
  }
  return result;
}

bool Install() {
  HANDLE xam = GetModuleHandleA("xam.xex");
  SceneCreateFn scene_create = 0;
  if (!xam || !Resolve(xam, 855, &scene_create) ||
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
      !Resolve(xam, 842, &g_xui.register_class) || !Resolve(xam, 807, &g_xui.cast) ||
      !Resolve(xam, 838, &g_xui.instance) || !Resolve(xam, 918, &g_xui.navigation) ||
      !Resolve(xam, 867, &g_xui.set_text) || !Resolve(xam, 897, &g_xui.set_slider) ||
      !Resolve(xam, 898, &g_xui.get_slider) || !Resolve(xam, 813, &g_xui.focus_user) ||
      !Resolve(xam, 863, &g_xui.send) || !Resolve(xam, 887, &g_xui.broadcast) ||
      !Resolve(xam, 917, &g_xui.visual)) return false;
  UI_DIAG(4, 0);
  g_create_detour = PowerPcDetour((void*)scene_create, (void*)SceneCreateHook);
  bool installed = g_create_detour.Install();
  UI_DIAG(6, installed);
  return installed;
}
}  // namespace

VOID GuideUiTick() {
  // Chain after Nova when present. The worker only installs the detour.
  static DWORD ticks = 0;
  if (g_install_state) return;
  ++ticks;
  UI_DIAG(1, ticks);
  if (ticks < 150) return;
  HANDLE nova = GetModuleHandleA("Nova.xex");
  UI_DIAG(2, nova);
  if (!nova && ticks < 600) return;
  LONG state = Install() ? 1 : -1;
  InterlockedExchange(&g_install_state, state);
  UI_DIAG(3, state);
}
