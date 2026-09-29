// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_IMAGE_FILEREADER_H
#define TIV_IMAGE_FILEREADER_H


#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace tiv {
    // A file read at offsets, which threads can share. Read rather than mapped, since a mapping of a
    // file cut short under it faults.
    class FileReader {

    public:
        explicit FileReader(const std::filesystem::path &file);
        ~FileReader();

        FileReader(const FileReader &) = delete;
        FileReader(FileReader &&) = delete;
        FileReader &operator=(const FileReader &) = delete;
        FileReader &operator=(FileReader &&) = delete;

        [[nodiscard]] bool valid() const;
        [[nodiscard]] std::uint64_t size() const { return _size; }

        // Up to bytes at the offset, fewer at the end of the file.
        std::size_t read(std::uint64_t offset, std::uint8_t *out, std::size_t bytes) const;

    private:
#ifdef _WIN32
        void *_handle;
#else
        int _fd = -1;
#endif
        std::uint64_t _size = 0;
    };
}


#endif //TIV_IMAGE_FILEREADER_H
