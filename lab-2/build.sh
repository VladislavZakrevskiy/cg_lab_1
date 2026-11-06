#!/bin/sh
set -eu
cd -- "$(dirname -- "$0")"
. ./env.sh

# Reuse downloaded sources, not the first lab's build cache or binaries.
set --
for dep in glfw glm imgui vk-bootstrap vulkanmemoryallocator; do
    source="$PWD/../lab-1/build-debug/_deps/$dep-src"
    if [ -f "$source/CMakeLists.txt" ] || [ -f "$source/imgui.cpp" ]; then
        name=$(printf '%s' "$dep" | tr '[:lower:]' '[:upper:]')
        set -- "$@" "-DFETCHCONTENT_SOURCE_DIR_$name=$source"
    fi
done
cmake -S "$PWD" -B "$PWD/build-debug" -DCMAKE_BUILD_TYPE=Debug "$@"
cmake --build "$PWD/build-debug" --parallel 8
