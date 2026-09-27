// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_BENCH_SUPPORT_REGISTER_H
#define TIV_BENCH_SUPPORT_REGISTER_H


namespace bench {

    // Benchmarks over corpus files register once the corpus is known, so each file adds its own.
    void register_bitmap();
    void register_bmp();
    void register_decode();
    void register_pyramid();
    void register_upload();
    void register_loader();
    void register_startup();

}


#endif //TIV_BENCH_SUPPORT_REGISTER_H
