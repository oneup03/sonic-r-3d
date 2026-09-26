/**
 * platform_3ds.c — libctru implementation of platform.h for the Nintendo 3DS.
 *
 * 3DS counterpart to sdl/src/platform/platform_sdl.c and dc/src/platform_dc.c:
 * input (HID), timing, the SD data path, quit/HOME handling, buffered file
 * opens. The renderer lives in r_c3d_*.c / render_c3d.c, sound in sfx_3ds.c /
 * music_3ds.c, the network transport in net_transport_3ds.c.
 */

#include <3ds.h>
#include <citro3d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/iosupport.h>
#include <unistd.h>
#include <strings.h>

#include "platform.h"
#include "pad_bits.h"
#include "sonicr_paths.h"
#include "r_c3d_internal.h"
#include "bottom_panel.h"
#include "net_uds_3ds.h"

extern int    __system_argc;
extern char **__system_argv;

/* libctru's default main-thread stack is 32 KB; the screen loops keep large
 * locals (screen_misc.c alone is 8k lines). 512 KB is cheap next to the 6 MB
 * of static tables the game already carries. */
u32 __stacksize__ = 512 * 1024;

unsigned char s_keystate[256];

extern unsigned char g_keyPressState[320];
extern short g_joystickConfigWords[];
extern char  g_joystickSlots[4][282];
extern short g_joystickDeviceFlags[8];
extern char  g_joystickDeviceNames[4][260];

#define PAD_NAME            "Nintendo 3DS"
#define JOY_BUTTONS_PER_SLOT 80
#define DS_BUTTONS_PER_PAD   7
#define JOY_CFG_MAX          32
#define CPAD_DEADZONE        40     /* circle pad range is about +/-156 */

static int  s_earlyInited = 0;
static int  s_c3dInited = 0;
static char s_basePath[256];
static int  s_isNew3ds = 0;
int g_rc3dCpuMhz = 0;
char g_rc3dSpeedupInfo[48] = "";
static u64  s_timeBase = 0;


/* ---------------------------------------------------------------------------
 * Unattended testing hooks, for driving the game from an emulator script.
 *
 *   ARGS.TXT      whitespace-separated command-line options for main()
 *                 (e.g. "--unlock --stereo-debug-depth"); read before main.
 *   AUTOTEST.TXT  scripted input, one command per line, frame = pump count
 *                 at 30 Hz:
 *                   <frame> <frames> KEY[+KEY...]   hold keys (A B X Y L R ZL ZR
 *                                                   START SELECT UP DOWN LEFT RIGHT)
 *                   <frame> SHOT <name>             print "AUTOTEST shot <name>"
 *                   <frame> LOG <text>              print the text
 *                   <frame> EXIT                    exit(0)
 *                   ENV <name>=<value>              setenv before main
 * Both files live next to the .3dsx. Missing files are simply ignored.
 * ------------------------------------------------------------------------- */
#define AT_MAX 256
typedef struct { int frame, frames; u32 keys; int kind; int phase; int tx, ty; char text[48]; } AtCmd;
enum { AT_KEYS = 0, AT_SHOT, AT_LOG, AT_EXIT, AT_TOUCH };
static AtCmd s_at[AT_MAX];
static int   s_atCount = 0;
static int   s_atFrame = 0;       /* last frame index handled */
static u64   s_atPhaseStartMs = 0; /* wall-clock origin of the phase (30 Hz frames) */
static u32   s_held = 0;          /* hidKeysHeld() | scripted keys */
static u32   s_atKeys = 0;        /* scripted keys for the current game frame */
static int   s_atTouch = 0, s_atTouchX = 0, s_atTouchY = 0;   /* scripted touch this pump */

/* Phases: "WHEN <text>" starts a new phase that activates (and resets the
 * frame counter) once <text> shows up on stderr. Commands belong to the phase
 * they were written under; phase 0 runs from boot. */
