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
enum class StartCause : int {
    Manual  = 0,   // picked from the grid or the album view
    Gapless = 1,   // the gapless coordinator advanced into it
    Resume  = 2,   // restored at launch from playback_state
    Shuffle = 3,   // chosen by shuffle
    Unknown = 4,   // migrated from the old play_history log, which recorded
                   // only (path, when) — see db.cpp's SCHEMA_STEPS
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

// One row of a ranking. `key` is what the query grouped by (a track_key, or an
// album / artist / genre name); label and subLabel are for display and may be
// empty when the track has since left the library. The counts are correct
// either way — see the note on the joins in db_stats.cpp.
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

// One local calendar day, identified by the unix second of its local midnight.
struct DayBucket {
    int64_t dayUnix = 0;
    int64_t plays   = 0;
    int64_t msHeard = 0;
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
struct PlayEvent {
    int64_t     id           = 0;
    std::string trackKey;
    std::string filePath;
    int64_t     startedAt    = 0;
    int         utcOffsetMin = 0;
    int64_t     endedAt      = 0;   // 0 while the event is still open
    int64_t     msHeard      = 0;
    int64_t     durationMs   = 0;
    bool        completed    = false;
    StartCause  startCause   = StartCause::Unknown;
    EndCause    endCause     = EndCause::Unknown;
};
