// Asserts must stay live even though the app builds Release (NDEBUG).
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <string>

// The REAL header, not a restatement of its rules. ui_text.hh is header-only
// and depends on nothing but <string>, which is what makes this test one
// include and no link line — keep it that way.
#include "ui_text.hh"

int main() {
    // ── The three that are not "th" ────────────────────────────────────────
    assert(std::string(ordinalSuffix(1)) == "st");
    assert(std::string(ordinalSuffix(2)) == "nd");
    assert(std::string(ordinalSuffix(3)) == "rd");
    assert(std::string(ordinalSuffix(4)) == "th");

    // ── The teens, which is the only reason this is a function ────────────
    // 11, 12 and 13 end in 1, 2 and 3 but take "th". The naive version emits
    // "11st", which reads as a typo rather than as a bug and so survives.
    assert(std::string(ordinalSuffix(11)) == "th");
    assert(std::string(ordinalSuffix(12)) == "th");
    assert(std::string(ordinalSuffix(13)) == "th");
    assert(std::string(ordinalSuffix(14)) == "th");

    // ── And the suffix comes back on the far side of them ─────────────────
    assert(std::string(ordinalSuffix(21)) == "st");
    assert(std::string(ordinalSuffix(22)) == "nd");
    assert(std::string(ordinalSuffix(23)) == "rd");
    assert(std::string(ordinalSuffix(24)) == "th");

    // ── Every hundred repeats the teen exception ──────────────────────────
    // This is the half that a "% 10 unless 11..13" rule gets wrong: it is the
    // LAST TWO digits that matter, so 111 is 111th, not 111st.
    assert(std::string(ordinalSuffix(101)) == "st");
    assert(std::string(ordinalSuffix(111)) == "th");
    assert(std::string(ordinalSuffix(112)) == "th");
    assert(std::string(ordinalSuffix(113)) == "th");
    assert(std::string(ordinalSuffix(121)) == "st");
    assert(std::string(ordinalSuffix(211)) == "th");
    assert(std::string(ordinalSuffix(1013)) == "th");

    // ── A rank of zero or less is not a rank ──────────────────────────────
    assert(std::string(ordinalSuffix(0))  == "");
    assert(std::string(ordinalSuffix(-1)) == "");
    assert(ordinal(0).empty());
    assert(ordinal(-7).empty());

    // ── The whole string, which is what a row actually draws ──────────────
    assert(ordinal(1)    == "1st");
    assert(ordinal(2)    == "2nd");
    assert(ordinal(3)    == "3rd");
    assert(ordinal(7)    == "7th");
    assert(ordinal(11)   == "11th");
    assert(ordinal(21)   == "21st");
    assert(ordinal(50)   == "50th");
    assert(ordinal(101)  == "101st");
    assert(ordinal(111)  == "111th");

    // Nothing above 50 can appear in a ranked playlist today (that is the
    // cap), but the function is not what should be enforcing that, so the
    // larger cases are pinned rather than assumed unreachable.

    printf("ui_text_test: all assertions passed\n");
    return 0;
}
