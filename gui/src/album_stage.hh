#pragma once

#include "album_disc.hh"

#include <algorithm>

// The album page's one group: the sleeve, the lines under or beside it,
// this disc's track list, and the optional ruler.
//
// The sleeve is the anchor. It is the only thing on the page that arrived
// with the record — titles, durations, the disc label and the ruler are
// drawn by the app. In the wide layout the track list's frame is centred
// on the sleeve (the picture, not the lines under it), and the whole
// group is centred between the bars. A single disc whose tracks fit has
// no ruler, so the group is narrower and the side margins grow. Another
// disc, or a disc that scrolls, adds one column of strokes and the
// margins shrink by that column. The strokes stack downward: a longer
// disc adds strokes under the ones it already has, and never beside
// them, so the column is one width on every disc and changing disc does
// not move the sleeve. Every stroke is the same width. The one on screen
// is a different colour, drawn by the caller.
//
// Tall albums use the same group. The sleeve is its left edge, the lines
// are centred in the column beside the sleeve, and the list is the width
// of that header, starting under it. The ruler sits to the right of the
// list and is part of the group's width, so the picture, the lines, the
// list and the strokes centre together. The strokes are centred on the
// list's full height. Omitting the column narrows the group and the
// side margins grow. Tall singles are a different picture: the sleeve at
// the top, centred, the lines under it, and the one row centred in what
// remains. A single has no ruler.
//
// The frame is one rectangle for every disc of the album. A short disc
// centres its rows inside that rectangle, so a single track sits on the
// sleeve's middle. The rectangle does not follow the track count, so
// changing disc does not move the sleeve.

// OS/2 sCapHeight / unitsPerEm of the faces a track row draws.
// NewCM10-Regular is 683/1000, Bold 686/1000 — close enough to share.
// NewCMMono10-Regular (the Math slot: track numbers and durations) is
// 611/1000. Re-measure if those files change.
inline constexpr float kAlbumRowCapEm     = 0.683f;
inline constexpr float kAlbumRowMonoCapEm = 0.611f;

// The column names each disc. Past this many, the first ones are the
// column and the rest are not drawn; the frame and the list are
// unaffected.
inline constexpr int kAlbumStageMaxDiscs = 64;

// textStyled() takes the top of the em and puts the baseline at top+size.
// Capitals and lining figures stand on that baseline, so centring the em
// leaves them low — the floor the row used to share with the quality
// mark. The returned top puts the cap-band's middle on centerY.
inline float albumRowTextTop(float centerY, float sizePx, float capEm) {
    return centerY - sizePx * (1.0f - 0.5f * capEm);
}

// Which stroke of this disc is the one on screen. `pages` is that disc's
// group. A disc that fits is one stroke, and the answer is 0. The scroll
// range is split into `pages` equal parts; the end belongs to the last.
inline int albumScrollMarkActive(int scrollY, int contentH, int viewH, int pages) {
    if (pages <= 1 || viewH <= 0 || contentH <= viewH) return 0;
    int range = contentH - viewH;
    if (range <= 0) return 0;
    if (scrollY < 0) scrollY = 0;
    if (scrollY > range) scrollY = range;
    int idx = (int)((long long)scrollY * (long long)pages / (long long)range);
    if (idx >= pages) idx = pages - 1;
    if (idx < 0) idx = 0;
    return idx;
}

// How many strokes one disc contributes, from the same extent the list
// scrolls. One when the page fits. One per screen when it does not — a
// disc three screens long is three strokes.
inline int albumDiscPages(int contentH, int viewH) {
    if (viewH <= 0 || contentH <= viewH) return 1;
    int pages = (contentH + viewH - 1) / viewH;
    return pages < 2 ? 2 : pages;
}

// The current disc's rows, plus the variant strip on the last disc.
// The trailing pad albumDiscContentH adds is slack for the scroll end,
// not another track, so it does not count as overflow.
inline bool albumStageBlockOverflows(float rowViewH, int trackCount,
                                    float pitch, float stripH) {
    if (trackCount < 0) trackCount = 0;
    if (pitch < 0.0f) pitch = 0.0f;
    if (stripH < 0.0f) stripH = 0.0f;
    return (float)trackCount * pitch + stripH > rowViewH + 0.5f;
}

