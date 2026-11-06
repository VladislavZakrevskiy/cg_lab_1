#include "application.hpp"

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

struct Vertex {
    glm::vec3 position, color;
};

struct alignas(16) Uniform {
    glm::mat4 model, view, projection;
    glm::vec4 baseColor;
};

static_assert(offsetof(Uniform, model) == 0);
static_assert(offsetof(Uniform, view) == 64);
static_assert(offsetof(Uniform, projection) == 128);
static_assert(offsetof(Uniform, baseColor) == 192);
static_assert(sizeof(Uniform) == 208);

std::array<Vertex, 12> makeVertices() {
    const float phi = (1.0f + std::sqrt(5.0f)) / 2.0f;

    const std::array<glm::vec3, 12> points = {{
        {-1, phi, 0},
        {1, phi, 0},
        {-1, -phi, 0},
        {1, -phi, 0},
        {0, -1, phi},
        {0, 1, phi},
        {0, -1, -phi},
        {0, 1, -phi},
        {phi, 0, -1},
        {phi, 0, 1},
        {-phi, 0, -1},
        {-phi, 0, 1},
    }};

    std::array<Vertex, 12> result{};

    for (size_t i = 0; i < points.size(); ++i) {
        const auto p = glm::normalize(points[i]);
        result[i] = {p, (p + glm::vec3(1.0f)) * 0.5f};
    }

    return result;
}

const auto vertices = makeVertices();
constexpr std::array<uint16_t, 60> indices = {{
    0, 11, 5,
    0, 5, 1,
    0, 1, 7,
    0, 7, 10,
    0, 10, 11,
    1, 5, 9,
    5, 11, 4,
    11, 10, 2,
    10, 7, 6,
    7, 1, 8,
    3, 9, 4,
    3, 4, 2,
    3, 2, 6,
    3, 6, 8,
    3, 8, 9,
    4, 9, 5,
    2, 4, 11,
    6, 2, 10,
    8, 6, 7,
    9, 8, 1,
}};

struct Scene {
    glm::vec3 position{0.0f}, rotation{0.0f}, scale{1.0f}, color{1.0f};

    bool perspective = true;
    float fov = 45.0f, orthoHeight = 3.0f;

    bool playing = false;
    float speed = 1.0f, radius = 1.5f;
    double phase = 0.0;
    glm::vec3 animationOffset{0.0f};
};

Scene scene;
double previousTime = -1.0;

void advanceAnimation(Scene& s, double dt) {
    if (!s.playing)
        return;

    s.phase += dt * s.speed;
    const float t = static_cast<float>(s.phase);
    s.animationOffset = s.radius * glm::vec3(std::cos(t), 0.5f * std::sin(2.0f * t), std::sin(t));
}

glm::mat4 modelMatrix(const Scene& s) {
    auto model = glm::translate(glm::mat4(1.0f), s.position + s.animationOffset);
    model = glm::rotate(model, glm::radians(s.rotation.z), glm::vec3(0, 0, 1));
    model = glm::rotate(model, glm::radians(s.rotation.y) + static_cast<float>(s.phase),
                        glm::vec3(0, 1, 0));
    model = glm::rotate(model, glm::radians(s.rotation.x), glm::vec3(1, 0, 0));

    return glm::scale(model, s.scale);
}

glm::mat4 projectionMatrix(const Scene& s, float aspect) {
    auto projection = s.perspective ? glm::perspective(glm::radians(s.fov), aspect, 0.1f, 100.0f)
                                    : glm::ortho(-s.orthoHeight * aspect, s.orthoHeight * aspect,
                                                 -s.orthoHeight, s.orthoHeight, 0.1f, 100.0f);
    projection[1][1] *= -1.0f;
    return projection;
}

struct Buffer {
    VkBuffer handle = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    void* mapped = nullptr;
};

Buffer vertexBuffer, indexBuffer, uniformBuffer;
VkDescriptorSetLayout descriptorLayout = VK_NULL_HANDLE;
VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
VkPipeline pipeline = VK_NULL_HANDLE;

bool check(VkResult result, const char* operation) {
    if (result == VK_SUCCESS)
        return true;

    std::cerr << operation << " failed: VkResult " << result << '\n';
    return false;
}

bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, Buffer& buffer) {
    const VkBufferCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };

    const VmaAllocationCreateInfo allocation = {
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                 VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO,
    };

    VmaAllocationInfo result{};
    if (!check(vmaCreateBuffer(graphics::internal::context.allocator, &info, &allocation,
                               &buffer.handle, &buffer.allocation, &result),
               "vmaCreateBuffer"))
        return false;

    buffer.mapped = result.pMappedData;
    return buffer.mapped != nullptr;
}

bool upload(const Buffer& buffer, const void* data, size_t size) {
    std::memcpy(buffer.mapped, data, size);
    return check(

        vmaFlushAllocation(graphics::internal::context.allocator, buffer.allocation, 0, size),
        "vmaFlushAllocation");
}

VkShaderModule loadShader(const char* filename) {
    const std::string path = std::string(LAB1_SHADER_DIR) + '/' + filename;
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
    const auto vert = loadShader("icosahedron.vert.spv"), frag = loadShader("icosahedron.frag.spv");
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
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, color)},
    };

    const VkPipelineVertexInputStateCreateInfo vertexInput = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &binding,
        .vertexAttributeDescriptionCount = 2,
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

} // namespace

namespace application {

bool initialize() {
    const auto& ctx = graphics::internal::context;

    const VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                                               VK_SHADER_STAGE_VERTEX_BIT, nullptr};

