#include "accessibility_menu.h"

#include "accessibility_text.h"
#include "memory.h"
#include "netplay/game_layout.h"
#include "ppc_runtime.h"
#include "runtime_config.h"
#include "runtime_log.h"
#include "spatial_audio.h"
#include "speech.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <exception>
#include <memory>
#include <locale>
#include <sstream>
#include <string>
#include <vector>

// Options, Accessibility. The entry is game cursor 7 of the Options screen (muProcOption, sora_menu_main): the game
// highlights only cursors below its item count of 7 (0x806A5770, 0x806A586C), so with the cursor on 7 none of its
// buttons is lit and the runtime draws the entry instead. Three event hooks carry it:
// - muProcMenu::moveCursor (main_menu.cpp, through MoveOptionsCursor): the moves onto and off the entry, which the
//   game's navigation table (7 entries) cannot express;
// - the menu input reader 0x806A4E04 (here): A on the entry opens the page; while the page is open its input drives
//   the page and the Options screen receives none;
// - setMsg (main_menu.cpp): the description panel is cleared while the entry is selected.
// Settings are saved in Config.toml, never in the game's save data.

extern "C" void func_806A4E04(CpuContext* ctx);  // muProcMenu: the menu input of every muMenuMain screen (sora_menu_rule)

// main_menu.cpp: whether a proc is muMenuMain's current one.
namespace MainMenuSpeech {
bool IsCurrentProc(uint32_t proc);
}

