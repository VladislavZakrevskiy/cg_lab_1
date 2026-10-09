#pragma once

#include "graphics_internal.hpp"

namespace application {

bool initialize();
void shutdown();
bool selfTest();

void update(double time, bool smoke_test = false);
void render(const graphics::internal::FrameData& fd);

} // namespace application
