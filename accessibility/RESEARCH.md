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
- Highlighted button = model `proc+0x644` + cursor (`sora_menu_rule 0x806A586C`); the bases set in each enter (Top 0,
  Group 6, Solo 12, Vault 22, Options 29, Data 36, Records 108, Trophies & Stickers 175) tie captions to cursors through
  the models' textures (verified).
- Tooling: vtable dumps of `sora_menu_main` need text base `0x940040CC` (image + section-1 offset); `0x940040D4` puts
  every entry 8 bytes high.
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

## Shared message window (MuNoticeWnd, main.dol)

- Constructor `0x800BF204`, destructor `0x800BF2CC`, vtable `0x80456D54`. Fields: `+0xFC` type (0 no buttons, 1 two
  buttons, 2 one button, 3 busy), `+0x100` selection (1 = first label, left; 0 = second, or B), `+0x104` state,
  `+0x4C`/`+0x50` message table and entry, `+0x118`/`+0x11C` copied text. Created by `0x800BEEB8` (table + entry) or
  `0x800BF048` (text). Every slot prints through `0x800BF850(win, slot, text, length, flag)`: body slot 0 (types 0,
  3) or 1 (types 1, 2); labels slots 2, 3 (type 1), 4 (type 2). States: 0 open (enter `0x800BFB14`), 1 input (process
  `0x800BFCF4`), 2 close (enter `0x800BFF34`). Callers include scBoot, Vault, stage and character select, results,
  Wi-Fi, Subspace and the name screens.
- scBoot (`muBootNandTask`): first boot asks `nand_boot` entry 0 (Yes 14 / No 15), then "Creating..." (1), then 2.

## Options details

- Sound: balance u16 0-100 at `proc+0x66C`; left adds 1, right subtracts 1; `fn_80076F30` (direct call, also used by
  load paths) applies music = min(1, 2(100-v)/100), effects = min(1, 2v/100). No number shown.
- Rumble: pad state `proc+0x66C+4*pad` (-1 none, 0 GameCube, 1 Classic, 2 Wii Remote, 3 Nunchuk); rumble byte
  `*(GameGlobal+0x24)+0x820+pad`, shown on only when 1 and not Classic; toggles inlined in the input override
  `0x9401D3A8`. Names: 120 records at `*(GameGlobal+0x28)`, 0x124 each, UTF-16 name at `+0xE0` (6 units), rumble
  flag `+0xEC`; selected index `proc+0x68C`.
- My Music: state `proc+0x664` (0 stage picker, task at `+0x694`); group `+0x670`; song ids `+0x698`, count `+0x6D4`;
  group 4 merges 4 and 5 split at `+0x688`. Frequency getter `fn_80078D30`, setter `fn_80078CD8` (hooked); titles
  `snd_title` via the song info record (`fn_800791FC`, `+0x10`).
- Name list (`muNameTask`, sora_menu_name, resident; Controls and Names): list at `+0x1F0` (s16 record at `+2`, -1/-2
  "New entry"), count `+0x3D8`, row `+0x1EC`, scroll `+0x1EE`, state `+0x9C` (2 list); redraw `0x80677354`.
  "@New entry" (main.dol): '@' draws as an icon in the menu font, not a character (inferred).
- Message-table markup payloads can hold NUL bytes (`0C 00 00 00 FF`); an entry ends at a NUL outside markup.

## Rules screens (muRuleTask, sora_menu_rule, resident)

Recorded 2026-10-26 from the module's disassembly (`dis.py 18`), the rules archive (`menu2/sc_selcharacter2_en.pac`,
byte-identical to `mu_menumain_en.pac` entry 16) and the main-menu title archive; constants as used by `rules.cpp`.

- Task: ctor `0x806A3B24(this, x, archive, panel, mode, prio)` runs once per opening and enters the first page itself;
  process `0x806A4844` (per frame); dtor `0x806A4678`. Pages (`muProcMenu`, vtable at `proc+0x668`): `task+0x268` Rules
  (vtable `0x806AE520`), `+0x26C` More Rules (`0x806AEC00`), `+0x270` Item Switch (`0x806AECC8`), `+0x274` Random Stage
  Switch (`0x806AEE48`). Current page `+0x278` (5 none), closed u8 `+0x288`, mode `+0x28C`, panel `+0x290`.
- Enter / moveCursor (vtable +0xC / +0x20; all four override moveCursor; none inlined): Rules `0x806A5BEC` /
  `0x806A6F68`, More Rules `0x806A84F4` / `0x806A9498`, Item Switch `0x806AA5E8` / `0x806AAA6C`, Stage Switch
  `0x806AB724` / `0x806AC16C`. Enter r5 = the page it came from (6 = task opening). Page process returns 1 back, 2
  forward; next page = `*(proc+0x634)` - 30. Back from the mode's first page closes (`+0x288` = 1).
- Mode (verified): 0, 1, 2, 6 open Rules; 3 Item Switch alone; 4, 5 Stage Switch alone. Callers: main menu mode 0 (proc
  30, task at `muMenuMain+0x7C0`, built anew each time; on `+0x288` the menu returns to proc 1); character select (see
  there); Tourney modes 1 and 4; Rotation mode 1. Start also backs out except in the main menu (`0x806A4BBC(task, 0)`).
- Titles: only the main menu's panel setSelect (`0x9403A290`) draws one: Rules screen 14 "RULES", More Rules 15 "MORE
  RULES" (`MenMainTtlJ.08`, `.09`); the switch pages use screen 32 (inferred: title bar hidden). The character select's
  setSelect `0x8069F104` is empty. Item Switch and Random Stage Switch show their own title art everywhere
  (`MenMainSwitch06Title`, `MenMainSwitch10Panel`). No button-hint text exists.
- Text: rules table (archive msbin type 1 idx 0): 0-16 Rules descriptions, 17-26 More Rules, 27-75 item names, 76-79
  NONE..HIGH, 80-128 item descriptions, 129-132 frequency descriptions, 133 / 134 turn all on / off, 135-146 item
  categories. The ctor installs it in the panel MuMsg (`*(panel+0x48)`) and restores the old table on close; Rules and
  More Rules print with `setMsg(panel, entry, 1, null)` (panel `proc+0x658`). The switch pages clear the panel and print
  into their own MuMsg `proc+0x66C` (Stage Switch: idx 1, = `stage_name`).