// Named, not anonymous: the accessibility sources share one unity translation unit (cmake/PublicProducts.cmake).
namespace AccessibilityMenuPage {

namespace GL = GameLayout;

// muProcOption (Options): vtable at +0x668 (built by 0x806CD510), cursor u16 at +0x42, navigation table pointer at
// +0x63C (7 entries of 16 bytes: u16 neighbour up, down, left, right at +0, +4, +8, +12, a repeat-reset byte after
// each), cursor sound at vtable +0x24 (0x94018E8C, playSE(20)).
constexpr uint32_t kOptionsVtable = 0x940448A0;
constexpr uint32_t kProcVtableOffset = 0x668;
constexpr uint32_t kProcCursorOffset = 0x42;
constexpr uint32_t kProcNavTableOffset = 0x63C;
constexpr uint32_t kVtableCursorSound = 0x24;
constexpr uint32_t kNavEntryBytes = 16;
constexpr uint16_t kGameItems = 7;
// The repeat reset after a move (moveCursor 0x806A51C8-0x806A524C): for each of the proc's 9 pad statuses (+72, 168
// bytes apart), fn_800BA364(status, mask), the mask built from the target's reset bytes (+2 up 8, +6 down 4, +10 left
// 1, +14 right 2).
constexpr uint32_t kResetRepeat = 0x800BA364;
constexpr uint32_t kPadStatusOffset = 72;
constexpr uint32_t kPadStatusBytes = 168;
constexpr uint32_t kPadStatuses = 9;

// The menu input reader's result (0x806A4E04): directions with key repeat, A or Start as decide, B as back.
constexpr uint32_t kInputUp = 0x1;
constexpr uint32_t kInputDown = 0x2;
constexpr uint32_t kInputLeft = 0x4;
constexpr uint32_t kInputRight = 0x8;
constexpr uint32_t kInputDecide = 0x10;
constexpr uint32_t kInputBack = 0x20;
constexpr uint32_t kInputStart = 0x100;
constexpr uint32_t kInputA = 0x200;

// Moves the game's table has no room for: the entry ends the bottom row (Sound, My Music, Erase Data, Accessibility)
// and sits under Controls. Off the entry: up and down to Controls, left to Erase Data, right around to Sound.
constexpr uint16_t kControls = 3;
constexpr uint16_t kSound = 4;
constexpr uint16_t kEraseData = 6;

enum class Row { SpatialAudio, Height, No3dEffect, AudioDescription, DescriptionVolume };
constexpr int kVolumeStep = 10;
constexpr int kVolumeMax = 200;

struct State {
    bool open = false;
    size_t focus = 0;
    uint32_t proc = 0;          // the Options screen the page belongs to
    uint64_t inputFrame = 0;    // the overlay frame the Options screen last read its input in
    uint64_t frame = 0;         // overlay frames drawn
};
State g;

bool IsOptions(uint32_t proc) {
    uint32_t vtable = 0;
    return proc != 0 && Memory::TryRead32(proc + kProcVtableOffset, vtable) && vtable == kOptionsVtable &&
           MainMenuSpeech::IsCurrentProc(proc);
}

std::vector<Row> Rows() {
    std::vector<Row> rows;
    if (SpatialAudio::Supported()) rows.insert(rows.end(), {Row::SpatialAudio, Row::Height, Row::No3dEffect});
    rows.insert(rows.end(), {Row::AudioDescription, Row::DescriptionVolume});
    return rows;
}

std::string RowKey(Row row) {
    switch (row) {
    case Row::SpatialAudio: return "spatial_audio";
    case Row::Height: return "height";
    case Row::No3dEffect: return "no_3d_effect";
    case Row::AudioDescription: return "audio_description";
    case Row::DescriptionVolume: return "description_volume";
    }
    return {};
}

std::string OnOff(bool on) { return AccessibilityText::String(on ? "on" : "off"); }

int DescriptionVolume() {
    return std::clamp(RuntimeConfigFile::AudioDescriptionVolume(100) / kVolumeStep * kVolumeStep, 0, kVolumeMax);
}

std::string Value(Row row) {
    switch (row) {
    case Row::SpatialAudio: return OnOff(SpatialAudio::Enabled());
    case Row::Height:
        return AccessibilityText::StringAt("accessibility_menu.heights",
                                           static_cast<size_t>(SpatialAudio::HeightSetting()));
    case Row::No3dEffect: return OnOff(SpatialAudio::No3dEffect());
    case Row::AudioDescription: return OnOff(RuntimeConfigFile::AudioDescriptionEnabled(true));
    case Row::DescriptionVolume: return AccessibilityText::Percent(static_cast<unsigned>(DescriptionVolume()));
    }
    return {};
}

// Changes a row by `step` (-1 left, +1 right, 0 for A: toggles flip, the height steps round). Returns whether the value
// changed; the new value is saved.
bool Change(Row row, int step) {
    switch (row) {
    case Row::SpatialAudio: {
        const bool on = SpatialAudio::Enabled();
        if ((step < 0 && !on) || (step > 0 && on)) return false;
        SpatialAudio::SetEnabled(!on);
        RuntimeConfigFile::SetSpatialAudioEnabled(!on);
        return true;
    }
    case Row::No3dEffect: {
        const bool on = SpatialAudio::No3dEffect();
        if ((step < 0 && !on) || (step > 0 && on)) return false;
        SpatialAudio::SetNo3dEffect(!on);
        RuntimeConfigFile::SetSpatialAudioNo3dEffect(!on);
        return true;
    }
    case Row::AudioDescription: {
        const bool on = RuntimeConfigFile::AudioDescriptionEnabled(true);
        if ((step < 0 && !on) || (step > 0 && on)) return false;
        RuntimeConfigFile::SetAudioDescriptionEnabled(!on);
        return true;
    }
    case Row::Height: {
        static constexpr const char* kNames[] = {"true", "low", "medium", "high"};
        const int current = static_cast<int>(SpatialAudio::HeightSetting());
        const int next = step == 0 ? (current + 1) % 4 : current + step;
        if (next < 0 || next > 3) return false;
        SpatialAudio::SetHeight(static_cast<SpatialAudio::Height>(next));
        RuntimeConfigFile::SetSpatialAudioHeight(kNames[next]);
        return true;
    }
    case Row::DescriptionVolume: {
        const int current = DescriptionVolume();
        const int next = current + step * kVolumeStep;
        if (step == 0 || next < 0 || next > kVolumeMax) return false;
        RuntimeConfigFile::SetAudioDescriptionVolume(next);
        return true;
    }
    }
    return false;
}

// "Spatial audio, Off, <description>, 1 of 5".
std::string RowLine(const std::vector<Row>& rows, size_t index) {
    const std::string key = "accessibility_menu.rows." + RowKey(rows[index]);
    return AccessibilityText::Join({AccessibilityText::String(key + ".name"), Value(rows[index]),
                                    AccessibilityText::String(key + ".description"),
                                    AccessibilityText::Position(index + 1, rows.size())});
}

void PlayCursorSound(CpuContext* ctx, uint32_t proc) {
    uint32_t vtable = 0, sound = 0;
    if (!Memory::TryRead32(proc + kProcVtableOffset, vtable) || !Memory::TryRead32(vtable + kVtableCursorSound, sound) ||
        sound == 0) {
        return;
    }
    CpuContext call = *ctx;
    call.gpr[3] = proc;
    InvokeIndirectCpu(sound, &call);
}

void Open(uint32_t proc) {
    g.open = true;
    g.focus = 0;
    g.proc = proc;
    const auto rows = Rows();
    Speech::Say(AccessibilityText::String("accessibility_menu.title"), true);
    Speech::Say(RowLine(rows, g.focus), false);
    Speech::Say(AccessibilityText::String("accessibility_menu.hint"), false);
}

void Close() {
    g.open = false;
    Speech::Say(AccessibilityMenu::EntryLine(), true);
}

// One frame of the page's input, as the game's menu reader decoded it.
void PageInput(CpuContext* ctx, uint32_t proc, uint32_t input) {
    const auto rows = Rows();
    if (input & kInputBack) {
        Close();
        return;
    }
    if (input & (kInputUp | kInputDown)) {
        const size_t count = rows.size();
        g.focus = (input & kInputUp) ? (g.focus + count - 1) % count : (g.focus + 1) % count;
        PlayCursorSound(ctx, proc);
        Speech::Say(RowLine(rows, g.focus), true);
        return;
    }
    const int step = (input & kInputLeft) ? -1 : (input & kInputRight) ? 1 : (input & kInputDecide) ? 0 : 2;
    if (step == 2 || !Change(rows[g.focus], step)) return;
    PlayCursorSound(ctx, proc);
    Speech::Say(Value(rows[g.focus]), true);
}

// ---- drawing: the page over the whole window, in the game's 456-unit layout space (y up) ----
constexpr uint32_t kInk = 0x0B0D16FFu, kPanel = 0x1C2133FFu, kFocus = 0x2B3350FFu, kSnow = 0xF4F4F8FFu,
                   kDim = 0xA8ACBEFFu, kOrange = 0xFF8C28FFu;

ImU32 Col(uint32_t rgba, float alpha = 1.0f) {
    return IM_COL32(rgba >> 24, (rgba >> 16) & 255, (rgba >> 8) & 255, static_cast<int>((rgba & 255) * alpha));
}

std::shared_ptr<GL::Font> MenuFont() {
    static const auto melee = GL::LoadFont("/system/font/font_melee.brfnt");
    static const auto hira = GL::LoadFont("/system/font/font_hira.brfnt");
    return GL::FontReady(melee.get()) ? melee : hira;
}

struct Canvas {
    ImDrawList* list = nullptr;
    GL::View view;
    ImVec2 P(float x, float y) const { return ImVec2(view.centerX + x * view.scale, view.centerY - y * view.scale); }
};

void Text(const Canvas& c, const std::string& text, float x, float y, float size, uint32_t color, int originH,
          float maxWidth) {
    const auto font = MenuFont();
    if (!GL::FontReady(font.get())) return;
    const auto u = GL::Utf16(text);
    const float width = GL::MeasureText(*font, u, size);
    if (maxWidth > 0.0f && width > maxWidth) size *= maxWidth / width;
    GL::DrawText(c.list, c.view, *font, u, x, y, size, color, color, 1.0f, originH, 1);
}

void DrawPage() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    if (ImGui::Begin("##accessibility", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                                                     ImGuiWindowFlags_NoInputs)) {
        Canvas c{ImGui::GetWindowDrawList(), GL::GameView()};
        c.view.centerX = vp->Pos.x + vp->Size.x * 0.5f;
        c.view.centerY = vp->Pos.y + vp->Size.y * 0.5f;
        c.view.scale = std::min(vp->Size.y / 470.0f, vp->Size.x / 960.0f);
        c.list->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y), Col(kInk, 0.92f));
        Text(c, AccessibilityText::String("accessibility_menu.title"), 0, 190, 40, kOrange, 1, 600);
        const auto rows = Rows();
        const float top = 120.0f, spacing = 52.0f;
        for (size_t i = 0; i < rows.size(); ++i) {
            const float y = top - spacing * static_cast<float>(i);
            const bool focused = i == g.focus;
            c.list->AddRectFilled(c.P(-320, y + 22), c.P(320, y - 22), Col(focused ? kFocus : kPanel));
            if (focused) c.list->AddRect(c.P(-320, y + 22), c.P(320, y - 22), Col(kOrange), 0.0f, 0, 3.0f);
            const std::string key = "accessibility_menu.rows." + RowKey(rows[i]);
            Text(c, AccessibilityText::String(key + ".name"), -300, y, 20, focused ? kSnow : kDim, 0, 330);
            Text(c, Value(rows[i]), 300, y, 22, kSnow, 2, 250);
        }
        const std::string description =
            AccessibilityText::String("accessibility_menu.rows." + RowKey(rows[g.focus]) + ".description");
        Text(c, description, 0, top - spacing * static_cast<float>(rows.size()) - 10, 16, kDim, 1, 760);
        Text(c, AccessibilityText::String("accessibility_menu.hint"), 0, -214, 15, kDim, 1, 760);
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

