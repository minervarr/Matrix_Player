#pragma once
#include "core/library.h"
#include "core/stats.h"
#include <string>
#include <map>

struct EqAssignment {
    std::string name;
    std::string source;
    std::string form;
};

// One pair of headphones the listener actually uses. Distinct from
// EqAssignment, which says which pair is on a given OUTPUT right now: a DAC has
// no frequency response, so "the profile for this device" was never the real
// relationship — several pairs share one jack. This is the inventory; the
// assignment is the current state of one output.
struct EqHeadphone {
    std::string name;
    std::string source;
    std::string form;
    int64_t     lastUsed = 0;   // unix seconds
    int64_t     useCount = 0;
    bool        pinned   = false;
};

// Per-track counters. No longer a stored table: these are derived from
// play_events on demand, which is why they can't drift out of step with the
// log. Keyed by trackKey() (core/variants.h), not by file path and not by
// Track::id — neither of those survives a rename or a rescan.
struct TrackStats {
    int64_t playCount    = 0;
    int64_t skipCount    = 0;
    int64_t listenTimeMs = 0;
    // Unix seconds of the first and last time this identity reached the
    // transport, 0 when it never did. They come out of the same scan as the
    // counters above, so asking for them costs nothing.
    int64_t firstPlayed  = 0;
    int64_t lastPlayed   = 0;
};

// What was playing when the app last closed, for resume on launch.
struct PlaybackState {
    std::string filePath;
    int         positionMs = 0;
    float       volume     = 1.0f;
};

class Db {
public:
    bool open(const std::string& dbPath);
    void close();

    void saveTracks(const std::vector<Track>& tracks);
    std::vector<Track> loadTracks();

    void saveAlbums(const std::vector<Album>& albums);
    std::vector<Album> loadAlbums();

    void saveSetting(const std::string& key, const std::string& value);
    std::string loadSetting(const std::string& key);

    void addMusicRoot(const std::string& path);
    void removeMusicRoot(const std::string& path);
    std::vector<std::string> loadMusicRoots();

    std::map<std::string, FileCache> loadFileCache();
    void removeTracksByPaths(const std::vector<std::string>& paths);

    // ── Listening log ───────────────────────────────────────────────────────
    // One row per listen, opened when a track reaches the transport and closed
    // when it leaves. Two phases rather than one write at the end because the
    // app can be killed mid-track: an open row is a fact worth keeping, and
    // open() closes any it finds as EndCause::AppExit.
    //
    // beginPlayEvent() returns the row id to close the event with, or 0 if
    // nothing was recorded (empty key). Everything derived — play counts,
    // rankings, histograms — is computed from this log and nowhere else, so
    // there is no second copy of the truth to keep in step.
    int64_t beginPlayEvent(const std::string& trackKey,
                           const std::string& filePath,
                           int64_t durationMs,
                           StartCause cause,
                           int64_t whenUnixSec);
    void endPlayEvent(int64_t eventId, int64_t msHeard, bool completed,
                      EndCause cause);

    // ── Aggregate queries ───────────────────────────────────────────────────
    // All are read-only over play_events, take a StatsRange (default = all of
    // time) and are safe to call on an empty database.
    //
    // The rankings LEFT JOIN tracks for their display labels. That join is
    // best-effort on purpose: a track deleted from the library loses its name
    // but KEEPS its plays. topAlbums/topArtists/topGenres group BY the joined
    // columns, so those rows can only be counted while the track still exists
    // — the one place a deletion costs history, and it costs it only here.
    Totals totals(const StatsRange& range);

    // TopSort picks the measure the ranking is ordered by; the other measure
    // is returned regardless, so a caller can flip between them without a
    // second query.
    std::vector<TopEntry> topTracks (const StatsRange& range, int limit,
                                     TopSort sort = TopSort::Plays);
    std::vector<TopEntry> topAlbums (const StatsRange& range, int limit,
                                     TopSort sort = TopSort::Plays);
    std::vector<TopEntry> topArtists(const StatsRange& range, int limit,
                                     TopSort sort = TopSort::Plays);
    std::vector<TopEntry> topGenres (const StatsRange& range, int limit,
                                     TopSort sort = TopSort::Plays);