#define AT_MAX_PHASES 32
static char  s_atTrigger[AT_MAX_PHASES][48];
static int   s_atPhases = 1;          /* phase 0 always exists */
static int   s_atPhase = 0;           /* active phase */
static char  s_atLine[256];
static int   s_atLineLen = 0;
static const devoptab_t *s_atPrevErr = NULL;
static int   s_atPending = 0;
static FILE *s_atLog = NULL;      /* AUTOTEST.LOG: every stderr line, flushed */

static void at_scan_line(const char *line)
{
    if (s_atPhase + 1 < s_atPhases && strstr(line, s_atTrigger[s_atPhase + 1]) != NULL) {
        s_atPhase++;
        s_atFrame = -1;
        s_atPhaseStartMs = osGetTime();
        s_atPending = 1;   /* announced from at_tick: we are inside a write() here */
    }
}

static void at_flush_line(void)
{
    s_atLine[s_atLineLen] = '\n';
    svcOutputDebugString(s_atLine, (s32)s_atLineLen + 1);
    if (s_atLog) {
        fwrite(s_atLine, 1, (size_t)s_atLineLen + 1, s_atLog);
        fflush(s_atLog);
    }
    s_atLine[s_atLineLen] = '\0';
    at_scan_line(s_atLine);
    s_atLineLen = 0;
}

/* Whole lines only: newlib hands an unbuffered stream to write() one
 * fragment at a time, and one svcOutputDebugString per fragment splits every
 * log line across the emulator's log. */
static ssize_t at_stderr_write(struct _reent *r, void *fd, const char *ptr, size_t len)
{
    (void)r; (void)fd;
    for (size_t i = 0; i < len; i++) {
        char c = ptr[i];
        if (c == '\n') {
            at_flush_line();
        } else {
            s_atLine[s_atLineLen++] = c;
            if (s_atLineLen >= (int)sizeof(s_atLine) - 2) {
                at_flush_line();
            }
        }
    }
    return (ssize_t)len;
}

static const devoptab_t s_atErrTab = {
    .name = "at_stderr",
    .structSize = 0,
    .write_r = at_stderr_write,
};


static u32 at_key(const char *name)
{
    static const struct { const char *n; u32 k; } tab[] = {
        {"A",KEY_A},{"B",KEY_B},{"X",KEY_X},{"Y",KEY_Y},{"L",KEY_L},{"R",KEY_R},
        {"ZL",KEY_ZL},{"ZR",KEY_ZR},{"START",KEY_START},{"SELECT",KEY_SELECT},
        {"UP",KEY_DUP},{"DOWN",KEY_DDOWN},{"LEFT",KEY_DLEFT},{"RIGHT",KEY_DRIGHT},
    };
    for (unsigned i = 0; i < sizeof(tab) / sizeof(tab[0]); i++) {
        if (strcasecmp(tab[i].n, name) == 0) return tab[i].k;
    }
    return 0;
}

