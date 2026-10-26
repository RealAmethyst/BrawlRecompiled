#include "accessibility_text.h"
#include "hle_stubs.h"
#include "memory.h"
#include "ppc_runtime.h"
#include "runtime_log.h"
#include "speech.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

// Screen-reader speech for the main menu's screens with lists of their own (muMenuMain procs in sora_menu_main):
// Stadium (proc 25) and its Multi-Man Brawl list, Movies (14) and the Subspace Emissary movies (19), Sound Test (15),
// Notices (17) and Brawl Records (18). main_menu.cpp speaks the screens built on the shared cursor (Vault, Data,
// Records and the others).
//
// Event hooks only: each screen's enter (it opens) and its input function, wrapped and the cursor fields compared
// before and after (the input runs every frame, so the cost is a few reads per frame while the screen is open).
// Notices and Brawl Records print their dates and values with MuMsg::printf; the hook on it keeps what the open
// screen printed, slot by slot, so the values are spoken as the game shows them (a fighter's name is spoken from the
// reviewed name plates instead); the screens' exits stop that.
//
// As in main_menu.cpp, Gecko codes and SD mods move on-demand modules off their translated home: the enter hooks
// never fire then, and the other hooks, finding no open screen, stay silent.

extern "C" void func_940310A0(CpuContext* ctx);  // muProcStadium enter (vtable +0xC)
extern "C" void func_940314DC(CpuContext* ctx);  // muProcStadium process (vtable +0x14)
extern "C" void func_94031AC0(CpuContext* ctx);  // muProcStadium moveCursor override: the top list's input
extern "C" void func_94031CB4(CpuContext* ctx);  // muProcStadium: the Multi-Man Brawl list's input
extern "C" void func_9402047C(CpuContext* ctx);  // muProcInfo enter (Notices)
extern "C" void func_94020CBC(CpuContext* ctx);  // muProcInfo: the list's input
extern "C" void func_94020BE0(CpuContext* ctx);  // muProcInfo exit (vtable +0x18): frees its MuMsg
extern "C" void func_94021548(CpuContext* ctx);  // muProcCount enter (Brawl Records)
extern "C" void func_94021E60(CpuContext* ctx);  // muProcCount: the list's input
extern "C" void func_94021D94(CpuContext* ctx);  // muProcCount exit (vtable +0x18): frees its MuMsg
extern "C" void func_94025C4C(CpuContext* ctx);  // muProcMovie enter
extern "C" void func_9402698C(CpuContext* ctx);  // muProcMovie: the movie grid's input
extern "C" void func_94026F0C(CpuContext* ctx);  // muProcMovieAdv enter (Subspace Emissary movies)
extern "C" void func_940276C0(CpuContext* ctx);  // muProcMovieAdv: the list's input
extern "C" void func_94023500(CpuContext* ctx);  // muProcSoundTest enter
extern "C" void func_940247E4(CpuContext* ctx);  // muProcSoundTest: music mode's input (state 0)
extern "C" void func_94023B88(CpuContext* ctx);  // muProcSoundTest moveCursor override: sound-effect mode's input
extern "C" void func_800B9230(CpuContext* ctx);  // MuMsg::printf (this, slot, format, ...)

// main_menu.cpp: whether a proc is muMenuMain's current one.
namespace MainMenuSpeech {
bool IsCurrentProc(uint32_t proc);
}

