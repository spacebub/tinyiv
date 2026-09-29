// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "image/Mapped.h"

namespace tiv {
    namespace {
        // A file up to this size is read ahead whole, for the decoder about to go through it. A larger one is
        // streamed or only probed, so just its head is read ahead: asking for all of an image larger than
        // memory fills it and stalls the disk.
        constexpr std::size_t READ_AHEAD_WHOLE = std::size_t{1} << 30U;
        constexpr std::size_t READ_AHEAD_HEAD = std::size_t{16} << 20U;

        std::size_t read_ahead(const std::size_t size) {
            return size <= READ_AHEAD_WHOLE ? size : READ_AHEAD_HEAD;
        }

        void fail(std::string *error, const std::filesystem::path &file, const std::string &why) {
            if (error != nullptr) {
                *error = file.string() + ": " + why;
            }
        }

        std::string last_error() {
#ifdef _WIN32
            return std::error_code(static_cast<int>(GetLastError()), std::system_category()).message();
#else
            return std::error_code(errno, std::generic_category()).message();
#endif
        }
    }

    Mapped::~Mapped() {
        release();
    }

    Mapped::Mapped(Mapped &&other) noexcept
        : _data(std::exchange(other._data, nullptr)), _size(std::exchange(other._size, 0)) {
    }

    Mapped &Mapped::operator=(Mapped &&other) noexcept {
        if (this != &other) {
            release();

            _data = std::exchange(other._data, nullptr);
            _size = std::exchange(other._size, 0);
        }

        return *this;
    }

    void Mapped::release() {
        if (_data != nullptr) {
#ifdef _WIN32
            UnmapViewOfFile(_data);
#else
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): munmap takes what mmap gave, minus the const.
            munmap(const_cast<std::uint8_t *>(_data), _size);
#endif
        }

        _data = nullptr;
        _size = 0;
    }

#ifdef _WIN32
    bool Mapped::open(const std::filesystem::path &file, Mapped *out, std::string *error, const Use use) {
        // Shared for deleting too, so a file on screen can still be removed or renamed.
        HANDLE handle = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);

        if (handle == INVALID_HANDLE_VALUE) {
            fail(error, file, last_error());

            return false;
        }

        LARGE_INTEGER length{};

        if (GetFileType(handle) != FILE_TYPE_DISK || GetFileSizeEx(handle, &length) == 0) {
            fail(error, file, "not a regular file");
            CloseHandle(handle);

            return false;
        }

        out->release();

        if (length.QuadPart == 0) {
            CloseHandle(handle);

            return true;
        }

        const auto size = static_cast<std::size_t>(length.QuadPart);
        HANDLE mapping = CreateFileMappingW(handle, nullptr, PAGE_READONLY, 0, 0, nullptr);
        const std::string mappingError = mapping == nullptr ? last_error() : std::string();

        CloseHandle(handle);

        if (mapping == nullptr) {
            fail(error, file, mappingError);

            return false;
        }

        void *memory = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
        const std::string viewError = memory == nullptr ? last_error() : std::string();

        // The view holds the mapping, and with it the file, open on its own.
        CloseHandle(mapping);

        if (memory == nullptr) {
            fail(error, file, viewError);

            return false;
        }

        // Decoders read front to back, and the read ahead hides the disk.
        if (use == Use::Through) {
            WIN32_MEMORY_RANGE_ENTRY range{memory, read_ahead(size)};

            PrefetchVirtualMemory(GetCurrentProcess(), 1, &range, 0);
        }

        out->_data = static_cast<const std::uint8_t *>(memory);
        out->_size = size;

        return true;
    }
#else
    bool Mapped::open(const std::filesystem::path &file, Mapped *out, std::string *error, const Use use) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open() is variadic in C.
        const int fd = ::open(file.c_str(), O_RDONLY | O_CLOEXEC);

        if (fd < 0) {
            fail(error, file, last_error());

            return false;
        }

        struct stat status{};

        if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode)) {
            fail(error, file, "not a regular file");
            close(fd);

            return false;
        }

        out->release();

        if (status.st_size == 0) {
            close(fd);

            return true;
        }

        const auto size = static_cast<std::size_t>(status.st_size);
        void *memory = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);

        close(fd);

        // NOLINTNEXTLINE(performance-no-int-to-ptr,cppcoreguidelines-pro-type-cstyle-cast): MAP_FAILED is a C macro.
        if (memory == MAP_FAILED) {
            fail(error, file, last_error());

            return false;
        }

        // Decoders read front to back, and the read ahead hides the disk.
        if (use == Use::Through) {
            madvise(memory, size, MADV_SEQUENTIAL);
            madvise(memory, read_ahead(size), MADV_WILLNEED);
        } else {
            madvise(memory, size, MADV_RANDOM);
        }

        out->_data = static_cast<const std::uint8_t *>(memory);
        out->_size = size;

        return true;
    }
#endif
}
