#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Compatibility entry point. The project build is now driven by `make`; this
# helper only builds its local Skia dependency and never configures Awesome.

set -Eeuo pipefail
exec "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/build-skia-vulkan.sh" "$@"