// ---- the entry on the Options screen, beside the game's 3D buttons ----
// Each Options button is a model (scene+0x3E0 holds the muMenuMain models; Options uses 29-35, the enter's
// `lwz r5,116(r5)` at 0x94018820) whose bone "rotN" the CHR animation moves. MuObject+0xC is its ScnMdl; the ScnMdl's
// ResMdl (+0xE8) names the bone in its node dictionary (ResDic at ResMdl + *(s32*)(+0x14): entries of 16 bytes from
// +0x18, name and data offsets at +8 and +12 from the dictionary; the node's matrix id at +0x10, GetResNode 0x8018EDA4);
// the world matrices (MTX34) are at *(ScnMdl+0xEC) + id*0x30 (GetScnMtxPos 0x801AEF80), translation in column 3. The
// menu draws with gfCamera 0 (pointer 0x8059FFC0; view MTX34 at +0, perspective flag u32 +0x100, vertical field of view
// +0xD8 in radians, aspect +0xE4), which MenuRoot copies into its scene camera every frame (0x800B0794); projecting
// mirrors gfCamera::calcProjection3Dto2D (0x80019A2C).
constexpr uint32_t kSceneManagerPointer = 0x805A0060;
constexpr uint32_t kSceneModels = 0x3E0;
constexpr uint32_t kFirstOptionsModel = 29;
constexpr uint32_t kMuObjectScnMdl = 0x0C;
constexpr uint32_t kScnMdlResMdl = 0xE8;
constexpr uint32_t kScnMdlWorldMatrices = 0xEC;
constexpr uint32_t kResMdlNodeDictionary = 0x14;
constexpr uint32_t kNodeMatrixId = 0x10;
constexpr uint32_t kCameraPointer = 0x8059FFC0;
constexpr uint32_t kCameraPerspective = 0x100;
constexpr uint32_t kCameraFovY = 0xD8;
constexpr uint32_t kCameraAspect = 0xE4;
constexpr uint16_t kMyMusic = 5;

