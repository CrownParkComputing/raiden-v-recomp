#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")"
export TEMPEST_RENDER_RESOLUTION="${TEMPEST_RENDER_RESOLUTION:-1080p}"
exec python3 run.py "$@"
