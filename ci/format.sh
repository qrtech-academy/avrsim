#!/usr/bin/env bash
#
# Format (or check the formatting of) all C++ files using clang-format.
#
# Usage:
#   ci/format.sh          Format files in place.
#   ci/format.sh --check  Fail if any file is not already formatted.
set -euo pipefail

# Root directory.
ROOT_DIR="$(dirname "${BASH_SOURCE[0]}")/.."

# The formatter to run. CI pins a version here so that it agrees with the one used locally.
CLANG_FORMAT="${CLANG_FORMAT:-clang-format}"

################################################################################
# Terminate the script if clang-format is not installed.
# Globals:
#   None
# Arguments:
#   None
################################################################################
check_clang_format() {
    if ! command -v "$CLANG_FORMAT" &> /dev/null
    then
        echo "error: $CLANG_FORMAT not found. Install it, e.g. 'sudo apt -y install clang-format'." >&2
        exit 1
    fi
}

################################################################################
# Find the C++ files and store them in the given array.
# Globals:
#   None
# Arguments:
#   $1 - Name of the array variable to populate with file paths.
################################################################################
select_files() {
    local -n out=$1
    mapfile -t out < <(find include source \( -name "*.hpp" -o -name "*.cpp" \) | sort)
}

################################################################################
# Format the given files in place or check their formatting.
# Globals:
#   None
# Arguments:
#   $1 - Run command, e.g. --check. Empty string formats files in place.
#   $2 - Name of the array variable containing files to format.
################################################################################
format_files() {
    local arg="$1"
    local -n files=$2

    # Format selected files.
    if [[ "${arg:-}" == "--check" ]]
    then
        "$CLANG_FORMAT" --dry-run --Werror "${files[@]}"
        echo "Checked ${#files[@]} file(s), all formatted."
    else
        local count=0
        for file in "${files[@]}"
        do
            before=$(md5sum "$file")
            "$CLANG_FORMAT" -i "$file"
            after=$(md5sum "$file")
            if [[ "$before" != "$after" ]]
            then
                echo "Formatted: $file"
                ((++count))
            fi
        done
        echo "Formatted $count of ${#files[@]} file(s)."
    fi
}

# Navigate to the root directory.
cd "$ROOT_DIR"

# Check if clang-format is installed.
check_clang_format

# Select files to format.
select_files FILES

# Format selected files.
format_files "${1:-}" FILES
