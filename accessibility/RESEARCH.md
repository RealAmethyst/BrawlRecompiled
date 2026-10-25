# Accessibility research record

Findings for the accessibility work, with their evidence. Status lives in `TICKETS.md`; this file holds facts.
Addresses are for USA RSBE01 rev 1 in the Brawl Recompiled layout (modules at the homes in
`wiicompiled/projects/brawl/module_layout.json`). "Verified" means read in code or data; "inferred" means not
confirmed. First recorded 2026-10-25 against upstream `7f3ac07` and decomp `d5154f7`.

## Runtime hook mechanism (verified)

- A wrapper hook: a `CpuContext*` function registered with
  `REGISTER_NATIVE_FUNCTION_AS(0xADDR, Fn, "Fn")` (`runtime/include/abi_bridge.h`). The `_AS` form keeps the base
  translation (`translator/src/Translator.Core/RuntimeNativeIndex.cs`, `excludesBase = !as`), so the wrapper calls
  the original through its alias `extern "C" void func_ADDR(CpuContext*)` (uppercase hex). Existing example:
  `runtime/src/hle/os/os_init.cpp` wrapping `0x801E0480`.
- `PPC_NATIVE_OVERRIDE` and plain `REGISTER_NATIVE_FUNCTION` replace the function and drop the translation.
- The translator scans `runtime/src` for registrations; any added or removed hook needs a full re-translation
  (translate, generate-data-init, emit-build-shards). A stale translation fails at start-up ("Stale generated
  indirect dispatch winner", `runtime/src/abi_bridge.cpp`). Only literal `#if 0` hides a registration from the scan.
- Natives are dispatched through the registry from direct, indirect and virtual calls, and from the PowerPC
  interpreter (`runtime/src/ppc_interpreter.cpp`). Small leaf functions the translator inlines cannot be hooked.
- Copy `r3..r10` before calling the original. A wrapper that passes `ctx` on gets the "complete context" register
  contract: cheap for menu functions, a cost for anything hot in a match.
- Guest memory: `Memory::Read8/16/32`, `TryRead32` (`runtime/include/memory.h`); values come back host-order; an
  unmapped read throws `Memory::AccessViolation`. No guest UTF-16 or msbin string reader exists yet.
- Guest code, including every guest OSThread (cooperative fibers), runs on the process main thread. Hooks should
  only copy data into a queue; speech calls belong on their own thread.
- Mods: a replaced resident module (Project+Ex and REX replace `sora_menu_sel_char`) runs the mod's bytes in the
  interpreter at the disc's addresses, so a wrapper still fires but may see different code and layouts. With Gecko
  codes active, non-resident modules (`sora_menu_main`) are linked in the heap instead of their homes, so hooks there
  never fire. A fail-closed "is this function still the disc's code" query does not exist yet
  (`runtime/src/guest_code_patch.cpp` keeps that state privately).
- Many runtime comments are stale notes from the Mario Kart Wii / NSMBW / Paper Mario origins of the runtime.

## Text sources

- String tables: `.msbin` files, read by `MuMsg` in main.dol: `create 0x800B8930`, `setMsgData 0x800B8C7C`,
  `printIndex 0x800B91B8` (this, slot, entry, bin), `getMsgData 0x800B9348`, `printf 0x800B9230` (verified).
- Format: big-endian u32 offset table (first offset = table size, so entries = off[0]/4); entry i spans
  `off[i]..off[i+1]` (the last to the end of the file) and ends at its first NUL. UTF-8 text (verified).
- Markup (verified: skip table `fn_8006CF54` / jump table `0x804541F8`, tag processor `fn_8006D438` / `0x80454270`,
  default case `fn_8006E830`; the rule parses every entry of the 47 English `menu/msg` tables and 6 toy tables).
  Every byte below `0x20` is a code; lengths include the code byte:
  - 1 byte: `0x09` tab, `0x0A` line break (speak as a space), `0x13` pop style.
  - 2: `0x02` font, `0x07`, `0x08`, `0x0E`, `0x12` push style, `0x1B` alpha, `0x1D`.
  - 3: `0x0D` scale, `0x0F`, `0x10`, `0x1C`. 5: `0x03`, `0x04` colour, `0x0C` text colour, `0x11` outline.
  - `0x0B r n`: ruby, 3+r+n bytes, reading (r bytes) then base (n bytes); keep the base.
  - `0x14 m`: 2 + 2 per set bit of m&7; `0x16 m`: 2 + 4 per set bit.
  - Others (`0x01`, `0x05`, `0x06`, `0x15`, `0x17`-`0x1A`, `0x1E`, `0x1F`) never occur in message files: fail closed.
  - No button glyphs or variables appear inline; some strings are composed in code around an entry. Full-width forms
    (U+FF01..U+FF5E, U+3000) occur and are folded to ASCII.
- On disc: 47 English tables under `files/menu/msg/` (main menu help, stage names, item names, events, Subspace
  `adv_*`, records, system messages) and trophy/sticker tables under `files/toy/` (verified). Several are also
  embedded in the menu `.pac` archives.
- Art-only labels (text baked into textures, no string source): main-menu buttons, rules/items/options labels,
  character-select fighter names, stage-select names (verified from the archives' texture names).
- Stage names have a string source: `stage_name` via `muMenu::exchangeMuStageKindToMsgID 0x800AF6D8` (verified).
- Fighter names: no string table. main.dol holds an internal English/Japanese table used by
  `muMenu::exchangeMuStockchkind2MuCharName 0x800AFA30`, incomplete and with internal names (inferred to be shown
  on records screens). Character-select names are art-only.
- Item names: `item_name` via `muMenu::getItemNameID 0x800AFAB0` (not traced).

## Scenes and menus

- `gfSceneManager` (main.dol, verified): `getInstance 0x8002D018`, `setNextScene 0x8002D5AC` (by name),
  `changeNextScene 0x8002D020` (current at `+4`, next at `+8`). Scene name strings: `muMenuMain`, `scSelChar`,
  `scSelStage`, `scMelee`, `scTitle`. Scene classes live in `sora_scene` (resident).
- Main menu: one scene `muMenuMain` switching `muProc*` objects. `+0x35C` proc array, `+0x3D0` current id, `+0x3D4`
  previous id. Proc vtable at `proc+0x668`: `+0xC` enter (this, x, prevId), `+0x14` process, `+0x18` exit,
  `+0x20` moveCursor, `+0x24` cursor sound. Next proc id at `proc+0x634`.
- Proc ids (verified, built by `sora_scene fn_1_11FBC 0x806CD510`): 0 Top, 1 Vs (Group), 2 1p (Solo), 3 Wifi,
  4 Coll (Vault), 5 Option, 6 Data, 7 OptScreen, 8 OptDeflick, 9 OptVib, 10 OptBtn, 11 OptSound, 12 OptSong,
  13 OptDel, 14 Movie, 15 SoundTest, 16 Record, 17 Info, 18 Count, 19 MovieAdv, 20 MoviePlayer, 21 Diagram, 22 Name,
  23 Figure, 24 Special, 25 Stadium, 26 WifiFriend, 27 WifiAnybody, 28 OptWifi; 30 = rules task (`muRuleTask`,
  `+0x7C0`); 34 = none.
- Enter addresses: Top `0x94018F18`, Vs `0x94019708`, 1p `0x9401716C`. After enter returns, `proc+0x42` holds the
  initial cursor on every path.
- Items (cursor order; art-only labels, texture names from the decoded archive; description = `main` entry
  cursor + base, base 0/6/12):
  - Top (6): Group, Solo, Nintendo Wi-Fi Connection, Vault, Options, Data (`MenMainFrTop10, 11, 15, 12, 13, 14`).
  - Group (6): Brawl, Special Brawl, Tourney, Rotation, Rules, Names (`MenMainFrMul01, 02, 05, 04, 03, 06`).
  - Solo (6): Classic, Adventure Mode, All-Star, Events, Stadium, Training (`MenMainSg00, 10, 02, 04, 05, 06`).
    While All-Star is locked (inferred flag: bit 0 of `*(*(0x805A00E0)+0x24)+0x268`, read by `fn_8004E7C8`) the
    enter patches the navigation table so index 2 is unreachable.
- Titles (art, `MenMainTitle_TopN` material `lambert35`, frame = screen number given to `setSelect`): Top
  "SELECT MODE" (frames 1-6), Group "GROUP" (7), Solo "SOLO" (8), Rules "RULES" (14). Screens from frame 13 on may
  also show the parent title (second material); not verified. No section headers on Top, Group or Solo.
- Main-menu cursor (verified): `muProcMenu::moveCursor 0x806A50F4` (in resident `sora_menu_rule`, shared by most
  procs) returns 1 on a change; cursor u16 at `proc+0x42`; navigation table at `proc+0x63C`. Some procs override it
  (option, sound, vibration, sound test, special, stadium screens).
- Description bar: `muMainPanelCtrlTask` in `sora_menu_main`, `setSelect(this, screenNo, msgIdx) 0x9403A290`
  stores the `main` entry index at `+0x40`; `processAnim 0x94039F1C` prints it with
  `MuMsg::printIndex(*(panel+0x48), 0, *(panel+0x40), *(panel+0x80))`. `printIndex 0x800B91B8` falls back to the
  MuMsg's own table `*(MuMsg+4)` when that last argument is null, and in practice it is null: the menu's procs pass a
  null table to `setMsg`/`setSelect` (verified in the code, and descriptions were silent until the fallback was
  followed). The panel's vtable is at `+0x3C`.
- `setMsg(this, entry, printNow, table) 0x9403A048` (panel vtable `+0x64`) prints at once and stores entry `+0x40`,
  table `+0x80`; Options and its screens call it on every selection change. `setSelect` (vtable `+0x74`) runs in
  enter methods (and in Top, Group and Solo on moves).
- Navigation table: `*(proc+0x63C)`, 16 bytes per item, u16 neighbour per direction bit 1/2/4/8 at +0/+4/+8/+12;
  `moveCursor` returns 1 only when the cursor changed (verified).
- Options (proc 5, enter `0x940187C0`, shared `moveCursor`): 7 items, art labels `MenMainOp00, 01, 02, 03, 04, 05,
  07` = Screen, Deflicker, Rumble, Controls, Sound, My Music, Erase Data; descriptions from the table at
  `0x94041890` = 29, 30, 31, 32, 34, 33, 36; A opens proc cursor+7. Title frame 11 "OPTIONS". Returning from a
  screen puts the cursor on its button.
- Options screens (title frames 20-25, 27; a parent "OPTIONS" breadcrumb may also show, not verified):
  - Screen (7, enter `0x9401AB98`, process `0x9401ACAC`): cursor = value, 0 "4:3 STANDARD", 1 "16:9 WIDESCREEN"
    (`MenMainOption03Font1/2`); left/right toggles and calls `setMsg(60+cursor)`; saved to the record on B.
  - Deflicker (8, enter `0x9401B0E8`): cursor = value from record `+0x19`; A toggles, `setMsg(63-cursor)`; OFF/ON art
    `MenMainWave05.0/.1`.
  - Rumble (9, enter `0x9401CDEC`, own moveCursor `0x9401D3A8`): tabs CONTROLLER / NAMES (`setMsg(64+tab)`); per-port
    toggles by each controller's A (`fn_8004DA94` get / `fn_8004DAB0` set); names tab partly mapped.
  - Controls (10, enter `0x9401E068`): hands over to a `sora_menu_name` task (not mapped).
  - Sound (11, enter `0x9401B4A8`, own moveCursor `0x9401BB4C`): row 0 balance slider (u16 0-100 at `proc+0x66C`,
    which end is 0 not verified), row 1 Sound Test; `setMsg(68+row)`.
  - My Music (12, enter `0x9401E5B8`): reuses the stage picker (`sora_menu_sel_stage`); not mapped.
  - Erase Data (13, enter `0x9401C0A0`, process `0x9401C170`, shared moveCursor): 5 items `MenMainDelete03Font01-05` =
    P1 High Score, Brawl Records, Adventure, Vault Data, Erase All; descriptions 73-77. Dialog: state word
    `proc+0x664` (10-way switch in process), MuMsg `*(proc+0x66C)` printing the question in slot 0 (entries 0, 1, 2
    escalating, 3 "erasing") and YES / NO (entries 4, 5) in slots 1, 2; answer byte `proc+0x670` (0 YES, 1 NO,
    reset to NO with each question). The table is a nested archive in `mu_menumain_en.pac`, not a loose disc file.
  - Settings record: `GameGlobal::getGlobalRecordMenuDatap 0x8004E5CC` = `*(gameGlobal+0x24) + 0x810`.

## Boot and title

- Scene manager instance pointer at `0x805A0060`; fields: `+4` current scene, `+0x10` current sequence, `+0x280`
  scene argument, `+0x288` process step. Scene objects: name at `+0`, vtable at `+4`. `scMemoryChange` between
  scenes shows nothing.
- Boot (`sqBoot`): `scStrap` (argument 0) shows the wrist-strap reminder, the ESRB online notice and NOW LOADING,
  then `scBoot` (save checks; message windows only on first boot or errors, text in `nand_boot`), the prize check,
  then the main menu. The USA game does not show the title at boot.
- `scStrap` tasks in `sora_menu_title` (non-resident), each with a show function: strap `0x940E2398` (art
  `MenBoot02_En`, `MenBoot00_En`, `MenBoot01_En`; reading order not verified; ends after 1200 frames or a button
  after 60), ESRB `0x940E2DEC` (MuMsg `*(task+0x50)`, entry 0/1/2 by language byte `task+0x54`: game text), NOW
  LOADING `0x940E31BC` (art `nowloading`), "4 WAYS TO PLAY!" `0x940E27A4` (argument 1, idle loop only; art
  `MenCont05`).
- Title (`sqTitle`, reached from the main menu's back): `scTitle` start `0x806C9FF0`; the opening movie plays first
  with argument 0. `muTitleTask` show `0x940E3E90` runs once per `scTitle`; art: the logo, PRESS ANY BUTTON
  (`press_any_button`), copyright lines (`enTitleFront_copy`). No menu. About 16 s idle starts the loop: "4 WAYS TO
  PLAY!", a demo battle, How to Play, the opening movie; `scTitle` is rebuilt for each return.
: per player, `0x80689548` maps the token position to a fighter
  kind and calls `0x80696F60` (inferred `setCharKind`) only when it differs from `area+0x1B8`. Area index at
  `+0x1B0`, costume at `+0x1BC`; areas at `muSelCharTask+0x44+4n`.
- Stage select: `muSelectStageTask::processDefault 0x806B30CC`; cursor not located.
- The decomp has no source for `MuMsg`, the scene manager, `muMenu` or any menu module, and almost no symbols in
  the menu modules; the community headers under `include/lib/BrawlHeaders` give some layouts (several checked
  correct, some `.lst` addresses wrong).

## Speech library: Prism (verified against v0.18.3 source and docs)

- Latest release v0.18.3 (2026-09-30), MPL-2.0 (compatible with GPLv3 as a separate component; ship its license
  files and notices).
- Linux backends: Orca over D-Bus (priority 100, linked against glibmm/giomm 2.68, speech goes through the running
  Orca), Speech Dispatcher (97, `libspeechd.so.2` loaded at run time). `prism_registry_create_best` picks the first
  that initializes. Windows: NVDA, ZDSR, JAWS, OneCore, SAPI, UIA. NVDA support needs MSVC's `midl.exe`, so the
  llvm-mingw Windows build should use the official prebuilt DLL.
- API: `prism_config_init`, `prism_init`, `prism_registry_create_best`, `prism_backend_get_features`,
  `prism_backend_output(b, utf8, interrupt)` / `prism_backend_speak`, `prism_backend_stop`, `prism_backend_free`,
  `prism_shutdown`. One backend is not thread-safe: one owner thread.
- The Orca backend's calls are synchronous D-Bus calls: never call it from the game thread.
- Speech Dispatcher backend uses `spd_stop` for interrupt and stop, which leaves queued messages playing
  (`spd_cancel` would discard them). Keep queueing in our own speech thread and hand Prism one line at a time.
- Load at run time (`dlopen` / `LoadLibraryW` from the engine folder), not linked: a linked `libprism.so` would stop
  the game starting on systems without glibmm.

## Launchers (verified)

- Linux launcher: Avalonia 11.2.3 has no AT-SPI support, so no screen reader can read it. It is also not keyboard
  operable: every button is a custom `PaperButton` (a `Border`) that only reacts to a left mouse click, and dialogs
  have no keyboard dismissal.
- Avalonia gained an AT-SPI2 backend in 12.0.0 (PR #20735, merged 2026-02-26; latest stable 12.1.4). X11 only
  (XWayland on GNOME Wayland); open issues include the main window missing from the tree when the accessibility bus
  starts late (#22376) and Orca not reading `LabeledBy` (#21724); no live-region support.
- Windows launcher (WinForms): mostly standard controls with names; gaps are the binding buttons (named only by the
  binding), the unnamed room-code box, colour-only selected page and unannounced status text.
- Launcher-only features: disc install, mods, save import, custom online server. The in-game F10 overlay (ImGui) has
  display, controls and audio settings but no screen-reader output. Headless `--extract` and `--import-save` exist.

### Avalonia 12.1.4 with Orca 51.1: test result (2026-10-25)

- Throwaway test window (TabControl, Buttons, CheckBox, ComboBox, TextBox, status TextBlock) on GNOME Wayland via
  XWayland. Orca announced the window title only; Tab and Shift+Tab were silent.
- The AT-SPI tree (read-only query) is present and correct in shape: frame, page tabs, buttons with names, and the
  focused state moves with Tab. Every layout element is also exposed as a named panel (Border, ContentPresenter...).
- Event recording (read-only listener): window activate/deactivate events arrive, but only one
  `object:state-changed:focused` was emitted for several Tab presses, and Orca did not speak even that one.
- Avalonia emits focus from `WindowBaseAutomationPeer.FocusChanged` through `AtSpiServer.EmitFocusChange`, focused=1
  only, with no matching focused=0 for the previous element. Root cause not established.
- Conclusion: Avalonia 12.1.4's AT-SPI support does not make the launcher usable with Orca today.
