// album_stage_test — the album page's one group.
//
// The sleeve, the lines under it, the track list and the ruler are placed
// from plain numbers. A wrong anchor does not crash: the sleeve slides,
// a single sits at the top, or the margins go lopsided. This locks the
// geometry without a Canvas.
#undef NDEBUG
#include <cassert>
#include <cmath>
#include <cstdio>

#include "album_stage.hh"

static bool near(float a, float b) {
    return std::fabs(a - b) < 0.05f;
}

static AlbumStageInput wideInput() {
    AlbumStageInput in;
    in.wide = true;
    in.panelX = 130;
    in.panelY = 0;
    in.panelW = 1660;
    in.panelH = 1080;
    in.pad = 65;
    in.artGap = 19;
    in.belowGap = 19;
    in.columnGap = 65;
    in.rulerGap = 32;
    in.artSize = 440;
    in.identH = 120;
    in.sceneBand = 0;
    in.discBand = 0;
    in.discHeaderH = 0;
    in.readingMeasure = 820;
    in.rowOverhang = 7;
    in.minList = 280;
    in.rowPitch = 65;
    in.trackCount = 8;
    in.stripH = 0;
    in.contentPad = 65;
    in.discCount = 1;
    in.discIndex = 0;
    in.single = false;
    in.markThin = 2;
    in.markH = 65;
    in.markHMin = 12;
    in.markGap = 13;
    in.markGapMin = 4;
    in.discGap = 32;
    in.discGapMin = 19;
    return in;
}

static void setDiscs(AlbumStageInput& in, const int* counts, int n) {
    in.discCount = n;
    for (int i = 0; i < n && i < kAlbumStageMaxDiscs; ++i)
        in.discTracks[i] = counts[i];
}

static void checkInside(const AlbumStageInput& in, const AlbumStage& s) {
    const float innerL = in.panelX + in.pad;
    const float innerR = in.panelX + in.panelW - in.pad;
    assert(s.groupX >= innerL - 0.05f);
    assert(s.groupX + s.groupW <= innerR + 0.05f);
    const float left = s.groupX - innerL;
    const float right = innerR - (s.groupX + s.groupW);
    assert(near(left, right));
    assert(left >= -0.05f);
}

static void checkRowOnArt(const AlbumStage& s, int tracks, float pitch) {
    const float artCenter = s.artY + s.artSize * 0.5f;
    const float rowCenter = s.rowOrigin + 0.5f * (float)tracks * pitch;
    assert(near(rowCenter, artCenter));
}

static float markCenterX(const AlbumMark& m) {
    return m.x + m.w * 0.5f;
}