// Fitted lengths for the column. Preferred sizes are kept while the
// stack sits inside `cap` (the sleeve). Past that, strokes shorten
// first — a row is the reference, not a minimum — then the gap inside
// a disc, then the gap between discs. The between-disc gap stays wider
// than the inner one for as long as the sleeve allows, so two discs
// still read as two groups.
struct AlbumRulerFit {
    float markH = 0;
    float gap = 0;
    float discGap = 0;
    float stackH = 0;
};

inline AlbumRulerFit fitAlbumRuler(int marks, int innerGaps, int discBreaks,
                                   float markPref, float markMin,
                                   float gapPref, float gapMin,
                                   float discPref, float discMin,
                                   float cap) {
    AlbumRulerFit f;
    if (marks < 0) marks = 0;
    if (innerGaps < 0) innerGaps = 0;
    if (discBreaks < 0) discBreaks = 0;
    if (markPref < 0.0f) markPref = 0.0f;
    if (gapPref < 0.0f) gapPref = 0.0f;
    if (discPref < 0.0f) discPref = 0.0f;
    if (markMin < 0.0f) markMin = 0.0f;
    if (gapMin < 0.0f) gapMin = 0.0f;
    if (discMin < 0.0f) discMin = 0.0f;
    if (markMin > markPref) markMin = markPref;
    if (gapMin > gapPref) gapMin = gapPref;
    if (discMin > discPref) discMin = discPref;

    float markH = markPref;
    float gap = innerGaps > 0 ? gapPref : 0.0f;
    float dgap = discBreaks > 0 ? discPref : 0.0f;
    auto height = [&]() {
        return (float)marks * markH + (float)innerGaps * gap + (float)discBreaks * dgap;
    };
    auto commit = [&]() {
        f.markH = markH;
        f.gap = gap;
        f.discGap = dgap;
        f.stackH = height();
    };
    if (marks == 0 || !(cap > 0.0f) || height() <= cap) {
        commit();
        return f;
    }

    if (marks > 0 && markH > markMin) {
        const float room = cap - (float)innerGaps * gap - (float)discBreaks * dgap;
        const float fitted = room / (float)marks;
        if (fitted >= markMin) {
            markH = fitted;
            commit();
            return f;
        }
        markH = markMin;
    }
    if (height() <= cap) {
        commit();
        return f;
    }

    if (innerGaps > 0 && gap > gapMin) {
        const float room = cap - (float)marks * markH - (float)discBreaks * dgap;
        const float fitted = room / (float)innerGaps;
        if (fitted >= gapMin) {
            gap = fitted;
            commit();
            return f;
        }
        gap = gapMin;
    }
    if (height() <= cap) {
        commit();
        return f;
    }

    float dFloor = discMin;
    if (discBreaks > 0 && innerGaps > 0) {
        const float distinct = gap + std::max(gapMin, 1.0f);
        if (distinct > dFloor) dFloor = distinct;
        if (dFloor > dgap) dFloor = dgap;
    }
    if (discBreaks > 0 && dgap > dFloor) {
        const float room = cap - (float)marks * markH - (float)innerGaps * gap;
        const float fitted = room / (float)discBreaks;
        if (fitted >= dFloor) {
            dgap = fitted;
            commit();
            return f;
        }
        dgap = dFloor;
    }
    if (height() <= cap) {
        commit();
        return f;
    }

    // The separations have given what they can. Shorten the strokes
    // further, down to a pixel, before the whole column is scaled.
    if (marks > 0) {
        const float room = cap - (float)innerGaps * gap - (float)discBreaks * dgap;
        if (room >= (float)marks) {
            markH = room / (float)marks;
            commit();
            return f;
        }
    }
    const float nat = height();
    if (nat > cap && nat > 0.0f) {
        const float k = cap / nat;
        markH *= k;
        gap *= k;
        dgap *= k;
    }
    commit();
    return f;
}