    std::vector<HourBucket> hourHistogram (const StatsRange& range);
    std::vector<DayBucket>  dailyListening(const StatsRange& range);
    std::vector<PlayEvent>  recentlyPlayed(const StatsRange& range, int limit);
    TrackStats trackTotals(const std::string& trackKey);

    // Sessions folded out of the timestamps. gapSec is what separates one from
    // the next: 30 minutes by default, because shorter lets a meal break an
    // evening in two and longer merges that evening with the next morning.
    SessionStats sessions(const StatsRange& range, int gapSec = 1800);

    // ── Generated playlists ─────────────────────────────────────────────────
    // Nothing here is stored: a playlist IS one of these queries, recomputed
    // when the section opens. That is why no generated list can ever drift
    // from the log — there is no second copy of it to drift.
    //
    // The first two rank on IS_AFFINITY (see db_stats.cpp), so a play only
    // counts when it ran to the end AND did not itself come out of a playlist.
    // Without that second half the list feeds itself and its top row can never
    // be overtaken.

    // Most completed, deliberate plays. The "safe bet" list.
    std::vector<TopEntry> heavyRotation(const StatsRange& range, int limit);

    // Once loved, long untouched: at least `minPlays` completed deliberate
    // plays, and nothing at all since `notSinceUnix`.
    std::vector<TopEntry> forgottenFavourites(int limit, int minPlays,
                                              int64_t notSinceUnix);

    // Library tracks with no play_events row at all — the counterpart to the
    // rankings above, and the only one of the four that says anything on a
    // fresh install. The ONE query that starts from `tracks` rather than from
    // the log, grouped by track_key so three copies of one track are one
    // entry. Ordered artist ⨯ album ⨯ disc ⨯ track, so it plays as whole
    // albums in their own order rather than as a loose bag.
    //
    // limit <= 0 means NO limit: "play everything I don't know" is the whole
    // point, and capping it would quietly decide how much of your own library
    // you are allowed to meet.
    std::vector<TopEntry> neverHeard(int limit);

    // Skipped plays as a fraction of judgeable plays, 0..1 (0 if none).
    // Rows migrated from the old play_history log never recorded how they
    // ended, so they are excluded here rather than counted as "not skipped",
    // which would quietly deflate the number for anyone with prior history.
    double skipRate(const StatsRange& range);

    // Resume on launch. loadPlaybackState() returns false when nothing was
    // ever saved (fresh install), leaving `out` untouched.
    void savePlaybackState(const PlaybackState& st);
    bool loadPlaybackState(PlaybackState& out);

    void saveEqAssignment(const std::string& deviceKey,
                          const std::string& name,
                          const std::string& source,
                          const std::string& form);
    void clearEqAssignment(const std::string& deviceKey);
    bool loadEqAssignment(const std::string& deviceKey, EqAssignment& out);

    // ── Driver inventory ────────────────────────────────────────────────────
    // Pinned first, then MOST USED, with recency only as the tie-break among
    // equal counts. The sidebar block shows four rows, so the order decides
    // what is reachable in one click; ranking by recency put whatever was
    // touched last on top, which is not the same question as "which of these
    // do I actually use". Pinned still outranks the count, because pinning is
    // the listener saying "keep this one visible" outright and a use-count
    // ranking would be free to push it off the four.
    //
    // creditEqHeadphone() both admits a new pair and refreshes its counts — it
    // is called once a profile has survived a minute of real listening, never
    // on mere selection, so a mis-click can't take a slot. It also prunes the
    // unpinned tail, keeping the list short enough to stay readable.
    std::vector<EqHeadphone> loadEqHeadphones(int limit);
    void creditEqHeadphone(const std::string& name, const std::string& source,
                           const std::string& form, int64_t whenUnixSec);
    void setEqHeadphonePinned(const std::string& name, const std::string& source,
                              const std::string& form, bool pinned);
    void removeEqHeadphone(const std::string& name, const std::string& source,
                           const std::string& form);

    ~Db() { close(); }

private:
    struct Impl;
    Impl* impl_ = nullptr;
};
