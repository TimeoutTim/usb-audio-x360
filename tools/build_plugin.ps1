# Builds bin/usb_audio360.xex natively on Windows using an installed Xbox 360 XDK.
# No Wine or XexTool required; see BUILDING.md.
param(
  [string]$XdkPath = 'C:\Program Files (x86)\Microsoft Xbox 360 SDK',
  [string]$BuildDir = (Join-Path $env:TEMP 'usb-audio360-build\build'),
  [ValidateSet(0, 1)]
  [int]$DebugApi = 0
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$outputDir = if ($DebugApi -eq 1) { Join-Path $projectRoot 'bin\debug' } else { Join-Path $projectRoot 'bin' }

$cl = Join-Path $XdkPath 'bin\win32\cl.exe'
$link = Join-Path $XdkPath 'bin\win32\link.exe'
$imagexex = Join-Path $XdkPath 'bin\win32\imagexex.exe'
foreach ($tool in @($cl, $link, $imagexex)) {
  if (-not (Test-Path $tool)) { throw "XDK tool not found: $tool (is `$XdkPath correct?)" }
}

New-Item -ItemType Directory -Force -Path $BuildDir, $outputDir, (Join-Path $BuildDir 'tmp') | Out-Null
$env:TMP = Join-Path $BuildDir 'tmp'
$env:TEMP = $env:TMP

$sources = @(
  'driver.cpp','audio.cpp','settings.cpp','settings_ini.cpp','guide_ui.cpp',
  'remote_volume.cpp','diagnostics.cpp','xbox_usb_transport.cpp',
  'uac_descriptors.cpp','uac_clock.cpp','detour.cpp'
)
$headers = @(
  'audio.h','settings.h','settings_ini.h','guide_ui.h','remote_volume.h',
  'remote_volume_policy.h','guide_icons.h','guide_layout.h','diagnostics.h',
  'xbox_usb_transport.h','transfer_ownership.h','cleanup_lifecycle.h',
  'device_claim_gate.h','isoch_result.h','stream_test_budget.h','feedback_pacer.h',
  'control_deadline.h','test_tone.h','uac_descriptors.h','uac_clock.h',
  'uac_setup_policy.h','pcm_packet.h','playback_pacer.h','playback_profile.h',
  'capture_profile.h','mic_pcm.h','mic_gain.h','mic_test.h','surround_downmix.h',
  'usb_strings.h','mic_packet_policy.h','debug_command.h','detour.h'
)

Get-ChildItem $BuildDir -File -ErrorAction SilentlyContinue | Remove-Item -Force
foreach ($f in $sources + $headers + @('xex.xml')) {
  Copy-Item (Join-Path $projectRoot "src\$f") (Join-Path $BuildDir $f) -Force
}

$incXbox = Join-Path $XdkPath 'include\xbox'
$incTech = Join-Path $XdkPath 'TechPreview\Jul12Compiler\include\xbox'

foreach ($src in $sources) {
  $obj = [System.IO.Path]::ChangeExtension($src, '.obj')
  Write-Host "cl $src"
  & $cl /nologo /c /O2 /GS- /D_XBOX /DNDEBUG `
    "/I$BuildDir" "/DUSB_AUDIO360_DEBUG_API=$DebugApi" `
    "/I$incXbox" "/I$incTech" `
    "/Fo$(Join-Path $BuildDir $obj)" (Join-Path $BuildDir $src)
  if ($LASTEXITCODE -ne 0) { throw "compile failed: $src" }
}

$objs = $sources | ForEach-Object { Join-Path $BuildDir ([System.IO.Path]::ChangeExtension($_, '.obj')) }
$mapFile = Join-Path $BuildDir 'usb_audio360.map'
$peFile = Join-Path $BuildDir 'usb_audio360.pe'

Write-Host 'linking...'
& $link /nologo /dll /release /xex:no `
  "/libpath:$(Join-Path $XdkPath 'lib\xbox')" "/map:$mapFile" "/out:$peFile" `
  $objs xapilib.lib xboxkrnl.lib libcMT.lib
if ($LASTEXITCODE -ne 0) { throw 'link failed' }

$xexOut = Join-Path $BuildDir 'usb_audio360.xex'
Write-Host 'imagexex...'
& $imagexex "/IN:$peFile" "/OUT:$xexOut" "/CONFIG:$(Join-Path $BuildDir 'xex.xml')"
if ($LASTEXITCODE -ne 0) { throw 'imagexex failed' }

Copy-Item $xexOut (Join-Path $outputDir 'usb_audio360.xex') -Force
if ($DebugApi -eq 1) {
  $manifestPy = Join-Path $PSScriptRoot 'debug_manifest.py'
  python $manifestPy $mapFile $peFile (Join-Path $outputDir 'usb_audio360.json')
  if ($LASTEXITCODE -ne 0) { throw 'debug_manifest.py failed' }
}
$hash = (Get-FileHash (Join-Path $outputDir 'usb_audio360.xex') -Algorithm SHA256).Hash.ToLower()
Write-Host "sha256 $hash"
Write-Host "built $(Join-Path $outputDir 'usb_audio360.xex')"