bool ReadF32(uint32_t addr, float& value) {
    uint32_t bits = 0;
    if (!Memory::TryRead32(addr, bits)) return false;
    std::memcpy(&value, &bits, sizeof value);
    return std::isfinite(value);
}

// The world position of an Options button (cursor 0-6), from its rotN bone.
bool ButtonPosition(uint16_t cursor, std::array<float, 3>& position) {
    uint32_t manager = 0, scene = 0, object = 0, model = 0, resMdl = 0, matrices = 0, dictionaryOffset = 0, count = 0;
    if (!Memory::TryRead32(kSceneManagerPointer, manager) || !Memory::TryRead32(manager + 4, scene) ||
        !Memory::TryRead32(scene + kSceneModels + (kFirstOptionsModel + cursor) * 4, object) || object == 0 ||
        !Memory::TryRead32(object + kMuObjectScnMdl, model) || model == 0 ||
        !Memory::TryRead32(model + kScnMdlResMdl, resMdl) || resMdl == 0 ||
        !Memory::TryRead32(model + kScnMdlWorldMatrices, matrices) || matrices == 0 ||
        !Memory::TryRead32(resMdl + kResMdlNodeDictionary, dictionaryOffset)) {
        return false;
    }
    const uint32_t dictionary = resMdl + dictionaryOffset;
    if (!Memory::TryRead32(dictionary + 4, count) || count > 256) return false;
    for (uint32_t i = 1; i <= count; ++i) {
        const uint32_t entry = dictionary + 8 + i * 16;
        uint32_t nameOffset = 0, nodeOffset = 0, id = 0;
        if (!Memory::TryRead32(entry + 8, nameOffset) || !Memory::TryRead32(entry + 12, nodeOffset)) return false;
        if (AccessibilityText::GuestCString(dictionary + nameOffset, 8) != "rotN") continue;
        if (!Memory::TryRead32(dictionary + nodeOffset + kNodeMatrixId, id) || id > 1024) return false;
        const uint32_t matrix = matrices + id * 0x30;
        return ReadF32(matrix + 0x0C, position[0]) && ReadF32(matrix + 0x1C, position[1]) &&
               ReadF32(matrix + 0x2C, position[2]);
    }
    return false;
}