struct AlbumStageInput {
    bool  wide = true;
    bool  single = false;          // one track: no number
    float panelX = 0, panelY = 0, panelW = 0, panelH = 0;
    float pad = 0;
    float artGap = 0;              // under the sleeve; beside it when tall
    float belowGap = 0;            // tall: between the header and the list
    float columnGap = 0;           // wide: sleeve to the text column
    float rulerGap = 0;            // wide: row box to the ruler
    float artSize = 0;
    float identH = 0;              // the lines under or beside the sleeve
    float sceneBand = 0;
    float discBand = 0;            // footer when the album has another disc
    float discHeaderH = 0;
    float readingMeasure = 0;      // the text column, before the row overhang
    float rowOverhang = 0;
    float rowPitch = 0;
    float minList = 0;             // text column: give the ruler up before this
    int   trackCount = 0;          // the disc on screen
    float stripH = 0;              // variant strip on the disc on screen
    float contentPad = 0;          // albumDiscContentH's trailing pad
    int   discCount = 1;
    int   discIndex = 0;
    // Every disc, so the column is the album's and does not change when
    // the page does. discCount == 1 reads trackCount instead.
    int   discTracks[kAlbumStageMaxDiscs] = {};
    float lastStripH = 0;          // variant strip on the last disc
    float markThin = 0;            // every stroke, and the column's width
    float markH = 0;               // preferred stroke length: one row
    float markHMin = 0;
    float markGap = 0;             // inside one disc
    float markGapMin = 0;
    float discGap = 0;             // between discs — wider than markGap
    float discGapMin = 0;
};

struct AlbumStage {
    float artX = 0, artY = 0, artSize = 0;
    float textX = 0, textY = 0, textW = 0;
    bool  centerLines = true;

    float listX = 0, listY = 0, listW = 0, listH = 0;
    int   listLeft = 0, listTop = 0, listRight = 0, listBot = 0;
    float headerH = 0, headerY = 0;
    float rowViewH = 0;
    float rowOrigin = 0;           // first row at scroll 0
    float rowX = 0, rowW = 0;
    float colX = 0, colW = 0;

    float footerX = 0, footerY = 0, footerW = 0, footerH = 0;

    float groupX = 0, groupW = 0;

    float rulerX = 0, rulerY = 0, rulerW = 0, rulerH = 0;
    int   discGroups = 0;
    int   discPages[kAlbumStageMaxDiscs] = {};
    int   markCount = 0;
    int   discActive = 0;
    float markH = 0;
    float markThin = 0, markGap = 0, discGap = 0;

    bool  showTrackNumber = true;
    bool  overflows = false;
    int   scrollContentH = 0;
    int   scrollViewH = 0;
};

struct AlbumMark {
    float x = 0, y = 0, w = 0, h = 0;
    bool  lit = false;
};

inline int albumStageDiscCount(const AlbumStageInput& in) {
    int n = in.discCount;
    if (n < 1) n = 1;
    if (n > kAlbumStageMaxDiscs) n = kAlbumStageMaxDiscs;
    return n;
}

inline int albumStageDiscTrackCount(const AlbumStageInput& in, int index, int nDiscs) {
    if (nDiscs <= 1) return std::max(0, in.trackCount);
    if (index < 0 || index >= nDiscs) return 0;
    return std::max(0, in.discTracks[index]);
}

// The variant strip belongs to the last disc even when another disc is
// on screen, so that disc's group does not grow as the page changes.
inline float albumStageDiscStrip(const AlbumStageInput& in, int index, int nDiscs) {
    if (index != nDiscs - 1) return 0.0f;
    float strip = std::max(0.0f, in.lastStripH);
    if (nDiscs <= 1) strip = std::max(strip, std::max(0.0f, in.stripH));
    return strip;
}

inline AlbumStage layoutAlbumStage(const AlbumStageInput& in);
inline void placeAlbumRuler(AlbumStage& s, const AlbumStageInput& in);

