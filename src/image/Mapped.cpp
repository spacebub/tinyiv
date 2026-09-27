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

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "image/Mapped.h"

namespace tiv {
    namespace {
        void fail(std::string *error, const std::filesystem::path &file, const std::string &why) {
            if (error != nullptr) {
                *error = file.string() + ": " + why;
            }
        }

        std::string last_error() {
            return std::error_code(errno, std::generic_category()).message();
        }
    }

    Mapped::~Mapped() {
        release();
    }

    Mapped::Mapped(Mapped &&other) noexcept : _data(std::exchange(other._data, nullptr)), _size(std::exchange(other._size, 0)) {
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
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): munmap takes what mmap gave, minus the const.
            munmap(const_cast<std::uint8_t *>(_data), _size);
        }

        _data = nullptr;
        _size = 0;
    }

    bool Mapped::open(const std::filesystem::path &file, Mapped *out, std::string *error) {
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
        madvise(memory, size, MADV_SEQUENTIAL);
        madvise(memory, size, MADV_WILLNEED);

        out->_data = static_cast<const std::uint8_t *>(memory);
        out->_size = size;

        return true;
    }
}
