// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Catch2 v3 either as the installed package (vcpkg, every product build) or
// as the amalgamated pair (a bare POSIX machine running cmake/portable).
#pragma once

#if defined(MV_CATCH2_AMALGAMATED)
#include "catch_amalgamated.hpp"
#else
#include <catch2/catch_test_macros.hpp>
#endif