inline AlbumStage layoutAlbumStage(const AlbumStageInput& in) {
    AlbumStage s;
    s.artSize = std::max(0.0f, in.artSize);
    s.headerH = std::max(0.0f, in.discHeaderH);
    s.showTrackNumber = !in.single;
    s.markThin = std::max(0.0f, in.markThin);
    s.markGap = std::max(0.0f, in.markGap);
    s.discGap = std::max(0.0f, in.discGap);
    s.discActive = in.discIndex;
    if (s.discActive < 0) s.discActive = 0;
    {
        const int nDiscs = albumStageDiscCount(in);
        if (s.discActive >= nDiscs) s.discActive = nDiscs - 1;
    }

    const float pad = std::max(0.0f, in.pad);
    const float innerL = in.panelX + pad;
    const float innerT = in.panelY + pad;
    const float innerW = std::max(0.0f, in.panelW - pad * 2.0f);
    const float columnH = std::max(0.0f, in.panelH - pad * 2.0f - std::max(0.0f, in.sceneBand));
    const float footerH = std::max(0.0f, in.discBand);
    const int tracks = std::max(0, in.trackCount);
    const float pitch = std::max(0.0f, in.rowPitch);
    const float strip = std::max(0.0f, in.stripH);
    const float rowsBlock = (float)tracks * pitch + strip;

    if (in.wide) {
        // Sleeve + the lines under it, centred in the column. The picture's
        // own middle is the anchor; the lines hang below it.
        const float identBlock = s.artSize + std::max(0.0f, in.artGap) + std::max(0.0f, in.identH);
        s.artY = innerT;
        if (identBlock < columnH)
            s.artY = innerT + (columnH - identBlock) * 0.5f;
        s.textX = 0; // filled once artX is known
        s.textY = s.artY + s.artSize + std::max(0.0f, in.artGap);
        s.textW = s.artSize;
        s.centerLines = true;

        const float artCenter = s.artY + s.artSize * 0.5f;
        const float bandTop = innerT + s.headerH;
        const float bandBot = innerT + columnH - footerH;
        float rowView = 0.0f;
        if (bandBot > bandTop) {
            const float up = artCenter - bandTop;
            const float dn = bandBot - artCenter;
            if (up > 0.0f && dn > 0.0f)
                rowView = 2.0f * std::min(up, dn);
            if (pitch > 0.0f && rowView < pitch && (bandBot - bandTop) >= pitch)
                rowView = std::min(bandBot - bandTop, std::max(rowView, pitch));
            if (rowView < 1.0f)
                rowView = bandBot - bandTop;
        }
        s.rowViewH = std::max(0.0f, rowView);
        s.listH = s.headerH + s.rowViewH;
        s.listY = artCenter - s.rowViewH * 0.5f - s.headerH;
        const float minY = innerT;
        const float maxY = innerT + columnH - footerH - s.listH;
        if (s.listY < minY) s.listY = minY;
        if (maxY >= minY && s.listY > maxY) s.listY = maxY;
        if (maxY < minY) {
            s.listY = minY;
            s.listH = std::max(0.0f, innerT + columnH - footerH - minY);
            s.rowViewH = std::max(0.0f, s.listH - s.headerH);
        }

        // One column, or none. Strokes stack inside it, so neither another
        // disc nor a longer track list widens the group. The column is the
        // thin stroke: lighting one does not change its width.
        const int nDiscs = albumStageDiscCount(in);
        bool anyLong = false;
        for (int i = 0; i < nDiscs; ++i) {
            const int t = albumStageDiscTrackCount(in, i, nDiscs);
            const float dstrip = albumStageDiscStrip(in, i, nDiscs);
            if (albumStageBlockOverflows(s.rowViewH, t, pitch, dstrip))
                anyLong = true;
        }
        float rw = 0.0f;
        if (s.artSize > 0.0f && s.markThin > 0.0f && (nDiscs > 1 || anyLong))
            rw = s.markThin;
        auto groupOf = [&](float col, float ruler) {
            return s.artSize + std::max(0.0f, in.columnGap)
                 + col + 2.0f * std::max(0.0f, in.rowOverhang)
                 + (ruler > 0.0f ? std::max(0.0f, in.rulerGap) + ruler : 0.0f);
        };

        float col = std::max(0.0f, in.readingMeasure);
        float group = groupOf(col, rw);
        if (group > innerW && col > in.minList) {
            float over = group - innerW;
            float cut = std::min(over, col - std::max(0.0f, in.minList));
            col -= cut;
            group -= cut;
        }
        if (group > innerW && rw > 0.0f) {
            rw = 0.0f;
            group = groupOf(col, rw);
        }
        if (group > innerW) {
            float room = innerW - s.artSize - std::max(0.0f, in.columnGap);
            col = std::max(1.0f, room - 2.0f * std::max(0.0f, in.rowOverhang));
            rw = 0;
            group = groupOf(col, 0);
            if (group > innerW) {
                // The sleeve alone is wider than the padded rect. Keep the
                // list at one pixel and let the sleeve stay; the caller
                // already shrank it for height.
                col = 1.0f;
                group = groupOf(col, 0);
            }
        }
        if (group < innerW && in.readingMeasure > col) {
            // col may have been cut while a ruler was still in the budget,
            // then the ruler was dropped. Give the list back what now fits.
            float room = innerW - (group - col);
            col = std::min(in.readingMeasure, std::max(col, room));
            group = groupOf(col, rw);
        }

        s.rulerW = rw;
        s.colW = std::max(1.0f, col);
        s.rowW = s.colW + 2.0f * std::max(0.0f, in.rowOverhang);
        s.groupW = s.artSize + std::max(0.0f, in.columnGap) + s.rowW
                 + (s.rulerW > 0.0f ? std::max(0.0f, in.rulerGap) + s.rulerW : 0.0f);
        if (s.groupW > innerW) s.groupW = innerW;
        s.groupX = innerL + std::max(0.0f, (innerW - s.groupW) * 0.5f);
        s.artX = s.groupX;
        s.textX = s.artX;
        s.colX = s.artX + s.artSize + std::max(0.0f, in.columnGap);
        s.rowX = s.colX - std::max(0.0f, in.rowOverhang);
        s.listX = s.rowX;
        s.listW = s.rowW;
        if (s.rulerW > 0.0f)
            s.rulerX = s.rowX + s.rowW + std::max(0.0f, in.rulerGap);
    } else if (in.single) {
        // Tall single. The sleeve is at the top, centred inside the pad.
        // The lines sit under it, centred on the same axis. The one row
        // is centred in the area under those lines. Nothing about this
        // picture changes with the track count of some other album: a
        // single has no ruler and no disc column.
        s.centerLines = true;
        s.rulerW = 0;
        const float gap = std::max(0.0f, in.artGap);
        const float under = std::max(0.0f, in.belowGap);
        const float identH = std::max(0.0f, in.identH);
        const float over = std::max(0.0f, in.rowOverhang);
        s.artY = innerT;
        s.artX = innerL + std::max(0.0f, (innerW - s.artSize) * 0.5f);
        s.textW = std::min(std::max(0.0f, in.readingMeasure), innerW);
        if (s.textW < 1.0f) s.textW = std::max(1.0f, innerW);
        s.textX = innerL + std::max(0.0f, (innerW - s.textW) * 0.5f);
        s.textY = s.artY + s.artSize + gap;

        float col = s.textW;
        float rowW = col + 2.0f * over;
        if (rowW > innerW && innerW > 0.0f) {
            rowW = innerW;
            col = std::max(1.0f, rowW - 2.0f * over);
        }
        s.colW = std::max(1.0f, col);
        s.rowW = std::max(1.0f, rowW);
        s.listW = s.rowW;
        s.listX = innerL + std::max(0.0f, (innerW - s.listW) * 0.5f);
        s.rowX = s.listX;
        s.colX = s.listX + over;

        const float remTop = s.textY + identH + under;
        const float remBot = innerT + columnH - footerH;
        const float remH = std::max(0.0f, remBot - remTop);
        const float content = s.headerH + rowsBlock;
        if (content <= remH) {
            s.listH = content;
            s.listY = remTop + (remH - content) * 0.5f;
        } else {
            s.listY = remTop;
            s.listH = remH;
        }
        s.rowViewH = std::max(0.0f, s.listH - s.headerH);

        const float gL = std::min(s.artX, std::min(s.textX, s.listX));
        const float gR = std::max(s.artX + s.artSize,
                          std::max(s.textX + s.textW, s.listX + s.listW));
        s.groupX = gL;
        s.groupW = std::max(0.0f, gR - gL);
    } else {
        // Tall album. Sleeve, lines, list and the optional ruler are one
        // group, centred. The lines are centred in the column beside the
        // sleeve, and that column is vertically centred on the picture.
        // The list is the header's width and starts under it; a short
        // disc stays at the top of that area. The ruler is to the right
        // of the list, centred on the list's full height. Dropping it
        // narrows the group.
        s.centerLines = true;
        const float gap = std::max(0.0f, in.artGap);
        const float under = std::max(0.0f, in.belowGap);
        const float identH = std::max(0.0f, in.identH);
        const float headerBlock = std::max(s.artSize, identH);
        s.artY = innerT + std::max(0.0f, (headerBlock - s.artSize) * 0.5f);
        s.textY = innerT + std::max(0.0f, (headerBlock - identH) * 0.5f);
        s.listY = innerT + headerBlock + under;
        s.listH = std::max(0.0f, innerT + columnH - footerH - s.listY);
        s.rowViewH = std::max(0.0f, s.listH - s.headerH);

        const int nDiscs = albumStageDiscCount(in);
        bool anyLong = false;
        for (int i = 0; i < nDiscs; ++i) {
            const int t = albumStageDiscTrackCount(in, i, nDiscs);
            const float dstrip = albumStageDiscStrip(in, i, nDiscs);
            if (albumStageBlockOverflows(s.rowViewH, t, pitch, dstrip))
                anyLong = true;
        }
        float rw = 0.0f;
        if (s.artSize > 0.0f && s.markThin > 0.0f && (nDiscs > 1 || anyLong))
            rw = s.markThin;
        // The sleeve and the lines share one block: the reading measure,
        // not the panel. The ruler is the block's right edge, so the
        // whole group — picture, lines, list, strokes — centres, and the
        // strokes are not a mark left on the panel's edge.
        const float minCol = std::max(1.0f, in.minList);
        auto fitBlock = [&](float rulerExtra) {
            const float cap = std::max(0.0f, innerW - rulerExtra);
            float block = std::min(std::max(0.0f, in.readingMeasure), cap);
            float col = block - s.artSize - gap;
            if (!(col >= minCol)) {
                const float need = s.artSize + gap + minCol;
                if (need <= cap) {
                    block = need;
                    col = minCol;
                } else {
                    block = cap;
                    col = block - s.artSize - gap;
                }
            }
            if (col < 1.0f) {
                col = 1.0f;
                block = std::min(cap, s.artSize + gap + col);
                if (block < s.artSize + gap + col)
                    col = std::max(1.0f, block - s.artSize - gap);
            }
            return std::pair<float, float>(block, col);
        };
        float extra = rw > 0.0f ? std::max(0.0f, in.rulerGap) + rw : 0.0f;
        auto fitted = fitBlock(extra);
        if (fitted.first + extra > innerW + 0.5f && rw > 0.0f) {
            rw = 0.0f;
            extra = 0.0f;
            fitted = fitBlock(extra);
        }
        const float col = std::max(1.0f, fitted.second);
        const float block = s.artSize + gap + col;

        s.rulerW = rw;
        s.textW = col;
        s.groupW = block + extra;
        if (s.groupW > innerW) s.groupW = innerW;
        s.groupX = innerL + std::max(0.0f, (innerW - s.groupW) * 0.5f);
        s.artX = s.groupX;
        s.textX = s.artX + s.artSize + gap;
        s.listX = s.groupX;
        s.listW = block;
        s.colW = std::max(1.0f, s.listW);
        s.colX = s.listX;
        s.rowX = s.listX;
        s.rowW = s.listW;
        if (s.rulerW > 0.0f)
            s.rulerX = s.listX + s.listW + std::max(0.0f, in.rulerGap);
    }

    s.overflows = albumStageBlockOverflows(s.rowViewH, tracks, pitch, strip);
    // Wide: a short disc centres its rows on the sleeve, inside the frame.
    // Tall: the list begins under the sleeve, short or long.
    if (in.wide && !s.overflows) {
        const float slack = std::max(0.0f, s.listH - s.headerH - rowsBlock);
        s.headerY = s.listY + slack * 0.5f;
        s.rowOrigin = s.headerY + s.headerH;
    } else {
        s.headerY = s.listY;
        s.rowOrigin = s.listY + s.headerH;
    }

    s.footerH = footerH;
    s.footerX = s.rowX;
    s.footerY = s.listY + s.listH;
    s.footerW = s.rowW;

    s.listLeft = (int)s.listX;
    s.listTop = (int)s.listY;
    s.listRight = (int)(s.listX + s.listW);
    s.listBot = (int)(s.listY + s.listH);
    if (s.listRight < s.listLeft) s.listRight = s.listLeft;
    if (s.listBot < s.listTop) s.listBot = s.listTop;
    s.scrollViewH = s.listBot - s.listTop;
    if (!s.overflows) {
        s.scrollContentH = s.scrollViewH;
    } else {
        s.scrollContentH = albumDiscContentH(s.headerH, tracks, (int)pitch, strip,
                                            std::max(0.0f, in.contentPad));
    }
    placeAlbumRuler(s, in);
    return s;
}

