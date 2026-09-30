# Self-hosted GitHub Actions runner

This Compose service provides the Linux, Wine, Clang, Python, and 7-Zip
environment used by `.github/workflows/build.yml`. Microsoft's Xbox 360 XDK
and XexTool remain outside the repository and are mounted read-only.

The workflow deliberately runs on `push` and manual dispatch, not
`pull_request`. Do not enable untrusted pull-request jobs on this runner: a
workflow executes repository code with access to the mounted toolchain and
the persistent runner environment.

## Server setup

1. Install Docker Engine with the Compose plugin.
2. Create a private toolchain directory on the server and place these files in
   it:

   - `XBOX360 SDK 21256.3.exe`
   - `xextool.exe` from XexTool v6

   The runner user inside the container (UID 1001) must be able to read both
   files. They are mounted read-only and are never uploaded as build artifacts.
3. In GitHub, open **Settings → Actions → Runners → New self-hosted runner** for
   the repository. Copy the time-limited registration token from the displayed
   `config.sh` command.
4. Configure and start the service:

   ```bash
   cd deploy/github-runner
   cp .env.example .env
   chmod 600 .env
   # Edit .env: set both host paths and the temporary RUNNER_TOKEN.
   docker compose build --pull
   docker compose up -d
   docker compose logs -f runner
   ```

   Registration succeeded when the log says `Listening for Jobs`, and GitHub
   shows the runner as idle with the `xbox360-xdk` label.
5. After registration, clear `RUNNER_TOKEN` in `.env` and recreate the
   container. The registered credentials persist in the `runner-state` volume,
   so the one-hour token is no longer required:

   ```bash
   docker compose up -d --force-recreate
   ```

The first build validates the XDK installer SHA-256 and extracts it into the
`xdk-cache` volume. Later builds reuse the extracted XDK and Wine prefix.

## Workflow behavior

- Every pushed commit on every branch is tested and built on this runner.
- Manual runs are available through **Actions → Build → Run workflow**.
- Each successful run uploads `usb_audio360.xex` and its SHA-256 checksum for
  14 days.
- Pushing a tag matching `v*` (for example, `v0.3.0`) publishes or updates a
  GitHub Release with those two files and generated release notes.

The release job runs on a GitHub-hosted runner. It receives only the finished
XEX and checksum—not the XDK, XexTool, Wine prefix, or extraction cache.

## Maintenance and recovery

The GitHub runner updates itself in the persistent `runner-state` volume.
Rebuild the image periodically for operating-system and Wine updates:

```bash
docker compose build --pull
docker compose up -d
```

If the repository runner registration is deleted in GitHub, remove and
recreate the `runner-state` volume, obtain a fresh registration token, and
start the service again. Removing that volume discards only runner state; the
separate `xdk-cache` volume can be retained.
