// SPDX-FileCopyrightText: Copyright 2026 AlterDigga
// SPDX-License-Identifier: GPL-3.0-or-later

#include "cotl_i2.h"
#include "cotl_terms.h"
#include "cotl_test_util.h"

using cotl::I2Table;
using cotl::Terms;

TEST(terms_enum_name_known_and_unknown) {
    CHECK_EQ(Terms::EnumName("FollowerRole", 1), std::string("Worker"));
    CHECK_EQ(Terms::EnumName("FollowerRole", 99999), std::string("#99999"));
    CHECK_EQ(Terms::EnumName("NoSuchEnum", 1), std::string("#1"));
}

TEST(terms_lookup_uses_game_formats) {
    const auto t = I2Table::FromMap({
        {"Traits/Worker", "Worker"},
        {"Traits/NaturallySkeptical", "Skeptical"},
        {"TarotCards/Hearts1/Name", "Sample Card"},
    });
    const Terms terms(&t);
    CHECK_EQ(terms.Role(1), std::string("Worker"));
    CHECK_EQ(terms.Trait(1), std::string("Skeptical"));
    const int32_t hearts1 = [] {
        for (int32_t v = 0; v < 1000; ++v)
            if (Terms::EnumName("TarotCard", v) == "Hearts1") return v;
        return -1;
    }();
    REQUIRE(hearts1 >= 0);
    CHECK_EQ(terms.TarotName(hearts1), std::string("Sample Card"));
}

// Missing term: identifier, unknown value: "#<value>"
TEST(terms_missing_term_falls_back_to_identifier) {
    const auto t = I2Table::FromMap({});
    const Terms terms(&t);
    CHECK_EQ(terms.Role(1), std::string("Worker"));
    CHECK_EQ(terms.Item(99999), std::string("#99999"));
}

TEST(terms_without_table_uses_identifiers) {
    const Terms terms(nullptr);
    CHECK_EQ(terms.Trait(1), std::string("NaturallySkeptical"));
}

// Game texts partly contain rich text (<sprite ...>) the companion font cannot do
TEST(terms_strip_markup_from_game_text) {
    const auto t = I2Table::FromMap({
        {"FollowerThoughts/AfterHoliday", "Grieved <sprite name=\"icon_Trait_FearOfDeath\"> twice"},
    });
    const Terms terms(&t);
    int32_t v = -1;
    for (int32_t i = 0; i < 2000 && v < 0; ++i)
        if (Terms::EnumName("Thought", i) == "AfterHoliday") v = i;
    REQUIRE(v >= 0);
    CHECK_EQ(terms.Thought(v), std::string("Grieved twice"));
}

// Game texts for labels with color_markup: icons as characters, colours as spans
TEST(terms_text_with_icons_hearts_become_coloured_glyphs) {
    using cotl::TextWithIcons;
    const std::string blue = "{c:" + std::string(cotl::kIconColorBlue) + "}\xE1\xB8\x80{/c}"; // U+1E00
    CHECK_EQ(TextWithIcons("<sprite name=\"icon_BlueHeart\"> +1"), blue + " +1");
    const std::string half = "{c:" + std::string(cotl::kIconColorRed) + "}\xE1\xB8\x81{/c}"; // U+1E01
    CHECK_EQ(TextWithIcons("Gain an extra <sprite name=\"icon_HalfRedHeart\">"), "Gain an extra " + half);
}

TEST(terms_text_with_icons_keeps_hex_colours_and_line_breaks) {
    using cotl::TextWithIcons;
    CHECK_EQ(TextWithIcons("Deal <color=#01D5A2>2</color> damage<br>now"),
             std::string("Deal {c:#FF01D5A2}2{/c} damage\nnow"));
    CHECK_EQ(TextWithIcons("Gain <color=red>half</color> a heart."), std::string("Gain half a heart."));
}

TEST(terms_text_with_icons_drops_unknown_sprites_and_spaces) {
    using cotl::TextWithIcons;
    CHECK_EQ(TextWithIcons("Fervour <sprite name=\"icon_blackSoul\"> refills"), std::string("Fervour refills"));
    CHECK_EQ(TextWithIcons("<b>Bold</b> text  "), std::string("Bold text"));
}