// Named, not anonymous: the accessibility sources share one unity translation unit (cmake/PublicProducts.cmake).
namespace DataScreenSpeech {

// muProc base (sora_menu_rule fn_18_11C4).
constexpr uint32_t kProcCursorOffset = 0x42;  // u16
constexpr uint32_t kProcVtableOffset = 0x668;
// MuMsg: its message table at +4 (setMsgData 0x800B8C7C stores it there; printIndex 0x800B91B8 prints from it when
// given no table).
constexpr uint32_t kMuMsgTableOffset = 0x4;
// The enters' third argument: the proc the menu came from.
constexpr uint32_t kMoviePlayerProc = 20;

// Stadium (enter 0x940310A0): its MuMsg prints the selected item's name in slot 0 (entry = item kind 0-3: Target
// Smash!!, Home-Run Contest, Multi-Man Brawl, Boss Battles) and its description in slot 1 (kind + 5); in the
// Multi-Man list slot 1 is entry 10 + cursor. The item kinds are u32 from +0x674, their count (3, or 4 once Boss
// Battles is unlocked: fn_8004E7EC) at +0x670. State +0x664: 1 top list, 2 Multi-Man list opening (cursor reset to 0
// at 0x9403163C), 3 Multi-Man list, 4 closing (the top cursor is put back on Multi-Man Brawl, then state 1).
constexpr uint32_t kStadiumStateOffset = 0x664;
constexpr uint32_t kStadiumMuMsgOffset = 0x66C;
constexpr uint32_t kStadiumItemCountOffset = 0x670;
constexpr uint32_t kStadiumItemsOffset = 0x674;
constexpr uint32_t kStadiumKinds = 4;
constexpr uint32_t kStadiumMultiManKind = 2;  // entry 2 "Multi-Man Brawl"
constexpr uint32_t kStadiumDescriptionShift = 5;
constexpr uint32_t kMultiManDescriptionFirst = 10;
constexpr uint32_t kMultiManTypes = 6;  // A sends codes 11, 37-41, which sqMenuMain (0x806DC708) maps to types 0-5
enum : uint32_t { kStadiumTopList = 1, kStadiumMultiManOpening = 2, kStadiumMultiManList = 3, kStadiumClosing = 4 };

// Notices (enter 0x9402047C, rows printed by 0x940207BC): the notice ids (u32) at *(+0x678), their count at +0x67C,
// the highlighted row (0-3) at +0x680 and the first shown at +0x684. Row r prints the notice in slot 2r
// (info_en entry getInfoMsgID(id) = byte 0x80407B50[id], 0x800AF5A4) and its date in slot 2r + 1 with printf
// ("%2d/%2d/%4d\n%2d:%02d:%02d", month, day, year, time, at 0x940452AC for languages other than Japanese).
constexpr uint32_t kNoticeIdsOffset = 0x678;
constexpr uint32_t kNoticeCountOffset = 0x67C;
constexpr uint32_t kNoticeHighlightOffset = 0x680;
constexpr uint32_t kNoticeFirstRowOffset = 0x684;
constexpr uint32_t kNoticeMuMsgOffset = 0x688;
constexpr uint32_t kNoticeMessageIds = 0x80407B50;
constexpr uint32_t kNoticeIds = 117;  // ids 0-116: one bit pair each in the notice bitfield (list builder 0x9402133C)
constexpr uint32_t kNoticeRows = 4;

// Brawl Records (enter 0x94021548, rows printed by 0x94021878, input 0x94021E60): 39 records from the 12-byte table at
// 0x94041A48 (+0 getter, +4 type, +6 u16 label entry, +8 u16 description entry, count_en); 7 rows shown, the
// highlighted one (0-6) at +0x678, the first shown (0-32) at +0x67C. Row r prints the label in slot 2r and the value in
// slot 2r + 1 with printf ("%d", "%d:%02d" hours and minutes, "- ", or a fighter's name as the format).
constexpr uint32_t kRecordHighlightOffset = 0x678;
constexpr uint32_t kRecordFirstRowOffset = 0x67C;
constexpr uint32_t kRecordMuMsgOffset = 0x680;
constexpr uint32_t kRecordTable = 0x94041A48;
constexpr uint32_t kRecordBytes = 12;
constexpr uint32_t kRecordLabelOffset = 6;
constexpr uint32_t kRecordDescriptionOffset = 8;
constexpr uint32_t kRecords = 39;
constexpr uint32_t kRecordRows = 7;
// A fighter value is printed with the fighter's name as the format: exchangeMuStockchkind2MuCharName 0x800AFA30
// returns *(0x80456564 + 4 * stock kind), or *(0x804561E4 + 4 * stock kind) when fn_8004D9E8 is non-zero. The name
// plates (character_select.fighters) are by MuSelchkind; exchangeMuSelchkind2MuStockchkind 0x800AF82C is byte 3 of
// the 16-byte records at 0x80455458, one-to-one for the fighters 0-39 (40 and 41, no fighter and Random, give 255).
constexpr uint32_t kCharacterNameTables[] = {0x80456564, 0x804561E4};
constexpr uint32_t kStockKinds = 61;  // entries 0-53 named, 54-60 null
constexpr uint32_t kSelchkindRecords = 0x80455458;
constexpr uint32_t kSelchkindRecordBytes = 16;
constexpr uint32_t kSelchkindStockOffset = 3;
constexpr uint32_t kFighterSelchkinds = 40;

// Movies (enter 0x94025C4C): the movie ids (u32) at *(+0x674), count byte +0x678 (at most 6: ids 0-4 that are
// unlocked, after folder 121 when any Subspace movie is), page byte +0x679, column +0x67A, row +0x67B. The selection
// is the page's 6 + the lengths of the rows above (byte table 0x94041CA8, two per item count) + the column (process
// 0x94025FB4). Names and descriptions: u16 pairs at 0x94041CB4 + 4 * id (movie_en); the folder uses 5 and 11.
constexpr uint32_t kMovieIdsOffset = 0x674;
constexpr uint32_t kMovieCountOffset = 0x678;
constexpr uint32_t kMoviePageOffset = 0x679;
constexpr uint32_t kMovieColumnOffset = 0x67A;
constexpr uint32_t kMovieRowOffset = 0x67B;
constexpr uint32_t kMovieMuMsgOffset = 0x67C;
constexpr uint32_t kMovieRowLengths = 0x94041CA8;
constexpr uint32_t kMovieEntries = 0x94041CB4;
constexpr uint32_t kMoviesPerPage = 6;
constexpr uint32_t kMovieRowsPerPage = 2;
constexpr uint32_t kMovieLastId = 4;
constexpr uint32_t kSubspaceFolderId = 121;
constexpr uint32_t kSubspaceFolderName = 5;
constexpr uint32_t kSubspaceDescription = 11;  // "Movies from The Subspace Emissary."

// Subspace Emissary movies (enter 0x94026F0C, rows printed by 0x940275C4): ids 5-120 (u32) at *(+0x674), count byte
// +0x678, highlighted row byte +0x679 (9 rows shown), first shown +0x67A; names u16 at 0x94041CD6 + 2 * id.
constexpr uint32_t kSubspaceIdsOffset = 0x674;
constexpr uint32_t kSubspaceCountOffset = 0x678;
constexpr uint32_t kSubspaceRowOffset = 0x679;
constexpr uint32_t kSubspaceFirstRowOffset = 0x67A;
constexpr uint32_t kSubspaceMuMsgOffset = 0x67C;
constexpr uint32_t kSubspaceNames = 0x94041CD6;
constexpr uint32_t kSubspaceFirstId = 5, kSubspaceLastId = 120;

// Sound Test (enter 0x94023500): state +0x664 (0 music, 3 sound effects, 2 sound effects while the effects input waits
// for a sound group to load: 0x94023E5C sets it from state 3, the process sets 3 again at 0x94023A8C); rows (+0x42)
// 0 the mode, 1 the category, 2 the song or sound; music: series +0x684, song +0x688; sound effects: group +0x68C,
// sound +0x690 (shown as its number + 1). Message tables (msbin pointers, set up by 0x940230E0 from the menu archive):
// +0x694 series names (entry 21 "Source: "), +0x698 song titles, +0x69C source games, +0x6A8 sound-effect group
// names; +0x6AC the sound-effect list, 768 bytes per group with the group's name entry at +0.
constexpr uint32_t kSoundTestStateOffset = 0x664;
constexpr uint32_t kSoundTestSeriesOffset = 0x684;
constexpr uint32_t kSoundTestSongOffset = 0x688;
constexpr uint32_t kSoundTestGroupOffset = 0x68C;
constexpr uint32_t kSoundTestSoundOffset = 0x690;
constexpr uint32_t kSoundTestSeriesTableOffset = 0x694;
constexpr uint32_t kSoundTestTitleTableOffset = 0x698;
constexpr uint32_t kSoundTestSourceTableOffset = 0x69C;
constexpr uint32_t kSoundTestGroupTableOffset = 0x6A8;
constexpr uint32_t kSoundTestEffectListOffset = 0x6AC;
constexpr uint32_t kSoundTestSourceLabelEntry = 21;
constexpr uint32_t kEffectGroupBytes = 768;
constexpr uint32_t kEffectGroups = 101;   // the group wraps at 101 (0x940243F0)
constexpr uint32_t kSeries = 21;          // series 0-20 (wrap at 0x94024BE4)
enum : uint32_t { kSoundTestMusic = 0, kSoundTestEffectsLoading = 2, kSoundTestEffects = 3 };
enum : uint16_t { kModeRow = 0, kCategoryRow = 1, kSongRow = 2, kSoundTestRows = 3 };
// sndSystem (*(0x805A01D8)): song info records (48 bytes: title entry +0x10, source entry +0x14) at *(+0x194), their
// count at +0x198; songs per series u32 at +0x19C + 4 * series; a series' songs (info record indexes) at
// +0x1FC + 200 * series + 4 * song (Sound Test display 0x940250B8).
constexpr uint32_t kSoundSystemPointer = 0x805A01D8;
constexpr uint32_t kSoundInfoRecordsOffset = 0x194;
constexpr uint32_t kSoundInfoCountOffset = 0x198;
constexpr uint32_t kSoundSeriesCountsOffset = 0x19C;
constexpr uint32_t kSoundSeriesSongsOffset = 0x1FC;
constexpr uint32_t kSoundSeriesBytes = 200;
constexpr uint32_t kSoundInfoRecordBytes = 48;
constexpr uint32_t kSoundInfoTitleOffset = 0x10;
constexpr uint32_t kSoundInfoSourceOffset = 0x14;
constexpr uint32_t kMaxSoundRecords = 1024;
constexpr uint32_t kMaxSeriesSongs = kSoundSeriesBytes / 4;

enum class Kind { Stadium, Notices, BrawlRecords, Movies, SubspaceMovies, SoundTest };

// The screen that opened last; the vtable tells a later object at the same address apart.
struct OpenScreen {
    uint32_t proc;
    uint32_t vtable;
    Kind kind;
};
std::optional<OpenScreen> g_open;

// What the open screen's MuMsg printed with printf, by slot (Notices: dates; Brawl Records: values).
constexpr uint32_t kPrintedSlots = 16;
struct Printed {
    uint32_t proc = 0;
    uint32_t muMsgOffset = 0;
    bool fighterNames = false;  // Brawl Records: a format that is a fighter's name is kept as its name plate
    std::array<std::string, kPrintedSlots> slots{};
};
Printed g_printed;

void Say(const std::string& text, bool interrupt) {
    if (!text.empty()) Speech::Say(text, interrupt);
}

void Remember(uint32_t proc, Kind kind) {
    uint32_t vtable = 0;
    if (!Memory::TryRead32(proc + kProcVtableOffset, vtable)) {
        g_open.reset();
        RT_LOG(RT_TAG_ACCESSIBILITY) << "data screen at 0x" << std::hex << proc << std::dec << " unreadable"
                                     << std::endl;
        return;
    }
    g_open = OpenScreen{proc, vtable, kind};
}

bool IsOpen(uint32_t proc, Kind kind) {
    if (!g_open || g_open->proc != proc || g_open->kind != kind) return false;
    uint32_t vtable = 0;
    return Memory::TryRead32(proc + kProcVtableOffset, vtable) && vtable == g_open->vtable &&
           MainMenuSpeech::IsCurrentProc(proc);
}

// The message table of the MuMsg at proc+`offset`.
uint32_t MuMsgTable(uint32_t proc, uint32_t offset) {
    return Memory::Read32(Memory::Read32(proc + offset) + kMuMsgTableOffset);
}

// The text of a guest MuMsg::printf call, formatted from its registers before the call: the format at r5, integer
// arguments from r6 (the game's calls here pass only "%d" fields; the ninth argument on goes on the caller's stack,
// from r1 + 8). Anything but %d, %Nd, %0Nd and %% fails, as does an unreadable format.
std::optional<std::string> FormatGuestPrintf(const CpuContext& ctx) {
    constexpr uint32_t kMaxFormatBytes = 256;
    constexpr uint32_t kRegisterArguments = 5;  // r6-r10
    constexpr unsigned kMaxWidth = 16;
    try {
        uint32_t argument = 0;
        const auto next = [&]() {
            const uint32_t i = argument++;
            const uint32_t raw =
                i < kRegisterArguments ? ctx.gpr[6 + i] : Memory::Read32(ctx.gpr[1] + 8 + 4 * (i - kRegisterArguments));
            return static_cast<int32_t>(raw);
        };
        const uint32_t format = ctx.gpr[5];
        uint32_t at = 0;
        bool tooLong = false;
        const auto read = [&]() -> char {  // a format with no NUL in its first bytes reads as ending there
            if (at >= kMaxFormatBytes) {
                tooLong = true;
                return 0;
            }
            return static_cast<char>(Memory::Read8(format + at++));
        };
        std::string text;
        for (char c = read(); c != 0; c = read()) {
            if (c != '%') {
                text += c;
                continue;
            }
            c = read();
            if (c == 0) break;
            if (c == '%') {
                text += '%';
                continue;
            }
            const bool zeros = c == '0';
            if (zeros) c = read();
            unsigned width = 0;
            for (; c >= '0' && c <= '9'; c = read()) {
                width = width * 10 + static_cast<unsigned>(c - '0');
                if (width > kMaxWidth) return std::nullopt;
            }
            if (c != 'd' && c != 'i') return std::nullopt;
            std::string number = std::to_string(next());
            if (number.size() < width) {
                const size_t sign = zeros && number.front() == '-' ? 1 : 0;
                number.insert(sign, width - number.size(), zeros ? '0' : ' ');
            }
            text += number;
        }
        if (tooLong) return std::nullopt;
        return AccessibilityText::Prepare(text);
    } catch (const Memory::AccessViolation&) {
        return std::nullopt;
    }
}

// The date as shown, without the padding of its "%2d" fields after a slash ("3/ 4/2008" reads "3/4/2008").
std::string NoticeDate(std::string date) {
    for (size_t at = date.find("/ "); at != std::string::npos; at = date.find("/ ", at)) date.erase(at + 1, 1);
    return date;
}

// When `format` is one of the internal fighter names, the fighter's name plate (character_select.fighters); an empty
// string for a name with no fighter of its own (bosses, Sand Bag, "Zelda/Sheik"). nullopt when it is no such name.
std::optional<std::string> FighterPlate(uint32_t format) {
    for (const uint32_t table : kCharacterNameTables) {
        for (uint32_t stock = 0; stock < kStockKinds; ++stock) {
            if (Memory::Read32(table + 4 * stock) != format) continue;
            for (uint32_t fighter = 0; fighter < kFighterSelchkinds; ++fighter)
                if (Memory::Read8(kSelchkindRecords + fighter * kSelchkindRecordBytes + kSelchkindStockOffset) == stock)
                    return AccessibilityText::StringAt("character_select.fighters", fighter);
            AccessibilityText::LogOnce("brawl records: fighter value with no name plate");
            return std::string();
        }
    }
    return std::nullopt;
}

// ---- Stadium ----

// "Home-Run Contest, Smash Sandbag as far as you can with a bat!, 2 of 4".
std::string StadiumItem(uint32_t proc) {
    const uint16_t cursor = Memory::Read16(proc + kProcCursorOffset);
    const uint32_t count = Memory::Read32(proc + kStadiumItemCountOffset);
    if (count > kStadiumKinds || cursor >= count) return {};
    const uint32_t kind = Memory::Read32(proc + kStadiumItemsOffset + 4u * cursor);
    if (kind >= kStadiumKinds) return {};
    const uint32_t table = MuMsgTable(proc, kStadiumMuMsgOffset);
    const std::string name = AccessibilityText::MessageEntry(table, kind);
    if (name.empty()) return {};
    return AccessibilityText::Join({name, AccessibilityText::MessageEntry(table, kind + kStadiumDescriptionShift),
                                    AccessibilityText::Position(cursor + 1u, count)});
}

// "100-Man Brawl, 100 enemies! Can you outlast them all?, 2 of 6": the label is art (MenKumiteTtl01-06), the
// description game text.
std::string MultiManItem(uint32_t proc) {
    const uint16_t cursor = Memory::Read16(proc + kProcCursorOffset);
    if (cursor >= kMultiManTypes) return {};
    const std::string name = AccessibilityText::StringAt("stadium.multi_man", cursor);
    if (name.empty()) return {};
    const uint32_t table = MuMsgTable(proc, kStadiumMuMsgOffset);
    return AccessibilityText::Join({name, AccessibilityText::MessageEntry(table, kMultiManDescriptionFirst + cursor),
                                    AccessibilityText::Position(cursor + 1u, kMultiManTypes)});
}

// ---- Notices ----

// "Ness, the PSI-powered youth, has joined the brawl!, 3/4/2008 13:05:22, 2 of 9".
std::string NoticeLine(uint32_t proc) {
    const uint32_t count = Memory::Read32(proc + kNoticeCountOffset);
    const uint32_t highlight = Memory::Read32(proc + kNoticeHighlightOffset);
    const uint32_t first = Memory::Read32(proc + kNoticeFirstRowOffset);
    if (count > kNoticeIds || highlight >= kNoticeRows || first >= count || first + highlight >= count) return {};
    const uint32_t selected = first + highlight;
    const uint32_t id = Memory::Read32(Memory::Read32(proc + kNoticeIdsOffset) + 4 * selected);
    if (id >= kNoticeIds) return {};
    const std::string text =
        AccessibilityText::MessageEntry(MuMsgTable(proc, kNoticeMuMsgOffset), Memory::Read8(kNoticeMessageIds + id));
    if (text.empty()) return {};
    return AccessibilityText::Join({text, NoticeDate(g_printed.slots[2 * highlight + 1]),
                                    AccessibilityText::Position(selected + 1, count)});
}

// ---- Brawl Records ----

// "Play Time, 12:05, Total time spent playing Smash Bros. Brawl., 3 of 39".
std::string RecordLine(uint32_t proc) {
    const uint32_t highlight = Memory::Read32(proc + kRecordHighlightOffset);
    const uint32_t first = Memory::Read32(proc + kRecordFirstRowOffset);
    if (highlight >= kRecordRows || first > kRecords - kRecordRows) return {};
    const uint32_t selected = first + highlight;
    const uint32_t record = kRecordTable + selected * kRecordBytes;
    const uint32_t table = MuMsgTable(proc, kRecordMuMsgOffset);
    const std::string label = AccessibilityText::MessageEntry(table, Memory::Read16(record + kRecordLabelOffset));
    if (label.empty()) return {};
    return AccessibilityText::Join(
        {label, g_printed.slots[2 * highlight + 1],
         AccessibilityText::MessageEntry(table, Memory::Read16(record + kRecordDescriptionOffset)),
         AccessibilityText::Position(selected + 1, kRecords)});
}

// ---- Movies ----

// "Snake Joins the Brawl, <description>, 4 of 6".
std::string MovieItem(uint32_t proc) {
    const uint32_t count = Memory::Read8(proc + kMovieCountOffset);
    const uint32_t page = Memory::Read8(proc + kMoviePageOffset);
    const uint32_t column = Memory::Read8(proc + kMovieColumnOffset);
    const uint32_t row = Memory::Read8(proc + kMovieRowOffset);
    if (count == 0 || count > kMoviesPerPage || page * kMoviesPerPage >= count || row >= kMovieRowsPerPage) return {};
    const uint32_t onPage = std::min(count - page * kMoviesPerPage, kMoviesPerPage);
    uint32_t index = column;
    for (uint32_t above = 0; above < row; ++above) index += Memory::Read8(kMovieRowLengths + (onPage - 1) * 2 + above);
    if (index >= onPage) return {};
    const uint32_t selected = page * kMoviesPerPage + index;
    const uint32_t id = Memory::Read32(Memory::Read32(proc + kMovieIdsOffset) + 4 * selected);
    uint32_t nameEntry = kSubspaceFolderName, descriptionEntry = kSubspaceDescription;
    if (id != kSubspaceFolderId) {
        if (id > kMovieLastId) return {};
        nameEntry = Memory::Read16(kMovieEntries + 4 * id);
        descriptionEntry = Memory::Read16(kMovieEntries + 4 * id + 2);
    }
    const uint32_t table = MuMsgTable(proc, kMovieMuMsgOffset);
    const std::string name = AccessibilityText::MessageEntry(table, nameEntry);
    if (name.empty()) return {};
    return AccessibilityText::Join({name, AccessibilityText::MessageEntry(table, descriptionEntry),
                                    AccessibilityText::Position(selected + 1, count)});
}

// "<movie name>, 12 of 40".
std::string SubspaceMovieItem(uint32_t proc) {
    const uint32_t count = Memory::Read8(proc + kSubspaceCountOffset);
    const uint32_t selected = Memory::Read8(proc + kSubspaceRowOffset) + Memory::Read8(proc + kSubspaceFirstRowOffset);
    if (selected >= count) return {};
    const uint32_t id = Memory::Read32(Memory::Read32(proc + kSubspaceIdsOffset) + 4 * selected);
    if (id < kSubspaceFirstId || id > kSubspaceLastId) return {};
    const std::string name = AccessibilityText::MessageEntry(MuMsgTable(proc, kSubspaceMuMsgOffset),
                                                             Memory::Read16(kSubspaceNames + 2 * id));
    if (name.empty()) return {};
    return AccessibilityText::Join({name, AccessibilityText::Position(selected + 1, count)});
}

// ---- Sound Test ----

struct SoundTestState {
    uint32_t mode, series, song, group, sound;
    uint16_t row;
    bool operator==(const SoundTestState&) const = default;
};

// The mode, with the brief loading state counted as sound effects (it is entered only from them).
SoundTestState ReadSoundTest(uint32_t proc) {
    uint32_t mode = Memory::Read32(proc + kSoundTestStateOffset);
    if (mode == kSoundTestEffectsLoading) mode = kSoundTestEffects;
    return {mode, Memory::Read32(proc + kSoundTestSeriesOffset),
            Memory::Read32(proc + kSoundTestSongOffset),  Memory::Read32(proc + kSoundTestGroupOffset),
            Memory::Read32(proc + kSoundTestSoundOffset), Memory::Read16(proc + kProcCursorOffset)};
}

// MUSIC / SOUND EFFECTS (MenSoundtest03, 04), as the current mode.
std::string SoundTestMode(const SoundTestState& state) {
    if (state.mode == kSoundTestMusic) return AccessibilityText::StringAt("sound_test.modes", 0);
    if (state.mode == kSoundTestEffects) return AccessibilityText::StringAt("sound_test.modes", 1);
    return {};
}

// The category shown: the music series, or the sound-effect group.
std::string SoundTestCategory(uint32_t proc, const SoundTestState& state) {
    if (state.mode == kSoundTestMusic) {
        if (state.series >= kSeries) return {};
        return AccessibilityText::MessageEntry(Memory::Read32(proc + kSoundTestSeriesTableOffset), state.series);
    }
    if (state.mode != kSoundTestEffects || state.group >= kEffectGroups) return {};
    const uint32_t list = Memory::Read32(proc + kSoundTestEffectListOffset);
    const uint32_t entry = Memory::Read32(list + kEffectGroupBytes * state.group);
    return AccessibilityText::MessageEntry(Memory::Read32(proc + kSoundTestGroupTableOffset), entry);
}

// "Main Theme, Source: Super Smash Bros. Brawl, 1 of 12": the song's title and source as the display prints them (the
// source line is entry 21 "Source: " followed by the source game), and its place in the series.
std::string SoundTestSong(uint32_t proc, const SoundTestState& state) {
    if (state.series >= kSeries) return {};
    uint32_t sound = 0;
    if (!Memory::TryRead32(kSoundSystemPointer, sound) || sound == 0) return {};
    const uint32_t songs = Memory::Read32(sound + kSoundSeriesCountsOffset + 4 * state.series);
    if (songs > kMaxSeriesSongs || state.song >= songs) return {};
    const uint32_t index =
        Memory::Read32(sound + kSoundSeriesSongsOffset + kSoundSeriesBytes * state.series + 4 * state.song);
    const uint32_t infos = Memory::Read32(sound + kSoundInfoCountOffset);
    if (infos > kMaxSoundRecords || index >= infos) return {};
    const uint32_t info = Memory::Read32(sound + kSoundInfoRecordsOffset) + kSoundInfoRecordBytes * index;
    const std::string title = AccessibilityText::MessageEntry(Memory::Read32(proc + kSoundTestTitleTableOffset),
                                                              Memory::Read32(info + kSoundInfoTitleOffset));
    if (title.empty()) return {};
    const std::string label = AccessibilityText::MessageEntry(Memory::Read32(proc + kSoundTestSeriesTableOffset),
                                                              kSoundTestSourceLabelEntry);
    const std::string game = AccessibilityText::MessageEntry(Memory::Read32(proc + kSoundTestSourceTableOffset),
                                                             Memory::Read32(info + kSoundInfoSourceOffset));
    const std::string source = label.empty() || game.empty() ? std::string() : label + " " + game;
    return AccessibilityText::Join({title, source, AccessibilityText::Position(state.song + 1, songs)});
}

// The sound effect's number, as shown (No. index + 1).
std::string SoundTestNumber(const SoundTestState& state) { return std::to_string(state.sound + 1); }

// A row as the cursor reaches it: "Music, 1 of 3"; "Category, Kirby, 2 of 3"; on the song row "Title, <song item>"
// (the song's own place in its series stands for the row's); "No., 5, 3 of 3". Labels are art (MenSoundtest12, 13,
// 16).
std::string SoundTestRow(uint32_t proc, const SoundTestState& state) {
    switch (state.row) {
    case kModeRow:
        return AccessibilityText::Join({SoundTestMode(state), AccessibilityText::Position(1, kSoundTestRows)});
    case kCategoryRow: {
        const std::string category = SoundTestCategory(proc, state);
        if (category.empty()) return {};
        return AccessibilityText::Join({AccessibilityText::String("sound_test.category"), category,
                                        AccessibilityText::Position(2, kSoundTestRows)});
    }
    case kSongRow:
        if (state.mode == kSoundTestMusic) {
            const std::string song = SoundTestSong(proc, state);
            if (song.empty()) return {};
            return AccessibilityText::Join({AccessibilityText::String("sound_test.title_label"), song});
        }
        if (state.mode != kSoundTestEffects) return {};
        return AccessibilityText::Join({AccessibilityText::String("sound_test.number"), SoundTestNumber(state),
                                        AccessibilityText::Position(3, kSoundTestRows)});
    default: return {};
    }
}

// After a frame of either mode's input: a new mode, row or value, interrupting. A song or sound that moved into
// another series or group speaks that category first.
void SpeakSoundTestChange(uint32_t proc, const SoundTestState& before) {
    const SoundTestState after = ReadSoundTest(proc);
    if (after == before) return;
    if (after.mode != before.mode) {
        Say(SoundTestMode(after), true);
    } else if (after.row != before.row) {
        Say(SoundTestRow(proc, after), true);
    } else if (after.row == kCategoryRow) {
        Say(SoundTestCategory(proc, after), true);
    } else if (after.row == kSongRow) {
        const bool categoryChanged =
            after.mode == kSoundTestMusic ? after.series != before.series : after.group != before.group;
        const std::string category = categoryChanged ? SoundTestCategory(proc, after) : std::string();
        const std::string value =
            after.mode == kSoundTestMusic ? SoundTestSong(proc, after) : SoundTestNumber(after);
        Say(AccessibilityText::Join({category, value}), true);
    }
}

void SoundTestInput(CpuContext* ctx, void (*input)(CpuContext*)) {
    const uint32_t proc = ctx->gpr[3];
    if (!IsOpen(proc, Kind::SoundTest) || AccessibilityText::SilencedByMods()) {
        input(ctx);
        return;
    }
    SoundTestState before{};
    try {
        before = ReadSoundTest(proc);
    } catch (const Memory::AccessViolation&) {
        input(ctx);
        return;
    }
    input(ctx);
    try {
        SpeakSoundTestChange(proc, before);
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, "sound test", e);
    }
}

