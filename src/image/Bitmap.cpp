// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

#ifdef _WIN32
#include <malloc.h>
#endif

#ifdef __linux__
#include <sys/mman.h>
#endif

#include "image/Bitmap.h"

namespace tiv {
#ifdef __linux__
    namespace {
        // Below this the kernel would not back the buffer with huge pages anyway.
        constexpr std::size_t HUGE_PAGE_WORTH = std::size_t{4} * 1024 * 1024;
    }
#endif

    void Bitmap::Free::operator()(std::uint8_t *memory) const {
#ifdef _WIN32
        _aligned_free(memory);
#else
        // NOLINTNEXTLINE(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory): pairs with aligned_alloc.
        std::free(memory);
#endif
    }

    Bitmap Bitmap::allocate(const int width, const int height, const Encoding encoding) {
        Bitmap held;

        held._width = width;
        held._height = height;
        held._encoding = encoding;

        const std::size_t wanted = held.bytes();

        if (wanted == 0) {
            return held;
        }

        const std::size_t rounded = (wanted + ALIGNMENT - 1) / ALIGNMENT * ALIGNMENT;
#ifdef _WIN32
        // The Windows C runtime has no aligned_alloc, as its free could not release one.
        auto *memory = static_cast<std::uint8_t *>(_aligned_malloc(rounded, ALIGNMENT));
#else
        // NOLINTNEXTLINE(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory): the bytes are raw pixels, freed by Free.
        auto *memory = static_cast<std::uint8_t *>(std::aligned_alloc(ALIGNMENT, rounded));
#endif

        if (memory == nullptr) {
            throw std::bad_alloc();
        }

#ifdef __linux__
        // A decode target of hundreds of megabytes faults in far fewer pages this way.
        if (rounded >= HUGE_PAGE_WORTH) {
            madvise(memory, rounded, MADV_HUGEPAGE);
        }
#endif

        held._pixels.reset(memory);

        return held;
    }
}
