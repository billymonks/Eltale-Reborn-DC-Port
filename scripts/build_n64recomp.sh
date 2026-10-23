#!/usr/bin/env bash
# Builds the host N64Recomp with this project's patches applied to the pinned submodule.
# usage: build_n64recomp.sh <build dir>
set -euo pipefail

build_dir="$(realpath -m "$1")"
src=tools/N64Recomp

git config --global --add safe.directory '*' 2>/dev/null || true

if [ ! -f "$src/CMakeLists.txt" ] || [ ! -e "$src/lib/rabbitizer/.git" ]; then
    git submodule update --init --recursive tools/N64Recomp
fi

# Each patch is applied on its own, so a tree that already has some of them gets the rest.
for name in gpr32 return-hooks; do
    patch="$(realpath tools/patches/n64recomp-$name.patch)"
    if git -C "$src" apply --reverse --check --ignore-whitespace "$patch" 2>/dev/null; then
        echo "N64Recomp: $name patch already applied"
    else
        git -C "$src" apply --ignore-whitespace "$patch"
        echo "N64Recomp: applied $name patch"
    fi
done

cmake -S "$src" -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_dir" --target N64Recomp