// ---- Openings ----

// The title (interrupting) and the selected item queued after it; with no title the item interrupts.
void SpeakOpening(const std::string& title, const std::string& item) {
    Say(title, true);
    Say(item, title.empty());
}

// Runs a screen's enter with its printf output expected (or none), then remembers the screen.
void Enter(CpuContext* ctx, void (*enter)(CpuContext*), Kind kind, uint32_t printedMuMsgOffset,
           bool fighterNames = false) {
    const uint32_t proc = ctx->gpr[3];
    g_printed = Printed{printedMuMsgOffset != 0 ? proc : 0, printedMuMsgOffset, fighterNames, {}};
    enter(ctx);
    Remember(proc, kind);
}

// The open Notices or Brawl Records screen closes: nothing more to keep from printf.
void StopKeepingPrints(CpuContext* ctx, void (*exit)(CpuContext*)) {
    const uint32_t proc = ctx->gpr[3];
    exit(ctx);
    if (g_printed.proc == proc) g_printed = Printed{};
}

// A list screen's input: speaks `line` (interrupting) when one of the cursor fields `fields` changed.
template <size_t N>
void ListInput(CpuContext* ctx, void (*input)(CpuContext*), Kind kind, const std::array<uint32_t, N>& fields,
               uint32_t fieldBytes, std::string (*line)(uint32_t proc), const char* what) {
    const uint32_t proc = ctx->gpr[3];
    if (!IsOpen(proc, kind) || AccessibilityText::SilencedByMods()) {
        input(ctx);
        return;
    }
    const auto read = [&](uint32_t offset) -> uint32_t {
        switch (fieldBytes) {
        case 1: return Memory::Read8(proc + offset);
        case 2: return Memory::Read16(proc + offset);
        default: return Memory::Read32(proc + offset);
        }
    };
    std::array<uint32_t, N> before{};
    try {
        for (size_t i = 0; i < N; ++i) before[i] = read(fields[i]);
    } catch (const Memory::AccessViolation&) {
        input(ctx);
        return;
    }
    input(ctx);
    try {
        for (size_t i = 0; i < N; ++i) {
            if (read(fields[i]) == before[i]) continue;
            Say(line(proc), true);
            return;
        }
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, what, e);
    }
}

} // namespace DataScreenSpeech

