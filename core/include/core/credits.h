#pragma once

#include <string>
#include <string_view>
#include <vector>

// People and companies named on a release, gathered from Vorbis comments
// and (when a downloader database is present) from that database's label
// and copyright columns. One role, then the people who hold it, in the
// order the files first named them. A name that repeats across tracks is
// one line.
//
// An act is the kind of work, and everyone in an act did that same kind.
// fromTags() groups lines by act. The sequence inside an act is a reading
// order, so the page can walk once — it is not a measure of who did more.
// Composer, lyricist and writer are one act. Arranger and remixer are
// another. Conductor through featured artist are the performance. Producer,
// engineer and mixer made the recording. Label and copyright issued it.
//
// Pure. credits_test links this translation unit alone. The file read that
// produces the "KEY=value" entries lives in library.cpp and is not part of
// this.

namespace credits {

enum class Act {
    Music,        // composer, lyricist, writer
    Setting,      // arranger, remixer — this version of a piece
    Performance,  // conductor, ensemble, soloist, performer, featured
    Recording,    // producer, engineer, mixer
    Issue         // label, copyright
};

struct CreditLine {
    Act act = Act::Music;
    std::string role;
    std::vector<std::string> people;
};

// The word the page prints over one act. One word, the same weight for
// every act that has anyone in it.
const char* actHeading(Act act);

// The arranger or the remixer authored what is playing: the release names
// one of them, and names no composer, lyricist or writer.
bool settingIsTheWork(const std::vector<CreditLine>& lines);

// True for a Vorbis key this page is willing to show. The key is compared
// case-insensitively, and spaces, underscores and hyphens are ignored, so
// "Mixing Engineer" and "MIXING_ENGINEER" are the same role.
bool isCreditTag(std::string_view key);

// `entries` are "KEY=value" comments from every track of one release.
// Unknown keys are ignored. An empty value is ignored. A value that
// contains ';' is several people.
std::vector<CreditLine> fromTags(const std::vector<std::string>& entries);

} // namespace credits