static void at_load(const char *base)
{
    char path[320];
    snprintf(path, sizeof(path), "%s/AUTOTEST.TXT", base);
    FILE *fp = fopen(path, "r");
    if (!fp) return;
    char line[160];
    while (s_atCount < AT_MAX && fgets(line, sizeof(line), fp)) {
        char *h = strchr(line, '#'); if (h) *h = '\0';
        char *nl = strpbrk(line, "\r\n"); if (nl) *nl = '\0';
        char a[48], b[48], c[96];
        if (strncmp(line, "ENV ", 4) == 0) {
            char *eq = strchr(line + 4, '=');
            if (eq) { *eq = '\0'; setenv(line + 4, eq + 1, 1); }
            continue;
        }
        if (strncmp(line, "WHEN ", 5) == 0 && s_atPhases < AT_MAX_PHASES) {
            strncpy(s_atTrigger[s_atPhases], line + 5, sizeof(s_atTrigger[0]) - 1);
            s_atPhases++;
            continue;
        }
        int n = sscanf(line, "%47s %47s %95[^\n]", a, b, c);
        if (n < 2) continue;
        AtCmd *cmd = &s_at[s_atCount];
        memset(cmd, 0, sizeof(*cmd));
        cmd->frame = atoi(a);
        cmd->phase = s_atPhases - 1;
        if (strcasecmp(b, "SHOT") == 0)      { cmd->kind = AT_SHOT; if (n > 2) strncpy(cmd->text, c, sizeof(cmd->text) - 1); }
        else if (strcasecmp(b, "LOG") == 0)  { cmd->kind = AT_LOG;  if (n > 2) strncpy(cmd->text, c, sizeof(cmd->text) - 1); }
        else if (strcasecmp(b, "EXIT") == 0) { cmd->kind = AT_EXIT; }
        else if (n > 2 && strncasecmp(c, "TOUCH", 5) == 0) {
            cmd->kind = AT_TOUCH;
            cmd->frames = atoi(b);
            sscanf(c + 5, "%d %d", &cmd->tx, &cmd->ty);
        }
        else if (n > 2) {
            cmd->kind = AT_KEYS;
            cmd->frames = atoi(b);
            char *tok = strtok(c, "+ ");
            while (tok) { cmd->keys |= at_key(tok); tok = strtok(NULL, "+ "); }
        } else continue;
        s_atCount++;
    }
    fclose(fp);
    /* Hook stderr so WHEN triggers can watch the game's own log lines. Not
     * when 3dslink owns stderr — the socket devoptab needs its own fd. */
    if (__3dslink_host.s_addr == 0) {
        snprintf(path, sizeof(path), "%s/AUTOTEST.LOG", base);
        s_atLog = fopen(path, "w");
        s_atPrevErr = devoptab_list[STD_ERR];
        devoptab_list[STD_ERR] = &s_atErrTab;
    }
    fprintf(stderr, "AUTOTEST: %d commands, %d phases loaded\n", s_atCount, s_atPhases);
}

/* Called once per pump; returns the scripted keys for this frame. */
static u32 at_tick(void)
{
    u32 keys = 0;
    /* Frames are wall-clock 30 Hz ticks since the phase began, so a press
     * lasts the same real time however often the game polls the pad. */
    if (s_atPhaseStartMs == 0) s_atPhaseStartMs = osGetTime();
    int f = (int)((osGetTime() - s_atPhaseStartMs) * 30 / 1000);
    int prev = s_atFrame;
    s_atFrame = f;
    s_atTouch = 0;
    if (s_atPending) {
        s_atPending = 0;
        fprintf(stderr, "AUTOTEST: phase %d (%s)\n", s_atPhase, s_atTrigger[s_atPhase]);
    }
    for (int i = 0; i < s_atCount; i++) {
        AtCmd *c = &s_at[i];
        if (c->phase != s_atPhase) continue;
        const int fired = (prev < c->frame && f >= c->frame);   /* crossed this tick */
        switch (c->kind) {
            case AT_KEYS:
                if (f >= c->frame && f < c->frame + c->frames) keys |= c->keys;
                if (fired) fprintf(stderr, "AUTOTEST: keys %08lX for %d frames\n", (unsigned long)c->keys, c->frames);
                break;
            case AT_SHOT:
                if (fired) fprintf(stderr, "AUTOTEST shot %s\n", c->text);
                break;
            case AT_LOG:
                if (fired) fprintf(stderr, "AUTOTEST: %s\n", c->text);
                break;
            case AT_EXIT:
                if (fired) { fprintf(stderr, "AUTOTEST: exit\n"); exit(0); }
                break;
            case AT_TOUCH:
                if (f >= c->frame && f < c->frame + c->frames) { s_atTouch = 1; s_atTouchX = c->tx; s_atTouchY = c->ty; }
                if (fired) fprintf(stderr, "AUTOTEST: touch %d,%d for %d frames\n", c->tx, c->ty, c->frames);
                break;
        }
    }
    return keys;
}

/* ARGS.TXT -> argv, before main() runs (constructors run after libctru's
 * __appInit, so the SD card is already mounted and cwd is our folder). */