- Rules page: cursor u16 `+0x42`; u8 values `+0x66C` style (0 time, 1 stock, 2 coin), `+0x66D` time (0 infinite),
  `+0x66E` handicap (shown 0 off, 1 on, 2 auto; gmSetRule stores 0 off, 1 auto, 2 on), `+0x66F` damage 5-20, `+0x670`
  stage choice (0 Choose .. 4 Loser's Pick), `+0x673` stock. Rows: Rules, Time Limit (Stock in a stock match), Handicap,
  Damage Ratio, Stage Choice, Item Switch, More Rules (art `MenMainRule00font`). Enabled u8 per row `+0x6A0`; `+0x6A7`
  More Rules shown (inferred unlock, bit 2 of `*(GameGlobal+0x24)+0x268`). Descriptions `0x806A81E4` (jump table
  `0x806AE500`). Values are working copies of gmSetRule `*(*(0x805A00E0)+0x1C)`, written back on leaving the page.
- More Rules: u8 `+0x66C..+0x670` = rule +8..+0xC: Stock Time Limit (0 infinite), Team Attack, Pause, Score Display,
  Damage Gauge (0 off, 1 on); row 5 Random Stage Switch hidden unless `+0x69A` (inferred unlock, bit 3). Enabled
  `+0x694`. Descriptions 17, 18+v, 20+v, 22+v, 24+v, 26.
- Item Switch: row `+0x674`, column `+0x670`; place 0 frequency (row 0); row 1: columns 0-1 place 1 (Turn All), else the
  column; rows 2-8: (row-1)*7 + column; item bit = place-2 (49 items). Frequency s8 `+0x678` (0-3); mask hi `+0x680` /
  lo `+0x684` (menu record +0 / +8 / +0xC, saved on back). Item i: name 27+n, description 80+n, n = `0x806AD3C4[i]`,
  category `0x806AD300[i]`. Text `0x806AB118` runs on every visible change; moveCursor returns 0 for a Turn All press.
- Random Stage Switch: only unlocked stages listed, Brawl slots into `+0x688[]` (count `+0x704`), Melee into `+0x708[]`
  (count `+0x730`); column `+0x670` (0-4 Brawl, 5-6 Melee), row `+0x674` (0 Turn All), Melee rows `+0x67C` (the row
  below is Custom Stages, shown when u8 `+0x760`, i.e. word `0x805A02A4` == 0). Game places: 0 / 1 Brawl / Melee Turn
  All, 2-32 Brawl, 33-42 Melee, 43 custom. Names: Brawl entry `0x806AEDA0[slot]`, Melee 31+slot, custom 41. Mask hi
  `+0x738` / lo `+0x73C` (Melee bits 40-49, custom 50). A refuses to clear the last stage. Quirk (verified): with stages
  locked, a toggle flips the bit of the shown position while the icon shows the slot's bit.
- On / Off = the switch art's colour-animation frame (0 OFF, 1 ON): `*(*(object+0x14)+0x18)` is an AnmObjMatClrRes
  (vtable `0x80465D08`, frame float +0x18); objects `*(proc+0x64C)`: items 32+i, item Turn All 81, stage Turn All 85 /
  86, Brawl 87+p, Melee 118+q, custom 128. A Turn All at frame 1.0 reads TURN ALL ON and turns its group on.
- Open: which Turn All model's "MELEE NORMAL STAGES" label is visible (both draw it); gmSetRule +0x18 (Special Brawl
  greying); whether the GROUP breadcrumb shows on Rules / More Rules.

## Stage select (muSelectStageTask, sora_menu_sel_stage, resident)

Extended 2026-10-26 from the module's disassembly (`dis.py 11`) and the archive `menu2/sc_selmap_en.pac`; constants as
used by `stage_select.cpp`.

- Create `0x806B0984(params)`: mode `+0x40` = params[0]; init `0x806B0EB0` runs once per opening for modes 0-2 (page 0,
  lists and models exist on return); close `0x806B0E5C`. Modes: 0 VS, 1 VS Wi-Fi, 2 My Music (from `0x9401E5B8`), 3-6
  random pick without a screen; Wi-Fi = modes 1, 4, 6 (mask 41 on mode-1; selection modes 12-18).
- processDefault `0x806B30CC`. Free pointer; hover update `0x806B626C` (every input frame) stores the region from hit
  test `0x806B6EA8` at `+0x244`: 0 none, 1 forced refresh (custom list rebuilt), 2..n+1 icons, 41-47 custom rows (loaded
  rows only), 48 / 49 up / down arrow, 50 scroll thumb, 51 / 52 track above / below, 53 Change, 54 Random, 55 Back. Page
  `+0x228` (0 Brawl, 1 Melee, 2 custom); per page `+0x22C+8p` icon id list, `+0x230+8p` count. Kind = u16 at
  `(*(0x806B9298+8*page))[id*4]`; name = `stage_name[byte 3 of 0x80407AAC+4*kind]` (same names as the art).
- Title (VS only): "STAGE SELECT" (`MenSelmapBack07`) on the Back model `+0x5C`, which My Music does not load (no title,
  Back or Random there). No page title and no button hints exist.
- Change (`+0x1F4`) is captioned with the next page: PAT frame = bytes `0x806B9140[next]` = {3, 1, 2} = NORMAL STAGES,
  MELEE STAGES, CUSTOM STAGES. Next = page+1 mod 3, skipping empty pages, and page 2 unless mode 0 with custom count
  `+0x604C` > 0. The press is inside the decision `0x806B5630` (writes `+0x228` at `0x806B5940`), then show page
  `0x806B1F58` (also run by init and the random reveal). `0x806B36DC` (per frame, mode 0) rebuilds the custom list and
  re-sets the caption when the built counts `+0x605C` / `+0x6060` change.
- Random (`+0x1FC`) "RANDOM". Back exists when u8 `+0x6150` (1, except Tourney: params+44); VIS frame (`0x806B1C18`) 0
  "BACK", 1 "LEAVE" (Wi-Fi), 2 none.
- My Music: page 1 gains id 255 "MENU" (icon and preview frame 100); MuStageKind 30 is skipped; no page 2; the cursor
  starts on icon 0 (frame 9). In VS it starts at (3.5, -17.5); what lies under it is not determined.
- Custom page (mode 0): file task pointer `0x806BB388`; list `+0x28C` (12 bytes: u8 flags, 0x80 = source; +4 index; +8
  load slot), count `+0x604C` (max 2000), sorted newest first (inferred from the file names). Hovered list index
  `+0x24C` = top + region - 41 (top = `(int)` float `+0x6088`). Slot info `fileTask+0x70+88*slot`: +0 status (3
  loaded), +20 UTF-16 (5 units, traded stages; inferred sender), +32 UTF-16 (16 units) the stage name (verified: the
  preview prints it), +72 OSTime, +80 type (row mark). Row printer `0x806B2908`: slot 1+r date ("%4d.%02d.%02d" when the
  language byte `*(GameGlobal+0x24)+0x82A` is 0, else "%2d.%02d.%4d"), 8+r time "%2d:%02d", 15+r sender, 22+r name.
- Decision `0x806B5630` returns 1 chosen, 2 back, 3 random. A off an icon chooses the last previewed icon `+0x248`;
  Start with nothing chosen means Random. Random stores page `+0x6154`, index `+0x6158`, kind `+0x254` (1 stage, 3
  custom) and sets state `+0x224` = 4; `0x806B4D70` shows that stage, then state 2 (the reveal). State 11 = result ready
  (`+0x274` 1 chosen, 2 back).
- Hooks used (translated, called directly, not inlined): init, hover, decision, show page, `0x806B36DC`, `0x806B4D70`.

## Character select (muSelCharTask, sora_menu_sel_char, resident)

Recorded 2026-10-26 from the module's disassembly (`dis.py 10`), the disc archive `menu2/sc_selcharacter_en.pac` and
`sel_char_en.msbin`. The community headers' `muSelCharHand` layout is 4 bytes low from `+0x9C` on.