// A world position on screen (ImGui pixels), through gfCamera 0. The picture is `view` (456 layout units tall); the
// camera's aspect sets its width, which is also how the renderer keeps 3D unstretched in Fill.
bool Project(const std::array<float, 3>& p, const GL::View& view, ImVec2& screen) {
    uint32_t camera = 0, perspective = 0;
    std::array<float, 12> m{};
    float fov = 0.0f, aspect = 0.0f;
    if (!Memory::TryRead32(kCameraPointer, camera) || camera == 0 ||
        !Memory::TryRead32(camera + kCameraPerspective, perspective) || perspective != 1 ||
        !ReadF32(camera + kCameraFovY, fov) || !ReadF32(camera + kCameraAspect, aspect) || fov < 0.01f ||
        aspect < 0.5f) {
        return false;
    }
    for (uint32_t i = 0; i < m.size(); ++i) {
        if (!ReadF32(camera + i * 4, m[i])) return false;
    }
    const float x = m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3];
    const float y = m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7];
    const float z = m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11];
    if (z > -0.01f) return false;  // behind the camera
    const float c = 1.0f / std::tan(fov * 0.5f);
    const float halfHeight = 228.0f * view.scale;
    // ndc x = (c / aspect) x / -z spans the picture's width, halfHeight * aspect on each side.
    screen = ImVec2(view.centerX + c * x / -z * halfHeight, view.centerY - c * y / -z * halfHeight);
    return true;
}