static char *s_argvStore[32];
static char  s_argText[512];
__attribute__((constructor)) static void args_init(void)
{
    FILE *fp = fopen("ARGS.TXT", "r");
    if (!fp) return;
    size_t n = fread(s_argText, 1, sizeof(s_argText) - 1, fp);
    fclose(fp);
    s_argText[n] = '\0';
    int argc = 0;
    s_argvStore[argc++] = (__system_argc > 0 && __system_argv) ? __system_argv[0] : "SonicR.3dsx";
    char *tok = strtok(s_argText, " \t\r\n");
    while (tok && argc < 31) { s_argvStore[argc++] = tok; tok = strtok(NULL, " \t\r\n"); }
    s_argvStore[argc] = NULL;
    __system_argc = argc;
    __system_argv = s_argvStore;
}

/* Rough CPU clock from a counted loop against the fixed-rate system tick:
 * tells whether the New 3DS 804 MHz mode actually engaged (~268 otherwise). */
static int cpu_mhz_estimate(void)
{
    u32 n = 4000000;
    u64 t0 = svcGetSystemTick();
    __asm__ volatile("1: subs %0, %0, #1\n bne 1b" : "+r"(n) : : "cc");
    u64 dt = svcGetSystemTick() - t0;
    if (dt == 0) return 0;
    /* Calibrated against a stock 268 MHz console (read 321 with 2.0). */
    return (int)((1.67 * 4000000.0 * (double)SYSCLOCK_ARM11 / (double)dt) / 1.0e6 + 0.5);
}

/* New 3DS 804 MHz + L2: ask PTM (what osSetSpeedupEnable does, but keeping
 * the result), and if the clock still reads low, the kernel call PTM itself
 * makes — Luma's hb loader allows it. Everything is logged and summarised in
 * g_rc3dSpeedupInfo, which goes to the debug log with the boot messages. */
static void try_speedup(void)
{
    int before = cpu_mhz_estimate();
    Result rp = ptmSysmInit();
    if (R_SUCCEEDED(rp)) {
        rp = PTMSYSM_ConfigureNew3DSCPU(3);
        ptmSysmExit();
    }
    int after = cpu_mhz_estimate();
    fprintf(stderr, "3ds: speedup via ptm:sysm -> %08lX, cpu ~%d -> ~%d MHz\n", (unsigned long)rp, before, after);
    if (after < 500) {
        Result rk = svcKernelSetState(10, 3);
        after = cpu_mhz_estimate();
        fprintf(stderr, "3ds: speedup via svcKernelSetState(10) -> %08lX, cpu ~%d MHz\n", (unsigned long)rk, after);
        if (after >= 500) {
            snprintf(g_rc3dSpeedupInfo, sizeof(g_rc3dSpeedupInfo), "SPEEDUP OK SVC");
        } else {
            /* Result codes' low halves; the full values are in the log. */
            snprintf(g_rc3dSpeedupInfo, sizeof(g_rc3dSpeedupInfo), "SPEEDUP FAIL P%04lX K%04lX",
                     (unsigned long)(rp & 0xFFFF), (unsigned long)(rk & 0xFFFF));
        }
    } else {
        snprintf(g_rc3dSpeedupInfo, sizeof(g_rc3dSpeedupInfo), "SPEEDUP OK PTM");
    }
    g_rc3dCpuMhz = after;
}

/* ---------------------------------------------------------------------------
 * Early init. main.c calls platform_base_path() and probes the data folder
 * BEFORE platform_init(), so everything the game needs from the very first
 * call (speed-up, gfx, debug output, cwd) is brought up lazily here.
 * ------------------------------------------------------------------------- */
