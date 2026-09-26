# Playing Sonic R on Nintendo 3DS

*Looking for Windows / macOS / Linux instead? See [desktop.md](desktop.md). Dreamcast: [dreamcast.md](dreamcast.md). Back to the [README](../README.md).*

> **You supply your own game data.** This port is only the game engine — it does
> not include any of Sonic R's assets. You must own a copy of the game to provide
> them. See [Game data you supply](#game-data-you-supply).

The 3DS build renders the game in **stereoscopic 3D** on the top screen (driven
by the hardware 3D slider) and puts the **race HUD on the touch screen**, along
with a touch slider for the stereo convergence. It is built for
the **New 3DS / New 2DS** (the extra CPU speed is enabled automatically); an
original 3DS/2DS will boot it, but frame rate there is best-effort.

---

## Get it

1. Your console needs custom firmware (Luma3DS). Any modern CFW setup from
   [3ds.hacks.guide](https://3ds.hacks.guide) works.
2. Download `sonic-r-3d-3ds.zip` from the
   [rolling release](https://github.com/oneup03/sonic-r-3d/releases/tag/rolling).
   It holds two builds of the same game:
   - **`SonicR.cia`** — an installable title. **Use this on a New 3DS / 2DS**:
     only an installed title can ask for the 804 MHz clock and the L2 cache,
     which give the game headroom to hold 30 fps. Install it with FBI (or any
     CIA installer); it appears on the HOME menu.
   - **`3ds/SonicR/SonicR.3dsx`** — the Homebrew Launcher build. Same game,
     but it always runs at 268 MHz without the L2 cache: Luma's 3dsx loader
     turns both off for every homebrew process, and Luma's *New 3DS CPU*
     option only reaches installed titles, so setting it does not help here.
     It still holds 30 fps in races on a New 3DS, just with less to spare.
     The only choice on an original 3DS/2DS, which has no faster clock anyway.
3. Copy the game data to `sdmc:/3ds/SonicR/` (next section). Both builds look
   there; the `.3dsx` also has to live in that folder.
4. **Sound needs the DSP firmware dump** at `sdmc:/3ds/dspfirm.cdc`. Dump it once
   with Luma3DS: open the Rosalina menu (default L+Down+Select), pick
   *Miscellaneous options…*, then *Dump DSP firmware*. Without it the game runs
   silently.
5. Launch **Sonic R** from the HOME menu (CIA) or the Homebrew Launcher (3dsx).

## Game data you supply

Everything lives in one folder next to the executable:

```
sdmc:/3ds/SonicR/
    SonicR.3dsx
    GENERAL/  ISLAND/  CITY/  RUIN/  FACTORY/  EMERALD/  AI/  BIN/
    SOUND/SFX/        the PC sound effects (*.WAV)
    MUSIC/            track2.adx … track21.adx
    SAVE/  GHOST/     created on first run
```

| Folder     | Contents                                             |
|------------|------------------------------------------------------|
| `GENERAL`  | Shared textures, character sheets, weather sprites   |
| `ISLAND`   | Resort Island track + textures                       |
| `CITY`     | Radical City track + textures                        |
| `RUIN`     | Regal Ruin track + textures                          |
| `FACTORY`  | Reactive Factory track + textures                    |
| `EMERALD`  | Radiant Emerald track + textures                     |
| `AI`       | Opponent pathfinding data                            |
| `BIN`      | Models, menus, titles, demos, environment maps       |
| `SOUND`    | Sound effects (`SOUND/SFX/*.WAV` from the PC release) |

Copy these from an installed copy of the PC release. The SD card is
case-insensitive, so the original mixed-case names are fine there; if you keep a
copy for the Azahar emulator on Linux, uppercase the tree first
(`dreamcast/tools/uppercase_tree.sh`) and keep the music files lowercase
(`track2.adx`).

Also copy the repo's bundled extras over the top: `DATA/BIN/OPTION/NET01.RAW`
and `DATA/SOUND/SFX/REPLAY*.WAV` (the split replay-commentary clips).

**Music** is the CD soundtrack as CRI ADX files, `MUSIC/track2.adx` through
`track21.adx` (44.1 kHz stereo), numbered by the original CD track. The same
files work in the desktop build.

## Controls

| 3DS                | Action                          |
|--------------------|---------------------------------|
| Circle Pad / D-Pad | Steer / menu navigation          |
| A                  | Jump / confirm                   |
| B or X             | Accelerate / back                |
| Y                  | Camera                           |
| L / ZL             | Drift left                       |
| R / ZR             | Drift right                      |
| START              | Start / pause                    |
| L+R+START+SELECT   | Quit to the Homebrew Launcher    |
| HOME               | Quit prompt: A quits, B resumes, HOME again opens the HOME Menu |

Buttons can be remapped under Options → Controls. That page has only the pad
row and Back on the 3DS: the keyboard rows of the PC game are gone, since there
is no keyboard to scan. The pad remap asks for a button per action in turn;
**SELECT** cancels it (there is no ESC key).

**Unlock all:** Options → Game → *UNLOCK ALL* (on the 3DS it replaces the
split-screen row) opens every character, Super Sonic and Radiant Emerald at
once and is remembered. Turning it off puts back what was unlocked before it
went on in that session; saving to a Load/Save slot while it is on keeps
everything unlocked in that slot.

## Stereoscopic 3D and the touch screen

- The **3D slider** sets the stereo separation. Slider down = mono, and the
  right eye is not rendered at all, which is faster.
- The **touch screen** shows the race HUD (rings, timer, positions, laps,
  minimap) so the top screen is unobstructed. In its middle sit a frame-rate
  and CPU-clock readout and the **CONV** slider: convergence, i.e. which
  distance sits exactly on the screen plane. Nearer geometry pops out, farther
  recedes.
- *3D DEPTH MAX* on the Graphics options page is the separation at full slider.
  (The desktop build's HUD-depth setting is not shown: the HUD is on the other
  screen.)
- All of it is saved to `SONICR.INF` on exit.

## Saves

Saves, ghosts, settings and the pad mapping are plain files under
`sdmc:/3ds/SonicR/` (`SAVE/`, `GHOST/`, `SONICR.INF`, `JOYSTICK.INF`), the same
layout as the desktop build.

## Multiplayer

Split-screen is not supported on the 3DS (one pad). The **Multiplayer** menu
item opens the network lobby, which has two transports:

- **ONLINE** — the same UDP protocol as the desktop and Dreamcast builds, over
  the console's Wi-Fi connection. Works on a LAN with LAN discovery (the host
  is found automatically); a direct Internet join can be forced with a
  `--host <ip>` line in `ARGS.TXT` next to the executable.
- **LOCAL WIRELESS** — 3DS to 3DS with no access point, up to four consoles.
  All players pick LOCAL; one hosts, the others join.

Press **SELECT** on the lobby's Host/Join question to switch (it does nothing
once a session is open). The rest of the lobby is the Dreamcast pad layout, and
a legend at the bottom of the screen names the buttons for the current step:

| Button        | Lobby action                                  |
|---------------|-----------------------------------------------|
| A / START     | Host; in the lobby, start the race (host only) |
| R             | Join                                          |
| Y             | Skip the sign-in / LAN-only search            |
| Left / Right  | Cycle character                               |
| Up / Down     | Cycle track                                   |
| L             | Toggle race mode                              |
| B / X         | Back / leave                                  |
| START         | Join the highlighted session in the picker    |

The lobby's text line (a chat prompt in the PC game) is not typeable on the
3DS; it never reached other players in this reimplementation anyway.

## Troubleshooting

- **"Sonic R: game data not found" on the bottom screen** — the data folders
  are not next to `SonicR.3dsx`. The message shows the folder it looked in.
- **No sound** — `sdmc:/3ds/dspfirm.cdc` is missing (see *Get it*).
- **Hangs on the Homebrew Launcher / black screens** — make sure you launched
  `SonicR.3dsx` from its own folder; the launcher shows the folder as one app.
- **The bottom screen reads 268MHZ on a New 3DS** — you are running the
  `.3dsx`, which cannot raise the clock, even with Luma's *New 3DS CPU* option
  set; install the `.cia` for the 804 MHz clock (see *Get it*). The speed-up
  attempt and its result are in the debug log.
- **The lobby's Host/Join does nothing online** — the console has no Wi-Fi
  connection (ONLINE needs an access point; use LOCAL WIRELESS without one).
- Debug output goes to `svcOutputDebugString` (visible in Azahar's log and over
  `3dslink -s`).

## Building from source

Requires devkitPro's 3DS toolchain (devkitARM, libctru, citro3d, picasso,
3dstools). Either:

