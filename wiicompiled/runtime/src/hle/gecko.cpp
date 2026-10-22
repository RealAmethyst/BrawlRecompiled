// gecko.cpp - Gecko/Ocarina codes (.gct) for mods, installed as Project+'s Brawl Mod Loader (a USB Loader GX
// derivative) installs them: its code handler at 0x80001800 (translated with the game: projects/brawl/gecko), the
// code list at the address gc.txt names (default 0x800022A8), gc.txt's pokes, and a branch to the handler where
// gc.txt's hooktype says. Codes then patch the game freely; patched functions run in the interpreter
// (guest_code_patch.cpp).
//
// The codes are the first <folder>/RSBE01.GCT on the virtual SD card (UserData/SD/<folder>, Project+'s layout:
// sd:/Project+/RSBE01.GCT and gc.txt), else UserData/Codes/RSBE01.gct with an optional gc.txt beside it.

#include "gecko.h"

#include "abi_bridge.h"
#include "guest_code_patch.h"
#include "memory.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>


namespace {

namespace fs = std::filesystem;

constexpr uint32_t kHandlerEntry = 0x800018A8u;
constexpr uint32_t kCodeListSlot = 0x80001804u;  // where the shipped handler reads the code list address
constexpr uint32_t kVbiHookSite = 0x801E9A2Cu;   // VISetNextFrameBuffer's blr
constexpr uint32_t kAxHookSite = 0x80200984u;    // __AXOutNewFrame's blr (hooktype 7, AXNextFrame)

struct Poke {
    uint32_t address, value;
    std::optional<uint32_t> ifAddress, ifValue;  // pokeifequal
};

struct GameConfig {
    uint32_t codeListStart = 0x800022A8u;
    uint32_t codeListEnd = 0x80003000u;
    int hookType = 1;
    std::vector<Poke> pokes;
};

uint32_t BranchTo(uint32_t at, uint32_t target) { return 0x48000000u | ((target - at) & 0x03FFFFFCu); }

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::optional<fs::path> FindFile(const fs::path& directory, const std::string& name) {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(directory, ec)) {
        if (entry.is_regular_file(ec) && Lower(entry.path().filename().string()) == Lower(name)) return entry.path();
    }
    return std::nullopt;
}

std::optional<fs::path> FindCodes() {
    const fs::path userData = RuntimeConfigFile::ApplicationDataDirectory();
    // Config.toml [mods] sd_codes: the card folder whose codes load when several builds share the card (the
    // launcher's "Codes to load"); otherwise the first folder with codes.
    std::string chosen;
    try {
        std::ifstream in(RuntimeConfigFile::ResolveConfigPath(), std::ios::binary);
        if (in) chosen = RuntimeConfigFile::FindConfigValue<std::string>(toml::parse(in, "Config.toml"), "mods", "sd_codes").value_or("");
    } catch (const std::exception& error) {
        RT_LOGF(RT_TAG_MOD, "[gecko] Config.toml: %s\n", error.what());
    }
    if (!chosen.empty()) {
        if (auto codes = FindFile(userData / "SD" / chosen, "RSBE01.GCT")) return codes;
        RT_LOGF(RT_TAG_MOD, "[gecko] the chosen SD folder '%s' has no RSBE01.GCT; using the first found\n", chosen.c_str());
    }
    std::error_code ec;
    std::vector<fs::path> folders;
    for (const auto& entry : fs::directory_iterator(userData / "SD", ec)) {
        if (entry.is_directory(ec)) folders.push_back(entry.path());
    }
    std::sort(folders.begin(), folders.end());
    for (const auto& folder : folders) {
        if (auto codes = FindFile(folder, "RSBE01.GCT")) return codes;
    }
    return FindFile(userData / "Codes", "RSBE01.gct");
}

bool MatchesGameId(const std::string& pattern) {
    static constexpr char kId[] = "RSBE01";
    for (size_t i = 0; i < 6; ++i) {
        if (i >= pattern.size() || (pattern[i] != '?' && std::toupper(static_cast<unsigned char>(pattern[i])) != kId[i])) return false;
    }
    return pattern.size() == 6;
}

std::vector<uint32_t> HexArguments(const std::string& line) {
    std::vector<uint32_t> out;
    const size_t open = line.find('('), close = line.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close < open) return out;
    std::stringstream args(line.substr(open + 1, close - open - 1));
    for (std::string token; std::getline(args, token, ',');) out.push_back(static_cast<uint32_t>(std::stoul(token, nullptr, 16)));
    return out;
}