static void early_init(void)
{
    if (s_earlyInited) {
        return;
    }
    s_earlyInited = 1;

    bool isNew = false;
    APT_CheckNew3DS(&isNew);
    s_isNew3ds = isNew ? 1 : 0;

    gfxInitDefault();
    gfxSet3D(true);

    /* stderr -> svcOutputDebugString, which Azahar logs and which 3dslink can
     * show. Every DebugLog / fprintf(stderr) in the game arrives this way. */
    consoleDebugInit(debugDevice_SVC);
    if (__3dslink_host.s_addr != 0) {
        link3dsStdio();
    }
    try_speedup();

    /* Where the game lives. hbmenu passes argv[0] = sdmc:/3ds/SonicR/SonicR.3dsx
     * and libctru already chdir()ed there; Azahar's direct load may give no argv
     * at all, hence the fixed fallback. */
    s_basePath[0] = '\0';
    if (__system_argc > 0 && __system_argv[0] && strncmp(__system_argv[0], "sdmc:/", 6) == 0) {
        strncpy(s_basePath, __system_argv[0], sizeof(s_basePath) - 1);
        char *slash = strrchr(s_basePath, '/');
        if (slash) {
            *slash = '\0';
        }
    }
    if (s_basePath[0] == '\0') {
        strcpy(s_basePath, "sdmc:/3ds/SonicR");
    }

    fprintf(stderr, "3ds: %s 3DS, cpu ~%d MHz, base path %s, argc %d\n",
            s_isNew3ds ? "New" : "Old", g_rc3dCpuMhz, s_basePath, __system_argc);

    /* SAVE/ and GHOST/ are created by the installer on desktop; nothing in the
     * shared code makes them, and a missing directory makes every save fail. */
    char dir[300];
    snprintf(dir, sizeof(dir), "%s/SAVE", s_basePath);  mkdir(dir, 0777);
    snprintf(dir, sizeof(dir), "%s/GHOST", s_basePath); mkdir(dir, 0777);

    at_load(s_basePath);
}

/* Data-not-found screen: main.c would exit silently otherwise. */
static void data_missing_screen(void)
{
    PrintConsole con;
    consoleInit(GFX_BOTTOM, &con);
    printf("\x1b[2;2HSonic R: game data not found.\n\n");
    printf("  Copy the game's data folders\n  (GENERAL, BIN, ISLAND, ...) into\n\n  %s\n\n", s_basePath);
    printf("  See docs/3ds.md.\n\n  Press START to exit.\n");
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) {
            break;
        }
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
    gfxExit();
    exit(0);
}

const char *platform_base_path(void)
{
    early_init();
    char probe[320];
    snprintf(probe, sizeof(probe), "%s/GENERAL/SONICR.BIT", s_basePath);
    FILE *fp = fopen(probe, "rb");
    if (fp == NULL) {
        fprintf(stderr, "3ds: %s missing\n", probe);
        data_missing_screen();
    }
    fclose(fp);
    return s_basePath;
}

/* fileio.h routes every game fOpen here: libctru's SD driver has no cache, so
 * give stdio a real buffer instead of the 1 KB default. */
FILE *sr3ds_fopen(const char *path, const char *mode)
{
    FILE *fp = fopen(path, mode);
    if (fp != NULL) {
        setvbuf(fp, NULL, _IOFBF, 64 * 1024);
    }
    return fp;
}

int platform_is_new_3ds(void)
{
    return s_isNew3ds;
}

/* ---------------------------------------------------------------------------
 * Display
 * ------------------------------------------------------------------------- */

static void platform_atexit(void)
{
    if (s_c3dInited) {
        RC3D_Shutdown();
        C3D_Fini();
        s_c3dInited = 0;
    }
    gfxExit();
}

int platform_init(int width, int height, int fullscreen, const char *title)
{
    (void)width; (void)height; (void)fullscreen; (void)title;
    early_init();

    if (!C3D_Init(0x80000)) {
        fprintf(stderr, "3ds: C3D_Init failed\n");
        return -1;
    }
    s_c3dInited = 1;
    RC3D_Init();
    atexit(platform_atexit);

    s_timeBase = osGetTime();
    fprintf(stderr, "3ds: app mem free %u KB, linear free %u KB\n",
            (unsigned)(osGetMemRegionFree(MEMREGION_APPLICATION) / 1024),
            (unsigned)(linearSpaceFree() / 1024));
    return 0;
}

