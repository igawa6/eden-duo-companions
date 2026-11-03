// SPDX-FileCopyrightText: Copyright 2026 AlterDigga
// SPDX-License-Identifier: GPL-3.0-or-later

// Reads the real I2 table from the unpacked romfs. Without COTL_ROMFS the tests are skipped.
#include <cstdlib>

#include "cotl_i2.h"
#include "cotl_test_util.h"

namespace {

const char* Romfs() {
    return std::getenv("COTL_ROMFS");
}

} // namespace

TEST(i2_load_real_table_has_many_terms) {
    if (Romfs() == nullptr) {
        std::printf("  skip (COTL_ROMFS not set)\n");
        return;
    }
    const auto t = cotl::I2Table::Load(ce_unity::MakeDirectoryRangeReader(Romfs()));
    REQUIRE(t.has_value());
    CHECK(t->Size() > 1000);
}

TEST(i2_real_table_english_texts) {
    if (Romfs() == nullptr) {
        return;
    }
    const auto t = cotl::I2Table::Load(ce_unity::MakeDirectoryRangeReader(Romfs()));
    REQUIRE(t.has_value());
    const auto berry = t->English("Inventory/BERRY");
    REQUIRE(berry.has_value());
    CHECK_EQ(*berry, std::string("Berry"));
    const auto worker = t->English("Traits/Worker");
    REQUIRE(worker.has_value());
    CHECK_EQ(*worker, std::string("Worker"));
    CHECK(!t->English("Definitely/Not/A/Term").has_value());
}

TEST(i2_from_map_lookup) {
    const auto t = cotl::I2Table::FromMap({{"Traits/Worker", "Worker"}});
    CHECK_EQ(t.English("Traits/Worker").value_or(""), std::string("Worker"));
    CHECK(!t.English("Traits/Farmer").has_value());
}
