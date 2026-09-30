#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
project_root=$(cd "$script_dir/.." && pwd)
workspace_root=$(cd "$project_root/.." && pwd)

installer=${XDK_INSTALLER:-$workspace_root/XBOX360 SDK 21256.3.exe}
xextool=${XEXTOOL:-$workspace_root/../XexTool_v6/xextool.exe}
runtime_root=${USB_AUDIO360_RUNTIME:-/tmp/usb-audio360-build}
xdk_payload="$runtime_root/xdk-21256.3"
xdk="$xdk_payload/XDK"
wine_prefix="$runtime_root/wine"
wine_prefix_ready="$wine_prefix/.usb-audio360-ready"
build_dir="$runtime_root/build"
output_dir="$project_root/bin"
debug_api=${USB_AUDIO360_DEBUG_API:-0}
if [[ "$debug_api" != 0 && "$debug_api" != 1 ]]; then
  echo "USB_AUDIO360_DEBUG_API must be 0 or 1" >&2
  exit 1
fi
if [[ "$debug_api" == 1 ]]; then output_dir="$output_dir/debug"; fi

mkdir -p "$runtime_root" "$build_dir" "$output_dir" "$build_dir/tmp"

if [[ ! -f "$xdk/bin/win32/cl.exe" ||
      ! -f "$xdk/include/xbox/xtl.h" ||
      ! -f "$xdk/lib/xbox/xboxkrnl.lib" ]]; then
  "$script_dir/extract_xdk_21256_3.sh" "$installer" "$xdk_payload"
fi

if [[ ! -f "$xextool" ]]; then
  echo "xextool not found: $xextool" >&2
  exit 1
fi

wine_env=(
  env
  WINEPREFIX="$wine_prefix"
  WINEARCH=win64
  WINEDEBUG=-all
  WINEDLLOVERRIDES=mscoree,mshtml=
)

cleanup_wine() {
  timeout --kill-after=2s 10s \
    "${wine_env[@]}" wineserver -k >/dev/null 2>&1 || true
  timeout --kill-after=2s 10s \
    "${wine_env[@]}" wineserver -w >/dev/null 2>&1 || true
}
trap cleanup_wine EXIT

if [[ ! -f "$wine_prefix_ready" ]]; then
  initializing_prefix="$runtime_root/wine-initializing.$$"
  rm -rf -- "$initializing_prefix"
  mkdir -p "$initializing_prefix"

  initializing_wine_env=(
    env
    WINEPREFIX="$initializing_prefix"
    WINEARCH=win64
    WINEDEBUG=-all
    WINEDLLOVERRIDES=mscoree,mshtml=
  )

  if ! timeout --kill-after=5s 180s \
      "${initializing_wine_env[@]}" wineboot --init; then
    timeout --kill-after=2s 10s \
      "${initializing_wine_env[@]}" wineserver -k >/dev/null 2>&1 || true
    rm -rf -- "$initializing_prefix"
    echo "Wine prefix initialization failed" >&2
    exit 1
  fi
  if ! timeout --kill-after=5s 30s \
      "${initializing_wine_env[@]}" wine cmd /c ver >/dev/null; then
    timeout --kill-after=2s 10s \
      "${initializing_wine_env[@]}" wineserver -k >/dev/null 2>&1 || true
    rm -rf -- "$initializing_prefix"
    echo "Wine prefix validation failed" >&2
    exit 1
  fi

  timeout --kill-after=2s 10s \
    "${initializing_wine_env[@]}" wineserver -k >/dev/null 2>&1 || true
  timeout --kill-after=2s 10s \
    "${initializing_wine_env[@]}" wineserver -w >/dev/null 2>&1 || true
  touch "$initializing_prefix/.usb-audio360-ready"
  rm -rf -- "$wine_prefix"
  mv "$initializing_prefix" "$wine_prefix"
fi