int main() {
    // Cap-band middle lands on the requested centre, for both faces.
    {
        const float top = albumRowTextTop(100.0f, 20.0f, kAlbumRowCapEm);
        const float capCenter = (top + 20.0f) - 0.5f * kAlbumRowCapEm * 20.0f;
        assert(near(capCenter, 100.0f));
        const float mono = albumRowTextTop(80.0f, 18.0f, kAlbumRowMonoCapEm);
        const float monoCenter = (mono + 18.0f) - 0.5f * kAlbumRowMonoCapEm * 18.0f;
        assert(near(monoCenter, 80.0f));
        // Centring the em is a different top: that is the floor this replaces.
        assert(!near(top, 100.0f - 10.0f));
    }

    // The lit stroke walks the disc in equal parts of the range. A list
    // that fits stays on the only stroke.
    {
        assert(albumDiscPages(400, 400) == 1);
        assert(albumDiscPages(100, 200) == 1);
        assert(albumDiscPages(401, 400) == 2);
        assert(albumDiscPages(1200, 400) == 3);
        assert(albumScrollMarkActive(0, 1000, 400, 2) == 0);
        assert(albumScrollMarkActive(299, 1000, 400, 2) == 0);
        assert(albumScrollMarkActive(300, 1000, 400, 2) == 1);
        assert(albumScrollMarkActive(600, 1000, 400, 2) == 1);
        assert(albumScrollMarkActive(0, 100, 200, 4) == 0);
        assert(albumScrollMarkActive(-10, 1000, 400, 4) == 0);
        assert(albumScrollMarkActive(0, 1000, 400, 4) == 0);
        assert(albumScrollMarkActive(149, 1000, 400, 4) == 0);
        assert(albumScrollMarkActive(150, 1000, 400, 4) == 1);
        assert(albumScrollMarkActive(300, 1000, 400, 4) == 2);
        assert(albumScrollMarkActive(450, 1000, 400, 4) == 3);
        assert(albumScrollMarkActive(900, 1000, 400, 4) == 3);
    }

    // Strokes keep a row's height while the stack fits the sleeve. Past
    // that they shorten, and the pause between discs stays wider than
    // the pause inside one.
    {
        const AlbumRulerFit fits = fitAlbumRuler(4, 3, 0, 65, 12, 13, 4, 32, 19, 440);
        assert(near(fits.markH, 65));
        assert(near(fits.gap, 13));
        assert(near(fits.discGap, 0));
        assert(near(fits.stackH, 4 * 65 + 3 * 13));

        const AlbumRulerFit tight = fitAlbumRuler(4, 2, 1, 65, 12, 13, 4, 32, 19, 100);
        assert(near(tight.markH, 12));
        assert(near(tight.gap, 10));
        assert(near(tight.discGap, 32));
        assert(tight.discGap > tight.gap);
        assert(near(tight.stackH, 100));

        const AlbumRulerFit crushed = fitAlbumRuler(20, 18, 1, 65, 12, 13, 4, 32, 19, 30);
        assert(crushed.stackH <= 30.05f);
        assert(crushed.stackH > 1.0f);
    }

    assert(!albumStageBlockOverflows(520, 8, 65, 0));
    assert(albumStageBlockOverflows(500, 8, 65, 0));
    assert(albumStageBlockOverflows(500, 40, 65, 0));

    // A single track has no number, no ruler, and sits on the sleeve.
    {
        AlbumStageInput in = wideInput();
        in.single = true;
        in.trackCount = 1;
        const AlbumStage s = layoutAlbumStage(in);
        assert(!s.showTrackNumber);
        assert(!s.overflows);
        assert(s.rulerW == 0.0f);
        assert(s.markCount == 0);
        assert(s.scrollContentH == s.scrollViewH);
        checkInside(in, s);
        checkRowOnArt(s, 1, in.rowPitch);
        assert(s.artX > in.panelX + in.pad + 1.0f);
        assert(s.centerLines);
    }

    // A normal album whose tracks fit: rows centred on the sleeve, no ruler.
    // The sleeve is not on the left pad.
    {
        AlbumStageInput in = wideInput();
        in.trackCount = 8;
        const AlbumStage s = layoutAlbumStage(in);
        assert(s.showTrackNumber);
        assert(!s.overflows);
        assert(s.rulerW == 0.0f);
        checkInside(in, s);
        checkRowOnArt(s, 8, in.rowPitch);
        assert(near(s.colW, in.readingMeasure));
    }

    // A long single disc. The frame stays centred on the sleeve, the rows
    // start at the top of that frame, and the ruler is one column of
    // row-tall strokes, centred on the sleeve.
    AlbumStage longStage;
    {
        AlbumStageInput in = wideInput();
        in.trackCount = 40;
        longStage = layoutAlbumStage(in);
        assert(longStage.overflows);
        assert(longStage.discGroups == 1);
        assert(longStage.markCount >= 2);
        assert(near(longStage.rowOrigin, longStage.listY + longStage.headerH));
        checkInside(in, longStage);
        const float artCenter = longStage.artY + longStage.artSize * 0.5f;
        const float frameRowsCenter = longStage.listY + longStage.headerH
                                    + longStage.rowViewH * 0.5f;
        assert(near(frameRowsCenter, artCenter));
        assert(near(longStage.rulerY + longStage.rulerH * 0.5f, artCenter));
        assert(longStage.rulerH <= longStage.artSize + 0.05f);
        assert(longStage.rulerX >= longStage.rowX + longStage.rowW + in.rulerGap - 0.05f);
        assert(near(longStage.rulerW, in.markThin));
        assert(near(longStage.markH, in.markH));
        assert(near(longStage.markGap, in.markGap));
        assert(longStage.scrollContentH > longStage.scrollViewH);

        const int expect = std::max(2, albumDiscPages(
            albumDiscContentH(longStage.headerH, 40, (int)in.rowPitch, 0, in.contentPad),
            longStage.scrollViewH));
        assert(longStage.markCount == expect);

        AlbumMark marks[16];
        assert(expect <= 16);
        const int n0 = albumStageMarks(longStage, 0, marks, 16);
        assert(n0 == expect);
        assert(marks[0].lit);
        assert(near(marks[0].w, in.markThin));
        assert(near(marks[0].h, in.markH));
        const float cx = markCenterX(marks[0]);
        for (int i = 1; i < n0; ++i) {
            assert(!marks[i].lit);
            assert(near(marks[i].w, marks[0].w));
            assert(near(markCenterX(marks[i]), cx));
            assert(near(marks[i].y, marks[i - 1].y + marks[i - 1].h + in.markGap));
            assert(marks[i].y > marks[0].y);
        }
        // Lighting another stroke does not move the column.
        const float y1 = marks[1].y;
        const int n1 = albumStageMarks(longStage, 1, marks, 16);
        assert(n1 == expect);
        assert(!marks[0].lit && marks[1].lit);
        assert(near(marks[1].w, in.markThin));
        assert(near(markCenterX(marks[0]), cx));
        assert(near(marks[1].y, y1));
        assert(near(markCenterX(marks[1]), cx));
    }

    // More tracks on the same frame are more strokes.
    {
        AlbumStageInput few = wideInput();
        few.trackCount = 20;
        const AlbumStage a = layoutAlbumStage(few);
        AlbumStageInput many = wideInput();
        many.trackCount = 40;
        const AlbumStage b = layoutAlbumStage(many);
        assert(a.markCount >= 2);
        assert(b.markCount > a.markCount);
        assert(near(a.rulerW, b.rulerW));
    }

    // The ruler widens the group by one column. Both groups stay centred,
    // and the long one leaves less margin.
    {
        AlbumStageInput fit = wideInput();
        fit.trackCount = 8;
        const AlbumStage a = layoutAlbumStage(fit);
        assert(longStage.groupW > a.groupW);
        assert(near(longStage.groupW - a.groupW, fit.rulerGap + longStage.rulerW));
        const float innerL = fit.panelX + fit.pad;
        const float marginFit = a.groupX - innerL;
        const float marginLong = longStage.groupX - innerL;
        assert(marginLong < marginFit);
        assert(marginLong > 0.0f);
    }

    // Two short discs: two strokes, one per disc, with the wider pause
    // between them, centred on the sleeve. The second sits below the first.
    {
        AlbumStageInput in = wideInput();
        const int counts[] = { 3, 4 };
        setDiscs(in, counts, 2);
        in.discIndex = 1;
        in.trackCount = 4;
        in.discBand = 32;
        in.discHeaderH = 45;
        const AlbumStage s = layoutAlbumStage(in);
        assert(!s.overflows);
        assert(s.discGroups == 2);
        assert(s.markCount == 2);
        assert(s.discPages[0] == 1);
        assert(s.discPages[1] == 1);
        assert(near(s.markH, in.markH));
        assert(near(s.discGap, in.discGap));
        const float artCenter = s.artY + s.artSize * 0.5f;
        assert(near(s.rulerY + s.rulerH * 0.5f, artCenter));
        AlbumMark marks[4];
        const int n = albumStageMarks(s, 0, marks, 4);
        assert(n == 2);
        assert(!marks[0].lit && marks[1].lit);
        assert(near(markCenterX(marks[0]), markCenterX(marks[1])));
        assert(near(marks[1].y, marks[0].y + marks[0].h + in.discGap));
        assert(marks[1].y > marks[0].y + marks[0].h);
        checkInside(in, s);
        checkRowOnArt(s, 4, in.rowPitch);
    }

    // Four discs, and the current disc is short. The long disc still
    // contributes its strokes, so the column — and the sleeve — do not
    // move when the page changes. Strokes of one disc sit closer together
    // than the gap before the next disc.
    {
        AlbumStageInput in = wideInput();
        const int counts[] = { 3, 3, 3, 40 };
        setDiscs(in, counts, 4);
        in.discIndex = 0;
        in.discBand = 32;
        in.discHeaderH = 45;
        in.trackCount = 3;
        const AlbumStage a = layoutAlbumStage(in);
        assert(!a.overflows);
        assert(a.discGroups == 4);
        assert(a.discPages[0] == 1);
        assert(a.discPages[3] > 1);
        assert(a.markCount == 1 + 1 + 1 + a.discPages[3]);
        assert(a.discGap > a.markGap);
        checkRowOnArt(a, 3, in.rowPitch);
        checkInside(in, a);

        in.discIndex = 3;
        in.trackCount = 40;
        const AlbumStage b = layoutAlbumStage(in);
        assert(b.overflows);
        assert(b.markCount == a.markCount);
        assert(near(a.artX, b.artX));
        assert(near(a.artY, b.artY));
        assert(near(a.listX, b.listX));
        assert(near(a.listY, b.listY));
        assert(near(a.listH, b.listH));
        assert(near(a.rulerX, b.rulerX));
        assert(near(a.rulerY, b.rulerY));
        assert(near(a.rulerH, b.rulerH));
        assert(near(a.groupW, b.groupW));

        AlbumMark marks[32];
        assert(b.markCount <= 32);
        const int n = albumStageMarks(b, 0, marks, 32);
        assert(n == b.markCount);
        int idx = 0;
        int lit = 0;
        for (int d = 0; d < b.discGroups; ++d) {
            const int pages = b.discPages[d];
            for (int p = 0; p < pages; ++p) {
                assert(near(markCenterX(marks[idx]), b.rulerX + b.rulerW * 0.5f));
                assert(near(marks[idx].h, b.markH));
                if (p + 1 < pages)
                    assert(near(marks[idx + 1].y,
                                marks[idx].y + marks[idx].h + b.markGap));
                if (marks[idx].lit) {
                    assert(d == 3);
                    assert(p == 0);
                    ++lit;
                }
                ++idx;
            }
            if (d + 1 < b.discGroups)
                assert(near(marks[idx].y,
                            marks[idx - 1].y + marks[idx - 1].h + b.discGap));
        }
        assert(idx == n);
        assert(lit == 1);
    }

    // A short album and a long one of the same set share the sleeve.
    {
        AlbumStageInput in = wideInput();
        const int counts[] = { 2, 2, 2, 40 };
        setDiscs(in, counts, 4);
        in.discBand = 32;
        in.discHeaderH = 45;
        in.trackCount = 2;
        const AlbumStage a = layoutAlbumStage(in);
        in.discIndex = 3;
        in.trackCount = 40;
        const AlbumStage b = layoutAlbumStage(in);
        assert(near(a.artX, b.artX));
        assert(near(a.artY, b.artY));
        assert(near(a.groupW, b.groupW));
        assert(near(a.footerY, b.footerY));
        assert(near(a.rulerH, b.rulerH));
    }

    // Many strokes on a short sleeve stay inside the picture, and the
    // groups stay distinguishable.
    {
        AlbumStageInput in = wideInput();
        in.panelH = 520;
        in.pad = 20;
        in.artSize = 80;
        in.identH = 20;
        in.artGap = 4;
        const int counts[] = { 30, 30, 30, 30 };
        setDiscs(in, counts, 4);
        in.trackCount = 30;
        in.discHeaderH = 0;
        in.discBand = 0;
        const AlbumStage s = layoutAlbumStage(in);
        assert(s.markCount > 4);
        assert(s.rulerH <= s.artSize + 0.05f);
        const float artCenter = s.artY + s.artSize * 0.5f;
        assert(near(s.rulerY + s.rulerH * 0.5f, artCenter));
        assert(s.discGap > s.markGap);
        assert(near(s.rulerW, in.markThin));
    }

    // A narrow panel keeps the group inside, giving the ruler up before
    // the list disappears.
    {
        AlbumStageInput in = wideInput();
        in.panelW = 130 + 500;
        in.artSize = 220;
        in.trackCount = 40;
        in.minList = 280;
        const AlbumStage s = layoutAlbumStage(in);
        checkInside(in, s);
        assert(s.rulerW == 0.0f);
        assert(s.markCount == 0);
        assert(s.colW >= 1.0f);
    }

    // Tall album. The sleeve, the lines beside it, the list and the ruler
    // are one group. A disc that fits and is alone has no ruler, so the
    // group is narrower and the margins grow. The lines are centred on
    // the sleeve. A short list starts under the header.
    {
        AlbumStageInput fit = wideInput();
        fit.wide = false;
        fit.panelX = 0;
        fit.panelY = 130;
        fit.panelW = 1600;
        fit.panelH = 1920;
        fit.trackCount = 8;
        const AlbumStage a = layoutAlbumStage(fit);
        assert(a.centerLines);
        assert(a.rulerW == 0.0f);
        assert(a.markCount == 0);
        assert(!a.overflows);
        checkInside(fit, a);
        const float artMid = a.artY + a.artSize * 0.5f;
        assert(near(a.textY + fit.identH * 0.5f, artMid));
        assert(near(a.textX, a.artX + a.artSize + fit.artGap));
        // The list is the reading measure, shared with the sleeve. It does
        // not stretch to the panel, so the group has margins to centre in.
        assert(near(a.listW, fit.readingMeasure));
        assert(near(a.textW, fit.readingMeasure - a.artSize - fit.artGap));
        assert(near(a.groupW, a.listW));
        assert(near(a.listX, a.artX));
        assert(near(a.rowOrigin, a.listY + a.headerH));
        assert(a.listY >= a.artY + a.artSize - 0.05f);
        const float innerL = fit.panelX + fit.pad;
        assert(a.artX > innerL + 1.0f);

        const int counts[] = { 4, 40 };
        AlbumStageInput many = fit;
        setDiscs(many, counts, 2);
        many.discBand = 32;
        many.discHeaderH = 45;
        many.trackCount = 4;
        const AlbumStage b = layoutAlbumStage(many);
        assert(b.rulerW > 0.0f);
        assert(near(b.rulerW, many.markThin));
        assert(b.discPages[0] == 1);
        assert(b.discPages[1] >= 2);
        assert(b.markCount == 1 + b.discPages[1]);
        checkInside(many, b);
        assert(near(b.groupW - a.groupW, many.rulerGap + b.rulerW));
        assert(near(b.listW, a.listW));
        const float marginA = a.groupX - innerL;
        const float marginB = b.groupX - innerL;
        assert(marginB < marginA);
        assert(near(b.rulerX, b.listX + b.listW + many.rulerGap));
        assert(near(b.rulerX + b.rulerW, b.groupX + b.groupW));
        const float listMid = b.listY + b.listH * 0.5f;
        assert(near(b.rulerY + b.rulerH * 0.5f, listMid));
        assert(b.rulerH <= b.listH + 0.05f);
        assert(b.rulerY > b.listY + 1.0f);
        assert(b.centerLines);
        assert(near(b.textY + many.identH * 0.5f, b.artY + b.artSize * 0.5f));

        AlbumMark marks[8];
        assert(b.markCount <= 8);
        const int n = albumStageMarks(b, 0, marks, 8);
        assert(n == b.markCount);
        assert(marks[0].lit);
        for (int i = 0; i < n; ++i)
            assert(near(marks[i].w, many.markThin));
        assert(near(marks[1].y, marks[0].y + marks[0].h + b.discGap));

        many.discIndex = 1;
        many.trackCount = 40;
        const AlbumStage c = layoutAlbumStage(many);
        assert(c.overflows);
        assert(near(c.artX, b.artX));
        assert(near(c.groupW, b.groupW));
        assert(near(c.rulerX, b.rulerX));
        assert(c.discPages[0] == 1);
        assert(c.discPages[1] == b.discPages[1]);
    }

    // Tall single. Sleeve at the top, centred. Lines under it. The row
    // is centred in the area that remains, and there is no ruler.
    {
        AlbumStageInput in = wideInput();
        in.wide = false;
        in.single = true;
        in.trackCount = 1;
        in.panelX = 0;
        in.panelY = 130;
        in.panelW = 1600;
        in.panelH = 1920;
        const AlbumStage s = layoutAlbumStage(in);
        assert(!s.showTrackNumber);
        assert(s.centerLines);
        assert(s.rulerW == 0.0f);
        assert(s.markCount == 0);
        assert(!s.overflows);
        checkInside(in, s);
        const float innerL = in.panelX + in.pad;
        const float innerW = in.panelW - in.pad * 2.0f;
        const float mid = innerL + innerW * 0.5f;
        assert(near(s.artY, in.panelY + in.pad));
        assert(near(s.artX + s.artSize * 0.5f, mid));
        assert(near(s.textY, s.artY + s.artSize + in.artGap));
        assert(near(s.textX + s.textW * 0.5f, mid));
        assert(near(s.listX + s.listW * 0.5f, mid));
        const float remTop = s.textY + in.identH + in.belowGap;
        const float columnH = in.panelH - in.pad * 2.0f - in.sceneBand;
        const float remBot = in.panelY + in.pad + columnH - in.discBand;
        const float rowMid = s.rowOrigin + in.rowPitch * 0.5f;
        assert(near(rowMid, (remTop + remBot) * 0.5f));
        assert(s.listY >= remTop - 0.05f);
    }

    // Artist page. The portrait is centred in the inner canvas, on both
    // axes. Its centre is never below that canvas centre. The arrow sits
    // in the air above the portrait and does not move it. The name is the
    // group under the portrait. The prose starts after that group.
    {
        ArtistPageInput in;
        in.panelX = 0;
        in.panelY = 130;
        in.panelW = 1080;
        in.panelH = 1800;
        in.pad = 65;
        in.heroGap = 19;
        in.afterHero = 65;
        in.imageSize = 660;
        in.nameH = 40;
        in.columnW = 820;
        const ArtistPage p = layoutArtistPage(in);
        const float innerL = in.panelX + in.pad;
        const float innerT = in.panelY + in.pad;
        const float innerW = in.panelW - in.pad * 2.0f;
        const float innerH = in.panelH - in.pad * 2.0f;
        const float mid = innerL + innerW * 0.5f;
        const float canvasMid = innerT + innerH * 0.5f;
        assert(near(p.arrowCx, mid));
        assert(p.arrowCy < p.imageY);
        assert(near(p.arrowCy, innerT + (p.imageY - innerT) * 0.5f));
        assert(near(p.imageX + p.imageSize * 0.5f, mid));
        assert(near(p.imageY + p.imageSize * 0.5f, canvasMid));
        assert(p.imageY + p.imageSize * 0.5f <= canvasMid + 0.05f);
        assert(near(p.nameY, p.imageY + p.imageSize + in.heroGap));
        assert(near(p.nameX + p.nameW * 0.5f, mid));
        assert(near(p.proseY, p.nameY + in.nameH + in.afterHero));
        assert(near(p.columnX + p.columnW * 0.5f, mid));
        assert(p.columnW <= innerW + 0.05f);
        assert(p.imageX >= innerL - 0.05f);

        in.imageSize = 0;
        const ArtistPage nameOnly = layoutArtistPage(in);
        assert(near(nameOnly.nameY, innerT));
        assert(near(nameOnly.proseY, nameOnly.nameY + in.nameH + in.afterHero));

        in.imageSize = 4000;
        in.nameH = 0;
        const ArtistPage clamped = layoutArtistPage(in);
        assert(near(clamped.imageSize, innerW));
        assert(near(clamped.imageX, innerL));
        const float centre = clamped.imageY + clamped.imageSize * 0.5f;
        assert(near(centre, canvasMid));
        assert(centre <= canvasMid + 0.05f);
        assert(near(clamped.proseY, clamped.imageY + clamped.imageSize + in.afterHero));
    }

    printf("album_stage_test: all assertions passed\n");
    return 0;
}
