// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/mods/dsmod_module_abi.h"

extern "C" const EdenDsmodModuleApi* eden_dsmod_get_module(uint32_t host_abi_version,
                                                           uint64_t host_abi_hash);