rm -f "$build_dir"/*.obj "$build_dir"/*.pe "$build_dir"/*.xex \
      "$build_dir"/*.map "$build_dir/xextool-info.txt"
cp "$project_root/src/driver.cpp" "$build_dir/driver.cpp"
cp "$project_root/src/audio.cpp" "$build_dir/audio.cpp"
cp "$project_root/src/audio.h" "$build_dir/audio.h"
cp "$project_root/src/xbox_usb_transport.h" "$build_dir/xbox_usb_transport.h"
cp "$project_root/src/xbox_usb_transport.cpp" "$build_dir/xbox_usb_transport.cpp"
cp "$project_root/src/transfer_ownership.h" "$build_dir/transfer_ownership.h"
cp "$project_root/src/cleanup_lifecycle.h" "$build_dir/cleanup_lifecycle.h"
cp "$project_root/src/device_claim_gate.h" "$build_dir/device_claim_gate.h"
cp "$project_root/src/isoch_result.h" "$build_dir/isoch_result.h"
cp "$project_root/src/stream_test_budget.h" "$build_dir/stream_test_budget.h"
cp "$project_root/src/feedback_pacer.h" "$build_dir/feedback_pacer.h"
cp "$project_root/src/control_deadline.h" "$build_dir/control_deadline.h"
cp "$project_root/src/test_tone.h" "$build_dir/test_tone.h"
cp "$project_root/src/uac_descriptors.h" "$build_dir/uac_descriptors.h"
cp "$project_root/src/uac_descriptors.cpp" "$build_dir/uac_descriptors.cpp"
cp "$project_root/src/uac_clock.h" "$build_dir/uac_clock.h"
cp "$project_root/src/uac_clock.cpp" "$build_dir/uac_clock.cpp"
cp "$project_root/src/uac_setup_policy.h" "$build_dir/uac_setup_policy.h"
cp "$project_root/src/pcm_packet.h" "$build_dir/pcm_packet.h"
cp "$project_root/src/playback_pacer.h" "$build_dir/playback_pacer.h"
cp "$project_root/src/playback_profile.h" "$build_dir/playback_profile.h"
cp "$project_root/src/debug_command.h" "$build_dir/debug_command.h"
cp "$project_root/src/detour.cpp" "$build_dir/detour.cpp"
cp "$project_root/src/detour.h" "$build_dir/detour.h"
cp "$project_root/src/xex.xml" "$build_dir/xex.xml"

wine_build=$("${wine_env[@]}" winepath -w "$build_dir" | tr -d '\r')
wine_xdk=$("${wine_env[@]}" winepath -w "$xdk" | tr -d '\r')

compile() {
  local source=$1
  local output=$2
  local command="set TMP=$wine_build\\tmp&& set TEMP=$wine_build\\tmp&& "
  command+="$wine_xdk\\bin\\win32\\cl.exe /nologo /c /O2 /GS- "
  command+="/D_XBOX /DNDEBUG /I$wine_build "
  command+="/DUSB_AUDIO360_DEBUG_API=$debug_api "
  command+="/I$wine_xdk\\include\\xbox "
  command+="/I$wine_xdk\\TechPreview\\Jul12Compiler\\include\\xbox "
  command+="/Fo$wine_build\\$output $wine_build\\$source"
  "${wine_env[@]}" wine cmd /c "$command"
}

compile driver.cpp driver.obj
compile audio.cpp audio.obj
compile xbox_usb_transport.cpp xbox_usb_transport.obj
compile uac_descriptors.cpp uac_descriptors.obj
compile uac_clock.cpp uac_clock.obj
compile detour.cpp detour.obj

link_command="set TMP=$wine_build\\tmp&& set TEMP=$wine_build\\tmp&& "
link_command+="$wine_xdk\\bin\\win32\\link.exe /nologo /dll /release /xex:no "
link_command+="/libpath:$wine_xdk\\lib\\xbox "
link_command+="/map:$wine_build\\usb_audio360.map "
link_command+="/out:$wine_build\\usb_audio360.pe "
link_command+="$wine_build\\driver.obj $wine_build\\audio.obj "
link_command+="$wine_build\\xbox_usb_transport.obj "
link_command+="$wine_build\\uac_descriptors.obj $wine_build\\uac_clock.obj "
link_command+="$wine_build\\detour.obj xapilib.lib xboxkrnl.lib libcMT.lib"
"${wine_env[@]}" wine cmd /c "$link_command"

"${wine_env[@]}" wine \
  "$xdk/bin/win32/imagexex.exe" \
  "/IN:$wine_build\\usb_audio360.pe" \
  "/OUT:$wine_build\\usb_audio360.xex" \
  "/CONFIG:$wine_build\\xex.xml"

(
  cd "$build_dir"
  "${wine_env[@]}" wine "$xextool" \
    -l usb_audio360.xex | tr -d '\r' >xextool-info.txt
)

grep -q '^  Devkit$' "$build_dir/xextool-info.txt"
grep -q '^  Compressed$' "$build_dir/xextool-info.txt"
grep -q '^  System Flash$' "$build_dir/xextool-info.txt"
grep -q '^  DLL Module$' "$build_dir/xextool-info.txt"
cp "$build_dir/usb_audio360.xex" "$output_dir/usb_audio360.xex"
if [[ "$debug_api" == 1 ]]; then
  python3 "$script_dir/debug_manifest.py" "$build_dir/usb_audio360.map" \
    "$build_dir/usb_audio360.pe" "$output_dir/usb_audio360.json"
fi

sha256sum "$output_dir/usb_audio360.xex"
echo "built $output_dir/usb_audio360.xex"
