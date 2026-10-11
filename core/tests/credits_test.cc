// credits_test — who a release names, gathered from Vorbis comments.
//
// Links src/credits.cpp and nothing else. No FLAC, no database: the file
// read is library.cpp's job, and this is the part that must not depend on
// a record being on disk.
#undef NDEBUG
#include "core/credits.h"

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

static const credits::CreditLine* findRole(const std::vector<credits::CreditLine>& lines,
                                           const char* role) {
    for (const auto& l : lines)
        if (l.role == role) return &l;
    return nullptr;
}

int main() {
    assert(credits::isCreditTag("Composer"));
    assert(credits::isCreditTag("MIXING ENGINEER"));
    assert(credits::isCreditTag("organization"));
    assert(!credits::isCreditTag("TITLE"));
    assert(!credits::isCreditTag("GENRE"));
    assert(!credits::isCreditTag("ARTIST"));

    // The same composer on every track is one line. Performers accumulate.
    // A semicolon is two people. Label and Organization share a role.
    // Composer and lyricist share an act. The company is a later act.
    const std::vector<std::string> tags = {
        "TITLE=Prelude",
        "ARTIST=Anne",
        "COMPOSER=Johann Sebastian Bach",
        "Performer=Anne (violin)",
        "PERFORMER=Anne (violin)",
        "LABEL=Archiv",
        "COMPOSER=Johann Sebastian Bach",
        "Performer=Ensemble X",
        "ORGANIZATION=Archiv",
        "MIXING ENGINEER=Alex",
        "COPYRIGHT=1980 Archiv",
        "PRODUCER=Wolf",
        "LYRICIST=A; B",
        "COMMENT=ignore me",
        "ENGINEER=",
    };
    const auto lines = credits::fromTags(tags);

    const char* order[] = {
        "Composer", "Lyricist", "Performer", "Producer", "Mixer", "Label", "Copyright"
    };
    const credits::Act acts[] = {
        credits::Act::Music, credits::Act::Music, credits::Act::Performance,
        credits::Act::Recording, credits::Act::Recording,
        credits::Act::Issue, credits::Act::Issue
    };
    assert(lines.size() == 7);
    for (int i = 0; i < 7; ++i) {
        assert(lines[(size_t)i].role == order[i]);
        assert(lines[(size_t)i].act == acts[i]);
    }
    assert(!credits::settingIsTheWork(lines));

    const auto* composer = findRole(lines, "Composer");
    assert(composer && composer->people.size() == 1);
    assert(composer->people[0] == "Johann Sebastian Bach");

    const auto* performer = findRole(lines, "Performer");
    assert(performer && performer->people.size() == 2);
    assert(performer->people[0] == "Anne (violin)");
    assert(performer->people[1] == "Ensemble X");

    const auto* lyricist = findRole(lines, "Lyricist");
    assert(lyricist && lyricist->people.size() == 2);
    assert(lyricist->people[0] == "A");
    assert(lyricist->people[1] == "B");

    const auto* label = findRole(lines, "Label");
    assert(label && label->people.size() == 1);
    assert(label->people[0] == "Archiv");

    const auto* mixer = findRole(lines, "Mixer");
    assert(mixer && mixer->people.size() == 1);
    assert(mixer->people[0] == "Alex");

    assert(credits::fromTags({}).empty());
    assert(credits::fromTags({ "TITLE=Only" }).empty());

    // Peers share an act. A remix is a setting of a piece, so it sits with
    // the arranger. A writer sits with the composer. Mixer stays with the
    // recording. File order is scrambled on purpose.
    const auto peers = credits::fromTags({
        "COPYRIGHT=Copy",
        "REMIXER=Remix",
        "MIXING ENGINEER=Mix",
        "COMPOSER=Bach",
        "WRITER=Song",
        "ARRANGER=Arr",
        "PRODUCER=Prod",
        "ENGINEER=Eng",
        "LYRICIST=Ly",
        "CONDUCTOR=Cond",
        "SOLOIST=Sol",
        "PERFORMER=Perf",
        "FEATUREDARTIST=Feat",
        "ORCHESTRA=Orch",
        "ENSEMBLE=Band",
        "LABEL=Lab",
    });
    const char* peerOrder[] = {
        "Composer", "Lyricist", "Writer",
        "Arranger", "Remixer",
        "Conductor", "Ensemble", "Soloist", "Performer", "Featured",
        "Producer", "Engineer", "Mixer",
        "Label", "Copyright"
    };
    const credits::Act peerActs[] = {
        credits::Act::Music, credits::Act::Music, credits::Act::Music,
        credits::Act::Setting, credits::Act::Setting,
        credits::Act::Performance, credits::Act::Performance, credits::Act::Performance,
        credits::Act::Performance, credits::Act::Performance,
        credits::Act::Recording, credits::Act::Recording, credits::Act::Recording,
        credits::Act::Issue, credits::Act::Issue
    };
    assert(peers.size() == 15);
    for (int i = 0; i < 15; ++i) {
        assert(peers[(size_t)i].role == peerOrder[i]);
        assert(peers[(size_t)i].act == peerActs[i]);
    }
    const auto* ensemble = findRole(peers, "Ensemble");
    assert(ensemble && ensemble->people.size() == 2);
    assert(ensemble->people[0] == "Orch");
    assert(ensemble->people[1] == "Band");
    assert(!credits::settingIsTheWork(peers));

    const auto remixOnly = credits::fromTags({ "REMIXER=Kai" });
    assert(remixOnly.size() == 1);
    assert(remixOnly[0].act == credits::Act::Setting);
    assert(credits::settingIsTheWork(remixOnly));
    assert(!credits::settingIsTheWork(credits::fromTags({ "PERFORMER=Anne" })));

    assert(std::string(credits::actHeading(credits::Act::Music)) == "Music");
    assert(std::string(credits::actHeading(credits::Act::Setting)) == "Arrangement");
    assert(std::string(credits::actHeading(credits::Act::Performance)) == "Performance");
    assert(std::string(credits::actHeading(credits::Act::Recording)) == "Recording");
    assert(std::string(credits::actHeading(credits::Act::Issue)) == "Release");

    printf("credits_test: all assertions passed\n");
    return 0;
}
