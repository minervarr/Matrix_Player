#pragma once
#include <string>

// Small pure text formatting for the UI. No Canvas, no Vulkan, no OS — header
// only, so ui_text_test links it by including it and nothing else.

// English ordinal suffix for a positive integer: 1 -> "st", 2 -> "nd",
// 3 -> "rd", everything else -> "th".
//
// The teens are the whole reason this is a function and not an inline ternary:
// 11, 12 and 13 take "th" despite ending in 1, 2 and 3, and so does every
// hundred above them (111th, 212th, 1013th). Getting that wrong produces
// "11st", which reads as a typo rather than as a bug and therefore survives.
//
// Non-positive input returns "" — a rank of 0 or below is not a rank, and
// inventing "0th" would be worse than saying nothing.
inline const char* ordinalSuffix(int n) {
    if (n <= 0) return "";
    const int lastTwo = n % 100;
    if (lastTwo >= 11 && lastTwo <= 13) return "th";
    switch (n % 10) {
        case 1:  return "st";
        case 2:  return "nd";
        case 3:  return "rd";
        default: return "th";
    }
}

// "1st", "2nd", "13th", "21st". The prefix a RANKED playlist row carries.
//
// An album row deliberately does NOT use this: there a bare "7" means "track 7
// of this disc", a position, while "7th" claims a standing. Keeping the two
// spellings apart is what lets a glance tell a ranking from a running order.
inline std::string ordinal(int n) {
    // The empty suffix is the "not a rank" signal, so there is no second
    // n <= 0 check here — one guard, in one place.
    const char* suffix = ordinalSuffix(n);
    if (!*suffix) return {};
    return std::to_string(n) + suffix;
}
