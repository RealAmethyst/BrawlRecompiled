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

- Status: done (2026-10-26: every speech build since is made by the script and has been played)
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
  dependencies (commit fc7e9a4). A clean run from an empty work folder succeeded (2 min 40 s with
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

- Status: done (listening confirmation 2026-10-26: "this is working")
- Parent: A11Y-11. Hooks in `runtime/src/accessibility/main_menu.cpp`.
- Behavior: on opening, the displayed title (mod string; the title is art) interrupts, then the selected item
  follows queued: name (mod string; art), description (the game's `main` message table), position among reachable
  items. Moving the cursor interrupts with the item alone. Solo with All-Star locked counts 5 items.
- Test 1 (2026-10-25): titles, names and positions heard; descriptions silent. Cause: the table lookup ignored
  printIndex's fallback to the MuMsg's own table. Fixed; retest pending.
- Not covered: button hints (none located), Project+ and other SD mods (silent).

### A11Y-15 Story: Options speech

- Status: done (listening confirmation 2026-10-26: "this is working")
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

- Status: done (listening confirmation 2026-10-26: "this is working")
- Parent: A11Y-11. Hooks in `runtime/src/accessibility/title_screens.cpp`: wrist-strap reminder (art, three
  passages, order unverified), ESRB online notice (game text), NOW LOADING (art), "4 Ways to Play!" (art), title
  screen: logo, Press Any Button, copyright lines (art).
- Decisions (2026-10-25): the title screen speaks logo, prompt and copyright; every screen of the idle loop is
  announced each time it appears; movies and the demo battle stay silent.
- Strap screen order (decision 2026-10-25): both right-hand tips, then the bottom instruction.
- Not covered: "SKIP MOVIE" hint (not verified shown). Save-data windows: see A11Y-18.

### A11Y-18 Story: message windows and stage select

- Status: done (listening confirmation 2026-10-26: "this is working")
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

### A11Y-19 Feature: the remaining menus

- Status: in progress (2026-10-26; Steam Audio, A11Y-12/13, follows once the menus are done)
- Parent: A11Y-11. Character select (fighter, costume, player slots, CPU/team settings), the rest of the stage select
  (Random, Change, Back, custom stages), the rules screens (proc id 30), Vault, Data, Sound Test, the Wii Remote name
  save/load menus, results screens, and the remaining Solo modes' menus.
- Each screen: RE first, art captions verified from the textures, event hooks, independent review, listening test.
- Order (2026-10-26): character select (A11Y-21), then the rules screens, the rest of the stage select and the match
  results (research started 2026-10-26), then the Solo menus, Vault, Data, Sound Test and the name save/load menus.

### A11Y-21 Story: character select speech

- Status: awaiting verification (2026-10-26)
- Parent: A11Y-19. Scene scSelChar, module `sora_menu_sel_char` (resident). Findings: `RESEARCH.md`, Character select.
- Decisions (2026-10-26):
  - Every human player's hand speaks. With two or more humans on the screen each line starts with the acting
    player's label ("P2, ..."); with one human there is no prefix.
  - Carrying a token across the grid speaks each fighter the preview shows as "name, row r, column c" (the game's
    layout; a short last row is centred, and its columns are the screen columns it sits under; where it sits half a
    column over, the nearest column to the left). Leaving a fighter (gap or off the grid) is silent.
  - Dropping the token adds no speech (the announcer calls the name), except Random, which has no call and speaks
    "Random".
  - Costume changes speak "Costume n of N" (no costume names exist; N from the game's costume table).
  - Teams speak as "Red team", "Green team", "Blue team" (the card shows the letter R, G or B on the team colour).
  - Opening speaks the mode name from the top bar ("Brawl", "Team Battle"...), then the rules line ("2-minute KO
    fest!") queued after it.
  - Another player's card is named by its place ("Card 3, CPU Level 5"); the player's own card needs no name.
  - Picking up or calling back a token is silent (the game's sound confirms it).
- Implementation: `runtime/src/accessibility/character_select.cpp`, wording in `strings_en.toml` `[character_select]`.
  Hover targets: tokens (with the fighter under them), Back, the mode name, Rules, the rule arrows and value, the ready
  banner, card parts (fighter and costume or team, name, name tab, handicap or stamina, CPU level, Zelda / Sheik and
  Pokémon halves); events: fighter preview, Random drop, player kind, costume, team, Zelda / Sheik, Pokémon, team
  battle, rule value, ready banner (queued), the CPU level / handicap / stamina / name lists. Top bar speech in the Group
  modes (selection modes 0-3) only.
- Not covered: Solo and Wi-Fi top-bar items (targets 9-26, titles); the Rules sub-screen opened from here; whether
  some mode shows LEAVE instead of BACK. The name list's "New entry" keyboard is the shared name keyboard (A11Y-15).
  Free hands over fighter icons say nothing (the game shows nothing either).
- Independent review (2026-10-26) found and these were fixed: the first frame's hover (the hand starts on its name
  tab) interrupted the opening, now queued after it; every drop spoke "Token, fighter" (the dropped token sits under
  the hand), now silent while the drop settles; Rotation's Winner Out / Loser Out toggle was silent (the flag is stored
  before the setter runs); bare "P2" / "Card 3" lines; X / Y could name the wrong player; "List Full" spoken while
  "New entry" shows. Also: a mod replacing a resident module (Project+, REX replace this one) did not silence speech;
  `GuestCodePatch::ReplacedModule()` now does. Open doubts: two players acting in one frame can cut each other off (the
  game's update order); pressing a portrait half falls through to the card, which then speaks again.
- Build: engine `64e832aa` (sha256 prefix) deployed 2026-10-26; previous `2afc6fd9` kept as a backup.
  Compiles; all 17 hooks registered in the translation; strings file parses. Listening test pending.

### A11Y-22 Story: rules screens speech

- Status: awaiting verification (2026-10-26)
- Parent: A11Y-19. muRuleTask (sora_menu_rule): Rules, More Rules, Item Switch, Random Stage Switch, from Group > Rules
  and from the character select. Research: scratch report, to be folded into `RESEARCH.md` with the implementation.
- Decisions (2026-10-26): settings speak "name, value, description, n of N"; Item Switch items "name, On/Off, category,
  description, n of N"; grids use n of N, not rows and columns.
- Decisions (2026-10-26, later): a value change speaks the new value, then its description; TURN ALL speaks "All
  on" / "All off" (mod wording).

- Implementation: `runtime/src/accessibility/rules.cpp`. Review (2026-10-26): no defects; the Melee section label
  ("MELEE NORMAL STAGES") stays silent because which Turn All model shows it is unverified.
- Build: engine `605c7857` deployed 2026-10-26 (previous `64e832aa` kept as a backup); compiles, all hooks
  registered, strings parse; independently reviewed and the findings fixed. Listening test pending.

### A11Y-23 Story: the rest of the stage select

- Status: awaiting verification (2026-10-26)
- Parent: A11Y-19. Random, Change, Back / Leave, the custom stages page, the "Stage Select" title.
- Decisions (2026-10-26): Change's caption names the page it switches to; pressing it speaks the page now shown, in
  the button's words ("Melee Stages"). Custom stage rows: "name, date, time, n of N". A random pick speaks the stage
  the game reveals.
- Decisions (2026-10-26, later): the uncaptioned scroll controls are "Up arrow", "Down arrow", "Scroll bar", "Page
  up", "Page down".

- Implementation: `stage_select.cpp` (extended). My Music's extra entry speaks "Menu" (art) and now counts in N.
- Build: engine `605c7857` deployed 2026-10-26 (previous `64e832aa` kept as a backup); compiles, all hooks
  registered, strings parse; independently reviewed and the findings fixed. Listening test pending.

### A11Y-24 Story: match results speech

- Status: awaiting verification (2026-10-26)
- Parent: A11Y-19. ifVsResultTask (main.dol) after a Group match.
- Decisions (2026-10-26): "Winner, <name>" when the winner line appears (also No Contest and teams); when the table
  appears, every player in place order ("1, P1, Mario, KOs +3, Falls -1, SDs 0, Total +2"); the KO page reads who you
  KO'd, who KO'd you and self-destructs on entering; the statistics list speaks its first entry, then the entry that
  becomes the top row on each scroll step.
- Decisions (2026-10-26, later): when the statistics list can scroll no further, the remaining visible statistics are
  read, queued.

- Implementation: `results.cpp`. Review fixes: the KO'd column showed a time where the screen shows "--:--"; No
  Contest no longer says "Winner" (the banner is blanked).
- Build: engine `605c7857` deployed 2026-10-26 (previous `64e832aa` kept as a backup); compiles, all hooks
  registered, strings parse; independently reviewed and the findings fixed. Listening test pending.

### A11Y-25 Story: Solo menus speech

- Status: awaiting verification (2026-10-26)
- Parent: A11Y-19. The Solo character select variants (titles, difficulty, level, stock, Multi-Man type, Home-Run
  co-op choice), Events (sora_menu_event), Stadium (proc 25) and its Multi-Man list.
- Decisions (2026-10-26): event rows "name, fighter, description, record, n of N".

- Decisions (2026-10-26, later): difficulty icons "Normal, selected, 2 of 5"; event rows add the record's difficulty
  ("1:23 45, Hard"). The prefix rule now also covers the top bar in Group and Solo modes.
- Implementation: `character_select.cpp` (Solo paths), `events.cpp`. Not covered: the Events "Names" button hint and
  the P2 "press START" prompt; Wi-Fi modes.
- Build: engine `605c7857` deployed 2026-10-26 (previous `64e832aa` kept as a backup); compiles, all hooks
  registered, strings parse; independently reviewed and the findings fixed. Listening test pending.

### A11Y-26 Story: Vault and Data menus, Sound Test, name save / load

- Status: awaiting verification (2026-10-26)
- Parent: A11Y-19. Vault, Trophies & Stickers, Data, Records menus; Notices, Brawl Records, Movies, Subspace movies,
  Sound Test; the Wii Remote name save / load screens (muNameTask).
- Decisions (2026-10-26): Sound Test songs "title, source game, n of N" (credits left out).

- Implementation: `main_menu.cpp` (Vault, Trophies & Stickers, Data, Records; name-list returns), `data_screens.cpp`
  (Stadium, Notices, Brawl Records, Movies, Sound Test; captures MuMsg::printf only while Notices or Brawl Records is
  open), `name_remote.cpp`. The "Transfer Names" hint stays silent (when it is shown is unverified). Sound Test's song
  row speaks the song's place in its series.
- Build: engine `605c7857` deployed 2026-10-26 (previous `64e832aa` kept as a backup); compiles, all hooks
  registered, strings parse; independently reviewed and the findings fixed. Listening test pending.

### A11Y-27 Story: the Vault's sub-screens

- Status: ready (after Steam Audio, decision 2026-10-26)
- Parent: A11Y-19. Trophy Gallery, Hoard, Coin Launcher, Sticker Center, Sticker Album, Album, Replays (its delete
  dialog is not a MuNoticeWnd), Challenges, Masterpieces, Chronicle, Stage Builder. Album, Replays and the Stage Builder
  list share one list engine in sora_menu_collect_viewer. Group Records' pages too.

### A11Y-12 Feature: Steam Audio spatial audio

- Status: ready: next session's first item unless the maintainer names other work (2026-10-26). First step: present
  the implementation plan (Steam Audio dependency, mixer changes, the PB map, settings) with A11Y-28 and get the
  go-ahead before building.
- Positional game sounds rendered with Steam Audio HRTF, including elevation (above and below).
- Decisions (2026-10-26): the listener is the game's camera (as the game pans today, plus height); every sound the
  game treats as positional (fighters, items, stage) gets HRTF, while menu sounds, music and the announcer stay as they
  are; loudness stays the game's own (distance, fades, ducking), only the direction changes; elevation strength is
  adjustable, with a "true angles" setting; the settings (HRTF on/off, elevation) live in a new in-game menu,
  Options > Accessibility (decision 2026-10-26: it belongs under Options); its form and upstream impact are designed
  with the accessibility designer before it is built.
- Decision (2026-10-26, after the trace): sounds the game starts as positional but gives no 3D effect (478
  sequences, BRSAR 3D flags 0) are placed too, with their own on/off setting. Sounds with no game distance volume
  (flags 0x4) keep the game's loudness and gain only a direction.
- Note: the game does not pan most positional sounds today (7498 wave sounds, every fighter sound and voice, have
  flags 0x5: volume and surround pan, which stereo ignores), so HRTF adds a direction they never had.

### A11Y-13 Spike: the audio pipeline

- Status: done (2026-10-26)
- Question: where positional information exists for each sound voice, and how Steam Audio can render from real
  positions.
- Findings (2026-10-26, details in `RESEARCH.md` once the spike closes): the game uses NW4R Sound3D for fighters, items
  and stage sounds; the listener is the gameplay camera (rebuilt each frame by the sndSystem update `0x800737E4`); each
  3D sound's world position is in guest memory every frame; `Sound3DManager::Update` (`0x801C6238`) computes volume
  from the 3D distance but zeroes the vertical axis for pan, so elevation is lost only at that last step. The runtime
  mixes AX voices at 32 kHz stereo in 3 ms frames (`ax_mix.cpp` `ProcessVoice`, a per-voice mono buffer before the
  stereo mix: the insertion point); a voice is known there only by its parameter block. Steam Audio v4.8.1
  (Apache-2.0) binaural effect fits per voice.
- Trace (2026-10-26): every 3D wave and sequence sound maps to the parameter blocks the mixer processes (sound,
  player, channel, voice, AX voice, PB), each hop verified; the game's distance gain is in the PB volume envelope and
  its pan only in the main L/R volumes (constant-power), so the pan-free level is sqrt(L^2+R^2). Proposed design: no
  guest hooks; at the AX command-list dispatch a native callback walks the voice manager, validates back-pointers and
  builds a PB -> listener-space direction map handed to the mix worker; voices that fail a check stay in the vanilla
  mix; guest memory is never written (netplay compares it). Open run-time checks: output mode (expected stereo), PB
  address normalisation, streams started as positional. Details to be folded into `RESEARCH.md`.

### A11Y-28 Story: Options > Accessibility settings screen

- Status: ready, together with A11Y-12 (2026-10-26); design questions open.
- Parent: A11Y-10. Holds the HRTF settings first (on/off, elevation strength with a "true angles" choice, placing the
  sounds that have no 3D effect on/off); more accessibility settings later.
- Decision (2026-10-26): it lives under Options as an item named for accessibility.
- Research (2026-10-26, `RESEARCH.md` "Options screen construction"): a real eighth game button needs heavy guest-data
  surgery (no free model, label art or proc; the cut Language item left only placeholders). Recommended: a screen the
  runtime draws itself, like the port's Online menu (ImGui, disc fonts, guest input blocked while open, settings saved
  through the runtime config file), entered as an eighth Options entry at cursor 7 ("E2") with the runtime drawing its
  highlight; fallback "E3": an X / Y button on the Options screen.
- Open for the designer: look for sighted players; position of the entry; wording (entry, description, title, rows,
  values); shown to everyone or only with a screen reader; languages; whether E3 is acceptable if E2 proves fragile.
- Open technically: a mapping-aware input read while guest input is blocked; screen-space placement; a sighted check
  of the Data screen after visiting cursor 7 (model 36 side effect).

## A11Y-20 Epic: audio descriptions for the Subspace Emissary cutscenes

- Status: in progress (2026-10-26). The designer prioritised it ahead of A11Y-12; first goal is the in-game playback
  and a repeatable production workflow, proven on `OPMovie`.
- Decisions (designer, 2026-10-26): descriptions are pre-recorded (Azure neural voice Aria, +10%) by the
  audio-describer agent and approved by listening; the `OPMovie` version 2 description is approved ("perfect"). In game,
  description plays on by default for any movie that has one; a config setting until A11Y-28 provides a switch and a
  description volume. While a description plays, all game sound (movie and any music the game starts with it) is
  lowered slightly; the in-game levels reproduce the approved preview mix exactly (movie 6 dB down for the whole
  described movie, 2 dB more under a clip, clips +2 dB). A pause holds the description and resume continues it where
  it stopped. The description audio is not committed: it ships later as a separate download that the runtime uses
  when present.
- 2026-10-26: all 171 disc movies (`movie/*.thp`: Subspace Emissary cutscenes, opening `OPMovie`, character endings
  `End_*`, `HowtoPlay`, trailers) converted to MKV outside the repository: H.264 video (CRF 14, full range kept) and
  FLAC audio. ffmpeg's `adpcm_thp` decoder is not used for audio: it rounds differently from the game and rejects the
  final packet of most files. A private decoder follows `THPAudioDecode` (`0x8025F508`): per-frame history at
  block +72/+74 (channel 1 +76/+78), coefficients +8 (channel 1 +40), data +80 (channel 1 + channel size),
  `clamp16((c1*h1 + c2*h2 + (nibble << scale << 11) + 1024) >> 11)`. Checked: video frame counts and audio sample
  totals match every source file. `13-00_end` and `33-01_end` have no audio track.
- The THP files carry no text. Character name cards are overlays drawn by `sora_adv_menu_telop` from
  `movie/param/<us|jp>/<movie>.pac`; schedule traced (`RESEARCH.md`, Movies). The 35 US movies with cards got an
  English SRT track (default on) with the card text as drawn, timed from the `MVPM` schedule; verified on `01-00`.
- Not done: `HowtoPlay` (and possibly the trailers) has instructions drawn into the video, not transcribed.
- Design input from the first listening review (`OPMovie`): ducking must be barely noticeable, with the description
  raised instead; names need a pronunciation list (Pikachu was mispronounced; `movies/pronunciation.tsv`).

### A11Y-29 Feature: in-game audio description playback

- Parent: A11Y-20. Status: done. The designer confirmed it in game on 2026-10-26 ("works perfectly"), build
  `8ea9ca5d` with the `OPMovie` description installed in `Play/engine/accessibility/audio-description/`.
- Implementation: `runtime/src/accessibility/audio_description.cpp` (hooks on the movie player's open, play, pause and
  stop; pack loader; movie clock), `runtime/include/audio_description_mix.h` (mixer), both output paths of
  `audio_backend.cpp`, Config.toml `[accessibility] audio_description` (default on). Production and packing:
  `accessibility/AUDIO_DESCRIPTION.md`, `accessibility/tools/ad_pack.py`. Facts: `RESEARCH.md`, "Movie player".
- Checks done: offline self-test (`ad_mix_check selftest`: clip timing, pause fade-hold-resume, loop, stop fade, clip
  limiter; it fails on a broken fade or loop handling); offline mix of `OPMovie` with the runtime's mixer and clock
  matches the approved preview within 0.4 LU in all 47 description windows (-14.6 LUFS overall); independent code
  review, its findings fixed (end-of-movie clock, fades on pause/stop/loop, UTF-8 paths, exception safety, overlap
  checks, pack TOML escaping); the accessibility unit compiles warning-free.
- Acceptance (in game, by the designer): the opening from the title screen and from the Vault describes in sync with
  the picture, at the approved balance; a movie without a description sounds unchanged; skipping stops the
  description; after the movie the game's sound is back at full level.
- Still to check: pausing (the opening cannot be paused); do it with the first described Subspace Emissary cutscene.
