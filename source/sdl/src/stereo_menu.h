/**
 * stereo_menu.h — Stereoscopic 3D rows on the Graphics options page.
 *
 * The Graphics page (items 23-30) carries six rows the port had deliberately
 * blanked, because they configured things that no longer exist: DirectDraw
 * resolution and colour depth, the software interlace mode, the D3D-era
 * "resolution level" viewport shrink, and two software-renderer flags. They
 * still occupy layout slots and the cursor still stops on them, so they are
 * exactly the space the stereo settings need — no new page, no new navigation.
 *
 * Their LABELS, though, are sprites baked into the game's UI atlas and say the
 * wrong thing. So these rows opt out of the sprite path entirely and draw
 * themselves as text with the port's pixel font, which is also what lets the
 * values read as numbers ("0.040") rather than the atlas's 0-8 tick bars.
 *
 * screen_misc.c defers to this module in three places: the blanking test, the
 * row draw, and the left/right adjust.
 */
#ifndef STEREO_MENU_H
#define STEREO_MENU_H

/* 1 if this options item is one of the stereo rows. Items are the global
 * s_optItemInfo indices, not page-relative. */
int  StereoMenuIsRow(int itemIndex);

/* Draw one stereo row. `rowY` is the item's Y in the menu's own units — the
 * same value DrawOptionItem would hand to the sprite path. */
void StereoMenuDrawRow(int itemIndex, int rowY);

/* Apply a left/right adjust (direction -1 or +1). Tri-state so the caller can
 * tell "not mine" from "mine, but nothing changed" — the latter must still be
 * claimed (or the legacy handler for that index runs) while not producing the
 * confirmation click that would suggest something happened.
 *
 *    0  not a stereo row — fall through to the legacy handler
 *    1  handled, a value changed
 *   -1  handled, inert (row is dimmed because stereo is off) */
int  StereoMenuAdjust(int itemIndex, int direction);

#endif /* STEREO_MENU_H */
