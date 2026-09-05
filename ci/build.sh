#!/usr/bin/env bash
#
# Build the library and the tool from scratch. The compile is -Werror, so a warning fails this.
#
# Usage:
#   ci/build.sh
set -euo pipefail

# Root directory.
ROOT_DIR="$(dirname "${BASH_SOURCE[0]}")/.."

# Navigate to the root directory.
cd "$ROOT_DIR"

echo "Building libavrsim.a and avrsim"
make --no-print-directory clean
make --no-print-directory
echo "Build complete."