inline void placeAlbumRuler(AlbumStage& s, const AlbumStageInput& in) {
    s.markCount = 0;
    s.discGroups = 0;
    s.markH = 0;
    if (s.rulerW <= 0.0f || s.artSize <= 0.0f) {
        s.rulerY = 0;
        s.rulerH = 0;
        return;
    }
    const int nDiscs = albumStageDiscCount(in);
    const float pitch = std::max(0.0f, in.rowPitch);
    int marks = 0;
    int innerGaps = 0;
    for (int i = 0; i < nDiscs; ++i) {
        const int t = albumStageDiscTrackCount(in, i, nDiscs);
        const float dstrip = albumStageDiscStrip(in, i, nDiscs);
        const bool over = albumStageBlockOverflows(s.rowViewH, t, pitch, dstrip);
        int pages = 1;
        if (over) {
            const int content = albumDiscContentH(
                s.headerH, t, (int)pitch, dstrip, std::max(0.0f, in.contentPad));
            pages = std::max(2, albumDiscPages(content, s.scrollViewH));
        }
        s.discPages[i] = pages;
        marks += pages;
        if (pages > 1) innerGaps += pages - 1;
    }
    for (int i = nDiscs; i < kAlbumStageMaxDiscs; ++i)
        s.discPages[i] = 0;
    const int discBreaks = nDiscs > 1 ? nDiscs - 1 : 0;
    // Wide: the column is centred on the sleeve and capped to it, because
    // the rows are centred on the sleeve too. Tall: the column stays to
    // the right of the list and is centred on the list's full height,
    // capped to that height.
    float cap = s.artSize;
    if (!in.wide) cap = std::max(0.0f, s.listH);
    const AlbumRulerFit fit = fitAlbumRuler(
        marks, innerGaps, discBreaks,
        std::max(0.0f, in.markH), std::max(0.0f, in.markHMin),
        std::max(0.0f, in.markGap), std::max(0.0f, in.markGapMin),
        std::max(0.0f, in.discGap), std::max(0.0f, in.discGapMin),
        cap);
    s.discGroups = nDiscs;
    s.markCount = marks;
    s.markH = fit.markH;
    s.markGap = fit.gap;
    s.discGap = fit.discGap;
    s.rulerH = fit.stackH;
    if (in.wide) {
        const float mid = s.artY + s.artSize * 0.5f;
        s.rulerY = mid - s.rulerH * 0.5f;
    } else {
        const float mid = s.listY + s.listH * 0.5f;
        s.rulerY = mid - s.rulerH * 0.5f;
    }
}

