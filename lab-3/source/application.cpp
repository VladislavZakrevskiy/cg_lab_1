#include "application.hpp"
#include "scene.hpp"
#define STB_IMAGE_IMPLEMENTATION
#include "../third_party/stb_image.h"

#include <imgui.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

using namespace lab;

const auto vertices = makeVertices();
Scene scene;
std::array<Model, 2> models;
std::array<int, 2> materials{3, 1};
std::array<bool, 2> effects{false, true};
int selectedModel = 0;
float phase = 0, speed = 1;
bool playing = true;
double lastTime = 0;
VkDeviceSize modelStride = 0;
struct Push { float time; int32_t enabled; };
static_assert(sizeof(Push) == 8 && offsetof(Push, enabled) == 4);
void resetScene() {
    scene = {};
    scene.camera.position = {0, 0, 8};
    models = {};
    models[0].position = {-1.3f, 0, 0};
    models[1].position = {1.3f, 0, 0};
    for (auto& model : models) model.albedo = glm::vec3(1);
    materials = {3, 1}; effects = {false, true};
    phase = 0; speed = 1; playing = true;
}
size_t maxPoints = 0, maxSpots = 0;

struct Buffer {
    VkBuffer handle = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize size = 0;
};

Buffer indexBuffer;
Buffer vertexBuffer, sceneBuffer, modelBuffer, pointBuffer, spotBuffer;
std::array<VkDescriptorSetLayout, 2> descriptorLayouts{};
VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
std::array<VkDescriptorSet, 7> descriptorSets{};
VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
VkPipeline pipeline = VK_NULL_HANDLE;

bool check(VkResult result, const char* operation) {
    if (result == VK_SUCCESS)
        return true;

    std::cerr << operation << " failed: VkResult " << result << '\n';
    return false;
}

bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, Buffer& buffer, bool deviceLocal = false) {
    const VkBufferCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };

    const VmaAllocationCreateInfo allocation = {
        .flags = deviceLocal ? 0u : VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO,
        .requiredFlags = deviceLocal ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT : 0u,
    };

    VmaAllocationInfo result{};
    if (!check(vmaCreateBuffer(graphics::internal::context.allocator, &info, &allocation,
                               &buffer.handle, &buffer.allocation, &result),
               "vmaCreateBuffer"))
        return false;

    buffer.size = size;
    buffer.mapped = result.pMappedData;
    return deviceLocal || buffer.mapped != nullptr;
}

bool upload(const Buffer& buffer, const void* data, size_t size) {
    if (size == 0)
        return true;
    if (!buffer.mapped || size > buffer.size)
        return false;

    std::memcpy(buffer.mapped, data, size);
    return check(

        vmaFlushAllocation(graphics::internal::context.allocator, buffer.allocation, 0, size),
        "vmaFlushAllocation");
}

VkShaderModule loadShader(const char* filename) {
    const std::string path = std::string(LAB3_SHADER_DIR) + '/' + filename;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    const auto size = file ? file.tellg() : std::streampos(-1);
    if (size <= 0 || static_cast<size_t>(size) % sizeof(uint32_t) != 0) {
        std::cerr << "Cannot read SPIR-V shader: " << path << '\n';
        return VK_NULL_HANDLE;
    }

    std::vector<uint32_t> code(static_cast<size_t>(size) / sizeof(uint32_t));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(code.data()), static_cast<std::streamsize>(size)) ||
        code.front() != 0x07230203) {
        std::cerr << "Invalid SPIR-V shader: " << path << '\n';
        return VK_NULL_HANDLE;
    }

    const VkShaderModuleCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = static_cast<size_t>(size),
        .pCode = code.data(),
    };

    VkShaderModule module = VK_NULL_HANDLE;
    return check(vkCreateShaderModule(graphics::internal::context.device, &info, nullptr, &module),

                 "vkCreateShaderModule")
               ? module
               : VK_NULL_HANDLE;
}

