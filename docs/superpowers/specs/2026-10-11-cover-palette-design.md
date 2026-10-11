# Cover palettes — design

**Goal:** Dynamic mode builds a full palette from the playing cover and
stores it on that release. The next play reads the row. A cover that
cannot carry readable type keeps today's nearest listed palette for that
listen.

Static is unchanged. Dynamic still never writes `ui_theme`, still rests
on Matrix when nothing is playing, and still returns to Matrix when
playback ends.

## Reading the sleeve

`paletteFromCover` does not use the coarse histogram that
`coverColorsFromRgba` keeps for nearest-theme matching. A bin that only
keeps the largest mass cannot tell two sleeves apart.

The picture is split into twelve by twelve regions, and each region is
sampled on a ten-by-ten lattice, so a mark in a corner is seen. Opaque
samples of the same pigment, measured in OKLab, become one voice. In the
dark the merge is wider, because the eye stops splitting shades the
metric still splits. A voice remembers how much of the sleeve it covers,
how much of it sits in the middle, how much sits on the rim, and how many
regions it is the most saturated point of. One noisy pixel is not a voice.
Four samples of one hue that pops are a dot. When the table of voices is
full, a new hue that is more its own color than the closest pair already
stored takes a slot, and that pair collapses.

A voice the eye cannot separate from the page is folded back into the
page. Separation is not OKLab chroma. A red dot on black and a night blue
on that same black are about equally "chromatic" in the metric, and only
the red is a color: it has a luminance step, and its colorfulness survives
at that lightness. The night blue has neither. It stays in the field. The
floor between those two was measured, and it is `kNoticeFloor`.

## Roles

- **Page.** The voice with the most area. When that color is too middle
  for a text ladder, its OKLab lightness moves toward the pole of its day
  or night side — relative luminance 0.45, the same cut as the rest of
  the themes — and its hue stays. The move stops at the first step the
  ladder accepts. It never goes all the way to black or white. The first
  step is the sampled color itself.
- **Ink.** The low-chroma voice on the far side of the page, weighted by
  how much of it there is. Lightness may move. Hue stays, so cream type
  stays cream. When the sleeve has no such ink, the type is black or
  white, tinted a little toward whatever low-chroma pigment is there.
- **Accent.** The voice that separates from the field, ranked by that
  notice, then by how much of it there is and whether it sits in the
  middle. A red dot qualifies. A night blue does not. A red that merely
  ties another mark is kept for the error, and the other mark is the
  accent. A red that leads clearly stays the accent. Lightness moves only
  as far as contrast needs, and a color that already reads is kept exactly.
  On the transport bar this accent is the center control in both poses.
  Previous and next stay the ink.
- **Album title.** The next mark that can be read as type without being
  pushed so far it stops being that color. Otherwise the title is a step
  past the body ink.
- **Mosaic fill, hairline, washes.** The marks after those. They are mixed
  only a little toward the page, so a second and a third hue stay
  recognizable instead of becoming one wash. A sleeve with one mark reuses
  it, dimmed into the page.
- **Track, rail, transport.** The next fields by area, and only when they
  clear the same notice floor. They are admitted only part way, so a black
  frame beside a bright photograph does not average into mud. A second
  field the eye can separate is also admitted into the transport bar. A
  dark voice on a dark sleeve pulls the transport down. The transport is
  then walked until dim text still clears it.
- **Error and warning.** A red voice that is not the accent, and an amber
  voice, at a lightness that reads. Otherwise the house red and the house
  amber. Each is walked away from the accent until they are 48
  channel-steps apart.
- **Quality marks.** They keep their meanings. When the sleeve has a color
  in that family, the mark takes its angle. On a dark track the DSD mark
  prefers the sleeve's own near-neutral paper. A chromatic accent that
  lands on a mark moves that mark's lightness. A quiet accent may share
  white or black with the DSD mark. If a nudge makes two marks
  indistinguishable, the semantic hue is put back.

Nothing is invented in a hue the sleeve never used, except those
fallbacks. `fillFromAnsi` is the derivation for an imported theme. It is
not used here, because it would repaint every role from three seeds and
wash the hue out.

## Gate

The cover path requires `paletteAcceptable`. It also
requires the album-title color at 4.5:1 on the page, because that string
is drawn under a variant and on a playlist tile and the shared gate does
not look at it. The accent and the error stay at least 48 channel-steps
apart.

A false return leaves the caller's palette untouched. Nothing is written
for that cover. Dynamic then uses `matchTheme`.

## Row

`cover_themes` is one row per release. The key is the folder name, a unit
separator, and the artist. The cover file's size and mtime are the token,
and `generator` is `kCoverThemeGenerator`. A miss on either rebuilds.
The blob is the 20 slots `paletteWriteSlots` writes.

The row is not a catalogue entry. `applyPalette` paints it.
`activeThemeId` stays the saved static id, so the chip stroke does not
follow the cover. The Themes page's dim line names the release.

The same art path on two releases writes two rows of the same colors.
A release with no folder name is not stored. A file that cannot be
stat'd is painted for the session and not stored.