void platform_get_desktop_size(int *w, int *h)
{
    if (w) *w = 400;
    if (h) *h = 240;
}

void *platform_native_window_handle(void)
{
    return NULL;
}

void platform_shutdown(void)
{
    /* atexit hook does the teardown, so a SCREEN_QUIT and a HOME exit match. */
}

void platform_gl_swap(void)
{
    /* Presented by FlipD3D -> R_StereoComposeFrame -> C3D_FrameEnd. */
}

void platform_get_drawable_size(int *w, int *h)
{
    if (w) *w = 400;
    if (h) *h = 240;
}

/* ---------------------------------------------------------------------------
 * Input
 * ------------------------------------------------------------------------- */

int platform_poll_events(unsigned char *keystateOut, int keystateSize)
{
    if (keystateOut && keystateSize > 0) {
        int n = (keystateSize < 256) ? keystateSize : 256;
        memcpy(keystateOut, s_keystate, n);
    }
    return 0;
}

void platform_pump_events(void)
{
    /* HOME, sleep and power all go through here; aptMainLoop() returns false
     * once the system wants us gone. The screen loops have no single exit and
     * nothing consumes platform_poll_events' quit flag, so exit() directly —
     * atexit(SaveGameSettings) in main.c still runs. Same as the DC build. */
    if (!aptMainLoop()) {
        exit(0);
    }
    hidScanInput();
    s_held = hidKeysHeld() | s_atKeys;
    u32 held = s_held;

    /* No keyboard on a 3DS; the DIK table stays empty. */
    memset(s_keystate, 0, sizeof(s_keystate));

    if ((held & (KEY_L | KEY_R | KEY_START | KEY_SELECT)) == (KEY_L | KEY_R | KEY_START | KEY_SELECT)) {
        exit(0);
    }

    if (held & KEY_TOUCH) {
        touchPosition tp;
        hidTouchRead(&tp);
        BottomPanel_Touch(tp.px, tp.py, 1);
    }
    else if (s_atTouch) {
        BottomPanel_Touch(s_atTouchX, s_atTouchY, 1);
    }
    else {
        BottomPanel_Touch(0, 0, 0);
    }
}

int platform_init_gamepads(void)
{
    g_joystickDeviceFlags[0] = (short)DS_BUTTONS_PER_PAD;
    strcpy(g_joystickDeviceNames[0], PAD_NAME);
    return 1;
}

/* Raw buttons for the network lobby (see MENUBTN_* in platform.h). */
unsigned int platform_menu_buttons(void)
{
    u32 h = s_held;
    unsigned int m = 0;
    if (h & KEY_A)      m |= MENUBTN_A;
    if (h & KEY_B)      m |= MENUBTN_B;
    if (h & KEY_X)      m |= MENUBTN_X;
    if (h & KEY_Y)      m |= MENUBTN_Y;
    if (h & KEY_START)  m |= MENUBTN_START;
    if (h & KEY_SELECT) m |= MENUBTN_SELECT;
    if (h & (KEY_L | KEY_ZL)) m |= MENUBTN_L;
    if (h & (KEY_R | KEY_ZR)) m |= MENUBTN_R;
    if (h & KEY_DUP)    m |= MENUBTN_UP;
    if (h & KEY_DDOWN)  m |= MENUBTN_DOWN;
    if (h & KEY_DLEFT)  m |= MENUBTN_LEFT;
    if (h & KEY_DRIGHT) m |= MENUBTN_RIGHT;
    return m;
}

/* Mirrors platform_dc.c: directions are fixed bits, the seven action buttons
 * go through the per-slot config table the remap UI edits. Button indices:
 *   0=A 1=B 2=X 3=Y 4=L(ZL) 5=R(ZR) 6=START */
