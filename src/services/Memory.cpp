// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>

// After a standard header, which is what defines __GLIBC__.
#ifdef __GLIBC__
#include <malloc.h>
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <SDL3/SDL.h>

#include "services/Memory.h"

namespace tiv {
    void Memory::give_back() {
#ifdef __GLIBC__
        malloc_trim(0);
#endif
    }

    std::uint64_t Memory::available() {
        constexpr std::uint64_t MIB = std::uint64_t{1024} * 1024;
#ifdef _WIN32
        MEMORYSTATUSEX status{};

        status.dwLength = sizeof status;

        if (GlobalMemoryStatusEx(&status) != 0) {
            return status.ullAvailPhys;
        }
#else
#ifdef __linux__
        // In kB: https://docs.kernel.org/filesystems/proc.html#meminfo
        std::ifstream meminfo("/proc/meminfo");
        std::string name;
        std::uint64_t amount = 0;
        std::string unit;

        while (meminfo >> name >> amount >> unit) {
            if (name == "MemAvailable:") {
                return amount * 1024;
            }
        }
#endif
#endif

        return static_cast<std::uint64_t>(SDL_GetSystemRAM()) * MIB;
    }
}
