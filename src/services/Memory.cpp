// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#include <cstdlib>

// After a standard header, which is what defines __GLIBC__.
#ifdef __GLIBC__
#include <malloc.h>
#endif

#include "services/Memory.h"

namespace tiv {
    void Memory::give_back() {
#ifdef __GLIBC__
        malloc_trim(0);
#endif
    }
}