int platform_poll_gamepads(unsigned short *joySlotState, int maxSlots)
{
    for (int s = 0; s < maxSlots; s++) {
        joySlotState[s] = 0;
    }
    if (maxSlots < 1) {
        return 0;
    }

    /* One poll per game frame: this is where the input script advances. The
     * event pump runs several times per frame inside WaitForFrameCap. */
    if (s_atCount > 0) {
        s_atKeys = at_tick();
        s_held = hidKeysHeld() | s_atKeys;
    }
    u32 h = s_held;
    circlePosition cp;
    hidCircleRead(&cp);

    unsigned short bits = 0;
    if (h & KEY_DLEFT)  bits |= PAD_LEFT;
    if (h & KEY_DRIGHT) bits |= PAD_RIGHT;
    if (h & KEY_DUP)    bits |= PAD_UP;
    if (h & KEY_DDOWN)  bits |= PAD_DOWN;
    if (cp.dx < -CPAD_DEADZONE) bits |= PAD_LEFT;
    if (cp.dx >  CPAD_DEADZONE) bits |= PAD_RIGHT;
    if (cp.dy >  CPAD_DEADZONE) bits |= PAD_UP;
    if (cp.dy < -CPAD_DEADZONE) bits |= PAD_DOWN;

    int heldBtn[DS_BUTTONS_PER_PAD] = {
        (h & KEY_A) != 0,
        (h & KEY_B) != 0,
        (h & KEY_X) != 0,
        (h & KEY_Y) != 0,
        (h & (KEY_L | KEY_ZL)) != 0,
        (h & (KEY_R | KEY_ZR)) != 0,
        (h & KEY_START) != 0,
    };

    unsigned char *pressBase = &g_keyPressState[0];
    const short *slotCfg = (const short *)&g_joystickSlots[0][0x104];
    for (int b = 0; b < DS_BUTTONS_PER_PAD; b++) {
        pressBase[b] = heldBtn[b] ? 0x80 : 0x00;
        if (heldBtn[b]) {
            if (b < 10) {
                bits |= (unsigned short)slotCfg[b];
            }
            else if (b < JOY_CFG_MAX) {
                bits |= (unsigned short)g_joystickConfigWords[b];
            }
        }
    }
    for (int b = DS_BUTTONS_PER_PAD; b < JOY_BUTTONS_PER_SLOT; b++) {
        pressBase[b] = 0;
    }
    joySlotState[0] = bits;
    if (s_atCount > 0 && bits != 0) {
        static u32 lastBits = 0;
        if (bits != lastBits) fprintf(stderr, "AUTOTEST: pad word %04X\n", bits);
        lastBits = bits;
    }
    return 1;
}

/* ---------------------------------------------------------------------------
 * Timing
 * ------------------------------------------------------------------------- */

uint32_t platform_get_time_ms(void)
{
    if (s_timeBase == 0) {
        s_timeBase = osGetTime();
    }
    return (uint32_t)(osGetTime() - s_timeBase);
}

/* Some shared code still spells it the SDL way (see platform_dc.c). */
uint32_t SDL_GetTicks(void)
{
    return platform_get_time_ms();
}

void platform_sleep_ms(int ms)
{
    if (ms > 0) {
        u64 t0 = svcGetSystemTick();
        svcSleepThread((s64)ms * 1000000LL);
        g_rc3dSleepTicks += svcGetSystemTick() - t0;
    }
}

/* ---------------------------------------------------------------------------
 * Audio / network hooks
 * ------------------------------------------------------------------------- */

int platform_audio_init(void)
{
    return 0;   /* ndsp is brought up by InitDirectSound (sfx_3ds.c) */
}

void platform_audio_shutdown(void)
{
}

/* The radio is brought up lazily by the transport the lobby picks (see
 * net_uds_3ds.c); the lobby itself must open regardless. */
int platform_net_init(void)
{
    return 0;
}

void platform_net_shutdown(void)
{
    net3ds_shutdown();
}

int platform_net_is_modem(void)
{
    return 0;
}

int platform_get_region(void)
{
    return 0;
}
