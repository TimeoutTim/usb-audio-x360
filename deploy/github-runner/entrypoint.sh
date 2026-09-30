#!/usr/bin/env bash
set -euo pipefail

cd /actions-runner

Xvfb "$DISPLAY" -screen 0 1024x768x24 -nolisten tcp &

if [[ ! -f .runner ]]; then
  : "${RUNNER_SCOPE_URL:?Set RUNNER_SCOPE_URL to the GitHub repository URL}"
  : "${RUNNER_TOKEN:?Set RUNNER_TOKEN to a current GitHub runner registration token}"

  ./config.sh \
    --url "$RUNNER_SCOPE_URL" \
    --token "$RUNNER_TOKEN" \
    --name "${RUNNER_NAME:-usb-audio360-builder}" \
    --labels "${RUNNER_LABELS:-xbox360-xdk}" \
    --work _work \
    --unattended \
    --replace
fi

exec ./run.sh
