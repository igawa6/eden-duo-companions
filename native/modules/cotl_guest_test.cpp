// SPDX-FileCopyrightText: Copyright 2026 AlterDigga
// SPDX-License-Identifier: GPL-3.0-or-later

#include "cotl_guest.h"
#include "cotl_test_util.h"

using cotl::ReadPtr;
using cotl::ReadRefList;
using cotl::ReadString;
using cotl::ReadValueList;

TEST(read_string_decodes_utf16) {
    FakeGuest g;
    g.PutString(0x1000, "Bramble");
    const auto s = ReadString(g, 0x1000);
    REQUIRE(s.has_value());
    CHECK_EQ(s.value_or(""), std::string("Bramble"));
}

TEST(read_string_rejects_overlong_length) {
    FakeGuest g;
    g.PutS32(0x1000 + 0x10, 300);
    CHECK(!ReadString(g, 0x1000, 256).has_value());
}

TEST(read_string_null_object_is_empty_optional) {
    FakeGuest g;
    CHECK(!ReadString(g, 0).has_value());
}

TEST(read_ref_list_returns_items) {
    FakeGuest g;
    g.PutRefList(0x2000, 0x3000, {0xA000, 0xB000, 0xC000});
    const auto items = ReadRefList(g, 0x2000);
    REQUIRE(items.has_value());
    CHECK_EQ(items->size(), size_t{3});
    CHECK_EQ((*items)[1], uint64_t{0xB000});
}

TEST(read_ref_list_rejects_size_over_max) {
    FakeGuest g;
    g.PutRefList(0x2000, 0x3000, {});
    g.PutS32(0x2000 + 0x18, 513);
    CHECK(!ReadRefList(g, 0x2000).has_value());
}

TEST(read_ref_list_rejects_null_items) {
    FakeGuest g;
    g.PutU64(0x2000 + 0x10, 0);
    g.PutS32(0x2000 + 0x18, 2);
    CHECK(!ReadRefList(g, 0x2000).has_value());
}

TEST(read_ref_list_rejects_size_beyond_array_length) {
    FakeGuest g;
    g.PutRefList(0x2000, 0x3000, {0xA000});
    g.PutS32(0x2000 + 0x18, 2); // _size > max_length of the array
    CHECK(!ReadRefList(g, 0x2000).has_value());
}

TEST(read_value_list_reads_ints) {
    FakeGuest g;
    g.PutIntList(0x2000, 0x3000, {7, -1, 42});
    const auto v = ReadValueList<int32_t>(g, 0x2000);
    REQUIRE(v.has_value());
    CHECK_EQ(v->size(), size_t{3});
    CHECK_EQ((*v)[2], 42);
}

TEST(read_ptr_unmapped_is_empty) {
    FakeGuest g;
    CHECK(!ReadPtr(g, 0xDEAD0000).has_value());
}

TEST(static_fields_follows_typeinfo) {
    FakeGuest g;
    const uint64_t main_base = 0x80000000;
    g.PutU64(main_base + 0x713AC50, 0x5000); // TypeInfo-Slot → Il2CppClass*
    g.PutU64(0x5000 + 0xB8, 0x6000);         // Il2CppClass.static_fields
    const auto sf = cotl::StaticFields(g, main_base, 0x713AC50);
    REQUIRE(sf.has_value());
    CHECK_EQ(sf.value_or(0), uint64_t{0x6000});
}
