#ifdef NDEBUG
#undef NDEBUG
#endif
#define STB_IMAGE_IMPLEMENTATION
#include "../third_party/stb_image.h"
#include <cassert>
#include <iostream>
#include <string>

int main() {
    struct ImageCase { const char* name; int width, height; };
    const ImageCase images[] = {
        {"checker.png", 256, 256}, {"bricks.png", 256, 256}, {"mosaic.png", 256, 256},
        {"baby_cute.jpg", 1000, 1000}, {"tuntunsahur.jpg", 552, 552},
        {"you_see_this_man.jpg", 598, 821},
    };
    for (const auto& image : images) {
        int width = 0, height = 0, channels = 0;
        const auto path = std::string(LAB3_TEXTURE_DIR) + "/" + image.name;
        auto* pixels = stbi_load(path.c_str(), &width, &height, &channels, 4);
        assert(pixels && width == image.width && height == image.height);
        stbi_image_free(pixels);
    }
    int width = 0, height = 0, channels = 0;
    assert(!stbi_load("/nonexistent/cg-lab3-missing.png", &width, &height, &channels, 4));
    const unsigned char broken[] = {137, 80, 78, 71, 13, 10, 26, 10};
    assert(!stbi_load_from_memory(broken, sizeof(broken), &width, &height, &channels, 4));
    std::cout << "PASS: three PNG assets and three user JPEGs, missing file, truncated PNG\n";
}