// Stadium opens on its top list (from Solo, or back from a mode), or straight on the Multi-Man list (back from some
// Multi-Man modes: state 3 at 0x940314B4), where slot 0 still shows "Multi-Man Brawl" (printed at 0x940313A4 with the
// cursor on that item): the section after the title. Title art MenStadiumPanel05Title.
extern "C" void DataScreensStadiumEnter_940310a0(CpuContext* ctx) {
    using namespace DataScreenSpeech;
    const uint32_t proc = ctx->gpr[3];
    Enter(ctx, &func_940310A0, Kind::Stadium, 0);
    if (!g_open || AccessibilityText::SilencedByMods()) return;
    try {
        const std::string title = AccessibilityText::String("stadium.title");
        if (Memory::Read32(proc + kStadiumStateOffset) == kStadiumMultiManList) {
            const std::string section =
                AccessibilityText::MessageEntry(MuMsgTable(proc, kStadiumMuMsgOffset), kStadiumMultiManKind);
            SpeakOpening(title, section);
            Say(MultiManItem(proc), title.empty() && section.empty());
        } else {
            SpeakOpening(title, StadiumItem(proc));
        }
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, "stadium open", e);
    }
}

// Stadium's frame: A on Multi-Man Brawl opens its list (state 1 to 2), its first type is spoken; B there closes it,
// and once the top list has focus again (state 4 to 1) its selected item is spoken.
extern "C" void DataScreensStadiumProcess_940314dc(CpuContext* ctx) {
    using namespace DataScreenSpeech;
    const uint32_t proc = ctx->gpr[3];
    uint32_t before = 0;
    const bool known = IsOpen(proc, Kind::Stadium) && Memory::TryRead32(proc + kStadiumStateOffset, before);
    func_940314DC(ctx);
    uint32_t after = 0;
    if (!known || AccessibilityText::SilencedByMods() || !Memory::TryRead32(proc + kStadiumStateOffset, after)) return;
    try {
        if (before == kStadiumTopList && after == kStadiumMultiManOpening)
            Say(MultiManItem(proc), true);
        else if (before == kStadiumClosing && after == kStadiumTopList)
            Say(StadiumItem(proc), true);
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, "stadium", e);
    }
}