- **Native (Fedora / Arch-style pacman)** — install `pacman`, import the
  devkitPro key and repositories as described on
  [devkitpro.org/wiki/devkitPro_pacman](https://devkitpro.org/wiki/devkitPro_pacman),
  then `sudo pacman -S 3ds-dev` and export `DEVKITPRO=/opt/devkitpro`,
  `DEVKITARM=$DEVKITPRO/devkitARM`. Without root, the same packages
  (`*.pkg.tar.zst` from `https://pkg.devkitpro.org/packages`) can be extracted
  into any prefix with `tar --strip-components=2`, then point `DEVKITPRO` there.
- **Container** — `docker run --rm -u $(id -u) -v "$PWD":/work -w /work/source/3ds devkitpro/devkitarm:latest make -j`
  (this is what CI does).
- **GitHub Actions** — `.github/workflows/build-3ds.yml` builds the `.3dsx`
  and the CIA on every push to a branch other than `main` and on pull requests
  that touch the game or 3DS code, and can be started by hand from the
  Actions tab (with an option to keep the debug log). The package is attached
  to the run as the `sonic-r-3d-3ds` artifact. On `main` the rolling release
  runs the same workflow and publishes the zip.

Then:

```
make -C source/3ds -j            # -> source/3ds/SonicR.3dsx
make -C source/3ds cia           # -> source/3ds/SonicR.cia (needs makerom, bannertool)
make -C source/3ds deploy-azahar # copy into the Azahar flatpak's SD card
make -C source/3ds run-azahar    # and launch it there
```

The CIA needs `makerom` ([Project_CTR](https://github.com/3DSGuy/Project_CTR/releases))
and `bannertool` ([3ds-bannertool](https://github.com/carstene1ns/3ds-bannertool/releases))
on the path or in `$DEVKITPRO/tools/bin`; the title settings are in
`source/3ds/cia/SonicR.rsf` (New 3DS clock, L2 cache, 124 MB mode, 512 KB stack).

To push a build to a console running the Homebrew Launcher's netloader (press Y
in the launcher): `3dslink -a <console ip> -s source/3ds/SonicR.3dsx`.