// USB Loader GX's gameconfig.txt format: sections headed by a game id (? matches any character) ending in ':'.
GameConfig ReadGameConfig(const fs::path& path) {
    GameConfig config;
    std::ifstream in(path);
    bool active = false;
    for (std::string line; std::getline(in, line);) {
        line.erase(std::remove_if(line.begin(), line.end(), [](unsigned char c) { return std::isspace(c); }), line.end());
        if (line.empty() || line[0] == '#') continue;
        if (line.back() == ':') {
            active = MatchesGameId(line.substr(0, line.size() - 1));
            continue;
        }
        if (!active) continue;
        try {
            const std::string key = Lower(line.substr(0, line.find_first_of("=(")));
            const auto value = [&] { return static_cast<uint32_t>(std::stoul(line.substr(line.find('=') + 1), nullptr, 16)); };
            if (key == "codeliststart") config.codeListStart = value();
            else if (key == "codelistend") config.codeListEnd = value();
            else if (key == "hooktype") config.hookType = static_cast<int>(std::stoul(line.substr(line.find('=') + 1)));
            else if (key == "poke") {
                const auto args = HexArguments(line);
                if (args.size() == 2) config.pokes.push_back({args[0], args[1], std::nullopt, std::nullopt});
            } else if (key == "pokeifequal") {
                const auto args = HexArguments(line);
                if (args.size() == 4) config.pokes.push_back({args[2], args[3], args[0], args[1]});
            }
        } catch (const std::exception&) {
            RT_LOGF(RT_TAG_MOD, "[gecko] gc.txt: cannot read '%s'\n", line.c_str());
        }
    }
    return config;
}

} // namespace

namespace Gecko {

void Install() {
    const auto codesPath = FindCodes();
    if (!codesPath) return;
    std::ifstream in(*codesPath, std::ios::binary);
    const std::vector<uint8_t> codes((std::istreambuf_iterator<char>(in)), {});
    static constexpr uint8_t kMagic[8] = {0x00, 0xD0, 0xC0, 0xDE, 0x00, 0xD0, 0xC0, 0xDE};
    if (codes.size() < 16 || std::memcmp(codes.data(), kMagic, 8) != 0) {
        RT_LOGF(RT_TAG_MOD, "[gecko] '%s' is not a Gecko code list\n", reinterpret_cast<const char*>(codesPath->u8string().c_str()));
        return;
    }
    GameConfig config;
    if (const auto gameConfig = FindFile(codesPath->parent_path(), "gc.txt")) config = ReadGameConfig(*gameConfig);
    if (codes.size() > config.codeListEnd - config.codeListStart || !Memory::Contains(config.codeListStart, codes.size())) {
        RT_LOGF(RT_TAG_MOD, "[gecko] %zu bytes of codes do not fit 0x%08X..0x%08X\n", codes.size(), config.codeListStart,
                config.codeListEnd);
        return;
    }

    // Guard main.dol's code first, so the pokes and the hook below are seen as the code patches they are.
    GuestCodePatch::Protect(0x80004000u, 0x800064E0u, "main.dol .init");
    GuestCodePatch::Protect(0x8000C860u, 0x804064E0u, "main.dol .text");

    Memory::Write32(kCodeListSlot, config.codeListStart);
    std::memcpy(Memory::GetPointer(config.codeListStart, codes.size()), codes.data(), codes.size());
    for (const Poke& poke : config.pokes) {
        if (!poke.ifAddress || Memory::Read32(*poke.ifAddress) == *poke.ifValue) Memory::Write32(poke.address, poke.value);
    }
    switch (config.hookType) {
    case 1: Memory::Write32(kVbiHookSite, BranchTo(kVbiHookSite, kHandlerEntry)); break;
    case 7: Memory::Write32(kAxHookSite, BranchTo(kAxHookSite, kHandlerEntry)); break;
    default: RT_LOGF(RT_TAG_MOD, "[gecko] hooktype %d is not supported; using the VI hook\n", config.hookType);
             Memory::Write32(kVbiHookSite, BranchTo(kVbiHookSite, kHandlerEntry));
    }
    GuestCodePatch::Sync();
    RT_LOGF(RT_TAG_MOD, "[gecko] %zu code lines from '%s' at 0x%08X, hooktype %d, %zu poke(s)\n", codes.size() / 8 - 2,
            reinterpret_cast<const char*>(codesPath->u8string().c_str()), config.codeListStart, config.hookType, config.pokes.size());
}

void RunVbiHook(CpuContext* ctx) {
    if (Memory::Read32(kVbiHookSite) != BranchTo(kVbiHookSite, kHandlerEntry)) return;
    InvokeIndirectCpu(kHandlerEntry, ctx);
    GuestCodePatch::Sync();
}

} // namespace Gecko