extern "C" void DataScreensStadiumMove_94031ac0(CpuContext* ctx) {
    using namespace DataScreenSpeech;
    ListInput<1>(ctx, &func_94031AC0, Kind::Stadium, {DataScreenSpeech::kProcCursorOffset}, 2, StadiumItem, "stadium");
}

extern "C" void DataScreensMultiManMove_94031cb4(CpuContext* ctx) {
    using namespace DataScreenSpeech;
    ListInput<1>(ctx, &func_94031CB4, Kind::Stadium, {DataScreenSpeech::kProcCursorOffset}, 2, MultiManItem,
                 "multi-man list");
}

// Notices: title art MenMainDataCmnTitle.08 (setFrameTex 8.0 at 0x94020544); its rows were printed by the enter.
extern "C" void DataScreensNoticesEnter_9402047c(CpuContext* ctx) {
    using namespace DataScreenSpeech;
    const uint32_t proc = ctx->gpr[3];
    Enter(ctx, &func_9402047C, Kind::Notices, kNoticeMuMsgOffset);
    if (!g_open || AccessibilityText::SilencedByMods()) return;
    try {
        SpeakOpening(AccessibilityText::String("notices.title"), NoticeLine(proc));
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, "notices open", e);
    }
}

extern "C" void DataScreensNoticesInput_94020cbc(CpuContext* ctx) {
    using namespace DataScreenSpeech;
    ListInput<2>(ctx, &func_94020CBC, Kind::Notices, {kNoticeHighlightOffset, kNoticeFirstRowOffset}, 4, NoticeLine,
                 "notices");
}

