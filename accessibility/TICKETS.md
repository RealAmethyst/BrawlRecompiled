# Accessibility fork: tickets

The authoritative tracker for the accessibility work in this fork. It lives on the `accessibility` branch only;
changes proposed upstream go on their own branches from upstream `master` and never include this folder.

Statuses: ready, in progress, blocked, awaiting verification, done, dropped.

## A11Y-1 Epic: build and run the port from source on native Linux

Build the game following the repository's own instructions, on the Linux development machine, and confirm it runs
before any accessibility work starts.

### A11Y-2 Task: disc image to extracted game data

- Status: done (2026-10-25)
- Input: the Redump NKit RVZ of USA Rev 1. `dolphin-tool header` reports RSBE01, revision 1.
- Extracted with `dolphin-tool extract -g` (the `dolphin-emu-tool` package) straight to a `Disc/DATA` folder;
  no ISO is needed, since the Linux launcher and runtime accept a Dolphin-extracted folder
  (`launcher/linux/MainWindow.cs`, `runtime/src/game_folder.cpp`).
- Evidence: `build_modules.py` with the decomp at `d5154f7` reproduces every tracked output byte for byte
  (`MAP_full.txt`, `MAP_modules.txt`, `module_layout.json`, `brawl_module_layout.inc`), and
  `game/main_translate.dol` matches the SHA-256 pinned in `recomp.yml`.
- Decomp pin: `doldecomp/brawl` after `d5154f7` renames functions and drops the start at `0x807C0994`, so its
  maps differ from the committed ones. Use `d5154f7` until upstream regenerates its maps.
- Game data stays out of git and is never published.

### A11Y-3 Task: native Linux translation and compile

- Status: awaiting verification (scripted build not yet tested in game)
- Upstream translates on Windows (`tools/build-brawl.ps1`) and compiles for Linux in WSL from that output
  (`tools/build-linux.sh`, which rewrites Windows paths with `wslpath`). No native-Linux translate step exists.
- Steps that worked, all under a local work folder (the NAS share cannot hold symlinks):
  - Translator: `dotnet build` of `Translator.Cli` (.NET SDK 10, `net8.0`, run with `DOTNET_ROLL_FORWARD=Major`).
  - Mirror `wiicompiled/runtime`, `aurora-main` and `projects` into the work folder, and `game/` beside it, so
    `recomp.yml`'s relative paths resolve; then the three translator steps from `build-brawl.ps1` (translate,
    generate-data-init, emit-build-shards) with the work folder as the current directory. 120,674 functions,
    600 shards. No path rewriting is needed.
  - Configure and compile as `build-linux.sh` does, with the pinned toolchain from `prepare-portable-tools.sh`.
- Dependency finding: on a machine with SDL3, Abseil, FreeType and libpng installed, aurora's `auto` providers use
  the system copies (the release instead bundles only libpng). The system Abseil's soname changes with every
  update. Force the bundled ones: `-DAURORA_SDL3_PROVIDER=vendor -DCMAKE_DISABLE_FIND_PACKAGE_absl=ON
  -DCMAKE_DISABLE_FIND_PACKAGE_Freetype=ON -DCMAKE_DISABLE_FIND_PACKAGE_PNG=ON`. The result links only
  libpng (bundled), plus the system libz and libbz2, beyond the C/C++ runtime.
- Linux launcher: `dotnet publish launcher/linux/BrawlRecompiled.Linux.csproj` works on Linux.
- Play folder staged like `tools/package-linux.sh` makes it, with `UserData/Config.toml` holding the launcher's
  defaults plus `[paths] dvd_root = "../../Disc/DATA"`, and a `play.sh` that starts the engine without the
  launcher.
- Scripted: `bash tools/build-linux.sh --translate --jobs N` runs the native translation and forces the bundled
  dependencies (uncommitted working-tree change). A clean run from an empty work folder succeeded (2 min 40 s with
  12 jobs). Its binary is not byte-identical to the hand-built one tested in A11Y-4 (768 bytes larger); the hand
  build used 16 translator threads and the script 12. Cause not investigated; the scripted binary is untested.
- Candidate upstream contribution: native-Linux translation in `tools/build-linux.sh`. Upstream's contribution
  rules (`wiicompiled/CONTRIBUTING.md`) require the PR description to be written by the human contributor.

