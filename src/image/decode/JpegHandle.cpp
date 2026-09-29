// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#include <csetjmp>
#include <cstdint>
// jpeglib.h names FILE without including it.
#include <cstdio>
#include <span>

#include <jpeglib.h>

#include "image/decode/JpegHandle.h"

namespace tiv::Decode {
    namespace {
        [[noreturn]] void jpeg_fail(j_common_ptr info) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): pub is the first member, as libjpeg requires.
            auto *error = reinterpret_cast<JpegError *>(info->err);

            // NOLINTNEXTLINE(cert-err52-cpp,modernize-avoid-setjmp-longjmp,cppcoreguidelines-pro-bounds-array-to-pointer-decay): libjpeg reports errors by longjmp only.
            std::longjmp(error->jump, 1);
        }

        void jpeg_quiet(j_common_ptr /*info*/) {
        }
    }

    JpegHandle::JpegHandle() {
        info.err = jpeg_std_error(&error.pub);
        error.pub.error_exit = jpeg_fail;
        error.pub.output_message = jpeg_quiet;
    }

    JpegHandle::~JpegHandle() {
        if (created) {
            jpeg_destroy_decompress(&info);
        }
    }

    bool jpeg_open(JpegHandle &handle, const std::span<const std::uint8_t> data) {
        // NOLINTNEXTLINE(cert-err52-cpp,modernize-avoid-setjmp-longjmp,cppcoreguidelines-pro-bounds-array-to-pointer-decay): libjpeg reports errors by longjmp only.
        if (setjmp(handle.error.jump) != 0) {
            return false;
        }

        if (!handle.created) {
            jpeg_create_decompress(&handle.info);
            handle.created = true;
        }

        jpeg_mem_src(&handle.info, data.data(), static_cast<unsigned long>(data.size()));
        jpeg_save_markers(&handle.info, JPEG_APP0 + 1, 0xFFFF);

        return jpeg_read_header(&handle.info, TRUE) == JPEG_HEADER_OK;
    }
}