extern "C" void DataScreensNoticesExit_94020be0(CpuContext* ctx) {
    DataScreenSpeech::StopKeepingPrints(ctx, &func_94020BE0);
}

// Brawl Records: title art MenMainDataCmnTitle.02 (setFrameTex 2.0 at 0x94021608).
extern "C" void DataScreensBrawlRecordsEnter_94021548(CpuContext* ctx) {
    using namespace DataScreenSpeech;
    const uint32_t proc = ctx->gpr[3];
    Enter(ctx, &func_94021548, Kind::BrawlRecords, kRecordMuMsgOffset, true);
    if (!g_open || AccessibilityText::SilencedByMods()) return;
    try {
        SpeakOpening(AccessibilityText::String("brawl_records.title"), RecordLine(proc));
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, "brawl records open", e);
    }
}

extern "C" void DataScreensBrawlRecordsInput_94021e60(CpuContext* ctx) {
    using namespace DataScreenSpeech;
    ListInput<2>(ctx, &func_94021E60, Kind::BrawlRecords, {kRecordHighlightOffset, kRecordFirstRowOffset}, 4,
                 RecordLine, "brawl records");
}

extern "C" void DataScreensBrawlRecordsExit_94021d94(CpuContext* ctx) {
    DataScreenSpeech::StopKeepingPrints(ctx, &func_94021D94);
}

