// Calendar arithmetic for the listening analytics.
//
// PURE, and deliberately so: no sqlite, no OS, and not even <ctime>. Every
// function here is integer math over a day count, so it gives the same answer
// on every platform, in every timezone, without a global TZ database being
// consulted behind its back — which is what makes it testable at all.
//
// The reason this file exists rather than the GUI computing ranges inline: a
// range boundary is a LOCAL midnight, and turning "seven days ago" into the
// right unix second is the kind of arithmetic that looks obvious, is not, and
// is invisible when wrong. Here it is one place with tests on it.

#include "core/stats.h"

namespace {

// Howard Hinnant's civil-from-days / days-from-civil, from "chrono-Compatible
// Low-Level Date Algorithms". Both shift the era to start on March 1st so that
// the leap day lands at the END of a year and needs no special case; they are
// exact for the whole int64 range and involve no lookup tables.
//
// `days` is days since 1970-01-01 (may be negative).

// All three are constexpr: they are closed-form integer arithmetic, so where a
// caller passes a constant the compiler can fold the whole thing away instead
// of emitting a call. Nothing else about them changes.
constexpr void civilFromDays(int64_t days, int& y, int& m, int& d) {
    days += 719468;                                     // shift epoch to 0000-03-01
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const int64_t doe = days - era * 146097;            // day of era,  [0, 146096]
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t yr  = yoe + era * 400;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);  // [0, 365]
    const int64_t mp  = (5 * doy + 2) / 153;            // month, March = 0
    const int64_t dd  = doy - (153 * mp + 2) / 5 + 1;   // [1, 31]
    const int64_t mm  = mp + (mp < 10 ? 3 : -9);        // back to January = 1
    y = (int)(yr + (mm <= 2 ? 1 : 0));
    m = (int)mm;
    d = (int)dd;
}

constexpr int64_t daysFromCivil(int y, int m, int d) {
    y -= (m <= 2 ? 1 : 0);
    const int64_t era = (int64_t)(y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = (int64_t)y - era * 400;                         // [0, 399]
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; // [0, 365]
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;          // [0, 146096]
    return era * 146097 + doe - 719468;
}

constexpr int64_t kDay = 86400;

// Floor division by a day. A plain `/` truncates toward zero, which for any
// instant before 1970 would round the day boundary the wrong way.
constexpr int64_t floorDay(int64_t sec) {
    return (sec - (((sec % kDay) + kDay) % kDay)) / kDay;
}

} // namespace

// ── Public API ──────────────────────────────────────────────────────────────

void statsCivilFromLocalDay(int64_t dayLocal, int& year, int& month, int& day) {
    civilFromDays(floorDay(dayLocal), year, month, day);
}

int statsWeekdayFromLocalDay(int64_t dayLocal) {
    // 1970-01-01 was a Thursday, hence the +4 before folding to a week.
    const int64_t days = floorDay(dayLocal);
    return (int)(((days + 4) % 7 + 7) % 7);
}

StatsRange rangeFor(RangePreset preset, int64_t nowUnixSec, int utcOffsetMin) {
    if (preset == RangePreset::AllTime) return StatsRange{};

    const int64_t offsetSec  = (int64_t)utcOffsetMin * 60;
    const int64_t nowLocal   = nowUnixSec + offsetSec;
    const int64_t todayLocal = floorDay(nowLocal);   // local days since epoch

    int64_t startDay = todayLocal;
    switch (preset) {
        // Inclusive of today, so "7 days" is seven calendar days on the
        // listener's own calendar — today plus the six before it — not a
        // 168-hour window that starts mid-afternoon.
        case RangePreset::Last7Days:  startDay = todayLocal - 6;  break;
        case RangePreset::Last30Days: startDay = todayLocal - 29; break;
        case RangePreset::Last90Days: startDay = todayLocal - 89; break;
        case RangePreset::ThisYear: {
            int y = 0, m = 0, d = 0;
            civilFromDays(todayLocal, y, m, d);
            startDay = daysFromCivil(y, 1, 1);
            break;
        }
        case RangePreset::AllTime: break;   // handled above
    }

    StatsRange r;
    // Back from the listener's clock to UTC, which is what started_at holds.
    r.fromUnix = startDay * kDay - offsetSec;
    r.toUnix   = 0;   // open: whatever plays next belongs to this range too
    return r;
}
