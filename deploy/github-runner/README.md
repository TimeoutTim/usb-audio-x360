# Self-hosted GitHub Actions runner

The build runs directly on a dedicated Ubuntu 24.04 amd64 host or LXC. A
native runner avoids Docker image layers and uses Wine 11.0 Staging, which is
required by the Xbox 360 XDK linker used by this project.

The workflow deliberately runs on `push` and manual dispatch, not
`pull_request`. Do not enable untrusted pull-request jobs on this runner: a
workflow executes repository code with access to the private toolchain and
persistent runner environment.

## Host requirements

Allocate at least 4 CPU cores, 4 GB RAM, and 25 GB of storage. A 30 GB disk is
recommended. The host must provide systemd and outbound HTTPS access; Docker
and LXC nesting are not required.

Install the build dependencies:

```bash
sudo apt update
sudo apt install -y \
  ca-certificates curl git clang libclang-rt-18-dev python3 p7zip-full \
  xvfb xauth jq sudo
```

Install the WineHQ Noble repository and the pinned Wine 11.0 Staging packages:

```bash
sudo dpkg --add-architecture i386
sudo install -d -m 0755 /etc/apt/keyrings
sudo curl -fsSL https://dl.winehq.org/wine-builds/winehq.key \
  -o /etc/apt/keyrings/winehq-archive.key
sudo curl -fsSL \
  https://dl.winehq.org/wine-builds/ubuntu/dists/noble/winehq-noble.sources \
  -o /etc/apt/sources.list.d/winehq-noble.sources
sudo apt update
sudo apt install -y \
  wine-staging=11.0.0~noble-1 \
  wine-staging-amd64=11.0.0~noble-1 \
  wine-staging-i386:i386=11.0.0~noble-1 \
  winehq-staging=11.0.0~noble-1
sudo apt-mark hold \
  winehq-staging wine-staging wine-staging-amd64 wine-staging-i386
wine --version
```

The last command must report `wine-11.0 (Staging)`.

## Private toolchain

Create a dedicated runner account and persistent directories:

```bash
sudo useradd --create-home --shell /bin/bash github-runner
sudo install -d -o github-runner -g github-runner /opt/actions-runner
sudo install -d -o github-runner -g github-runner /opt/xbox360
sudo install -d -o github-runner -g github-runner /var/cache/usb-audio360
```

Place the private tools at these exact paths:

- `/opt/xbox360/XBOX360 SDK 21256.3.exe`
- `/opt/xbox360/xextool.exe` from XexTool v6

They must be readable by `github-runner`. They remain outside the repository
and are never uploaded as artifacts. The first build validates the XDK
installer SHA-256 and extracts it under `/var/cache/usb-audio360`; later builds
reuse the extracted XDK and Wine prefix.

## Runner installation

Download and verify GitHub Actions Runner 2.336.0:

```bash
cd /opt/actions-runner
sudo curl -fL -o actions-runner-linux-x64-2.336.0.tar.gz \
  https://github.com/actions/runner/releases/download/v2.336.0/actions-runner-linux-x64-2.336.0.tar.gz
echo '04cf0be1aff4c3ec3554466c39124ca250e3effd8873bb7e8d68535aa9505d5d  actions-runner-linux-x64-2.336.0.tar.gz' \
  | sha256sum -c -
sudo tar xzf actions-runner-linux-x64-2.336.0.tar.gz
sudo chown -R github-runner:github-runner /opt/actions-runner
sudo ./bin/installdependencies.sh
```

Obtain a temporary token from **Settings → Actions → Runners → New
self-hosted runner**, then register and start the service:

```bash
sudo -u github-runner ./config.sh \
  --url https://github.com/TimeoutTim/usb-audio-x360 \
  --token YOUR_TEMPORARY_TOKEN \
  --name usb-audio360-builder \
  --labels xbox360-xdk \
  --work _work \
  --unattended \
  --replace
sudo ./svc.sh install github-runner
sudo ./svc.sh start
sudo ./svc.sh status
```

The workflow selects `[self-hosted, Linux, X64, xbox360-xdk]`. Do not leave an
older runner with the same labels online, because GitHub may dispatch the job
to either runner.

## Workflow behavior

- Every pushed commit on every branch is tested and built on this runner.
- Manual runs are available through **Actions → Build → Run workflow**.
- Pull requests do not trigger this workflow.
- Each successful run uploads `usb_audio360.xex` and its SHA-256 checksum for
  14 days.
- Pushing a tag matching `v*` publishes or updates a GitHub Release containing
  the XEX and checksum.

The release job runs on a GitHub-hosted runner. It receives only the finished
XEX and checksum, not the XDK, XexTool, Wine prefix, or extraction cache.
