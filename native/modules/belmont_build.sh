#!/usr/bin/env bash
set -euo pipefail
# Linux development build. No shared CMake or host changes.
sdk_root="${1:?SDK root required}"
output_dir="${2:?Output directory required}"
source_dir="$(cd -- "$(dirname -- "$0")" && pwd)"
mkdir -p -- "$output_dir"
cat > "$output_dir/belmont.exports" <<'EXPORTS'
{ global: eden_dsmod_get_module; eden_dsmod_get_extensions; eden_dsmod_get_font_extensions; local: *; };
EXPORTS
compiler="${CXX:-c++}"
includes=(-I"$sdk_root/src" -I"$sdk_root/externals/bc_decoder")
common=(-std=c++20 -O2 -Wall -Wextra -Werror -pthread "${includes[@]}")
"$compiler" -std=c++20 -O2 -fPIC -fvisibility=hidden -c \
  "$sdk_root/externals/bc_decoder/bc_decoder.cpp" -o "$output_dir/belmont-bc.o"
sources=("$source_dir/0100BA4026E94000.cpp" "$source_dir/belmont_assets.cpp" \
  "$source_dir/belmont_map_assets.cpp" "$source_dir/chained_echoes_unity.cpp" "$output_dir/belmont-bc.o")
"$compiler" "${common[@]}" -fPIC -fvisibility=hidden -shared "${sources[@]}" \
  -llz4 -Wl,--no-undefined -Wl,--version-script="$output_dir/belmont.exports" \
  -o "$output_dir/0100BA4026E94000.so"
"$compiler" "${common[@]}" "$source_dir/belmont_reader_test.cpp" "${sources[@]}" \
  -llz4 -o "$output_dir/belmont_reader_test"
"$output_dir/belmont_reader_test"
