param(
  [ValidateSet(0, 1)]
  [int]$DebugApi = 0
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$projectRoot = Split-Path $PSScriptRoot -Parent
$sourceDir = Join-Path $projectRoot 'src'
$outputRoot = Join-Path $projectRoot 'bin'
$buildDir = Join-Path $outputRoot 'windows-build'
$xdkRoot = $env:XDK_ROOT
if ([string]::IsNullOrWhiteSpace($xdkRoot)) {
  $programFilesX86 = [Environment]::GetEnvironmentVariable('ProgramFiles(x86)')
  $xdkRoot = Join-Path $programFilesX86 'Microsoft Xbox 360 SDK'
}
$xdkRoot = (Resolve-Path $xdkRoot).Path

$compiler = Join-Path $xdkRoot 'bin\win32\cl.exe'
$linker = Join-Path $xdkRoot 'bin\win32\link.exe'
$imageXex = Join-Path $xdkRoot 'bin\win32\imagexex.exe'
$xdkInclude = Join-Path $xdkRoot 'include\xbox'
$previewInclude = Join-Path $xdkRoot 'TechPreview\Jul12Compiler\include\xbox'
$libraryDir = Join-Path $xdkRoot 'lib\xbox'

foreach ($required in @(
  $compiler,
  $linker,
  $imageXex,
  (Join-Path $xdkInclude 'xtl.h'),
  (Join-Path $libraryDir 'xboxkrnl.lib')
)) {
  if (-not (Test-Path $required)) {
    throw "Required Xbox 360 SDK file not found: $required"
  }
}

New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
$sources = @(
  'driver.cpp',
  'audio.cpp',
  'settings.cpp',
  'settings_ini.cpp',
  'guide_ui.cpp',
  'remote_volume.cpp',
  'diagnostics.cpp',
  'xbox_usb_transport.cpp',
  'uac_descriptors.cpp',
  'uac_clock.cpp',
  'detour.cpp'
)
$objectFiles = @()

function Invoke-XdkTool([string]$Path, [string[]]$ToolArgs) {
  & $Path @ToolArgs
  if ($LASTEXITCODE -ne 0) {
    throw "Tool failed with exit code $LASTEXITCODE ($Path)"
  }
}

foreach ($source in $sources) {
  $sourcePath = Join-Path $sourceDir $source
  $objectPath = Join-Path $buildDir ([IO.Path]::ChangeExtension($source, '.obj'))
  $compileArgs = @(
    '/nologo', '/c', '/O2', '/GS-', '/D_XBOX', '/DNDEBUG',
    "/DUSB_AUDIO360_DEBUG_API=$DebugApi",
    "/I$sourceDir", "/I$xdkInclude", "/I$previewInclude",
    "/Fo$objectPath", $sourcePath
  )
  Invoke-XdkTool $compiler $compileArgs
  $objectFiles += $objectPath
}

$pePath = Join-Path $buildDir 'usb_audio360.pe'
$mapPath = Join-Path $buildDir 'usb_audio360.map'
$linkArgs = @(
  '/nologo', '/dll', '/release', '/xex:no',
  "/libpath:$libraryDir", "/map:$mapPath", "/out:$pePath"
) + $objectFiles + @('xapilib.lib', 'xboxkrnl.lib', 'libcMT.lib')
Invoke-XdkTool $linker $linkArgs

$stagedXex = Join-Path $buildDir 'usb_audio360.xex'
$imageArgs = @(
  "/IN:$pePath",
  "/OUT:$stagedXex",
  "/CONFIG:$(Join-Path $sourceDir 'xex.xml')"
)
Invoke-XdkTool $imageXex $imageArgs

$xexTool = $env:XEXTOOL
if ([string]::IsNullOrWhiteSpace($xexTool)) {
  $xexTool = Join-Path (Split-Path $projectRoot -Parent) '..\XexTool_v6\xextool.exe'
}
if (Test-Path $xexTool) {
  $xexTool = (Resolve-Path $xexTool).Path
  $xexInfo = (& $xexTool -l $stagedXex 2>&1 | Out-String)
  if ($LASTEXITCODE -ne 0) {
    throw "XexTool validation failed with exit code $LASTEXITCODE"
  }
  foreach ($flag in @('Devkit', 'Compressed', 'System Flash', 'DLL Module')) {
    if ($xexInfo -notmatch [regex]::Escape($flag)) {
      throw "XexTool output is missing the expected '$flag' flag"
    }
  }
} else {
  Write-Warning 'XexTool v6 not found; skipped packaged-XEX flag validation.'
}

$outputDir = $outputRoot
if ($DebugApi -eq 1) {
  $outputDir = Join-Path $outputRoot 'debug'
}
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
$outputXex = Join-Path $outputDir 'usb_audio360.xex'
Copy-Item $stagedXex $outputXex -Force

if ($DebugApi -eq 1) {
  $python = Get-Command python -ErrorAction SilentlyContinue
  if (-not $python) {
    throw 'Python is required to generate the debug API manifest.'
  }
  $manifest = Join-Path $outputDir 'usb_audio360.json'
  & $python.Source (Join-Path $PSScriptRoot 'debug_manifest.py') $mapPath $pePath $manifest
  if ($LASTEXITCODE -ne 0) {
    throw "Debug manifest generation failed with exit code $LASTEXITCODE"
  }
}

$hash = (Get-FileHash $outputXex -Algorithm SHA256).Hash.ToLowerInvariant()
Set-Content -Path (Join-Path $outputDir 'usb_audio360.sha256') `
  -Value "$hash  usb_audio360.xex" -NoNewline -Encoding Ascii
Write-Output "SHA256 $hash  $outputXex"
Write-Output "built $outputXex"