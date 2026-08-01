#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Listening analytics: the vocabulary of the play_events log and the result
// types of the aggregate queries declared on Db (core/db.h).
//
// PURE — no sqlite, no OS, no Canvas. core/tests/stats_test.cc leans on that,
// and so does any future GUI that wants to draw these numbers without linking
// the database.

// ── Why a play started ──────────────────────────────────────────────────────
// The most valuable single field in the log. A track the listener CHOSE says
// far more about taste than one that merely played next, and any future
// playlist generator has to weigh the two differently. It cannot be recovered
// after the fact, which is why it is recorded per event rather than derived.
// APPEND ONLY. These values are stored as integers in every existing row and
// are hardcoded in db_stats.cpp's IS_SKIP / IS_JUDGEABLE / IS_AFFINITY, so
// renumbering one silently reinterprets history.
enum class StartCause : int {
    Manual  = 0,   // picked from the grid or the album view
    Gapless = 1,   // the gapless coordinator advanced into it
    Resume  = 2,   // restored at launch from playback_state
    Shuffle = 3,   // chosen by shuffle
    Unknown = 4,   // migrated from the old play_history log, which recorded
                   // only (path, when) — see db.cpp's SCHEMA_STEPS
    // Started from a generated playlist — the listener picking a row in one,
    // shuffling it, or the queue advancing inside it. ONE value for the whole
    // run on purpose: what matters downstream is that the play came out of a
    // playlist, and IS_AFFINITY excludes it so the list cannot feed itself.
    //
    // The row is still written in full. The play is not counted toward taste;
    // it is not thrown away, so reconsidering this later costs nothing.
    Playlist = 5,
};

// ── How a play ended ────────────────────────────────────────────────────────
enum class EndCause : int {
    Natural  = 0,  // ran to the end of the track
    Next     = 1,  // listener skipped forward
    Prev     = 2,  // listener skipped back
    Stop     = 3,  // transport stopped
    Replaced = 4,  // another track was started over it
    AppExit  = 5,  // app closed or crashed mid-track — see Db::open()
    Unknown  = 6,  // migrated row
};

// Window over play_events.started_at, in unix seconds. 0 means "open" on that
// side, so a default-constructed range is all of time. Half-open: from <= t < to.
struct StatsRange {
    int64_t fromUnix = 0;
    int64_t toUnix   = 0;
};

// The ranges a listener actually asks for. Every boundary lands on a LOCAL
// midnight (or a local January 1st), which is why this is not a matter of
// subtracting 7*86400 from now: the answer to "the last seven days" must not
// shift by an hour because the clocks changed inside the window.
enum class RangePreset : int {
    Last7Days  = 0,
    Last30Days = 1,
    ThisYear   = 2,
    AllTime    = 3,
    Last90Days = 4,   // what "On Repeat" means: recent taste, not lifetime taste
};

// Boundaries computed on the listener's clock, returned in unix seconds. The
// right side is left OPEN (toUnix = 0) so a track played during the session is
// counted without recomputing the range.
StatsRange rangeFor(RangePreset preset, int64_t nowUnixSec, int utcOffsetMin);

// The `utcOffsetMin` rangeFor() wants, for the listener's clock at that
// instant. Defined in db_stats.cpp (pure <ctime> arithmetic, no sqlite) and
// already declared in the src-private db_schema.h, where beginPlayEvent needed
// it first. Re-declared here — not moved — because a CALLER of rangeFor() has
// no other way to obtain its second argument, and every one of them would
// otherwise re-derive the same gmtime/mktime dance by hand and get DST wrong
// in its own way.
int localUtcOffsetMinutes(int64_t whenUnixSec);

// ── Calendar arithmetic on a DayBucket ──────────────────────────────────────
// Pure integer math (Howard Hinnant's civil algorithms), NOT localtime(): a
// DayBucket's day is already expressed on the listener's clock, so handing it
// to localtime() would shift it a second time — an hour either side of
// midnight, which is exactly where a day boundary lives.
void statsCivilFromLocalDay(int64_t dayLocal, int& year, int& month, int& day);

