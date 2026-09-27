// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_SERVICES_MEMORY_H
#define TIV_SERVICES_MEMORY_H


namespace tiv::Memory {

    // Returns what the allocator holds freed to the system. It keeps freed buffers for reuse,
    // which saves zeroing fresh pages while decodes follow one another, so this is for once
    // they stop.
    void give_back();

}


#endif //TIV_SERVICES_MEMORY_H