// Movies: the enter sets the MOVIES title (setSelect 30, 0x94025DCC) when coming from Data; the Subspace list and the
// movie player never touch the panel (their only panel call is the Subspace list's setMsg at 0x94027050), so the title
// is still shown on the way back. Spoken on every enter, as the main menu's screens do.
extern "C" void DataScreensMoviesEnter_94025c4c(CpuContext* ctx) {
    using namespace DataScreenSpeech;
    const uint32_t proc = ctx->gpr[3];
    Enter(ctx, &func_94025C4C, Kind::Movies, 0);
    if (!g_open || AccessibilityText::SilencedByMods()) return;
    try {
        SpeakOpening(AccessibilityText::String("main_menu.titles.movies"), MovieItem(proc));
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, "movies open", e);
    }
}

extern "C" void DataScreensMoviesInput_9402698c(CpuContext* ctx) {
    using namespace DataScreenSpeech;
    ListInput<3>(ctx, &func_9402698C, Kind::Movies, {kMoviePageOffset, kMovieColumnOffset, kMovieRowOffset}, 1,
                 MovieItem, "movies");
}

// The Subspace Emissary movies have no title of their own (MOVIES stays); opening from Movies, the description bar's
// fixed line (setMsg entry 11 at 0x94027054) introduces the list. Back from a movie only the selection is spoken.
extern "C" void DataScreensSubspaceMoviesEnter_94026f0c(CpuContext* ctx) {
    using namespace DataScreenSpeech;
    const uint32_t proc = ctx->gpr[3], previous = ctx->gpr[5];
    Enter(ctx, &func_94026F0C, Kind::SubspaceMovies, 0);
    if (!g_open || AccessibilityText::SilencedByMods()) return;
    try {
        const std::string introduction =
            previous != kMoviePlayerProc
                ? AccessibilityText::MessageEntry(MuMsgTable(proc, kSubspaceMuMsgOffset), kSubspaceDescription)
                : std::string();
        SpeakOpening(introduction, SubspaceMovieItem(proc));
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, "subspace movies open", e);
    }
}