// 0 = Sunday, matching tm_wday.
int  statsWeekdayFromLocalDay(int64_t dayLocal);

// How a ranking is ordered. "Most played" and "most listened to" are different
// questions, and for albums the second is the honest one — a two-minute
// interlude does not weigh what a twenty-minute piece does. The unchosen
// measure is the first tie-break either way, and the name settles the rest, so
// the order is total and the output deterministic.
enum class TopSort : int {
    Plays     = 0,
    TimeHeard = 1,
};

// One row of a ranking. `key` identifies the group the query aggregated: a
// track_key for tracks, an artist or genre name, and for albums the pair
// album + artist joined by a unit separator (U+001F) — an album title alone is
// not an identity, two artists can both have a "Live". label and subLabel are
// the same information split for display and may be empty when the track has
// since left the library. The counts are correct either way — see the note on
// the joins in db_stats.cpp.
struct TopEntry {
    std::string key;
    std::string label;
    std::string subLabel;
    int64_t     plays   = 0;
    int64_t     msHeard = 0;
};

// Local-hour bucket, 0..23. LOCAL, not UTC: "when do you listen" is a question
// about the listener's own day, and each event stores the offset that was in
// force when it happened, so daylight saving and travel don't smear the answer.
struct HourBucket {
    int     hour    = 0;
    int64_t plays   = 0;
    int64_t msHeard = 0;
};

// One local calendar day.
//
// `dayLocal` is that day's midnight in LOCAL seconds — the instant already
// shifted into the listener's clock, never a UTC instant. Always a multiple of
// 86400, which is the property that makes the calendar helpers above plain
// division.
//
// Storing it this way is what keeps one calendar day in ONE bucket. The UTC
// instant of a local midnight moves with the offset, so grouping on THAT
// splits a daylight-saving day — or a day spent flying — into two rows and
// makes Totals::activeDays count it twice.
struct DayBucket {
    int64_t dayLocal = 0;
    int64_t plays    = 0;
    int64_t msHeard  = 0;
};

// A run of listening with no gap longer than `gapSec` between one play ending
// and the next starting. Sessions are derived here, at query time, rather than
// materialised: the log already holds the timestamps, and a stored session
// table would be a second copy of the truth able to drift from them.
//
// spanMs is wall clock (first start to last end) and msHeard is audio actually
// heard, so the two differ by whatever was spent paused or choosing. Rows
// migrated from the old play_history log carry no end time and contribute a
// zero-length span, which is the honest answer — nobody recorded one.
struct SessionStats {
    int64_t count     = 0;
    int64_t plays     = 0;
    int64_t msHeard   = 0;
    int64_t spanMs    = 0;   // summed across sessions
    int64_t longestMs = 0;   // wall-clock span of the longest single session
};

// The headline numbers for a range.
struct Totals {
    int64_t plays           = 0;
    int64_t skips           = 0;
    int64_t msHeard         = 0;
    int64_t distinctTracks  = 0;
    int64_t distinctAlbums  = 0;
    int64_t distinctArtists = 0;
    int64_t activeDays      = 0;   // distinct local days with at least one play
};

// One row of the log, for a "recently played" list or for a test to check.
//
// title/artist are resolved through the same representative join the rankings
// use and are empty when the track has left the library — so a deleted track
// still appears in the history, unnamed, exactly as it still ranks in
// topTracks(). filePath says which copy played and is provenance, not identity.
struct PlayEvent {
    int64_t     id           = 0;
    std::string trackKey;
    std::string filePath;
    std::string title;
    std::string artist;
    int64_t     startedAt    = 0;
    int         utcOffsetMin = 0;
    int64_t     endedAt      = 0;   // 0 while the event is still open
    int64_t     msHeard      = 0;
    int64_t     durationMs   = 0;
    bool        completed    = false;
    StartCause  startCause   = StartCause::Unknown;
    EndCause    endCause     = EndCause::Unknown;
};
