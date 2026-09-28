// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#if defined(_MSC_VER) && (defined(_M_X64) || defined(__x86_64__))
#include <array>
#include <immintrin.h>
#include <intrin.h>
#endif

#include "image/Simd.h"

namespace tiv {
    namespace {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(__x86_64__))
        // The CPUID and XGETBV bits of the Intel SDM. XGETBV says whether the system saves the
        // upper halves of the registers, which the AVX2 bit alone does not.
#if defined(__clang__)
        __attribute__((target("xsave")))
#endif
        bool detect() {
            constexpr int OSXSAVE = 1U << 27U;
            constexpr int AVX = 1U << 28U;
            constexpr int AVX2 = 1U << 5U;
            constexpr unsigned long long XMM_YMM = 0x6;
            std::array<int, 4> info{};

            __cpuid(info.data(), 0);

            if (info[0] < 7) {
                return false;
            }

            __cpuid(info.data(), 1);

            if ((info[2] & OSXSAVE) == 0 || (info[2] & AVX) == 0 || (_xgetbv(0) & XMM_YMM) != XMM_YMM) {
                return false;
            }

            __cpuidex(info.data(), 7, 0);

            return (info[1] & AVX2) != 0;
        }
#else
        bool detect() {
#ifdef __x86_64__
            return __builtin_cpu_supports("avx2");
#else
            return false;
#endif
        }
#endif
    }

    bool Simd::avx2() {
        static const bool held = detect();

        return held;
    }
}
