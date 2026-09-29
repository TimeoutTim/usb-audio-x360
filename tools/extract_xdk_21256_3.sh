#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: $0 <XBOX360 SDK 21256.3.exe> <destination>" >&2
  exit 2
fi

installer=$1
destination=$2
expected_sha256=efec946c7b4436d53a6c41bb6bcff8373387ec97557e92f0ef672c85eadc4bc7

actual_sha256=$(sha256sum "$installer" | awk '{print $1}')
if [[ "$actual_sha256" != "$expected_sha256" ]]; then
  echo "refusing unknown installer: SHA-256 is $actual_sha256" >&2
  exit 1
fi

command -v 7z >/dev/null
command -v dd >/dev/null

# CAB boundaries verified for the installer hash above. The executable is an
# unpacker followed by a series of independent Microsoft Cabinet archives.
offsets=(
  438272
  174654055
  250828290
  379237894
  447533807
  509109656
  543946771
  602771722
  632465480
  662612601
  789777119
  825764658
  910665123
  1129913590
  1408044771
)

installer_size=$(stat -c '%s' "$installer")
scratch=$(mktemp -d /tmp/xdk21256-cabs.XXXXXX)
trap 'rm -rf -- "$scratch"' EXIT
mkdir -p "$destination"

for ((index = 0; index < ${#offsets[@]}; ++index)); do
  start=${offsets[$index]}
  if ((index + 1 < ${#offsets[@]})); then
    end=${offsets[$((index + 1))]}
  else
    end=$installer_size
  fi
  length=$((end - start))
  cabinet="$scratch/payload-$index.cab"
  dd if="$installer" of="$cabinet" iflag=skip_bytes,count_bytes \
    skip="$start" count="$length" status=none
  7z x -y "-o$destination" "$cabinet" >/dev/null
done

echo "extracted XDK 21256.3 payload to $destination"