extern "C" void DataScreensSubspaceMoviesInput_940276c0(CpuContext* ctx) {
    using namespace DataScreenSpeech;
    ListInput<2>(ctx, &func_940276C0, Kind::SubspaceMovies, {kSubspaceRowOffset, kSubspaceFirstRowOffset}, 1,
                 SubspaceMovieItem, "subspace movies");
}

// Sound Test opens in music mode on the mode row (the enter zeroes state, row, series and song). Title art
// MenSoundtest06 on the music panel (model 105).
extern "C" void DataScreensSoundTestEnter_94023500(CpuContext* ctx) {
    using namespace DataScreenSpeech;
    const uint32_t proc = ctx->gpr[3];
    Enter(ctx, &func_94023500, Kind::SoundTest, 0);
    if (!g_open || AccessibilityText::SilencedByMods()) return;
    try {
        SpeakOpening(AccessibilityText::String("sound_test.title"), SoundTestRow(proc, ReadSoundTest(proc)));
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, "sound test open", e);
    }
}

extern "C" void DataScreensSoundTestMusicInput_940247e4(CpuContext* ctx) {
    DataScreenSpeech::SoundTestInput(ctx, &func_940247E4);
}

extern "C" void DataScreensSoundTestEffectsInput_94023b88(CpuContext* ctx) {
    DataScreenSpeech::SoundTestInput(ctx, &func_94023B88);
}

// Keeps what the open Notices or Brawl Records screen prints with printf (formatted from the call's registers before
// it runs). Every other caller passes straight through.
extern "C" void DataScreensMessagePrintf_800b9230(CpuContext* ctx) {
    using namespace DataScreenSpeech;
    const uint32_t muMsg = ctx->gpr[3], slot = ctx->gpr[4];
    uint32_t own = 0;
    const bool kept = g_printed.proc != 0 && muMsg != 0 && slot < kPrintedSlots &&
                      Memory::TryRead32(g_printed.proc + g_printed.muMsgOffset, own) && own == muMsg;
    std::string text;
    if (kept) {
        std::optional<std::string> plate;
        if (g_printed.fighterNames) {
            try {
                plate = FighterPlate(ctx->gpr[5]);
            } catch (const Memory::AccessViolation&) {
                plate = std::string();
            }
        }
        text = plate ? std::move(*plate) : FormatGuestPrintf(*ctx).value_or(std::string());
    }
    func_800B9230(ctx);
    if (kept) g_printed.slots[slot] = std::move(text);
}

REGISTER_NATIVE_FUNCTION_AS(0x940310A0, DataScreensStadiumEnter_940310a0, "DataScreensStadiumEnter_940310a0");
REGISTER_NATIVE_FUNCTION_AS(0x940314DC, DataScreensStadiumProcess_940314dc, "DataScreensStadiumProcess_940314dc");
REGISTER_NATIVE_FUNCTION_AS(0x94031AC0, DataScreensStadiumMove_94031ac0, "DataScreensStadiumMove_94031ac0");
REGISTER_NATIVE_FUNCTION_AS(0x94031CB4, DataScreensMultiManMove_94031cb4, "DataScreensMultiManMove_94031cb4");
REGISTER_NATIVE_FUNCTION_AS(0x9402047C, DataScreensNoticesEnter_9402047c, "DataScreensNoticesEnter_9402047c");
REGISTER_NATIVE_FUNCTION_AS(0x94020CBC, DataScreensNoticesInput_94020cbc, "DataScreensNoticesInput_94020cbc");
REGISTER_NATIVE_FUNCTION_AS(0x94021548, DataScreensBrawlRecordsEnter_94021548, "DataScreensBrawlRecordsEnter_94021548");
REGISTER_NATIVE_FUNCTION_AS(0x94021E60, DataScreensBrawlRecordsInput_94021e60, "DataScreensBrawlRecordsInput_94021e60");
REGISTER_NATIVE_FUNCTION_AS(0x94020BE0, DataScreensNoticesExit_94020be0, "DataScreensNoticesExit_94020be0");
REGISTER_NATIVE_FUNCTION_AS(0x94021D94, DataScreensBrawlRecordsExit_94021d94, "DataScreensBrawlRecordsExit_94021d94");
REGISTER_NATIVE_FUNCTION_AS(0x94025C4C, DataScreensMoviesEnter_94025c4c, "DataScreensMoviesEnter_94025c4c");
REGISTER_NATIVE_FUNCTION_AS(0x9402698C, DataScreensMoviesInput_9402698c, "DataScreensMoviesInput_9402698c");
REGISTER_NATIVE_FUNCTION_AS(0x94026F0C, DataScreensSubspaceMoviesEnter_94026f0c,
                            "DataScreensSubspaceMoviesEnter_94026f0c");
REGISTER_NATIVE_FUNCTION_AS(0x940276C0, DataScreensSubspaceMoviesInput_940276c0,
                            "DataScreensSubspaceMoviesInput_940276c0");
REGISTER_NATIVE_FUNCTION_AS(0x94023500, DataScreensSoundTestEnter_94023500, "DataScreensSoundTestEnter_94023500");
REGISTER_NATIVE_FUNCTION_AS(0x940247E4, DataScreensSoundTestMusicInput_940247e4,
                            "DataScreensSoundTestMusicInput_940247e4");
REGISTER_NATIVE_FUNCTION_AS(0x94023B88, DataScreensSoundTestEffectsInput_94023b88,
                            "DataScreensSoundTestEffectsInput_94023b88");
REGISTER_NATIVE_FUNCTION_AS(0x800B9230, DataScreensMessagePrintf_800b9230, "DataScreensMessagePrintf_800b9230");
