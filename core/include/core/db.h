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

// Per-track counters. No longer a stored table: these are derived from
// play_events on demand, which is why they can't drift out of step with the
// log. Keyed by trackKey() (core/variants.h), not by file path and not by
// Track::id — neither of those survives a rename or a rescan.
struct TrackStats {
    int64_t playCount    = 0;
    int64_t skipCount    = 0;
    int64_t listenTimeMs = 0;
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
    std::vector<TopEntry>   topTracks (const StatsRange& range, int limit);
    std::vector<TopEntry>   topAlbums (const StatsRange& range, int limit);
    std::vector<TopEntry>   topArtists(const StatsRange& range, int limit);
    std::vector<TopEntry>   topGenres (const StatsRange& range, int limit);
    std::vector<HourBucket> hourHistogram (const StatsRange& range);
    std::vector<DayBucket>  dailyListening(const StatsRange& range);
    std::vector<PlayEvent>  recentlyPlayed(int limit);
    TrackStats trackTotals(const std::string& trackKey);

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

    ~Db() { close(); }

private:
    struct Impl;
    Impl* impl_ = nullptr;
};
