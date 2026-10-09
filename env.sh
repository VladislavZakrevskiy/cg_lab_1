# Source from the project root: source ./env.sh
export VULKAN_SDK="${VULKAN_SDK:-$PWD/.local/vulkan-sdk/macOS}"
if [ ! -x "$VULKAN_SDK/bin/glslc" ]; then
    printf '%s\n' 'Vulkan SDK not found. Set VULKAN_SDK to its macOS directory.' >&2
    return 1
fi
export PATH="$VULKAN_SDK/bin:$PATH"
export DYLD_LIBRARY_PATH="$VULKAN_SDK/lib:${DYLD_LIBRARY_PATH:-}"
export VK_DRIVER_FILES="$VULKAN_SDK/share/vulkan/icd.d/MoltenVK_icd.json"
export VK_ADD_LAYER_PATH="$VULKAN_SDK/share/vulkan/explicit_layer.d"
