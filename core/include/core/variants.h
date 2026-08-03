#pragma once
#include "core/library.h"
#include <string>
#include <vector>

// Album variants: the same release held more than once in the library, either
// at a different QUALITY (the folder convention's "(24-96)" suffix, already
// parsed into Album::quality / Album::displayName) or as a different EDITION
// (Deluxe, Edición Especial, Remastered). The grid shows one tile per group —
// the best member — and the album view lists the rest below the artist bio.
//
// Pure string/struct logic, no OS and no GUI: core/tests/variants_test.cc
// links this translation unit on its own. Keep it that way.

// ── Name splitting ──────────────────────────────────────────────────────────
// Album/track names carry trailing bracketed "modifiers" — "(Deluxe)", "(PA)",
// "[feat. Sleepnet & Joker]", "(from the Netflix Series ...)" — and at most one
// dash-separated tail ("Senderos De Traición - Edición Especial"). The base
// name is what the user scans for; modifiers are secondary. Peels up to three
// stacked groups ("Tears (with X) [feat. Y]" → mod "with X · feat. Y") so
// callers can give the base name layout priority. Returns true only when
// something was split; otherwise base = s (whitespace-trimmed), mod empty.
//
// This lives in core/ rather than in the GUI because the grouping below and
// the drawing code MUST split names identically — otherwise a group's key
// disagrees with the tile that represents it.
bool splitNameModifier(const std::string& s, std::string& base, std::string& mod);

// ── Modifier classification (the "Borderless" layer, minimal form) ──────────
// What a single modifier segment MEANS, independent of the language it is
// written in. Only Edition folds two albums into one group: a remix, a live
// take or a "feat." credit is a different release, not another pressing of
// the same one.
enum class ModifierKind {
    None,      // empty
    Edition,   // deluxe / edición especial / remastered / anniversary / ...
    Remix,     // remixes / VIP remix / bootleg / rework / ...
    Other      // feat. X, with Y, from the series ..., anything unrecognised
};

// Classifies ONE segment. splitNameModifier joins stacked groups with " · ";
// callers holding a compound modifier should split on that separator first
// (variantKey does).
ModifierKind classifyModifier(const std::string& mod);

// ── Grouping ────────────────────────────────────────────────────────────────
// The key two albums must share to be the same release: release type, artist,
// and base name — case-folded and accent-folded, with Edition and Remix
// modifiers removed and any other modifier kept. So "Album" and
// "Album (Deluxe)" match, "Obsessed (Remixes)" and "Obsessed (X VIP Remix)"
// match, while "Album" and "Album (feat. X)" do not.
//
// Release type is part of the key so a group always belongs to exactly ONE of
// the grid's tabs. Without it a Single would fold together with its own remix
// EP, the group would surface only under the primary's filter, and the other
// member would vanish from the tab it belongs to.
std::string variantKey(const Album& a);

// ── Track identity ──────────────────────────────────────────────────────────
// The key listening history is recorded against. NOT the file path: a path
// changes the moment a folder is renamed or the library is reorganised, and
// history keyed on it orphans silently and unrecoverably.
//
//   fold(albumArtist ? albumArtist : artist)
//     ⨯ album, Edition modifiers stripped
//     ⨯ title, Edition modifiers stripped
//     ⨯ discNumber ⨯ trackNumber
//
// Only EDITION modifiers are stripped, from both halves — "Song (Remastered)"
// is the same recording as "Song", but "Song (X VIP Remix)" is different music
// and must keep its own history. That is the one place this differs from
// variantKey(), which drops Remix too because release type already keeps remix
// sets in their own grid tab; there is no release type here to do that job.
//
// Duration is deliberately NOT part of the key: a re-rip or a copy with
// different encoder padding differs by tens of milliseconds, while disc and
// track number disambiguate at least as well and never drift.
//
// Consequence, intended: the 16-44 and the 24-96 copy of a track share one
// history. That matches how variantKey() already treats those albums as one
// release — upgrading a file's quality should not split "I played this 200
// times" in two.
//
// An untitled track falls back to its filename stem, so untagged files still
// get a stable key rather than colliding with every other untagged file by
// the same artist.
std::string trackKey(const Track& t);

