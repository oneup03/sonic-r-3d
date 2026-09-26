/**
 * bottom_panel.h — touch-screen controls drawn under the race HUD.
 */
#ifndef BOTTOM_PANEL_H
#define BOTTOM_PANEL_H

/* Called every pump with the touch state (held = 0 when the pen is up). */
void BottomPanel_Touch(int px, int py, int held);

/* Draw the panel onto the bottom target. Called by the compose pass after the
 * race HUD has been replayed there. */
void BottomPanel_Draw(void);

/* The HOME-button prompt (quit / resume / HOME Menu), drawn on its own on the
 * bottom screen through RC3D_PresentBottomOnly. */
void BottomPanel_DrawPrompt(void);

#endif
