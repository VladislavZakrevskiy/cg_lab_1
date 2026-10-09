#include <cstdlib>
#include <iostream>
#include <string_view>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_vulkan.h>
#include "graphics_internal.hpp"
#include "application.hpp"

int main(int argc, char** argv) {
    bool smoke_test = false;
    if (argc == 2 && std::string_view(argv[1]) == "--self-test")
        return application::selfTest() ? EXIT_SUCCESS : EXIT_FAILURE;
    if (argc == 2 && std::string_view(argv[1]) == "--smoke-test") smoke_test = true;
    else if (argc != 1) {
        std::cerr << "Usage: cg-lab1 [--self-test | --smoke-test]\n";
        return EXIT_FAILURE;
    }
    int status = EXIT_SUCCESS;
    unsigned int frame = 0;
    double activeTime = 0.0, previousClock = 0.0;
    GLFWwindow* window = nullptr;
    glfwSetErrorCallback([](int code, const char* message) {
        std::cerr << "GLFW error " << code << ": " << message << '\n';
    });
    if (!glfwInit()) return EXIT_FAILURE;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    window = glfwCreateWindow(1280, 720, "CG Lab 1 - Icosahedron (variant 11)", nullptr, nullptr);
    if (!window) { status = EXIT_FAILURE; goto cleanup_glfw; }
    glfwSetFramebufferSizeCallback(window, [](GLFWwindow*, int width, int height) {
        if (width > 0 && height > 0) graphics::internal::resize(width, height);
    });
    glfwSetKeyCallback(window, [](GLFWwindow* w, int key, int, int action, int) {
        if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) glfwSetWindowShouldClose(w, GLFW_TRUE);
    });
    if (!ImGui::CreateContext()) { status = EXIT_FAILURE; goto cleanup_window; }
    ImGui::StyleColorsDark();
    if (!ImGui_ImplGlfw_InitForVulkan(window, true)) { status = EXIT_FAILURE; goto cleanup_imgui; }
    if (!graphics::internal::initialize(window)) { status = EXIT_FAILURE; goto cleanup_graphics; }
    if (!application::initialize()) { status = EXIT_FAILURE; goto cleanup_application; }
    previousClock = glfwGetTime();
    try {
        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();
            if (smoke_test && frame == 40) glfwSetWindowSize(window, 900, 600);
            if (smoke_test && frame == 80) glfwSetWindowSize(window, 1280, 720);
            if (smoke_test && frame == 110) {
                glfwIconifyWindow(window);
                glfwPollEvents();
                glfwWaitEventsTimeout(0.1);
                glfwRestoreWindow(window);
                glfwPollEvents();
            }
            int width = 0, height = 0;
            glfwGetFramebufferSize(window, &width, &height);
            if (width == 0 || height == 0 || glfwGetWindowAttrib(window, GLFW_ICONIFIED)) {
                glfwWaitEventsTimeout(0.1);
                previousClock = glfwGetTime();
                continue;
            }
            const double clock = glfwGetTime();
            activeTime += clock - previousClock;
            previousClock = clock;
            ImGui_ImplVulkan_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
            application::update(smoke_test ? frame / 60.0 : activeTime, smoke_test);
            ImGui::Render();
            const auto fd = graphics::internal::prepare();
            if (!fd.command_buffer) { status = EXIT_FAILURE; break; }
            application::render(fd);
            if (!graphics::internal::submitAndPresent()) { status = EXIT_FAILURE; break; }
            ++frame;
            if (smoke_test && frame == 240) glfwSetWindowShouldClose(window, GLFW_TRUE);
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        status = EXIT_FAILURE;
    }
cleanup_application:
    application::shutdown();
cleanup_graphics:
    graphics::internal::shutdown();
    ImGui_ImplGlfw_Shutdown();
cleanup_imgui:
    ImGui::DestroyContext();
cleanup_window:
    glfwDestroyWindow(window);
cleanup_glfw:
    glfwTerminate();
    if (smoke_test && frame != 240) status = EXIT_FAILURE;
    if (smoke_test && status == EXIT_SUCCESS) std::cout << "PASS: 240 Vulkan frames, scene controls, resize, minimize/restore, cleanup\n";
    return status;
}
