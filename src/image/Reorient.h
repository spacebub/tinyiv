// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_IMAGE_REORIENT_H
#define TIV_IMAGE_REORIENT_H


#include <filesystem>
#include <string>

#include "image/decode/Decode.h"

namespace tiv::Reorient {

    // True when the format keeps an orientation in its metadata that can be written here.
    [[nodiscard]] bool supported(Decode::Format format);

    // Makes the file say the orientation and changes nothing else: the pixel data stays byte
    // for byte, and writing back the orientation it had gives back the same file.
    bool write(const std::filesystem::path &file, int orientation, std::string *error = nullptr);

}


#endif //TIV_IMAGE_REORIENT_H
