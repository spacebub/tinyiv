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
#include <filesystem>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "image/FileReader.h"

namespace tiv {
#ifdef _WIN32
    FileReader::FileReader(const std::filesystem::path &file)
        : _handle(CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)) {
        LARGE_INTEGER length{};

        if (valid() && GetFileSizeEx(_handle, &length) != 0) {
            _size = static_cast<std::uint64_t>(length.QuadPart);
        }
    }

    FileReader::~FileReader() {
        if (valid()) {
            CloseHandle(_handle);
        }
    }

    bool FileReader::valid() const {
        return _handle != INVALID_HANDLE_VALUE;
    }

    std::size_t FileReader::read(const std::uint64_t offset, std::uint8_t *out, const std::size_t bytes) const {
        OVERLAPPED where{};
        DWORD got = 0;

        where.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFU);
        where.OffsetHigh = static_cast<DWORD>(offset >> 32U);

        return ReadFile(_handle, out, static_cast<DWORD>(bytes), &got, &where) != 0 ? got : 0;
    }
#else
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open() is variadic in C.
    FileReader::FileReader(const std::filesystem::path &file) : _fd(::open(file.c_str(), O_RDONLY | O_CLOEXEC)) {
        struct stat status{};

        if (_fd >= 0 && fstat(_fd, &status) == 0) {
            _size = static_cast<std::uint64_t>(status.st_size);
        }
    }

    FileReader::~FileReader() {
        if (_fd >= 0) {
            ::close(_fd);
        }
    }

    bool FileReader::valid() const {
        return _fd >= 0;
    }

    std::size_t FileReader::read(const std::uint64_t offset, std::uint8_t *out, const std::size_t bytes) const {
        const ssize_t got = ::pread(_fd, out, bytes, static_cast<off_t>(offset));

        return got > 0 ? static_cast<std::size_t>(got) : 0;
    }
#endif
}