- Task: vtable first entry processDefault `0x806876D4` (switch on `+0x40`: 0 main, 1 confirmed, 2 forward, 3/4 back,
  6-8 dialogs); create `0x806828C4` runs init `0x80682EB4(task, params)` once per opening, which builds the cards,
  may call next costume to split duplicates, and draws the banner, rules line and mode name. Cards (`muSelCharPlayerArea`)
  at `+0x44+4n`, count `+0x648`; grid icons: kinds u32 from `+0x70`, count `+0x138` (fighters the save has unlocked,
  in the module-17 table order, then Random); team battle u8 `+0x5C8`; stamina u8 `+0x5D0`; handicap rule u32 `+0x5CC`
  (2 = on); rule kind u32 `+0x5D4` (0 time, 1 stock, 2 coin), time `+0x5D8` (0 = infinite), stock `+0x5DC`; ready
  banner state `+0x5E8`; Rotation loser-out flag `+0x620`.
- Selection mode: u8 at `*(*(0x805A00E0)+0x10)+4` (getter `0x80683B28`, inlined). 0 Brawl (`sqVsMelee`), 1
  `sqQuMelee` (inferred Rotation: the logo shows WINNER OUT / LOSER OUT), 2 Special Brawl, 3 Tourney; 4-11 Solo, 12-18
  Wi-Fi (see "Solo modes").
- Rules overlay: hand target 4 (press `0x8068A4F8`) calls `0x80692E9C(css, idx)`, mode = bytes `0x806A0D18[idx]` =
  {0,3,5,6,2,1}: selection mode 0 → rules mode 0, 1 and 3 → 1, 2 → 6, 4-11 → 0, 12 and up → 2; targets 25 / 26 (Wi-Fi
  only) open mode 3 (Item Switch) / 5 (Stage Switch). Task `css+0x62C`; state u32 `css+0x630` (stored 1 at `0x80692FB4`;
  2 closing once `task+0x288` is set, applies the rules; 3 waiting; 0 after both exit); the CSS panel
  (`muSelcharPanelCtrlTask`) at `css+0x628`. `0x8069301C` runs it from processDefault while the state is non-zero;
  the state returning to 0 = back on the character select (hooked).
- Card: hand `+0x1A8`, token `+0x1AC`, place `+0x1B0`, player kind `+0x1B4` (0 none, 1 human, 2 CPU), MuSelchkind
  `+0x1B8`, costume `+0x1BC` (index into the fighter's list), team `+0x1C0` (0 R red, 1 B blue, 2 G green: flag CLR
  frames 0/30/60), name record `+0x1C8`, "NO." number `+0x1CC`, handicap or HP / 10 `+0x1D0`, CPU level `+0x1D4`
  (0-8, shown 1-9), controller `+0x1DC` (-1: no hand), Zelda / Samus / Trainer choices `+0x1E8/+0x1EC/+0x1F0`, task
  `+0x1F4`, list `+0x1FC`, MuMsg `+0x40C`. Shown kind = `exchangeCharKindDetail 0x806948D4` (tables `0x806A0D78`
  Zelda/Sheik, `0x806A0D7C` Samus, `0x806A0D80` Trainer, Charizard, Squirtle, Ivysaur). No control on this screen
  writes the Samus choice.
- Hand: area `+0x9C`, carried token `+0xA0`, mode `+0xA4` (0 free, 1 token flying back, 2 flying to it, 3 drop, 4
  rule value, 5 handicap list, 6 CPU level list, 7 name list, 8 keyboard), mode argument `+0xA8` (the card for 5-8),
  target `+0xAC`, target card `+0xB0` (written only for tokens and card parts). Token: holder hand `+0x10`, card `+0x14`,
  state `+0x18`. The hand processDefault `0x8069CB18` only fades animations; the task drives everything.
- What a hand points at: `0x8069DCD4(hand)` stores the hit test `0x8069DB68` at `+0xAC` every frame. Targets: 1 a
  grabbable token (own, or any CPU's); top bar (table `0x806A0C70`) 2 Back, 3 the mode name (A toggles team battle;
  Rotation: winner / loser out), 4 Rules, 5 right arrow (+1, `0x8068F99C`), 6 left arrow (-1, `0x8068FBD0`), 7 the value
  (hold A), 8 the ready banner, 9-26 Solo and Wi-Fi items (see "Solo modes"); card parts (table `0x806A0EF8`) 27 the
  card (next costume, next team in team battle), 28 name (pencil icon), 29 the name tab (cycles none / human / CPU
  through `0x8068BE7C(task, card, kind)`), 30 handicap or stamina, 31 CPU level, 32/33 Zelda (right half) / Sheik (left
  half) of the portrait (`0x80694934`), 34-37 Trainer, Charizard, Squirtle, Ivysaur quarters (`0x80694A04`), 38 mode 12
  only. Hands are never tested against fighter icons.
- Fighter: the token's position picks it. `0x80689548` (per frame) calls `setCharKind 0x80696F60(area, kind)` only on
  a change, live while a token is carried, 40 off the icons; not called during init. A in the grid
  `0x8068ADC4(task, card, pad)` picks up a token (`hand+0xA0` set) or drops it (refused on kind 40), then
  `0x8068AF60(task, card, kind)` confirms and plays the announcer's call (none for Random). `0x8068AF60` also runs for
  automatic placements (switching a card to CPU gives Random).
- Costumes: `getNumCharColor 0x800AF8D0` (record `0x80455458 + 16*kind`, pointer at +8 to 2-byte entries ending at a
  first byte of 12): most 6, Pikachu 4, Trainer and Pokémon, Lucario, Jigglypuff, Sonic 5, Wario 12. No names. Next
  `0x8069A22C`, previous `0x8069A340` (X / Y on GameCube and Classic, A on the card); team next `0x8069987C`, previous
  `0x80699BC4` (X / Y and A in team battle). Team battle `0x8068ED8C(task, on)` also sets the mode name.
- Card lists (`MuSelctChrList`): open `0x8069F240(list, type, value)`, process `0x8069FE88(list, pad, out)` (up / down
  move the cursor at once), type `+0` (0 names, 1 CPU level, 2 handicap, 3 stamina), cursor `+0x44`, name count
  `+0x6C`, records u16 from `+0x70` (sorted by record `+0xF0`). Rows: CPU level "n" and `sel_char` 15+n; handicap
  "n×10%" (full-width digits); stamina 1 then n×10; names: -1 and count+1 "New entry" (art; "List Full" with 120 names),
  0 the default label, then names. Applied by `0x8069AF1C`, `0x8069ABF8`, `0x8069B384`.
