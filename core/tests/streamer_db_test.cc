// Asserts must stay live even though the app builds Release (NDEBUG).
#undef NDEBUG
#include <cassert>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

// The REAL search-path logic, linked from src/streamer_db.cpp. Only the PURE
// half is exercised here: streamerSearchPath() opens nothing and stat()s
// nothing, which is exactly why the three layouts below can be asserted with
// no fixture library, no downloader, and no filesystem at all.
//
// What is deliberately NOT tested here: openAt() and the queries, which need
// a real sqlite file. The bug this file exists for was never in the SQL — it
// was in deciding which directory to open, and that decision is now pure.
#include "core/streamer_db.h"

namespace fs = std::filesystem;

// Paths are built through fs::path so the separators are whatever this
// platform actually uses — the same reason streamer_db.cpp does its own
// arithmetic with fs::path rather than with string concatenation.
static std::string P(std::initializer_list<const char*> parts) {
    fs::path p;
    for (const char* s : parts) p /= s;
    return p.u8string();
}

static bool has(const std::vector<std::string>& v, const std::string& want) {
    return std::find(v.begin(), v.end(), want) != v.end();
}

static size_t indexOf(const std::vector<std::string>& v, const std::string& want) {
    auto it = std::find(v.begin(), v.end(), want);
    assert(it != v.end());
    return (size_t)(it - v.begin());
}

int main() {
    // ── 1. The phone: the library sits BELOW the music root ─────────────────
    //
    // The downloader hardcodes <external>/Music/streamer as its own root
    // (streamer/gui/src/os/android_main.cc), while this app is seeded with
    // <external>/Music (android/src/main.cc). The database is therefore one
    // level BELOW our root, which the old two-probe open() could not reach
    // in either direction — it looked at the root and above it, never under.
    {
        const std::string root  = P({"/storage", "emulated", "0", "Music"});
        const std::string album = P({"/storage", "emulated", "0", "Music",
                                     "streamer", "ES", "0886446227511"});
        const std::string lib   = P({"/storage", "emulated", "0", "Music", "streamer"});

        auto sp = streamerSearchPath(album, root);
        assert(has(sp, lib));           // THE regression this file exists for
        assert(has(sp, album));
        assert(has(sp, root));

        // Nearest first. It matters: two downloader libraries can be nested,
        // and the album belongs to the closest one above it, never to the
        // outermost that happens to answer.
        assert(indexOf(sp, album) < indexOf(sp, lib));
        assert(indexOf(sp, lib) < indexOf(sp, root));
    }

    // ── 2. The desktop: the music root IS the downloader root ───────────────
    // This one worked before and must keep working — it is what made the bug
    // invisible on Linux while the phone showed no artist bio at all.
    {
        const std::string root  = P({"/home", "nava", "Uzick"});
        const std::string album = P({"/home", "nava", "Uzick", "US", "lb22hrzck0zpa"});

        auto sp = streamerSearchPath(album, root);
        assert(has(sp, root));
        assert(indexOf(sp, album) < indexOf(sp, root));
    }

    // ── 3. Root is <download_dir>/<country> ─────────────────────────────────
    // The second probe of the old open() ("<root>/../.streamer"). The walk
    // must still reach ONE level above the root, or fixing the phone would
    // have broken this layout instead.
    {
        const std::string root  = P({"/home", "nava", "Uzick", "US"});
        const std::string album = P({"/home", "nava", "Uzick", "US", "lb22hrzck0zpa"});
        const std::string above = P({"/home", "nava", "Uzick"});

        auto sp = streamerSearchPath(album, root);
        assert(has(sp, above));
        assert(indexOf(sp, root) < indexOf(sp, above));
    }

    // ── 4. Degenerate: the album folder IS the root ─────────────────────────
    {
        const std::string root = P({"/home", "nava", "Uzick"});
        auto sp = streamerSearchPath(root, root);
        assert(has(sp, root));
        assert(has(sp, P({"/home", "nava"})));     // still one above
    }

    // ── 5. Trailing separators name the same directory ──────────────────────
    // "/music/" and "/music" are one place. If they produced two answers they
    // would also produce two cache entries in PlayerWindow, and the same
    // library would be opened twice under two names.
    {
        const std::string album = P({"/storage", "emulated", "0", "Music",
                                     "streamer", "ES", "0886446227511"});
        const std::string root  = P({"/storage", "emulated", "0", "Music"});

        auto plain = streamerSearchPath(album, root);
        auto slash = streamerSearchPath(album + "/", root + "/");
        assert(plain == slash);
    }

    // ── 6. A root that does not contain the album ───────────────────────────
    // Refuse rather than climb. An unbounded walk reaches "/" and would bind
    // whatever database it met on the way, which is worse than finding none.
    {
        const std::string root  = P({"/mnt", "other"});
        const std::string album = P({"/home", "nava", "Uzick", "US", "lb22hrzck0zpa"});

        auto sp = streamerSearchPath(album, root);
        assert(sp.size() == 1);
        assert(sp[0] == album);
        assert(!has(sp, P({"/home"})));
        assert(!has(sp, P({"/home", "nava"})));
    }

    // ── 7. Empties are answers, not crashes ─────────────────────────────────
    {
        assert(streamerSearchPath("", "").empty());
        assert(streamerSearchPath("", P({"/home"})).empty());

        // No root known (nothing scanned yet): the album's own folder and
        // nothing above it. PlayerWindow refuses earlier than this, but the
        // function must not invent a walk it has no bound for.
        auto sp = streamerSearchPath(P({"/home", "nava", "Uzick", "US", "x"}), "");
        assert(sp.size() == 1);
    }

    printf("streamer_db_test: all assertions passed\n");
    return 0;
}