### A11Y-4 Story: the game boots and plays

- Status: done (2026-10-25)
- Evidence: the fork's maintainer started the build from the desktop menu (game only, no launcher) and confirmed it
  boots and runs with sound and controller input. Occasional slight audio stutter: see A11Y-5.

### A11Y-5 Bug: occasional slight audio stutter

- Status: ready (low priority)
- Observed in the first test on the native Linux build. Not yet compared with the upstream v0.4.0 Linux release,
  so it may be in the port itself or in this build.

### A11Y-6 Story: accessible launcher

- Status: blocked (parked 2026-10-25 by decision; game speech first)
- Blocked on: Avalonia's AT-SPI support working with Orca (focus events). Until then the game starts from a desktop
  entry without the launcher. Evidence for an Avalonia bug report is in `RESEARCH.md`.
- Research done (`RESEARCH.md`, Launchers): no screen reader can read the Linux launcher (Avalonia 11.2.3 has no
  AT-SPI) and it is mouse-only. Avalonia 12 added AT-SPI (X11/XWayland only, young, open bugs).
- Decision: build a throwaway Avalonia 12.1.4 test window and have it tested with Orca first; if it reads well,
  upgrade the Linux launcher and replace `PaperButton` with real buttons. Windows launcher gaps are small and
  separate.
- Test result: failed. Orca reads the window title but nothing on Tab; see `RESEARCH.md` for the AT-SPI evidence.

## A11Y-10 Epic: accessibility

Behavior is decided by the accessibility designer. Findings and evidence: `RESEARCH.md`.

Decisions (2026-10-25):
- Aim for upstream. Speech is on by default but only reaches screen-reader backends (Orca on Linux; NVDA, JAWS and
  similar on Windows). TTS backends such as SAPI, OneCore and Speech Dispatcher are never used, so a player without a
  running screen reader hears nothing extra.
- Vanilla first. A hook whose function a mod has changed stays silent; mod support is separate later work.
- Event-based hooks on the game's own functions wherever possible, found by reverse engineering.

### A11Y-11 Feature: speech through Prism

- Status: in progress
- All spoken game content comes from the game's own text (art-only labels get mod-owned localized strings); speech
  goes through one central pipeline using Prism.
- Prism v0.18.3 builds on Linux from source with the portable clang and only the Orca backend
  (`-DPRISM_BACKEND_DEFAULT=OFF -DPRISM_ENABLE_ORCA_BACKEND=ON`, shims/legacy/demos/tests off, `-fuse-ld=lld`):
  `libprism.so`, soname `libprism.so`, needs glibmm/giomm 2.68, sigc++ 3 and GLib. Source tarball SHA-256
  `44d67e8f46fa52f912127eb69ae46298371c7078f1e33d2a5d7001685844ae1b`.
- Orca 51.1 on the development machine exposes `org.gnome.Orca1.Service`, which the Orca backend requires.
- Plan: a speech module under `runtime/src` that loads Prism at run time from the engine folder, creates only
  allow-listed screen-reader backends by registry ID (Linux: `PRISM_BACKEND_ORCA`), owns the backend on its own
  thread with our own queue (interrupt clears the queue), and logs the chosen backend. Hooks only enqueue.
- Speech core implemented (`runtime/include/speech.h`, `runtime/src/accessibility/speech.cpp`); Prism built by
  `runtime/CMakeLists.txt` as an ExternalProject pinned in `runtime/cmake/PrismPin.cmake` and copied beside the game.
  Option `MKW_SCREEN_READER_SPEECH` (default ON). Windows: not wired yet (the module compiles to no-ops there).
- Text pipeline: `runtime/src/accessibility/accessibility_text.cpp` (message-table reader with the game's markup
  rule; mod wording from `runtime/assets/accessibility/strings_en.toml`).

### A11Y-14 Story: main menu speech (Select Mode, Group, Solo)

