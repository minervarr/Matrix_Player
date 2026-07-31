#pragma once
#include "core/db.h"
#include "sqlite3.h"
#include <cstdint>

// PRIVATE to core/src/ — deliberately not under include/core/, because nothing
// outside these two translation units may see a sqlite3*. It exists only so
// db.cpp's migration runner can drive the listening-analytics half that lives
// in db_stats.cpp, without either file reaching into the other's statics.

// Db's connection. Defined here rather than in either .cpp because both need
// it, and two identical-but-separate definitions of the same nested class is
// exactly the kind of duplication that drifts.
struct Db::Impl {
    sqlite3* db = nullptr;
};

// Creates play_events and its indices. Idempotent (CREATE IF NOT EXISTS), so
// db.cpp calls it unconditionally on every open, next to the main SCHEMA — a
// fresh database and an upgraded one reach the same shape by the same route.
void stats_createSchema(sqlite3* db);

// One-shot migration step: copies the old play_history log into play_events.
// See db.cpp's SCHEMA_STEPS for why this must run exactly once.
void stats_backfillFromPlayHistory(sqlite3* db);

// Closes any event left open by a crash or a hard kill (ended_at IS NULL) as
// EndCause::AppExit. Runs on every open, before anything can read the log.
void stats_closeOpenEvents(sqlite3* db);

// Minutes east of UTC in force at `whenUnixSec` in the machine's local zone
// (so +120 for CEST). Portable: derived from a gmtime/mktime round-trip rather
// than tm_gmtoff, which MSVC's <ctime> does not have.
int localUtcOffsetMinutes(int64_t whenUnixSec);