// The entry, one step along the bottom row past Erase Data (the step from My Music to Erase Data), sized from that
// step; highlighted while the game's cursor is on it, with its description under it.
void DrawEntry() {
    std::array<float, 3> myMusic{}, eraseData{};
    if (!ButtonPosition(kMyMusic, myMusic) || !ButtonPosition(kEraseData, eraseData)) {
        AccessibilityText::LogOnce("accessibility entry: the Options buttons' positions are unreadable; not drawn");
        return;
    }
    std::array<float, 3> entry{};
    for (int i = 0; i < 3; ++i) entry[i] = eraseData[i] * 2.0f - myMusic[i];
    const GL::View view = GL::GameView();
    ImVec2 centre, previous;
    if (!Project(entry, view, centre) || !Project(eraseData, view, previous)) {
        AccessibilityText::LogOnce("accessibility entry: the menu camera is unreadable; not drawn");
        return;
    }
    static bool logged = false;
    if (!logged) {
        logged = true;
        std::ostringstream message;
        message.imbue(std::locale::classic());  // no digit grouping from the user's locale
        message << "accessibility entry: Erase Data at " << eraseData[0] << ' ' << eraseData[1] << ' ' << eraseData[2]
                << ", My Music at " << myMusic[0] << ' ' << myMusic[1] << ' ' << myMusic[2] << "; entry drawn at "
                << centre.x << ", " << centre.y << " px (Erase Data at " << previous.x << ", " << previous.y << ")";
        RT_LOG(RT_TAG_ACCESSIBILITY) << message.str() << std::endl;
    }
    const float step = std::fabs(centre.x - previous.x);
    if (step < 4.0f) return;
    const float halfWidth = step * 0.44f, halfHeight = step * 0.22f;
    bool focused = false;
    try {
        focused = Memory::Read16(g.proc + kProcCursorOffset) == AccessibilityMenu::kEntryCursor;
    } catch (const Memory::AccessViolation&) {
        return;
    }
    ImDrawList* list = ImGui::GetForegroundDrawList();
    const ImVec2 lo(centre.x - halfWidth, centre.y - halfHeight), hi(centre.x + halfWidth, centre.y + halfHeight);
    list->AddRectFilled(lo, hi, Col(focused ? kFocus : kPanel, 0.95f), halfHeight * 0.4f);
    list->AddRect(lo, hi, Col(focused ? kOrange : kDim), halfHeight * 0.4f, 0, focused ? 4.0f : 2.0f);
    Canvas c{list, view};
    const float units = 1.0f / view.scale;  // layout units per pixel
    const float cx = (centre.x - view.centerX) * units, cy = (view.centerY - centre.y) * units;
    Text(c, AccessibilityText::String("accessibility_menu.entry"), cx, cy, halfHeight * units * 0.8f,
         focused ? kSnow : kDim, 1, halfWidth * 2.0f * units * 0.9f);
    if (focused) {
        Text(c, AccessibilityText::String("accessibility_menu.entry_description"), cx,
             cy - halfHeight * units * 1.6f, 14, kSnow, 1, 300);
    }
}

void Draw() {
    ++g.frame;
    // The Options screen has gone, or stopped reading its input (a screen change under way): the page closes.
    if (g.open && (g.frame - g.inputFrame > 2 || !IsOptions(g.proc))) {
        g.open = false;
    }
    if (g.open) {
        DrawPage();
    } else if (g.frame - g.inputFrame <= 2 && IsOptions(g.proc)) {
        DrawEntry();
    }
}

} // namespace AccessibilityMenuPage