- Status: awaiting verification
- Parent: A11Y-11. Hooks in `runtime/src/accessibility/main_menu.cpp`.
- Behavior: on opening, the displayed title (mod string; the title is art) interrupts, then the selected item
  follows queued: name (mod string; art), description (the game's `main` message table), position among reachable
  items. Moving the cursor interrupts with the item alone. Solo with All-Star locked counts 5 items.
- Test 1 (2026-10-25): titles, names and positions heard; descriptions silent. Cause: the table lookup ignored
  printIndex's fallback to the MuMsg's own table. Fixed; retest pending.
- Not covered: button hints (none located), Project+ and other SD mods (silent).

### A11Y-15 Story: Options speech

- Status: awaiting verification (deployed 2026-10-25)
- Parent: A11Y-11. Options list; Screen and Deflicker values; Sound (balance as percent from the sound-effects end,
  spoken on every step; Sound Test row); Rumble (tabs, each connected controller with On/Off, the Names tab's names
  with On/Off, toggles); My Music (stage picker through the stage-select speech, then songs with frequency percent,
  the A Play hint, frequency changes); Erase Data with its yes/no dialog; Controls and Group > Names name lists.
- Decisions (2026-10-25): slider values as position percent; spoken on every step; Rumble controller tab announces
  every connected controller on entry and the changed one on A.
- Name keyboard (`keyboard.cpp`, shared by every name entry): key under the cursor with its label and place (Delete
  and Switch characters name icon-only keys; decision 2026-10-25), each typed or cycled character, "Deleted X", the
  character set after a switch, and the name on the OK key.
- Controller setup (`button_setup.cpp`, scene scButton): controller choice, mapping list rows (button, action,
  description, place), Tap Jump / Shake Smash, Default / Test / Save, the action being chosen, the restore / save
  prompts with YES / NO, and the saved message. Button names follow the icons (Classic's stick row inferred to be the
  right stick, spoken as "Stick").
- Remaining: Sound Test, the Wii Remote name save/load menus (their dialogs are covered by A11Y-18), the test battle.

### A11Y-16 Story: boot and title screen speech

- Status: awaiting verification (built and deployed 2026-10-25)
- Parent: A11Y-11. Hooks in `runtime/src/accessibility/title_screens.cpp`: wrist-strap reminder (art, three
  passages, order unverified), ESRB online notice (game text), NOW LOADING (art), "4 Ways to Play!" (art), title
  screen: logo, Press Any Button, copyright lines (art).
- Decisions (2026-10-25): the title screen speaks logo, prompt and copyright; every screen of the idle loop is
  announced each time it appears; movies and the demo battle stay silent.
- Strap screen order (decision 2026-10-25): both right-hand tips, then the bottom instruction.
- Not covered: "SKIP MOVIE" hint (not verified shown). Save-data windows: see A11Y-18.

### A11Y-18 Story: message windows and stage select

- Status: awaiting verification (deployed 2026-10-25)
- MuNoticeWnd (main.dol, `notice_window.cpp`): every yes/no, one-button, message and busy window speaks its body and
  selected answer; covers first-boot save prompts and save errors, Vault, results, Wi-Fi and Subspace notices.
- Stage select (`stage_select.cpp`): the hovered stage's name (game `stage_name` text, same names as the art) and its
  place on the page. Random, Change, Back and the custom-stage page are silent (captions not verified).
- Speech stays silent once Gecko codes patch the game (`GuestCodePatch::Active`).

### A11Y-17 Task: review fixes for the speech layer

- Status: done (2026-10-25). An independent review found: a hang at `std::exit` (speech state destroyed while the
  worker waited; now never destroyed), Linux builds failing without glibmm or offline (now speech switches off with a
  message; Prism excluded from ALL), stale strings in the build folder (now copied on change), plus exception guards,
  a stale-panel check, an exception-safe enter flag, bounds and log-spam fixes, and a packaging name fix.

### A11Y-12 Feature: Steam Audio spatial audio

- Status: blocked on A11Y-4 and A11Y-13
- Positional game sounds rendered with Steam Audio HRTF, including elevation (above and below).

### A11Y-13 Spike: the audio pipeline

- Status: ready
- Question: where positional information exists for each sound voice. The runtime emulates the Wii AX audio
  library (`wiicompiled/runtime/src/hle/audio`); the game links NW4R snd (its symbols are in the decomp map,
  including `SoundArchive::ReadSound3DParam`). By the time a voice reaches AX it carries only volume and pan, so
  find where the game still has the emitter and listener positions, so Steam Audio can render from real positions.
  Unverified until traced.