bool createPipeline() {
    const auto& ctx = graphics::internal::context;
    const auto vert = loadShader("lighting.vert.spv"), frag = loadShader("lighting.frag.spv");
    if (!vert || !frag) {
        vkDestroyShaderModule(ctx.device, vert, nullptr);
        vkDestroyShaderModule(ctx.device, frag, nullptr);
        return false;
    }

    const VkPipelineShaderStageCreateInfo stages[] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vert,
         .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = frag,
         .pName = "main"},
    };

    const VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};

    const VkVertexInputAttributeDescription attributes[] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)},
    };

    const VkPipelineVertexInputStateCreateInfo vertexInput = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &binding,
        .vertexAttributeDescriptionCount = 3,
        .pVertexAttributeDescriptions = attributes,
    };

    const VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };

    const VkPipelineViewportStateCreateInfo viewport = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };

    const VkPipelineRasterizationStateCreateInfo rasterization = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0f,
    };

    const VkPipelineMultisampleStateCreateInfo samples = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };

    const VkPipelineDepthStencilStateCreateInfo depth = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE,
        .depthWriteEnable = VK_TRUE,
        .depthCompareOp = VK_COMPARE_OP_LESS,
    };

    const VkPipelineColorBlendAttachmentState attachment = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };

    const VkPipelineColorBlendStateCreateInfo blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &attachment,
    };

    const VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};

    const VkPipelineDynamicStateCreateInfo dynamic = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates = states,
    };

    const VkGraphicsPipelineCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &vertexInput,
        .pInputAssemblyState = &assembly,
        .pViewportState = &viewport,
        .pRasterizationState = &rasterization,
        .pMultisampleState = &samples,
        .pDepthStencilState = &depth,
        .pColorBlendState = &blend,
        .pDynamicState = &dynamic,
        .layout = pipelineLayout,
        .renderPass = ctx.render_pass,
    };

    const bool success =
        check(vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline),
              "vkCreateGraphicsPipelines");
    vkDestroyShaderModule(ctx.device, frag, nullptr);
    vkDestroyShaderModule(ctx.device, vert, nullptr);
    return success;
}

void destroyBuffer(Buffer& buffer) {
    if (buffer.handle)
        vmaDestroyBuffer(graphics::internal::context.allocator, buffer.handle, buffer.allocation);
    buffer = {};
}

void writeDescriptor(const Buffer& buffer, unsigned set, unsigned binding, VkDescriptorType type) {
    const VkDescriptorBufferInfo info{buffer.handle, 0, type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ? sizeof(ModelUniform) : buffer.size};
    const VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = descriptorSets[set], .dstBinding = binding,
        .descriptorCount = 1, .descriptorType = type, .pBufferInfo = &info,
    };
    vkUpdateDescriptorSets(graphics::internal::context.device, 1, &write, 0, nullptr);
}

struct Texture {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};
std::array<Texture, 6> textures;
VkSampler sampler = VK_NULL_HANDLE;
const char* textureNames[] = {"checker.png", "bricks.png", "mosaic.png", "baby_cute.jpg", "tuntunsahur.jpg", "you_see_this_man.jpg"};

