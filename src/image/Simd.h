// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_IMAGE_SIMD_H
#define TIV_IMAGE_SIMD_H


// GCC and Clang only compile AVX2 intrinsics in a function marked for them, MSVC takes them anywhere.
#if defined(_MSC_VER) && !defined(__clang__)
#define TIV_AVX2
#else
#define TIV_AVX2 __attribute__((target("avx2")))
#endif

namespace tiv::Simd {

    // True when both the processor and the system run AVX2.
    [[nodiscard]] bool avx2();

}


#endif //TIV_IMAGE_SIMD_H
