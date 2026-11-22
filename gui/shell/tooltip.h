/*
 * Tooltips (v0.3-design 11): a line of text in a small box beside what
 * the pointer rests on, after a moment (0.6 s), gone when it moves off,
 * presses or leaves. One at a time, for the LaunchPad and the WarpCenter.
 */
#ifndef WPS_TOOLTIP_H
#define WPS_TOOLTIP_H

#include <stdbool.h>

#include "ui.h"

void tooltip_init(ui_app *a);
/* The pointer rests on `anchor` of `parent` (its coordinates), which
 * says `text` ("" or NULL: nothing): shown after a moment, below the
 * anchor (above: `above`). Call it on every motion; the same anchor and
 * text keep it. */
void tooltip_hover(ui_surface *parent, ui_rect anchor, const char *text, bool above);
void tooltip_hide(void);
/* Self-tests: what is shown, or NULL. */
const char *tooltip_shown(void);

#endif