bool loadResources() {
    const auto& ctx = graphics::internal::context;
    VkCommandPool pool = VK_NULL_HANDLE;
    const VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr,
        VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, ctx.graphics_queue_index};
    if (!check(vkCreateCommandPool(ctx.device, &poolInfo, nullptr, &pool), "Upload pool")) return false;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    const VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        nullptr, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    std::vector<Buffer> staging;
    auto finish = [&](bool ok) {
        if (ctx.graphics_queue) vkQueueWaitIdle(ctx.graphics_queue);
        for (auto& b : staging) destroyBuffer(b);
        vkDestroyCommandPool(ctx.device, pool, nullptr);
        return ok;
    };
    if (!check(vkAllocateCommandBuffers(ctx.device, &alloc, &cmd), "Upload commands")) return finish(false);
    const VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
        VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
    if (!check(vkBeginCommandBuffer(cmd, &begin), "Begin upload")) return finish(false);
    auto stage = [&](const void* data, size_t size) -> VkBuffer {
        staging.emplace_back();
        if (!createBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging.back()) ||
            !upload(staging.back(), data, size)) return VK_NULL_HANDLE;
        return staging.back().handle;
    };
    std::array<uint16_t, 60> indices{};
    for (size_t i = 0; i < indices.size(); ++i) indices[i] = static_cast<uint16_t>(i);
    auto geometry = [&](const void* data, size_t size, VkBufferUsageFlags usage, Buffer& target) {
        const auto source = stage(data, size);
        if (!source || !createBuffer(size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, target, true)) return false;
        const VkBufferCopy copy{0, 0, size};
        vkCmdCopyBuffer(cmd, source, target.handle, 1, &copy);
        return true;
    };
    if (!geometry(vertices.data(), sizeof(vertices), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertexBuffer) ||
        !geometry(indices.data(), sizeof(indices), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, indexBuffer)) return finish(false);
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(ctx.physical_device, &properties);
    for (size_t i = 0; i < textures.size(); ++i) {
        const auto path = std::string(LAB3_TEXTURE_DIR) + "/" + textureNames[i];
        int width = 0, height = 0, channels = 0;
        auto* pixels = stbi_load(path.c_str(), &width, &height, &channels, 4);
        if (!pixels || width <= 0 || height <= 0 ||
            static_cast<unsigned>(width) > properties.limits.maxImageDimension2D ||
            static_cast<unsigned>(height) > properties.limits.maxImageDimension2D) {
            std::cerr << "Cannot load texture: " << path << "\n";
            stbi_image_free(pixels); return finish(false);
        }
        const auto source = stage(pixels, size_t(width) * size_t(height) * 4);
        stbi_image_free(pixels);
        if (!source) return finish(false);
        auto& texture = textures[i];
        const VkImageCreateInfo imageInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_SRGB,
            .extent = {uint32_t(width), uint32_t(height), 1}, .mipLevels = 1, .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        const VmaAllocationCreateInfo memory{.usage = VMA_MEMORY_USAGE_AUTO,
            .requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT};
        if (!check(vmaCreateImage(ctx.allocator, &imageInfo, &memory, &texture.image,
                                 &texture.allocation, nullptr), "Texture image")) return finish(false);
        VkImageMemoryBarrier barrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = texture.image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
        const VkBufferImageCopy copy{.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                    .imageExtent = {uint32_t(width), uint32_t(height), 1}};
        vkCmdCopyBufferToImage(cmd, source, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
        const VkImageViewCreateInfo view{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = texture.image, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_SRGB,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        if (!check(vkCreateImageView(ctx.device, &view, nullptr, &texture.view), "Texture view")) return finish(false);
    }
    const VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
                         0, 1, &barrier, 0, nullptr, 0, nullptr);
    if (!check(vkEndCommandBuffer(cmd), "End upload")) return finish(false);
    const VkSubmitInfo submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &cmd};
    if (!check(vkQueueSubmit(ctx.graphics_queue, 1, &submit, VK_NULL_HANDLE), "Submit upload")) return finish(false);
    if (!check(vkQueueWaitIdle(ctx.graphics_queue), "Wait upload")) return finish(false);
    const VkSamplerCreateInfo samplerInfo{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT, .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT};
    return finish(check(vkCreateSampler(ctx.device, &samplerInfo, nullptr, &sampler), "Sampler"));
}