    const VkDescriptorSetLayoutCreateInfo layoutInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &binding,
    };

    const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1};

    const VkDescriptorPoolCreateInfo poolInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &poolSize,
    };

    if (!createBuffer(sizeof(vertices), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertexBuffer) ||
        !createBuffer(sizeof(indices), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, indexBuffer) ||
        !createBuffer(sizeof(Uniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, uniformBuffer) ||
        !upload(vertexBuffer, vertices.data(), sizeof(vertices)) ||
        !upload(indexBuffer, indices.data(), sizeof(indices)) ||
        !check(vkCreateDescriptorSetLayout(ctx.device, &layoutInfo, nullptr, &descriptorLayout),
               "vkCreateDescriptorSetLayout") ||
        !check(vkCreateDescriptorPool(ctx.device, &poolInfo, nullptr, &descriptorPool),
               "vkCreateDescriptorPool"))
        return false;

    const VkDescriptorSetAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptorPool,
        .descriptorSetCount = 1,
        .pSetLayouts = &descriptorLayout,
    };

    if (!check(vkAllocateDescriptorSets(ctx.device, &allocation, &descriptorSet),
               "vkAllocateDescriptorSets"))
        return false;

    const VkDescriptorBufferInfo bufferInfo{uniformBuffer.handle, 0, sizeof(Uniform)};

    const VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = descriptorSet,
        .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .pBufferInfo = &bufferInfo,
    };

    vkUpdateDescriptorSets(ctx.device, 1, &write, 0, nullptr);

    const VkPipelineLayoutCreateInfo pipelineInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &descriptorLayout,
    };

    return check(vkCreatePipelineLayout(ctx.device, &pipelineInfo, nullptr, &pipelineLayout),

                 "vkCreatePipelineLayout") &&
           createPipeline();
}

void shutdown() {
    const auto& ctx = graphics::internal::context;
    vkQueueWaitIdle(ctx.graphics_queue);
    vkDestroyPipeline(ctx.device, pipeline, nullptr);
    vkDestroyPipelineLayout(ctx.device, pipelineLayout, nullptr);
    vkDestroyDescriptorPool(ctx.device, descriptorPool, nullptr);
    vkDestroyDescriptorSetLayout(ctx.device, descriptorLayout, nullptr);

    for (auto* buffer : {&uniformBuffer, &indexBuffer, &vertexBuffer}) {
        if (buffer->handle)
            vmaDestroyBuffer(ctx.allocator, buffer->handle, buffer->allocation);
        *buffer = {};
    }
}

void update(double time) {
    const double dt = previousTime < 0.0 ? 0.0 : std::max(0.0, time - previousTime);
    previousTime = time;

    ImGui::SetNextWindowPos(ImVec2(15, 15), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(340, 0), ImGuiCond_FirstUseEver);
    ImGui::Begin("Lab 1 - Icosahedron (variant 11)", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::TextUnformatted("12 vertices / 20 triangles / 30 edges");

    if (ImGui::CollapsingHeader("Projection", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Perspective", &scene.perspective);
        if (scene.perspective)
            ImGui::SliderFloat("FOV (degrees)", &scene.fov, 20.0f, 90.0f);
        else
            ImGui::SliderFloat("Ortho half-height", &scene.orthoHeight, 1.0f, 8.0f);
    }

    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::DragFloat3("Position", &scene.position.x, 0.05f, -10.0f, 10.0f, "%.2f",
                          ImGuiSliderFlags_AlwaysClamp);
        ImGui::SliderFloat3("Rotation (degrees)", &scene.rotation.x, -180.0f, 180.0f);
        ImGui::DragFloat3("Scale", &scene.scale.x, 0.02f, 0.05f, 5.0f, "%.2f",
                          ImGuiSliderFlags_AlwaysClamp);
    }

    if (ImGui::CollapsingHeader("Animation", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Button(scene.playing ? "Pause" : "Play"))
            scene.playing = !scene.playing;
        ImGui::SliderFloat("Speed", &scene.speed, 0.0f, 4.0f);
        ImGui::SliderFloat("Path radius", &scene.radius, 0.0f, 3.0f);
        ImGui::Text("Phase: %.2f rad", scene.phase);
    }

    ImGui::ColorEdit3("Base color", &scene.color.x);
    if (ImGui::Button("Reset all"))
        scene = {};
    ImGui::TextUnformatted("Vertex color * base color; Escape to exit.");
    ImGui::End();

    advanceAnimation(scene, dt);
}

void render(const graphics::internal::FrameData& fd) {
    const auto& ctx = graphics::internal::context;
    const auto extent = ctx.swapchain_extent;

    const Uniform uniform = {
        modelMatrix(scene),
        glm::lookAt(glm::vec3(4, 3, 6), glm::vec3(0), glm::vec3(0, 1, 0)),
        projectionMatrix(scene,
                         static_cast<float>(extent.width) / static_cast<float>(extent.height)),
        glm::vec4(scene.color, 1.0f),
    };

    if (!upload(uniformBuffer, &uniform, sizeof(uniform)))
        throw std::runtime_error("Uniform upload failed");

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
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1,
                            &descriptorSet, 0, nullptr);

    vkCmdDrawIndexed(cmd, static_cast<uint32_t>(indices.size()), 1, 0, 0, 0);

    vkCmdEndRenderPass(cmd);
    if (!check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer"))
        throw std::runtime_error("Command buffer end failed");
}

} // namespace application
