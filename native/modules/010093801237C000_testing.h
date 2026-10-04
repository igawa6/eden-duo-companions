// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Test seams of the Dread module (010093801237C000.cpp), compiled into it only when
// DSMOD_DREAD_TESTING is defined (the dsmod-dread-test target). They reach Reader internals
// with a synthetic host so the water, collider and EMMI logic can be checked without a game.
// The shipped module does not have these symbols.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "core/mods/dsmod_module_abi.h"

uint64_t DreadTestNameHash(const char* name);
uint32_t DreadTestColliderMask(const EdenDsmodHostApi& host, bool state_valid, bool owner_present,
                               bool actor_dead, uint32_t prior_mask, uint32_t enabled_mask);
void* DreadTestCreateReader(const EdenDsmodHostApi& host, uint64_t main_base);
void DreadTestDestroyReader(void* reader);
void DreadTestSetRoot(void* reader, uint64_t root);
// Water: the manifest's map.areas (water_pools), the live scenario, the tick clock, one
// RefreshWaterBoxes call, the published boxes, the DEBA8C-mirror component lookup, how often
// the scenario's blackboard props were walked by name, and the water generation.
bool DreadTestParseMap(void* reader, const char* manifest_json);
void DreadTestSetScenario(void* reader, const char* scenario);
void DreadTestSetTick(void* reader, uint64_t tick);
void DreadTestRefreshWater(void* reader);
size_t DreadTestWaterBoxes(void* reader, float* boxes, size_t capacity);
uint64_t DreadTestResolveWaterComponent(void* reader, uint64_t key);
uint64_t DreadTestWaterSectionWalks(void* reader);
uint64_t DreadTestWaterGen(void* reader);
// EmmyDefeatedInScenario, exposed for a synthetic blackboard fixture (DreadTestSetRoot
// pins the Game root first). Returns -1 for nullopt (section unreadable), 0 for alive, 1 for dead.
int DreadTestEmmyDefeated(void* reader, const char* scenario);
// dread::PublishVital into a snapshot whose "energy" int already holds a stale host value.
// Returns the int a widget would read; *published_float receives the float.
long long DreadTestPublishVital(float value, double* published_float);
// The page-chunked guest string reader behind CStrId names, scenario ids and the cutscene name,
// and the single-check memory facade.
std::string DreadTestReadCStrIdName(const EdenDsmodHostApi& host, uint64_t keyptr, int max);
bool DreadTestReadAsciiZ(const EdenDsmodHostApi& host, uint64_t address, int max,
                         bool reject_non_printable, std::string* out);
uint32_t DreadTestRead32(const EdenDsmodHostApi& host, uint64_t address);