bool ensureStorage(Buffer& buffer, VkDeviceSize required, unsigned binding) {
    if (required <= buffer.size)
        return true;

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(graphics::internal::context.physical_device, &properties);
    if (required > properties.limits.maxStorageBufferRange) {
        std::cerr << "Storage buffer exceeds device limit\n";
        return false;
    }
    const auto capacity = std::min<VkDeviceSize>(properties.limits.maxStorageBufferRange,
                                                std::max(required, buffer.size * 2));
    Buffer replacement;
    if (!createBuffer(capacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, replacement)) {
        destroyBuffer(replacement);
        return false;
    }
    destroyBuffer(buffer);
    buffer = replacement;
    writeDescriptor(buffer, 0, binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    return true;
}

void editLight(PointLight& light) {
    ImGui::DragFloat3("Position", &light.position.x, 0.05f, -30, 30, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::ColorEdit3("Color", &light.colorIntensity.x);
    ImGui::DragFloat("Intensity", &light.colorIntensity.w, 0.1f, 0, 100, "%.2f", ImGuiSliderFlags_AlwaysClamp);
}

} // namespace

namespace application {

bool initialize() {
    resetScene();
    const auto& ctx = graphics::internal::context;
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(ctx.physical_device, &properties);
    maxPoints = properties.limits.maxStorageBufferRange / sizeof(PointLight);
    maxSpots = properties.limits.maxStorageBufferRange / sizeof(SpotUniform);

    const VkShaderStageFlags both = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    const VkDescriptorSetLayoutBinding sceneBindings[] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, both, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
    };
    const VkDescriptorSetLayoutBinding modelBindings[] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, both, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    const auto alignment = properties.limits.minUniformBufferOffsetAlignment;
    modelStride = modelUniformStride(sizeof(ModelUniform), alignment);
    const VkDescriptorSetLayoutCreateInfo layouts[] = {
        {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
         .bindingCount = 3, .pBindings = sceneBindings},
        {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
         .bindingCount = 2, .pBindings = modelBindings},
    };
    for (size_t i = 0; i < descriptorLayouts.size(); ++i) {
        if (!check(vkCreateDescriptorSetLayout(ctx.device, &layouts[i], nullptr, &descriptorLayouts[i]),
                   "vkCreateDescriptorSetLayout"))
            return false;
    }

    const VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 6}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 6},
    };
    const VkDescriptorPoolCreateInfo poolInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 7, .poolSizeCount = 4, .pPoolSizes = sizes,
    };
    if (!check(vkCreateDescriptorPool(ctx.device, &poolInfo, nullptr, &descriptorPool), "vkCreateDescriptorPool"))
        return false;

    std::array<VkDescriptorSetLayout, 7> materialLayouts;
    materialLayouts.fill(descriptorLayouts[1]);
    materialLayouts[0] = descriptorLayouts[0];
    const VkDescriptorSetAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptorPool,
        .descriptorSetCount = 7, .pSetLayouts = materialLayouts.data(),
    };
    if (!check(vkAllocateDescriptorSets(ctx.device, &allocation, descriptorSets.data()), "vkAllocateDescriptorSets") ||
        !loadResources() ||
        !createBuffer(sizeof(SceneUniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, sceneBuffer) ||
        !createBuffer(modelStride * models.size(), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, modelBuffer) ||
        !ensureStorage(pointBuffer, sizeof(PointLight), 1) ||
        !ensureStorage(spotBuffer, sizeof(SpotUniform), 2))
        return false;

    writeDescriptor(sceneBuffer, 0, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    for (unsigned i = 0; i < textures.size(); ++i) {
        writeDescriptor(modelBuffer, i + 1, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC);
        const VkDescriptorImageInfo image{sampler, textures[i].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = descriptorSets[i + 1], .dstBinding = 1, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &image};
        vkUpdateDescriptorSets(ctx.device, 1, &write, 0, nullptr);
    }
    const VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};

    const VkPipelineLayoutCreateInfo pipelineInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 2, .pSetLayouts = descriptorLayouts.data(),
        .pushConstantRangeCount = 1, .pPushConstantRanges = &push,
    };
    return check(vkCreatePipelineLayout(ctx.device, &pipelineInfo, nullptr, &pipelineLayout), "vkCreatePipelineLayout") &&
           createPipeline();
}

void shutdown() {
    const auto& ctx = graphics::internal::context;
    vkQueueWaitIdle(ctx.graphics_queue);
    vkDestroyPipeline(ctx.device, pipeline, nullptr);
    vkDestroyPipelineLayout(ctx.device, pipelineLayout, nullptr);
    vkDestroyDescriptorPool(ctx.device, descriptorPool, nullptr);
    for (const auto layout : descriptorLayouts)
        vkDestroyDescriptorSetLayout(ctx.device, layout, nullptr);
    vkDestroySampler(ctx.device, sampler, nullptr);
    for (auto& texture : textures) {
        vkDestroyImageView(ctx.device, texture.view, nullptr);
        if (texture.image) vmaDestroyImage(ctx.allocator, texture.image, texture.allocation);
    }
    for (auto* buffer : {&indexBuffer, &vertexBuffer, &sceneBuffer, &modelBuffer, &pointBuffer, &spotBuffer})
        destroyBuffer(*buffer);
}