// One column. Every stroke is markThin, so lighting another page does
// not move the column and does not change a stroke's width. The caller
// paints the lit stroke in the primary colour. Inside a disc the gap is
// markGap; the next disc starts after discGap.
inline int albumStageMarks(const AlbumStage& s, int activePage,
                          AlbumMark* out, int cap) {
    if (!out || cap <= 0 || s.rulerW <= 0.0f || s.markCount <= 0 || s.markH <= 0.0f)
        return 0;
    if (s.discGroups <= 0) return 0;
    int page = activePage;
    if (page < 0) page = 0;
    if (s.discActive >= 0 && s.discActive < s.discGroups) {
        const int pages = s.discPages[s.discActive];
        if (pages > 0 && page >= pages) page = pages - 1;
    } else {
        page = -1;
    }
    int n = 0;
    float y = s.rulerY;
    const float slot = s.rulerW;
    for (int d = 0; d < s.discGroups; ++d) {
        const int pages = s.discPages[d] > 0 ? s.discPages[d] : 0;
        for (int p = 0; p < pages && n < cap; ++p) {
            const bool lit = d == s.discActive && p == page;
            float w = s.markThin;
            if (w < 0.0f) w = 0.0f;
            if (w > slot) w = slot;
            AlbumMark m;
            m.x = s.rulerX + (slot - w) * 0.5f;
            m.y = y;
            m.w = w;
            m.h = s.markH;
            m.lit = lit;
            out[n++] = m;
            y += s.markH;
            if (p + 1 < pages) y += s.markGap;
        }
        if (d + 1 < s.discGroups) y += s.discGap;
    }
    return n;
}