- Ready: rule `0x8068D338`; banner `0x8069186C(task, show)` only on a change. Start or A on the banner confirms.
- Text: `sel_char` 0 "PLAYER" + `%2d` card, 1 "NO." + `%02d`, 2 "CPU", 3 "  NONE" (name tab, printer `0x8069B1D8`);
  4-7 the rules line after its number, which is drawn as digits to its left ("2-minute KO fest!"; 0 draws an infinity
  sign; stamina shows 7 alone); 15-23 CPU level names. Art: name plates `MenSelchrChrNm` (frame from
  `exchangeMuSelchkind2MuStockchkind`, captions in `strings_en.toml`; the grid's small labels misspell "ZARO SUIT
  SAMUS"), card label P1-P4 / CPU (`setPlayerKind 0x80698348`), token "P" + place or "CPU", mode names Brawl, TEAM
  BATTLE, SPECIAL BRAWL, WINNER OUT, LOSER OUT, BACK (LEAVE in some mode, not traced), RULES, READY TO FIGHT!, CPU
  Lv., HANDICAP, STAMINA, team letters. No fighter-name table: `0x800AFA30`'s internal one has no Random, stores é as
  U+2208 and is not used here.
- Grid layout: icon i at bone `pos{i+1}` of `MenSelchrPos_TopN` at frame = icon count; rows fill left to right, top
  to bottom, a short last row centred (half a column over when the gap is odd). Row lengths by count: up to 5 one row,
  6-10 5, 11-18 6, 19-21 7, 22-24 8, 25-27 7, 28-32 8, 33-36 9, 37-40 10, 41-45 9, 46-50 10. A fresh save has 22 icons
  (8, 8, 6), everything unlocked 36 (4 × 9).

## Solo modes (selection modes 4-18, Events, Stadium)

Recorded 2026-10-26 from the disassembly of `sora_menu_sel_char` (module 10), `sora_menu_event` (module 9),
`sora_menu_main` and `sora_scene`, and decoded archive art; constants as used by `character_select.cpp`, `events.cpp`
and `data_screens.cpp`.

- Solo proc A (`0x94017500..`) passes an exit code to `0x806A5604(proc, code, 0)`: Classic 4, Adventure 5, All-Star 6,
  Events 8, Training 12; Stadium opens proc 25. `sqMenuMain` switch `0x806DC5DC` (jump table `0x807019A4`, codes 0-41;
  an earlier note's `0x806DD9A4` is wrong): 4 sqSingleSimple, 5 sqAdventure, 6 sqSingleAllstar, 7 sqSingleBoss, 8
  sqEvent, 9 sqTargetBreak, 10 sqHomerun, 12 sqTraining; 11 and 37-41 Multi-Man types 0-5 (`0x806DC708`).
- Selection modes (stored by each sequence's main function): 4 Classic, 5 All-Star, 6 Boss Battles, 7 Target Smash, 8
  Home-Run Contest, 9 Multi-Man Brawl (sqKumite), 10 Training, 11 Events; 12 sqNetFriendMelee, 13 sqNetFriendKumite, 14
  sqNetFriendHomerun, 16 sqNetAnyOkiraku, 17 sqNetAnyTeamMelee; 15 and 18 have no writer in `sora_scene`. Module 10
  groups 0-3, 4-11, 12-15, rest; `0x806839F0` is true for the last two (Wi-Fi).
- Character select cards `+0x648`: Classic and mode 18 one; 5-11 and 13-15 two (Events without a second player one);
  else four. Co-op u8 `+0x5FC`: set at init or when P2 joins (`0x806898C0` → `0x8069592C(area, pad)`, then the title is
  redrawn); never cleared.
- Title `0x8068ED8C(task, team)` (init and P2 join): table `0x806A0B20`, 12 bytes per mode (u16 model, solo frame +2,
  co-op frame +3, flags +4, record category +5). Modes 4-11 use model 180 `MenSelchrTitleS_TopN`; frames solo / co-op:
  Classic 1/1, All-Star 2/14, Boss Battles 12/13, Target Smash 6/7, Home-Run 8/9, Multi-Man 10/11, Training 5/5, Events
  3/4; co-op frames add "Co-op" (`MenSelchrRtitle.25`) beside the name.
- Top-bar targets (table `0x806A0C70`, visibility `0x806A16B8`, presses in `0x8068A358(task, card, pad)`): 9-13
  difficulty icons (modes 4-7; icon i while i < u32 `+0x5E4`; A sets u32 `+0x5E0` = target-9); 14 right arrow (+1), 15
  left (-1), wrapping at `+0x5E4`; 16-20 stock 1-5, 21 right / 22 left arrow (Classic only; u32 `+0x5DC`); 23 Home-Run
  play order (modes 8, 14; Solo only in co-op; toggles u32 `+0x600`); 24 Multi-Man type (modes 9, 13; u32 `+0x604`
  0-5, wrapping); 25 / 26 Wi-Fi only. The mode name and Rules are not hittable in Solo.
- Captions: difficulty strip `MenSelchrSngl04.1` EASY, NORMAL, HARD, VERY HARD, INTENSE (Target Smash: PAT frame 1,
  "LEVEL" 1-5); only the selected one is enlarged and captioned (CHR records `0x806A16F0`). `+0x5E4` is 5 except in
  Target Smash (levels unlocked). Home-Run: value 0 shows "Play Cooperatively", non-zero "Play Alternately" (CLR via
  bytes `0x806A0CE8`; an earlier note had them reversed). Multi-Man: game text `sel_char` 9-14 (table `0x806A0CF0`).
- Display setters (r4 = new value; the store follows the call): difficulty `0x80692298`, stock `0x806923A8`, Home-Run
  `0x80691DA4`, Multi-Man `0x80692048`. Inlined: the difficulty store `0x806888B0`, getters `0x80684DC4`, `0x80684DA4`.
  Records: `dispHighScore 0x806927A0` (slot-to-label mapping not traced).
- Events (`muEventTask`, processDefault `0x940A4490`): state `+0x1A0`, entered state `+0x1A4` (stored after the enter);
  table `0x940A6950`: 0 build (process `0x940A5740` returns 1 once = opening), 1 intro, 2 list (enter `0x940A58D8`,
  process `0x940A591C`), 3 difficulty (enter `0x940A5E54`, process `0x940A5FB8`), 4 start. Title art `MenEvent00Ttl`.
- Event list: 16-byte records at `task+0xFB0+0x404*table`, count u8 `+0x13B0+0x404*table` (+0 name, +4 description,
  u8 +8 P1 MuSelchkind (40 = player chooses), +10 P2, +11 flags (0x40 score), +12 number); table u32 `+0x17BC` (0 solo,
  1 co-op: another controller's Start, `0x940A4A3C` → `0x940A4DB4(task, 1)`; never back); cursor `+0x17B8`. Panel
  `0x940A5298(task, cursor, table)` on every change: MuMsg `+0x17C0` (`event_en`) slot 0 "#n " + name, 1 record, 2 solo
  fighter (`0x800AFA30`, or entry 2 "Choose"), 3 description. Record `task+0x3B0+0x600*table+24*event+8*diff` (bit 0x80
  cleared, u32 +4): "%d" or "%2d:%02d %02d", else "-" / "--:-- --"; the hardest cleared shows as a level icon (VIS
  frame hardest+1 of `MenEvent0012`). Difficulty u32 `+0x17C8`: 0 Easy, 1 Normal, 2 Hard (cursor positions).
- Stadium (proc 25): enter `0x940310A0`, process `0x940314DC`, moveCursor `0x94031AC0` (bits 4 / 8), Multi-Man input
  `0x94031CB4` (bits 1 / 2); both return 1 on a change. State `+0x664`: 0 intro (from Solo), 1 list, 2-3 Multi-Man list,
  4 back to the list, 5 leaving. Count `+0x670` (4 when `fn_8004E7EC` is 1, inferred Boss Battles unlocked, else 3),
  kinds `+0x674`; MuMsg `+0x66C` (archive type 1 idx 2): slot 0 name 0-3, slot 1 description kind+5, Multi-Man slot 1
  10+cursor. Art: title `MenStadiumPanel05Title`, Multi-Man labels `MenKumiteTtl01-06`. Codes: 9 Target Smash, 10
  Home-Run, 7 Boss Battles, Multi-Man 11 and 37-41. Return cursor 9 → 0, 10 → 1, 11 → 2, 7 → 3; codes 41-46 reopen the
  Multi-Man list (state 3) with cursor code-41.
- Solo results use `ifSimpleResultTask` (main.dol, create `0x800ED474`, `simple_result.msbin`); not mapped.

## Vs results (ifVsResultTask, main.dol)

Recorded 2026-10-26 from main.dol, `sora_melee` and `sora_scene` disassembly, `menu/result/Result_en.brres` and
`vsres_en.msbin`; constants as used by `results.cpp`.

- A main.dol gfTask, so hooks hold with modules relocated: vtable `0x8045AF08`, ctor `0x800E5858` (converts the result
  record in place to display units, `0x800EB314`), setup `0x800E5E34`, build `0x800E9878`, processAnim `0x800E67A4`
  (per frame), dtor `0x800E7CA8`. Created by `sora_melee 0x8095B8DC` (stOperatorRuleVsResult), exited by `0x8095B868`
  at state 12. vsres table at `task+0x4448`.
- Result info `ri = *(*(0x805A00E0)+0x18)`: `+0x00` mode tag (1 Tourney, 24 Basic Brawl), `+0x01` rule (0 time, 1 stock,
  2 coin), `+0x02` team battle, s8 `+0x03` SD penalty, `+0x0E` bit 0x40 Special Brawl, `+0x1E` winning team, `+0x1F`
  winning player, team places `+0x1304` (0x30 apart), `+0x1378` decision (9 No Contest).
- Player `rec = ri + 0x24 + 0x2AC*p`: `+0x00` gmCharacterKind, `+0x01` 0 human / 1 CPU / 3 empty, `+0x06` team (0 red, 1
  blue, 2 green), s8 `+0x0A` (> 0: the KO'd column shows the "--:--" placeholder; meaning unknown; an earlier note had
  the test inverted), `+0x0E` place (0-based), `+0x10` KOs, `+0x14` falls, u16 `+0x18` SDs, `+0x28` final coins, `+0x2C`
  coins collected, `+0x30` lost coins, u8 kinds `+0x34+i` (KO'd) / `+0xFC+i` (KO'd by) with counts `+0x1C4` / `+0x1C8`,
  statistics `+0x1C0+4k`, u8 `+0x1EC` hit rate, `+0x224` team attacks, `+0x228` coins dropped, `+0x254` KO frame.
- Task: state u32 `+0x40` (0 load, 1 intro, 2 winner line, 3 waiting for A / Start, 4 table and pages, 5 replay save,
  6-10 Wi-Fi, 11 fade, 12 done); page `+0x48+4p` (0 summary, 1 KOs, 2 statistics, 3 ready, 4 Wi-Fi notice); statistics
  top line `+0x98+4p`; player state `+0xA8+4p`; winner `+0xB8`, team `+0xBC`; own-controller humans mask u8 `+0xC1`
  (`rec+7 & 0x10`); stamina `+0xD4`; statistics line count `+0x4438+4p` (50, coin 56, team +2).
- Winner line `0x800E7BD0` runs once per showing (panel frame 106 or the skip): "NO CONTEST" (`0x8059E348`), team
  `*(0x8045B650 + 8*team)`, else the fighter via `0x800AFA8C` (English table `0x80456564`; same words as the name plates
  for every playable kind). On No Contest the WINNER letters are blanked (PAT0 `InfResultPanel_TopN__0`, `0x800E6CA4`).
- State 3 → 4 shows the table: `0x800E60E4(task, p)` prints full-width numbers into MuMsg `*(task+0x650)` (glyph table
  `0x8045B674`: 0-9 digits, 10 "+", 11 "-", 13 "P", 14 "CPU", 17 ":", 18 U+3000). Time: KOs, FALLS (falls - SDs), SDs
  (SDs × penalty), TOTAL; stock: last column KO'd (m:ss of the KO frame; placeholder on `+0x0A` > 0 or No Contest);
  coin: COINS COLLECTED, COINS LOST, TOTAL; stamina: Finishes, Misses, TOTAL. Labels are art (`InfResultHitnum_TopN__3`
  frames 0 time, 3 stock, 1 coin, 2 stamina); slot-to-label pairing verified through the `textN` bones.
- Label slot 2+p of `*(task+0x654)`: name tag (mode-melee entries `*(GameGlobal+8)+0x98+0x5C*i`, byte +3 == p, UTF-16
  at +0xC), else "P" + digit, or "CPU". Place numeral art: `rec+0x0E` + 1 (team place in team battle; none on No
  Contest). Card fighters are art.
- Pages: `0x800EB684(task, p)` per player per frame: A 0 → 1 → 2 → ready, Start → ready (`0x800E8040`, which can move
  other players), B back (`0x800E83AC`). Statistics: even lines labels (vsres 9+k; stamina 50 / 51; team 34; coin
  35-37), odd lines "%d" + raw unit (table `0x8040898C`, vsres 38-42); top line capped at rows-9 (`0x800ECBC4`). KO page
  `0x800E88CC`: fighter icons KO'd, then KO'd by, then SDs.
- Not mapped: mode name and rule line (vsres 0-8, 46-49), MVP laurel, REPLAY prompt, replay save, Wi-Fi states.

## Vault and Data menus (main-menu procs 4, 6, 14-19, 23)

Recorded 2026-10-26 from the disassembly of `sora_menu_main`, `sora_menu_rule` and `sora_scene` and decoded archive
art; constants as used by `main_menu.cpp` and `data_screens.cpp`.

- Shared-cursor screens (enter; shared moveCursor, setSelect and setMsg): Vault 4 `0x94017A4C`, Trophies & Stickers 23
  `0x9402F6B4`, Data 6 `0x940181E0`, Records 16 `0x9401FDCC`. Title frames 10 "VAULT", 19 "TROPHIES & STICKERS", 12
  "DATA", 28 "RECORDS" (breadcrumb visibility not verified).
- Vault: Trophies & Stickers, Stage Builder, Album, Challenges, Replays, Masterpieces, Chronicle (`MenMainColl01_1..7`);
  descriptions u32 `0x94044670` = main 22, 27, 25, 23, 26, 28, 24; rows 0-2 / 3-6. A: 0 → proc 23, 1-6 → exit ids 13,
  21, 20, 14, 35, 36. Return cursor (prevId-13 via `0x9404468C`): 13 → 1, 14 → 4, 20 → 3, 21 → 2, 38 → 5, 39 → 6;
  returning rebuilds `muMenuMain`, so the enter runs again.
- Trophies & Stickers: Trophy Gallery, Trophy Hoard, Coin Launcher, Sticker Center, Sticker Album (`MenMainColl02_6, 7,
  9, 10, 8`); descriptions 55+cursor; exits 17, 18, 19, 15, 16. Data: Movies, Sound Test, Records (37+cursor; A → proc
  14+cursor). Records: Group Records, Brawl Records, Notices (u16 `0x940419F4` = 78, 79, 80; procs bytes `0x940419F0` =
  21, 18, 17).
- Exit ids: `0x806A5604(proc, id, 0)` stores at `muMenuMain+0xAB0`, the scene passes it to `gfSceneManager+0x284`, a
  rebuilt menu reads it back from `+0xAA0` (`0x806A55C0`); `sqMenuMain` (jump table `0x807019A4`): 12 sqTraining, 13
  sqEdit, 14 sqReplay, 15 scSealList, 16 scSealDisp, 17 scTyFigList, 18 sqTyFigDisp, 19 sqCoinShooter, 20 scClearGetter,
  21 scSnapShot, 35 scVCTrial, 36 scSoftList.
- Notices (17): enter `0x9402047C`, input `0x94020CBC` (returns 1 on a change), exit `0x94020BE0`; setSelect(34, 0) and
  own title `MenMainDataCmnTitle.08`. Builder `0x9402133C`: ids 0-116 present when bit 2k+1 of `*(GameGlobal+0x24)+0x1C`
  is set, ascending time; list `*(proc+0x678)`, count `+0x67C`, highlight `+0x680` (0-3), first row `+0x684`; MuMsg
  `+0x688` (`info_en`): slot 2r = entry `0x80407B50[id]`, 2r+1 = date by printf (`0x940452AC`
  "%2d/%2d/%4d\n%2d:%02d:%02d"). No A action.
- Brawl Records (18): enter `0x94021548`, input `0x94021E60`, exit `0x94021D94`; title `MenMainDataCmnTitle.02`; 39
  rows, 7 shown, highlight `+0x678`, first `+0x67C` (both 0 on enter); MuMsg `+0x680` (`count_en`); rows `0x94041A48`
  (12 bytes: getter, type 0 number / 1 h:mm / 2 fighter, u16 label 0-38, u16 description 39-77); slot 2r label, 2r+1
  value, 14 description. Values exist only as printf calls (`0x94045360` "%d", `0x94045368` "%d:%02d", `0x94045370`
  "- ", or a fighter name from `0x80456564` / `0x804561E4` passed as the format): captured at
  `MuMsg::printf 0x800B9230`.
- Movies (14): enter `0x94025C4C`, input `0x9402698C`; setSelect(30, 37) only from Data. List `*(proc+0x674)`, count
  `+0x678` (max 6): folder 121 (Subspace Emissary) first when any of 5-120 is unlocked, then ids 0-4 (`fn_8004F2BC`).
  Page `+0x679`, column `+0x67A`, row `+0x67B`; row lengths `0x94041CA8[(n-1)*2+r]`; name, description u16
  `0x94041CB4+4*id` (`movie_en`; folder 5, 11). A: 121 → proc 19, else proc 20 (`+0x66C` = id). Subspace list (19):
  enter `0x94026F0C`, input `0x940276C0`, ids 5-120, 9 rows (row `+0x679`, first `+0x67A`), names u16
  `0x94041CD6+2*id`, fixed description 11.
- Sound Test (15): enter `0x94023500`, music input `0x940247E4`, effects moveCursor `0x94023B88`; setSelect(34, 0), art
  title `MenSoundtest06`; back to proc 6 from Data, else 11. State `+0x664` 0 music, 3 effects (2 while a group loads),
  4 leaving; row `+0x42` (0 mode, 1 category, 2 song or number); series `+0x684`, song `+0x688`, effect group `+0x68C`,
  sound `+0x690` (shown +1). Msbin pointers: `+0x694` series (`sound_en`; 21 "Source: "), `+0x698` `snd_title`,
  `+0x69C` source games, `+0x6A8` effect groups, `+0x6AC` effect list (768 bytes per group, name entry at +0). Song: snd
  `*(0x805A01D8)`, index `*(snd+0x1FC+200*series+4*song)`, info `*(snd+0x194)+48*index` (title +0x10, source +0x14).
- Text sources for screens not yet handled: Group Records (proc 21, enter `0x9402A170`, page `+0x670`, page change
  `0x9402B018`; main 82-89 page titles, 90-140 labels); Trophy Gallery `tyFigListTask` (`ty_fig_name_list`,
  `ty_fig_ext_list`, `ty_fig_category`; prints at `0x94004844`); Trophy Hoard `ty_fig_disp2`; Coin Launcher
  `mini_coin_shooter`; Sticker Center `tySealListTask` (`ty_seal_name_list`, `ty_seal_ext_list`); Album, Replays and
  the Stage Builder list share `muCollectionViewer` (`sora_menu_collect_viewer`; `snap_shot`, `replay`, `st_edit_sel`;
  the replay delete dialog is its own MuMsg, not MuNoticeWnd); Challenges `muClearGetTask` (`sc_clear_getter_en.pac`);
  Masterpieces `muVCTrialTask` (`vc_trial`); Chronicle `muSoftListTask` (`menu/soft_list/Soft_en.pac`; platform names
  are literals in the module).

## Name keyboard and controller setup

- MuName (sora_menu_name): open `0x80674614`, key cursor move `0x80674C28` (writes `+0x40` only on a change), action
  `0x80674E84` (the only place the typed UTF-8 text at `*(+4)` changes). Sets: ids at `+0x24` (u32), count `+0x38`,
  index `+0x3C`; ids 0 hiragana, 1 katakana, 2 letters, 3 numbers, 4 accented. Keys 0 Delete (icon), 1-11 characters,
  12 switch (icon), 13 "OK!". `+0x7C` = 0 while the last character is still cycling.
- scButton: `muButtonTask` processDefault `0x9403573C`; data `*(task+0x40)`: state `+0xB0`, controller `+0xB4` (0
  GameCube, 1 Wii Remote with Nunchuk, 2 Wii Remote, 3 Classic; order verified from the model), MuMsg `+0x164`
  (`button` table), mapping bytes `+0x19C`, toggles `+0x1A8` (0x80 Tap Jump, 0x40 Shake Smash; set = ON), list item
  `+0x1AC`, YES/NO byte `+0x1BC` (1 YES). List records per controller at `*(0x940425F0+8*type)` (12 bytes, id first).
  Action names/descriptions: byte pairs at `0x94042658`. Row action print `0x940359F8`. After saving, the menu
  reopens on Controls (proc 10).
- Art captions verified from decoded textures: controller names (InfButton01Cont06Jp), Default/Test/Save
  (InfButton07Font), YES/NO (InfButton11Yes/No), OFF/ON (InfButton08On), key labels (MenSelchrW3_*), "OK!"
  (MenSelchrEntryW05); row button icons per controller from the model's `pos%02d` bones.

## Wii Remote name transfer (muNameTask, sora_menu_name, resident)

Recorded 2026-10-26 from the disassembly of `sora_menu_name` and `sora_menu_main` and `name_remote_en.msbin`;
constants as used by `name_remote.cpp` and `main_menu.cpp`.

- Controls (proc 10) and Names (proc 22) create the task through `0x80675F0C` with `+0x730` = the `name_remote` table
  (menu archive type 1 idx 8), which makes it create a remote task at `task+0x408`. State `+0x9C`, previous `+0xA0`;
  table `0x8067B7E0` of (enter, process); the dispatcher (`0x806772D4`) runs the enter once per change and stores
  `+0xA0` afterwards, so an enter still sees the old state there.
- List, state 2 (enter `0x8067773C` sets model frames only and does not redraw; process `0x80677794`): B → result 3,
  state 4; A on a name → result 1, record `+0x3EC`; "New entry" → 5 (keyboard), or 3 when full. With a remote task, pad
  bit 0x1000 or 0x00800000 stores the selected record (-1 none) at `+0x404` and goes to state 9. Hint art
  `MenMainName00` (Wii Remote +) and `MenMainName01` "Transfer Names" (the GameCube `MenMainName04` is never drawn);
  whether the hint shows depends on a CHR0 frame (9) not decoded.
- States whose process returns 2 (back to the list): 3, 6, 8 (after the Names erase dialog), 10, 14, 16, 19, 23, 24;
  1 is the opening. 5 returns itself or 6; 7 never returns to 2 (an earlier note named 7 as the erase return: wrong);
  13, 17, 18, 20, 22 lead to 11, 14, 15, 16, 19 or 24.
- Own messages, MuMsg `task+0x154` slot 6 (table `*(task+0x458)`, else the MuMsg's own): state 9 enter `0x80678DF8`
  entry 0 (press 1 and 2), 12 `0x806791F0` entry 8 (loading), 14 `0x80679544` entry 11 (no data), 21 `0x8067A530` entry
  3 (saving); 19 `0x8067A18C` entry 9 + Wii Remote name (UTF-16 `+0x40E`) + 10; 23 `0x8067A88C` entry 4 + Wii name
  (record `+0x404`: `*(GameGlobal+0x28)+0x124*record+0xE0`) + 5. Composed by `0x80678B28` from raw entry spans.
- Choice (state 15 enter `0x80679708`, 16 enter `0x806799F0`, process `0x80679A84`): slots 7-9 = entries from pairs
  `0x8067B9F8`: 15 "To Wii", 16 "To Wii Remote", 17 "Back" (game text); enabled bytes `+0x450..+0x452`, disabled ones
  skipped; cursor `+0x454`; highlight `0x80678C44(task, cursor)` on opening and each change. A on Back does nothing
  (only B leaves). Slot 10 the Wii Remote's name, slot 11 the Wii name, labelled by icons only.
- MuNoticeWnd states (spoken by the notice hook): 13 error, 17 entry 7, 18 entry 6, 20 entry 1 or 2, 22 entry 12, 24
  entry 14. States 10 and 11 print nothing.
- Names erase dialog (proc 22, list frame `0x9402F264`): opened at `0x9402F2D4` (`proc+0x664` = 1, `+0x66C` bit 0x40
  cleared); MuMsg `+0x674` slot 0 = main 43 "Erase this name?"; input `0x9402F4C0`: left sets bit 0x40 (YES), right
  clears it (NO, the default), A erases only with the bit set, B cancels. Art `MenMainButton02Yes` /
  `MenMainButton03No`.

## Audio pipeline and positional sound

Recorded 2026-10-26 from the runtime audio sources, main.dol and `sora_melee` disassembly and a read-only parse of
`smashbros_sound.brsar`. The decomp map names no `nw4r::snd` function: names follow the nw4r headers by layout and
behaviour, and every offset was checked in this build's code. Verified unless marked.

- Runtime: 32 kHz stereo S16 (`runtime/src/hle/audio/audio.cpp:23-24`) to one SDL3 stream
  (`runtime/src/audio_backend.cpp:66`), queue capped at 120 ms. AX frame 3 ms, 96 samples; `Audio_HLE_Tick` (main
  thread) pushes each block and runs the guest AI callback that builds the next command list.
- Mixer `runtime/src/hle/audio/ax_mix.cpp`: `DispatchCommandList` (`:323`, guest thread) hands off to a mix worker
  (default on, `:72`); `ProcessPBList` (`:647`) reads, mixes and writes back each PB; `ProcessVoice` (`:992`) builds one
  mono 96-sample buffer per voice (decode, resample, volume envelope `:1004`, filters), then mixes it into main, aux and
  Wii Remote buses (`:1014-1031`). Spatialiser insertion point: after the filters, before the main L/R mixes.
- `OutputSamples` (`:1233`) sends main L/R only: the surround bus and Wii Remote speaker PCM never reach the host. A
  voice is known only by its PB address (no sound id). `runtime/src/music_attenuation.cpp` is inert in Brawl.
- sndSystem pointer `0x805A01D0`: `+0x18C` SoundArchivePlayer, `+0x26C` Sound3DManager, `+0x294` Sound3DListener (MTX34;
  interior `+0x2C4` 50, max-volume distance `+0x2C8` 60, unit distance `+0x2CC` 150). Update `0x800737E4` sets the
  listener each frame by MTXLookAt from the gameplay camera (`gfCameraManager` +108..+140; field roles inferred).
- Emitters: `snd3DGenerator` (ctor `0x80079550`) owns a Sound3DActor; `setPos 0x80079660`; `playSE 0x80079718` → 3D
  start `0x80074EF8`, else `sndSystem::playSE 0x800742B0`. Fighter `soSoundModuleImpl` `playSE`, `playStatusSE`,
  `playSEPos` are 3D, `playSENo3d 0x807613B8` is not; stage gimmicks call `snd3DGenerator` (114 `setPos` sites). Menus
  and announcer use plain `playSE`; BGM is a stream with no actor (inferred).
- Sound3DActorParam (24 bytes, refreshed each frame by `0x801C606C`): `+4` 3D flags from the BRSAR (bit 0 volume, 1 pan,
  2 surround pan, 3 priority), `+8` decay curve, `+9` ratio, `+0xC` position x, y, z.
- `Sound3DManager::Update 0x801C6238` (per 3D sound per frame): listener-space position = matrix × position; volume
  decays past 60 by full 3D distance; pan zeroes y and uses `atan2(x, -z)`. Height and zoom change loudness only.
- Chain to the PB (each hop verified at its point of use, each with a back-pointer):
  - BasicSound (vtable `0x804665E0`): `+0x18` == sndSystem `+0x26C` for 3D sounds (where set: inferred), `+0x24`
    Sound3DActorParam*, `+0x78` sound id, player at `+0xD8` (Wave `0x80466D60`, Seq `0x80466A58`, Strm `0x80466CA8`).
  - Wave: Channel at WsdPlayer `+0xD4`, owner callback `0x801D4EE8`. Sequence: tracks SeqPlayer `+0xB8+4i` (i < 16);
    track `+0xB8` = parent SeqPlayer (same offset, other object), list `+0xBC`, next `Channel+0xE0`, owner `0x801C5744`.
  - Channel: owner callback `+0xD4`, data `+0xD8`, Voice `+0xDC`. Voice: callback `+0xA4` (`0x801BE47C`), data `+0xA8`,
    active `+0xAC`, main out `+0xC8`, send `+0xCC`, list node `+0x124`, AxVoice `+0xC + ch*0x10 + out*4`. VoiceManager
    `0x804DD0EC`, first node `*(+8)`.
  - AxVoice `+0` AXVPB, `+0x3C` Voice; AXVPB `+0x14` userContext (AxVoice), `+0x18` index; `__AXSyncPBs 0x80201BE0`
    copies to `*(u32*)0x805A0B28 + index*0x140`, the PBs the mixer reads (max 96). Masking with `0x1FFFFFFF`: inferred.
- Gain lands in the PB `vol_env` (`Voice::CalcVe 0x801D1A60`, distance included); pan shapes only main and aux L/R
  (`CalcMixParam 0x801D1FAC`, constant-power table `0x8040C304`). Pan-free level `k = sqrt(L^2 + R^2)`. A stereo wave is
  one Voice with two AxVoices at pan -1 / +1.
- BRSAR (version 0x0103, 10348 sounds): WAVE 0x5 (volume + surround, no pan) 7498, every fighter SE and voice and most
  items; 0x4 (no distance volume) 1631, stages, bosses, enemies; 0xF 302 (menu, narration); SEQ 0x0 478; STRM 0xF 371.
  All decay curve 1, pan mode 0, curve 0. So in stereo, fighters are centred today with distance volume only.
- Output mode: `SCGetSoundMode 0x8020FD44` (stereo on failure) sets AxManager `0x804C1B8C` `+0` (0 stereo, 2 DPL2, 3
  mono); Brawl's init `0x80072AAC` resets DPL2 to stereo. The runtime skips SYSCONF (`runtime/src/hle/sc.cpp:39-50`):
  stereo (inferred).
- Steam Audio v4.8.1 (2026-02-11), Apache-2.0, Windows, Linux, macOS, Android, iOS: `iplBinauralEffectApply` takes 1-2
  channels from one direction (+x right, +y up, -z ahead, as GX camera space), outputs 2, not in place; HRTF resampled
  to the output rate. Small frames undocumented: benchmark 96 samples or buffer 2-3 AX frames.
- Design (proposed, no guest hook): at `DispatchCommandList`, before handoff, walk VoiceManager → Channel → sound (by
  callback and vtable) → Sound3DActorParam and listener → AxVoice → PB with `Memory::TryRead*`, checking back-pointers,
  and publish `{PB → direction, distance, sound id, flags}`. In `ProcessVoice` a mapped PB feeds a binaural effect with
  post-envelope samples × 0.7071·k into a host bus added to main L/R; PB ramps, dpop and aux sends unchanged. No guest
  writes (netplay hashes memory, `runtime/src/netplay/netplay_session.cpp:539`). Failed checks count by named reason
  and stay vanilla; a code-identity check of the anchors (`0x801D4A74`, `0x80201C08`, callbacks) gates the feature.
- Open run-time checks: output mode and listener constants; mapped PBs a subset of those mixed; `k` against
  `Voice+0xC8 × +0xCC`; where `+0x18` is set; fighter positions in world units; 3D streams (Voice callback `0x801CF238`,
  inferred); Brawl's own AX voices (`0x8007C9DC`, `0x8007D224`) stay vanilla.

## Options screen construction and an Accessibility entry

Recorded 2026-10-26 from the disassembly of `sora_scene`, `sora_menu_rule` and `sora_menu_main`, the Options models and
textures in `mu_menumain_en.pac` and `main_en.msbin`. Verified unless marked.

- Proc built by `0x806CD510`: 0x66C bytes (nothing past the `muProcMenu` base, ctor `0x806A4CE8`), vtable `+0x668 =
  0x940448A0`, count `+0x640` = 7. Vtable: enter `0x940187C0`, process `0x94018958`, exit `0x94018E18`, moveCursor
  `0x806A50F4` (shared), cursor sound `0x94018E8C` (`playSE(20)`).
- Enter: nav table `+0x63C = 0x940447B8`, animation descriptors `+0x648 = 0x94044828`, model base `+0x644` = 29; adds
  models 29-35 in a loop hard-coded to 7 (`0x9401883C`); cursor = prevId-7 on return; `0x806A5770(proc, 1)` highlights
  base+cursor; `setSelect(panel, 11, table[cursor])` from 7 words at `0x94041890`. Exit removes the same 7.
- Process (state `+0x664`): copies 7 descriptions from `0x940418B8` to the stack; on a move `0x806A586C(proc, old, 1)`
  and `setMsg(panel, desc[cursor], 1, 0)`. Decide (bit 16): next proc `+0x634` = cursor+7. Back (32): proc 0.
- Input reader `0x806A4E04` (all muProcMenu procs; registered, hookable), via `getSysPadStatus 0x8002AE48`. Repeat
  `+0x14`: 0x8 → 1 up, 0x4 → 2 down, 0x1 → 4 left, 0x2 → 8 right. Trigger `+0xC`: A 0x100 → 512|16, Start 0x1000 →
  256|16, B 0x200 → 32, X 0x400 → 1024, Y 0x800 → 2048, L 0x40 → 64, R 0x20 → 128 (button names from the community
  `gf_pad_status.h` defines; directions verified against the nav table and layout).
- Nav table (7 × 16 bytes): u16 neighbour up/down/left/right at +0/+4/+8/+12, reset-repeat byte at +2/+6/+10/+14
  (`fn_800BA364`). 0 Screen 4,4,3,1; 1 Deflicker 5,5,0,2; 2 Rumble 6,6,1,3; 3 Controls 6,6,2,0; 4 Sound 0,0,6,5; 5 My
  Music 1,1,4,6; 6 Erase Data 2,2,5,4. The descriptor table follows at once: an index 7 would read it.
- Layout (CHR animations): top row y 7.8, x -23.9, -7.9, 8.1, 24.1 (Screen to Controls); bottom row y -7.7, x -15.85,
  0.15, 16.19 (Sound, My Music, Erase Data); spacing 16. Positions are baked into each model's animations.
- No free slot: Options models 29-34 = archive entries 27-32, 35 = entry 34; entry 33 is skipped and absent; model 36
  is Data's. No ACCESSIBILITY art, no fitting `main` entry (144, 145, 147, 154 empty). Code hard-codes 7 and base 29; A
  at cursor 7 opens proc 14 (Movies); proc ids 0-28 are used; the runtime has no guest heap allocator.
- Cut item (inferred): `main` 35 "Language is set to English." is skipped by the description tables (29-34, 36),
  matching the skipped model and blank placeholder art: a removed Language option. Nothing reusable remains.
- Reusing another proc (Sound, Deflicker) brings its titles and writes the save record: not viable.
- Runtime precedent: `runtime/src/netplay/online_menu.cpp` draws a Brawl-styled ImGui menu with disc fonts and reads
  controllers itself (`ReadNav`: raw SDL and keyboard, no player mapping). While open, `ApplyInputBlockState`
  (`runtime/src/settings_overlay.cpp:1875`) blocks guest input: neutral pads (`runtime/src/frame_input.cpp:35`) and Wii
  Remotes (`runtime/src/hle/input/kpad.cpp:292`). Settings: `RuntimeUserConfig` getters and setters such as
  `RumbleEnabled` (`runtime/include/runtime_config.h:918-924`), saved with `WriteSetting`.
- Approach E2 (proposed): a runtime-drawn 8th entry as game cursor 7. Highlight loops stop at `+0x640`, so no game
  button lights at 7 (verified). moveCursor hook (Options vtable only): at cursor 7 skip the original, apply runtime
  neighbours, write `+0x42`, play the sound via vtable `+0x24`, return 1. setMsg `0x9403A048`: replace the stack garbage
  passed at 7 (`sp+36`) with empty entry 144. New `0x806A4E04` hook: at 7 strip bits 16, 512, 256 and open the runtime
  page. Settings in Config.toml, never the save. Absent when Gecko codes relocate `sora_menu_main` or mods silence
  speech (`AccessibilityText::SilencedByMods()`).
- E2 side effects (inferred): leaving 7 calls `0x806A586C(proc, 7, 1)` and idle states read model 36 (Data's); Data's
  enter resets its animations (`0x806A5770` at `0x940182E8`), so likely harmless; needs a sighted check. Runtime moves
  skip `fn_800BA364` or call it through `InvokeIndirectCpu`.
- Fallback E3: open the page with X or Y on Options (unused there, verified); input hook only, but not a list item.
- Open: entry placement and the mapping from 3D menu-camera units to `GameLayout` space (4:3 and 16:9); a host input
  path honouring the player's mapping and Wii Remotes while guest input is blocked; main-menu idle under long blocks;
  in-memory `main` table against the disc file; the entry's wording, look, visibility and languages (designer).
