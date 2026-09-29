// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_DECODE_JPEGHANDLE_H
#define TIV_DECODE_JPEGHANDLE_H


#include <csetjmp>
#include <cstdint>
// jpeglib.h names FILE without including it.
#include <cstdio>
#include <span>

#include <jpeglib.h>

namespace tiv::Decode {
    struct JpegError {
        jpeg_error_mgr pub{};
        // NOLINTNEXTLINE(cert-err52-cpp,cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays): libjpeg reports errors by longjmp only.
        std::jmp_buf jump{};
    };

    // A libjpeg decompressor whose errors jump back to the setjmp of whoever called into it last.
    struct JpegHandle {
        jpeg_decompress_struct info{};
        JpegError error{};
        bool created = false;

        JpegHandle();
        ~JpegHandle();

        JpegHandle(const JpegHandle &) = delete;
        JpegHandle(JpegHandle &&) = delete;
        JpegHandle &operator=(const JpegHandle &) = delete;
        JpegHandle &operator=(JpegHandle &&) = delete;
    };

    // Reads the header of the data, which has to outlive the handle. Keeps APP1 for the orientation.
    bool jpeg_open(JpegHandle &handle, std::span<const std::uint8_t> data);
}


#endif //TIV_DECODE_JPEGHANDLE_H