namespace AccessibilityMenu {

namespace Page = AccessibilityMenuPage;

bool Available() { return !AccessibilityText::SilencedByMods(); }

size_t OptionsItemCount() { return Available() ? Page::kGameItems + 1 : Page::kGameItems; }

std::string EntryLine() {
    return AccessibilityText::Join({AccessibilityText::String("accessibility_menu.entry"),
                                    AccessibilityText::String("accessibility_menu.entry_description"),
                                    AccessibilityText::Position(kEntryCursor + 1, kEntryCursor + 1)});
}

bool MoveOptionsCursor(CpuContext* ctx) {
    const uint32_t proc = ctx->gpr[3];
    const uint32_t input = ctx->gpr[4];
    if (!Available() || !Page::IsOptions(proc)) return false;
    uint16_t cursor = 0;
    try {
        cursor = Memory::Read16(proc + Page::kProcCursorOffset);
    } catch (const Memory::AccessViolation&) {
        return false;
    }
    // The game's order of checks: up, down, left, right.
    const int direction = (input & Page::kInputUp) ? 0 : (input & Page::kInputDown) ? 1 : (input & Page::kInputLeft) ? 2
                        : (input & Page::kInputRight) ? 3 : -1;
    uint16_t target = cursor;
    if (cursor == kEntryCursor) {
        static constexpr uint16_t kFromEntry[] = {Page::kControls, Page::kControls, Page::kEraseData, Page::kSound};
        if (direction >= 0) target = kFromEntry[direction];
    } else if ((cursor == Page::kControls && direction <= 1 && direction >= 0) ||
               (cursor == Page::kEraseData && direction == 3) || (cursor == Page::kSound && direction == 2)) {
        target = kEntryCursor;
    } else {
        return false;  // a move the game's own table covers
    }
    if (target == cursor) {
        ctx->gpr[3] = 0;
        return true;
    }
    // As moveCursor does it: the cursor sound, the new cursor, then each pad's key repeat reset as the target's table
    // entry asks (the entry has none to ask for).
    Page::PlayCursorSound(ctx, proc);
    try {
        Memory::Write16(proc + Page::kProcCursorOffset, target);
        if (target != kEntryCursor) {
            const uint32_t entry = Memory::Read32(proc + Page::kProcNavTableOffset) + target * Page::kNavEntryBytes;
            uint32_t mask = 0;
            if (Memory::Read8(entry + 2) != 0) mask |= 8;
            if (Memory::Read8(entry + 6) != 0) mask |= 4;
            if (Memory::Read8(entry + 10) != 0) mask |= 1;
            if (Memory::Read8(entry + 14) != 0) mask |= 2;
            for (uint32_t i = 0; i < Page::kPadStatuses; ++i) {
                CpuContext call = *ctx;
                call.gpr[3] = proc + Page::kPadStatusOffset + i * Page::kPadStatusBytes;
                call.gpr[4] = mask;
                InvokeIndirectCpu(Page::kResetRepeat, &call);
            }
        }
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, "options entry move", e);
    }
    ctx->gpr[3] = 1;
    return true;
}

void Draw() {
    try {
        Page::Draw();
    } catch (const std::exception& e) {
        AccessibilityText::LogOnce(std::string("accessibility page: ") + e.what());
    }
}

} // namespace AccessibilityMenu

// The menu input of every muMenuMain screen; on the Options screen, A on the entry opens the page, and the page takes
// the input while it is open.
extern "C" void AccessibilityMenuInput_806a4e04(CpuContext* ctx) {
    namespace Page = AccessibilityMenuPage;
    const uint32_t proc = ctx->gpr[3];
    func_806A4E04(ctx);
    if (!AccessibilityMenu::Available() || !Page::IsOptions(proc)) return;
    Page::g.inputFrame = Page::g.frame;
    Page::g.proc = proc;
    const uint32_t input = ctx->gpr[3];
    if (Page::g.open) {
        Page::PageInput(ctx, proc, input);
        ctx->gpr[3] = 0;
        return;
    }
    Page::g.open = false;
    uint16_t cursor = 0;
    try {
        cursor = Memory::Read16(proc + Page::kProcCursorOffset);
    } catch (const Memory::AccessViolation&) {
        return;
    }
    if (cursor == AccessibilityMenu::kEntryCursor && (input & Page::kInputDecide)) {
        ctx->gpr[3] = input & ~(Page::kInputDecide | Page::kInputA | Page::kInputStart);
        Page::Open(proc);
    }
}

REGISTER_NATIVE_FUNCTION_AS(0x806A4E04, AccessibilityMenuInput_806a4e04, "AccessibilityMenuInput_806a4e04");