// Strict "a is the better member" ordering, per TODO.md's decided ranking:
// DSD outranks PCM, then sample rate, then bit depth, then track count —
// quality wins over completeness, so a DSD standard edition takes the tile and
// a PCM deluxe drops into the variant strip. Album::name breaks the final tie
// so the result is stable across runs.
bool variantOutranks(const Album& a, const Album& b);

// ── Release-type classification ─────────────────────────────────────────────
// Album / EP / Single / Remix / Compilation / Live, ported from the sibling Android
// player's AlbumDao.classifyRelease(): track-count thresholds (1=Single,
// 2-4=EP, >4=Album), overridden by remix detection — the album name saying so,
// or a strict majority of track titles matching remix patterns.
//
// With ONE addition this app makes: on a MULTI-DISC release the majority is
// counted per disc, and the release only counts as a remix set if every disc
// is a remix disc. A two-disc album whose second disc is remixes and whose
// first is new material is an album with a bonus disc, not a remix release —
// and by flat track count it would otherwise tip over the majority and land in
// the wrong tab (Anyma's *Genesys II*: 10 originals, 11 remixes).
// Compilation is decided by the album NAME ALONE, and deliberately so. The two
// signals that look obvious — a spread of release years across the tracks, and
// track titles that already exist on other records — were both measured on the
// real library and neither works:
//
//   * Taggers write ONE date per release onto every track, so the year spread
//     inside an album is zero everywhere. The original years do survive, but in
//     the ℗ line of COPYRIGHT, and there a career-spanning *special edition*
//     outscores an actual anthology (1986-2000 vs 1987-2000), while an ordinary
//     album with lead singles already spreads two or three years.
//   * Title overlap ranks ordinary albums FIRST: a record whose deluxe twin is
//     also on disk scores 100%, above a real anthology's 69%. It is also
//     relative to what else is in the library, so adding or deleting one album
//     would silently reclassify others — and this function has to stay pure and
//     per-album, because its answer is cached in Db's albums table.
//
// The name is what the tables below can actually read, it is the mechanism
// isRemixAlbum() already uses, and it gives the same answer on every machine.
bool isCompilationAlbum(const std::string& albumName);

// A live record is decided the same way a remix set is — the album name, or a
// majority of track titles, counted PER DISC. The per-disc rule is what keeps
// the "Edición Especial" reissues out: each pairs a studio disc with a live
// bonus disc, so flat they read 32-48% live, and per disc their first disc
// answers no.
//
// Unlike the remix test, a title only counts when the marker sits inside a
// BRACKET: "live" is an ordinary English word, and a studio track called "Live
// and Let Die" must not vote.
bool isLiveTrackTitle(const std::string& title);

bool isRemixTrackTitle(const std::string& title);
Album::ReleaseType classifyReleaseType(const std::string& albumName,
                                       const std::vector<Track>& tracks);

// ── Format label ────────────────────────────────────────────────────────────
// What to print under a variant so it can be told from its siblings: "DSD",
// "MP3", or "" for everything else.
//
// Only the two that CHANGE THE MEANING of the numbers get a label. MP3 is
// lossy — its "16/44.1" is reconstructed, not the source — so it has to be
// said out loud. DSD is a different representation altogether. FLAC and WAV
// are lossless PCM and carry no surprise, so labelling them would be noise on
// every tile in the library.
//
// Read from Album::hasDsd first, then from the tracks' file extensions. The
// scanner does not index .mp3/.dsf/.dff yet (see TODO.md), so in practice this
// returns "" for every album today; it starts speaking the moment those files
// can enter the library.
std::string variantFormatLabel(const Album& a);

struct AlbumGroup {
    int              primary = -1;  // index into the albums vector — the tile
    std::vector<int> members;       // ALL members incl. primary, best first
};

// Partitions albums into groups. Total: every album lands in exactly one
// group, and an album with no siblings forms a group of one whose primary is
// itself. Group order follows each group's primary index, so the grid's order
// does not jump around between scans.
std::vector<AlbumGroup> groupAlbumVariants(const std::vector<Album>& albums);
