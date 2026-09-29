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
build_dir="$runtime_root/build"
output_dir="$project_root/bin"

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

if [[ ! -d "$wine_prefix/drive_c" ]]; then
  env WINEPREFIX="$wine_prefix" WINEDEBUG=-all wineboot -u
fi

rm -f "$build_dir"/*.obj "$build_dir"/*.pe "$build_dir"/*.xex \
      "$build_dir"/*.map "$build_dir/xextool-info.txt"
cp "$project_root/src/driver.cpp" "$build_dir/driver.cpp"
cp "$project_root/src/audio.cpp" "$build_dir/audio.cpp"
cp "$project_root/src/audio.h" "$build_dir/audio.h"
cp "$project_root/src/detour.cpp" "$build_dir/detour.cpp"
cp "$project_root/src/detour.h" "$build_dir/detour.h"
cp "$project_root/src/xex.xml" "$build_dir/xex.xml"

wine_build=$(env WINEPREFIX="$wine_prefix" WINEDEBUG=-all \
  winepath -w "$build_dir" | tr -d '\r')
wine_xdk=$(env WINEPREFIX="$wine_prefix" WINEDEBUG=-all \
  winepath -w "$xdk" | tr -d '\r')

compile() {
  local source=$1
  local output=$2
  local command="set TMP=$wine_build\\tmp&& set TEMP=$wine_build\\tmp&& "
  command+="$wine_xdk\\bin\\win32\\cl.exe /nologo /c /O2 /GS- "
  command+="/D_XBOX /DNDEBUG /I$wine_build "
  command+="/I$wine_xdk\\include\\xbox "
  command+="/I$wine_xdk\\TechPreview\\Jul12Compiler\\include\\xbox "
  command+="/Fo$wine_build\\$output $wine_build\\$source"
  env WINEPREFIX="$wine_prefix" WINEDEBUG=-all wine cmd /c "$command"
}

compile driver.cpp driver.obj
compile audio.cpp audio.obj
compile detour.cpp detour.obj

link_command="set TMP=$wine_build\\tmp&& set TEMP=$wine_build\\tmp&& "
link_command+="$wine_xdk\\bin\\win32\\link.exe /nologo /dll /release /xex:no "
link_command+="/libpath:$wine_xdk\\lib\\xbox "
link_command+="/map:$wine_build\\usb_audio360.map "
link_command+="/out:$wine_build\\usb_audio360.pe "
link_command+="$wine_build\\driver.obj $wine_build\\audio.obj "
link_command+="$wine_build\\detour.obj xapilib.lib xboxkrnl.lib libcMT.lib"
env WINEPREFIX="$wine_prefix" WINEDEBUG=-all wine cmd /c "$link_command"

env WINEPREFIX="$wine_prefix" WINEDEBUG=-all wine \
  "$xdk/bin/win32/imagexex.exe" \
  "/IN:$wine_build\\usb_audio360.pe" \
  "/OUT:$wine_build\\usb_audio360.xex" \
  "/CONFIG:$wine_build\\xex.xml"

(
  cd "$build_dir"
  env WINEPREFIX="$wine_prefix" WINEDEBUG=-all wine "$xextool" \
    -l usb_audio360.xex | tr -d '\r' >xextool-info.txt
)

grep -q '^  Devkit$' "$build_dir/xextool-info.txt"
grep -q '^  Compressed$' "$build_dir/xextool-info.txt"
grep -q '^  System Flash$' "$build_dir/xextool-info.txt"
grep -q '^  DLL Module$' "$build_dir/xextool-info.txt"
cp "$build_dir/usb_audio360.xex" "$output_dir/usb_audio360.xex"

sha256sum "$output_dir/usb_audio360.xex"
echo "built $output_dir/usb_audio360.xex"
