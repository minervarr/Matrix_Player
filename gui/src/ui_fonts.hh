#pragma once
#include <string>

#include "ui_icons.gen.h"   // kIconSetFingerprint

// The UI type family and the atlas cache name, in ONE place.
//
// PlayerWindow and ArtWindow each build their own MsdfFont but deliberately
// SHARE the on-disk cache, so whichever opens second gets a cache hit instead
// of re-rasterising. That only works if both agree on every path — they used
// to carry their own duplicated string literals (and ArtWindow kept a third
// copy for its Windows branch), which is exactly how such things drift apart.
namespace ui_fonts {

// New Computer Modern — the same Computer Modern lineage as the Latin Modern
// faces this replaced, and already the source of the Greek/Cyrillic fallback
// coverage (see bakeFallbackGlyphs), so base text and fallback text are now
// one consistent design instead of two near-identical ones.
inline const char* regular() { return "fonts/newcomputermodern/NewCM10-Regular.otf"; }
inline const char* bold()    { return "fonts/newcomputermodern/NewCM10-Bold.otf"; }
inline const char* italic()  { return "fonts/newcomputermodern/NewCM10-Italic.otf"; }
// Repurposes the unused Math style slot for a monospace face (numeric readouts
// that must not jitter as digits change) — this app never renders math.
inline const char* mono()    { return "fonts/newcomputermodern/NewCMMono10-Regular.otf"; }

// The icon font, baked into the same atlas (see ui_icons.hh).
inline const char* icons()   { return "fonts/icons/matrix-icons.otf"; }

// Cache filename, fingerprinted by the icon geometry. BARE — no directory
// component: unlike the faces above (which are read-only and ship beside the
// executable), this file is WRITTEN, so it lives in app_paths::stateDir()
// instead. Every caller joins it there; see app_paths.hh for why the two
// halves are separate at all.
//
// The bake is gated on MsdfFont::hasCodepoint(), which cannot tell that an
// icon's ARTWORK changed — only whether the codepoint exists. Folding the
// fingerprint into the filename makes any icon edit miss the old cache and
// re-bake, instead of silently drawing glyphs at their previous size. Cache
// files for superseded fingerprints are pruned by pruneStaleCaches().
inline std::string cacheFile() {
    return std::string("ui-atlas.") + kIconSetFingerprint + ".msdf.cache";
}

// What pruneStaleCaches() sweeps (in stateDir()). Deliberately just the
// suffix, not a "ui-atlas." prefix: that also catches caches written under the
// OLD naming (lmroman10-regular.msdf.cache), which would otherwise sit there
// at ~45 MB forever. Nothing else written there uses this extension.
inline const char* cacheSuffix() { return ".msdf.cache"; }

}  // namespace ui_fonts