void update(double time) {
    if (playing) phase += float(time - lastTime) * speed;
    lastTime = time;
    ImGui::SetNextWindowPos(ImVec2(15, 15), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(390, 670), ImGuiCond_FirstUseEver);
    ImGui::Begin("Lab 3 - Texture Mapping");
    ImGui::TextUnformatted("Variant 11: icosahedron / grade 4");
    ImGui::Text("Point lights: %zu | Spotlights: %zu", scene.points.size(), scene.spots.size());
    if (ImGui::Button("Reset all"))
        resetScene();
    ImGui::SameLine();
    ImGui::TextUnformatted("Escape: exit");

    if (ImGui::BeginTabBar("Controls")) {
        if (ImGui::BeginTabItem("Scene")) {
            if (ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
                auto& camera = scene.camera;
                ImGui::DragFloat3("Camera position", &camera.position.x, 0.05f, -30, 30, "%.2f", ImGuiSliderFlags_AlwaysClamp);
                ImGui::SliderFloat("Yaw", &camera.yaw, -180, 180, "%.1f", ImGuiSliderFlags_AlwaysClamp);
                ImGui::SliderFloat("Pitch", &camera.pitch, -89, 89, "%.1f", ImGuiSliderFlags_AlwaysClamp);
                ImGui::SliderFloat("Roll", &camera.roll, -180, 180, "%.1f", ImGuiSliderFlags_AlwaysClamp);
                ImGui::SliderFloat("FOV", &camera.fov, 20, 100, "%.1f", ImGuiSliderFlags_AlwaysClamp);
                if (ImGui::Button("Reset camera"))
                    camera = {};
            }
            if (ImGui::CollapsingHeader("Model and material", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Combo("Object", &selectedModel, "Icosahedron 1\0Icosahedron 2\0");
                ImGui::Combo("Texture", &materials[selectedModel], textureNames, 6);
                ImGui::Checkbox("Animated sampling", &effects[selectedModel]);
                ImGui::Checkbox("Play", &playing);
                ImGui::SliderFloat("Speed", &speed, 0, 3);
                auto& model = models[selectedModel];
                ImGui::DragFloat3("Model position", &model.position.x, 0.05f, -10, 10, "%.2f", ImGuiSliderFlags_AlwaysClamp);
                ImGui::SliderFloat3("Rotation", &model.rotation.x, -180, 180, "%.1f", ImGuiSliderFlags_AlwaysClamp);
                ImGui::DragFloat3("Scale", &model.scale.x, 0.02f, 0.05f, 5, "%.2f", ImGuiSliderFlags_AlwaysClamp);
                ImGui::ColorEdit3("Albedo", &model.albedo.x);
                ImGui::ColorEdit3("Specular color", &model.specular.x);
                ImGui::SliderFloat("Shininess", &model.shininess, 1, 256, "%.1f", ImGuiSliderFlags_AlwaysClamp);
            }
            if (ImGui::CollapsingHeader("Ambient and directional", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::ColorEdit3("Ambient color", &scene.ambientColor.x);
                ImGui::SliderFloat("Ambient intensity", &scene.ambientIntensity, 0, 1, "%.2f", ImGuiSliderFlags_AlwaysClamp);
                ImGui::ColorEdit3("Directional color", &scene.directionalColor.x);
                ImGui::SliderFloat("Directional intensity", &scene.directionalIntensity, 0, 5, "%.2f", ImGuiSliderFlags_AlwaysClamp);
                ImGui::SliderFloat("Light yaw", &scene.lightYaw, -180, 180, "%.1f", ImGuiSliderFlags_AlwaysClamp);
                ImGui::SliderFloat("Light pitch", &scene.lightPitch, -90, 90, "%.1f", ImGuiSliderFlags_AlwaysClamp);
                ImGui::TextWrapped("Direction is where rays travel. A directional light has no position.");
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Points")) {
            ImGui::TextWrapped("Inverse-square falloff. Intensity 0 disables a light.");
            ImGui::BeginDisabled(scene.points.size() >= maxPoints);
            if (ImGui::Button("Add point light"))
                scene.points.emplace_back();
            ImGui::EndDisabled();
            for (size_t i = 0; i < scene.points.size();) {
                ImGui::PushID(static_cast<int>(i));
                const bool open = ImGui::TreeNodeEx("Point", ImGuiTreeNodeFlags_DefaultOpen, "Point %zu", i + 1);
                bool remove = false;
                if (open) {
                    editLight(scene.points[i]);
                    remove = ImGui::Button("Remove point");
                    ImGui::TreePop();
                }
                ImGui::PopID();
                if (remove)
                    scene.points.erase(scene.points.begin() + static_cast<std::ptrdiff_t>(i));
                else
                    ++i;
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Spots")) {
            ImGui::TextWrapped("Angles are half-angles in degrees. Equal angles = hard edge.");
            ImGui::BeginDisabled(scene.spots.size() >= maxSpots);
            if (ImGui::Button("Add spotlight"))
                scene.spots.emplace_back();
            ImGui::EndDisabled();
            for (size_t i = 0; i < scene.spots.size();) {
                ImGui::PushID(static_cast<int>(i));
                const bool open = ImGui::TreeNodeEx("Spot", ImGuiTreeNodeFlags_DefaultOpen, "Spot %zu", i + 1);
                bool remove = false;
                if (open) {
                    auto& spot = scene.spots[i];
                    editLight(spot.light);
                    ImGui::SliderFloat("Direction yaw", &spot.yaw, -180, 180, "%.1f", ImGuiSliderFlags_AlwaysClamp);
                    ImGui::SliderFloat("Direction pitch", &spot.pitch, -90, 90, "%.1f", ImGuiSliderFlags_AlwaysClamp);
                    ImGui::SliderFloat("Outer angle", &spot.outerAngle, 1, 89, "%.1f", ImGuiSliderFlags_AlwaysClamp);
                    spot.innerAngle = std::min(spot.innerAngle, spot.outerAngle);
                    ImGui::SliderFloat("Inner angle", &spot.innerAngle, 0, spot.outerAngle, "%.1f", ImGuiSliderFlags_AlwaysClamp);
                    remove = ImGui::Button("Remove spot");
                    ImGui::TreePop();
                }
                ImGui::PopID();
                if (remove)
                    scene.spots.erase(scene.spots.begin() + static_cast<std::ptrdiff_t>(i));
                else
                    ++i;
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void render(const graphics::internal::FrameData& fd) {
    const auto& ctx = graphics::internal::context;
    const auto extent = ctx.swapchain_extent;

    const SceneUniform sceneUniform = {
        projectionMatrix(scene.camera.fov, static_cast<float>(extent.width) / extent.height) * viewMatrix(scene.camera),
        glm::vec4(scene.camera.position, 1),
        glm::vec4(scene.ambientColor, scene.ambientIntensity),
        glm::vec4(direction(scene.lightYaw, scene.lightPitch), 0),
        glm::vec4(scene.directionalColor, scene.directionalIntensity),
        glm::ivec4(static_cast<int>(scene.points.size()), static_cast<int>(scene.spots.size()), 0, 0),
    };
    for (size_t i = 0; i < models.size(); ++i) {
        const auto& model = models[i];
        const auto matrix = modelMatrix(model);
        const ModelUniform uniform{matrix, glm::transpose(glm::inverse(matrix)),
            glm::vec4(model.albedo, 1), glm::vec4(model.specular, model.shininess)};
        std::memcpy(static_cast<char*>(modelBuffer.mapped) + i * modelStride, &uniform, sizeof(uniform));
    }
    if (!check(vmaFlushAllocation(ctx.allocator, modelBuffer.allocation, 0, VK_WHOLE_SIZE), "Model upload"))
        throw std::runtime_error("Model upload failed");
    std::vector<SpotUniform> spots;
    spots.reserve(scene.spots.size());
    for (const auto& spot : scene.spots)
        spots.push_back(spotUniform(spot));

    // prepare() has waited for the sole in-flight frame before any buffer or descriptor changes.
    if (!ensureStorage(pointBuffer, std::max(size_t(1), scene.points.size()) * sizeof(PointLight), 1) ||
        !ensureStorage(spotBuffer, std::max(size_t(1), spots.size()) * sizeof(SpotUniform), 2) ||
        !upload(sceneBuffer, &sceneUniform, sizeof(sceneUniform)) ||
        !upload(pointBuffer, scene.points.data(), scene.points.size() * sizeof(PointLight)) ||
        !upload(spotBuffer, spots.data(), spots.size() * sizeof(SpotUniform)))
        throw std::runtime_error("Scene buffer upload failed");

    const auto cmd = fd.command_buffer;
    if (!check(vkResetCommandBuffer(cmd, 0), "vkResetCommandBuffer"))
        throw std::runtime_error("Command buffer reset failed");

    const VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };

    if (!check(vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer"))
        throw std::runtime_error("Command buffer begin failed");

    const VkClearValue clears[] = {{{{0.06f, 0.08f, 0.12f, 1.0f}}}, {.depthStencil = {1.0f, 0}}};

    const VkRenderPassBeginInfo pass = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = ctx.render_pass,
        .framebuffer = fd.framebuffer,
        .renderArea = {.extent = extent},
        .clearValueCount = 2,
        .pClearValues = clears,
    };

    vkCmdBeginRenderPass(cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);

    const VkViewport viewport{
        0, 0, static_cast<float>(extent.width), static_cast<float>(extent.height), 0, 1};

    const VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer.handle, &offset);
    vkCmdBindIndexBuffer(cmd, indexBuffer.handle, 0, VK_INDEX_TYPE_UINT16);
    for (size_t i = 0; i < models.size(); ++i) {
        const VkDescriptorSet sets[] = {descriptorSets[0], descriptorSets[materials[i] + 1]};
        const uint32_t dynamicOffset = static_cast<uint32_t>(i * modelStride);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 2,
                                sets, 1, &dynamicOffset);
        const Push push{phase, effects[i] ? 1 : 0};
        vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
        vkCmdDrawIndexed(cmd, 60, 1, 0, 0, 0);
    }

    vkCmdEndRenderPass(cmd);
    if (!check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer"))
        throw std::runtime_error("Command buffer end failed");
}

} // namespace application
