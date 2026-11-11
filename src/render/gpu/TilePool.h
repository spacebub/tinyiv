// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_RENDER_GPU_TILEPOOL_H
#define TIV_RENDER_GPU_TILEPOOL_H


#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "image/Bitmap.h"
#include "render/Painter.h"
#include "render/gpu/Api.h"
#include "render/gpu/Device.h"
#include "render/gpu/Staging.h"

namespace tiv::gpu {
// A slab is one image array and holds cells of one size. Cells are 256, 512 or 1024 wide and 256,
// 512 or 1024 high.
// Whole tiles get slabs of several layers, and every other size gets one layer.
// Releasing a tile frees the cell, so panning creates and destroys nothing.
class TilePool {

public:
    static constexpr int SIDE = 1024;
    static constexpr std::uint32_t WHOLE_LAYERS = 4;
    static constexpr int SMALLEST = 256;
    static constexpr std::size_t MOST_CELLS = std::size_t{SIDE / SMALLEST} * (SIDE / SMALLEST);

    struct Place {
        std::uint32_t slab = 0;
        std::uint32_t layer = 0;
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
    };

    TilePool(Device &device, Staging &staging, VkDescriptorSetLayout layout, VkSampler sampler);
    ~TilePool();

    TilePool(const TilePool &) = delete;
    TilePool(TilePool &&) = delete;
    TilePool &operator=(const TilePool &) = delete;
    TilePool &operator=(TilePool &&) = delete;

    [[nodiscard]] bool valid() const { return _pool != VK_NULL_HANDLE; }

    [[nodiscard]] Painter::Tile acquire(int width, int height, Bitmap::Encoding encoding);
    void release(Painter::Tile tile);

    [[nodiscard]] Place place(Painter::Tile tile) const;
    [[nodiscard]] VkImage image(std::uint32_t slab) const { return _slabs.at(slab).image; }
    [[nodiscard]] VkDescriptorSet set(std::uint32_t slab) const { return _slabs.at(slab).set; }
    [[nodiscard]] std::size_t resident_bytes() const;

    // Frees empty slabs once the GPU is done with them. One slab of whole tiles stays for each encoding.
    void trim();

private:
    struct Slab {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
        Bitmap::Encoding encoding = Bitmap::Encoding::Srgb;
        int cellWidth = SIDE;
        int cellHeight = SIDE;
        std::uint32_t layers = 1;
        std::uint32_t cells = 0;
        std::array<std::uint16_t, 2 * MOST_CELLS> sizes{};
        std::uint64_t used = 0;
        // Zero while alive, else the serial after which it can be destroyed.
        std::uint64_t retired = 0;
    };

    [[nodiscard]] bool make(Slab *slab, Bitmap::Encoding encoding, int cellWidth, int cellHeight);
    void destroy(Slab &slab);

    Device *_device;
    Staging *_staging;
    VkDescriptorSetLayout _layout;
    VkSampler _sampler;
    VkDescriptorPool _pool = VK_NULL_HANDLE;
    std::vector<Slab> _slabs;
};
}


#endif //TIV_RENDER_GPU_TILEPOOL_H
