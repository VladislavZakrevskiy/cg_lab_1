# Source from the project root: source ./env.sh
if [ -z "${VULKAN_SDK:-}" ]; then
    if [ -x "$PWD/.local/vulkan-sdk/macOS/bin/glslc" ]; then
        export VULKAN_SDK="$PWD/.local/vulkan-sdk/macOS"
    elif [ -x "$PWD/../lab-1/.local/vulkan-sdk/macOS/bin/glslc" ]; then
        export VULKAN_SDK="$(cd "$PWD/../lab-1/.local/vulkan-sdk/macOS" && pwd)"
    fi
fi
if [ "$(uname -s)" = Darwin ]; then
    if [ ! -x "${VULKAN_SDK:-}/bin/glslc" ]; then
        printf '%s\n' 'Vulkan SDK not found. Set VULKAN_SDK to its macOS directory.' >&2
        return 1
    fi
    export DYLD_LIBRARY_PATH="$VULKAN_SDK/lib:${DYLD_LIBRARY_PATH:-}"
    export VK_DRIVER_FILES="$VULKAN_SDK/share/vulkan/icd.d/MoltenVK_icd.json"
    export VK_ADD_LAYER_PATH="$VULKAN_SDK/share/vulkan/explicit_layer.d"
fi
if [ -n "${VULKAN_SDK:-}" ]; then
    export PATH="$VULKAN_SDK/bin:$PATH"
fi