// The artist page, above the prose. The portrait is centred in the inner
// canvas (the panel inside the pad) on both axes. The name sits under it,
// and the prose starts after the name. Nothing is reserved above the
// portrait: the up-arrow is a cue the caller draws when the page is about
// to turn back, and it must not push the portrait down to make room.
// heroGap is the only gap between portrait and name. afterHero is the
// break before the first description. The descriptions are drawn by the
// caller, in the column, starting at proseY.
struct ArtistPageInput {
    float panelX = 0, panelY = 0, panelW = 0, panelH = 0;
    float pad = 0;
    float heroGap = 0;
    float afterHero = 0;
    float imageSize = 0;
    float nameH = 0;
    float columnW = 0;
};

struct ArtistPage {
    float arrowCx = 0, arrowCy = 0;
    float imageX = 0, imageY = 0, imageSize = 0;
    float nameX = 0, nameY = 0, nameW = 0;
    float columnX = 0, columnW = 0;
    float proseY = 0;
};

inline ArtistPage layoutArtistPage(const ArtistPageInput& in) {
    ArtistPage p;
    const float pad = std::max(0.0f, in.pad);
    const float innerL = in.panelX + pad;
    const float innerT = in.panelY + pad;
    const float innerW = std::max(0.0f, in.panelW - pad * 2.0f);
    const float innerH = std::max(0.0f, in.panelH - pad * 2.0f);
    const float mid = innerL + innerW * 0.5f;
    // The canvas is the panel inside the margins, not the window. The
    // portrait's centre is that canvas's centre.
    const float canvasMid = innerT + innerH * 0.5f;

    p.columnW = std::max(0.0f, in.columnW);
    if (innerW > 0.0f && p.columnW > innerW) p.columnW = innerW;
    if (p.columnW < 1.0f) p.columnW = std::max(1.0f, innerW);
    p.columnX = innerL + std::max(0.0f, (innerW - p.columnW) * 0.5f);

    p.imageSize = std::max(0.0f, in.imageSize);
    if (innerW > 0.0f && p.imageSize > innerW) p.imageSize = innerW;
    p.imageX = mid - p.imageSize * 0.5f;

    const bool hasImage = p.imageSize > 0.5f;
    if (hasImage) {
        p.imageY = canvasMid - p.imageSize * 0.5f;
        // The portrait's centre stays on the canvas centre, or higher if a
        // later edit would have pushed it down, and it is never lower.
        // That is admiration for the artist — the regard kept for the
        // person who made the music — not the ordinary respect owed to
        // every human being.
        const float centre = p.imageY + p.imageSize * 0.5f;
        if (centre > canvasMid) p.imageY -= centre - canvasMid;
    } else {
        p.imageY = innerT;
    }

    // The cue sits in the air above the portrait. It is not a band the
    // portrait makes room for, so a page with no cue still centres the picture.
    p.arrowCx = mid;
    const float above = p.imageY - innerT;
    p.arrowCy = innerT + (above > 1.0f ? above * 0.5f : 0.0f);

    const bool hasName = in.nameH > 0.5f;
    float y = hasImage ? p.imageY + p.imageSize : innerT;
    if (hasName) {
        if (hasImage) y += std::max(0.0f, in.heroGap);
        p.nameW = p.columnW;
        p.nameX = p.columnX;
        p.nameY = y;
        y += std::max(0.0f, in.nameH);
    } else {
        p.nameW = 0.0f;
        p.nameX = p.columnX;
        p.nameY = y;
    }
    p.proseY = y + std::max(0.0f, in.afterHero);
    return p;
}
