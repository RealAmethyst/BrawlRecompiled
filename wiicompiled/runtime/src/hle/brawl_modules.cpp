// Brawl's .rel modules (files/module/*.rel). The translation holds every module pre-linked at its own home address
// above the game's MEM2 arena (projects/brawl/tools/build_modules.py), because the game itself loads many of them
// at the same heap address (every stage, every fighter) and translated code needs one address per instruction.
//
// OSLink/OSLinkFixed therefore link a module at its home instead of in the buffer the game read it into: the home is
// reset to the module's pristine pre-linked image, its bss cleared, and the header in the game's buffer is updated
// as the SDK's Link would, with section, prolog and epilog addresses pointing at the home. A module whose bytes are
// not the disc's (a mod's replacement) is not in the translation; it is linked in place by the game's own Link.
#include "hle_stubs.h"

#include <cstring>
#include <mutex>
#include <vector>

#include "abi_bridge.h"
#include "memory.h"
#include "runtime_log.h"

extern "C" void func_801DD964(CpuContext* ctx);  // Link(module, bss, fixed), translated
extern "C" void func_801DDEAC(CpuContext* ctx);  // OSUnlink(module), translated

namespace {

struct BrawlModule {
    uint32_t id;
    uint32_t image;    // home of the pre-linked image
    uint32_t bss;      // home of its bss
    uint32_t bssSize;
    uint32_t fixSize;  // bytes OSLinkFixed keeps: header and sections, without the relocation tables
    uint32_t crc32;    // of the unrelocated file's first fixSize bytes: anything else is a modded module
    const char* name;
};

constexpr BrawlModule kModules[] = {
#include "brawl_module_layout.inc"
};

constexpr uint32_t kModuleListHead = 0x800030C8u;  // __OSModuleInfoList.head
constexpr uint32_t kModuleListTail = 0x800030CCu;

std::mutex g_modulesMutex;
std::vector<uint8_t> g_pristine[sizeof(kModules) / sizeof(kModules[0])];  // each home's image before first use
std::vector<uint32_t> g_homeLinked;  // guest headers this file linked at a home (OSUnlink takes them back)

uint32_t Crc32(uint32_t address, uint32_t size) {
    static uint32_t table[256];
    static std::once_flag once;
    std::call_once(once, [] {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    });
    const auto* p = static_cast<const uint8_t*>(Memory::GetPointer(address, size));
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < size; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

const BrawlModule* FindModule(uint32_t id, size_t* index) {
    for (size_t i = 0; i < sizeof(kModules) / sizeof(kModules[0]); ++i) {
        if (kModules[i].id == id) {
            *index = i;
            return &kModules[i];
        }
    }
    return nullptr;
}

// Link (OSLink.c) for a vanilla module, with the code at its home. Returns false when the module is not one of
// the disc's (unknown id, or modified bytes): the caller links it in place.
bool LinkAtHome(uint32_t header, uint32_t gameBss) {
    std::lock_guard<std::mutex> lock(g_modulesMutex);
    size_t index = 0;
    const BrawlModule* m = FindModule(Memory::Read32(header), &index);
    if (m == nullptr || Memory::Read32(header + 0x1Cu) != 3u) {
        return false;
    }
    if (Crc32(header, m->fixSize) != m->crc32) {
        RT_LOGF(RT_TAG_OS, "REL module %s at 0x%08X is not the disc's (a mod): linking it in place, untranslated\n",
                m->name, header);
        return false;
    }

    // Every link starts from the pristine image: the game expects a module's data as the file has it.
    auto& pristine = g_pristine[index];
    if (pristine.empty()) {
        const auto* home = static_cast<const uint8_t*>(Memory::GetPointer(m->image, m->fixSize));
        pristine.assign(home, home + m->fixSize);
    } else {
        std::memcpy(Memory::GetPointer(m->image, m->fixSize), pristine.data(), m->fixSize);
    }
    std::memset(Memory::GetPointer(m->bss, m->bssSize ? m->bssSize : 4), 0, m->bssSize);

    // The header in the game's buffer, as Link leaves it: tables absolute, sections and entry points at the home.
    const uint32_t tail = Memory::Read32(kModuleListTail);
    Memory::Write32(header + 0x04u, 0);     // link.next
    Memory::Write32(header + 0x08u, tail);  // link.prev
    if (tail == 0) {
        Memory::Write32(kModuleListHead, header);
    } else {
        Memory::Write32(tail + 0x04u, header);
    }
    Memory::Write32(kModuleListTail, header);

    const uint32_t numSections = Memory::Read32(header + 0x0Cu);
    const uint32_t sections = header + Memory::Read32(header + 0x10u);
    Memory::Write32(header + 0x10u, sections);
    Memory::Write32(header + 0x24u, header + Memory::Read32(header + 0x24u));  // relOffset
    Memory::Write32(header + 0x28u, header + Memory::Read32(header + 0x28u));  // impOffset
    Memory::Write32(header + 0x48u, header + Memory::Read32(header + 0x48u));  // fixSize
    uint8_t bssSection = 0;
    for (uint32_t i = 1; i < numSections; ++i) {
        const uint32_t entry = sections + i * 8u;
        const uint32_t offset = Memory::Read32(entry);
        if (offset != 0) {
            Memory::Write32(entry, m->image + offset);  // keeps the exec bit (bit 0)
        } else if (Memory::Read32(entry + 4u) != 0) {
            bssSection = static_cast<uint8_t>(i);
            Memory::Write32(entry, m->bss);
        }
    }
    Memory::Write8(header + 0x33u, bssSection);
    for (uint32_t field = 0; field < 3; ++field) {  // prolog, epilog, unresolved
        const uint8_t section = Memory::Read8(header + 0x30u + field);
        if (section != 0) {
            const uint32_t at = header + 0x34u + field * 4u;
            Memory::Write32(at, Memory::Read32(at) + (Memory::Read32(sections + section * 8u) & ~1u));
        }
    }
    g_homeLinked.push_back(header);
    RT_LOGF(RT_TAG_OS, "REL module %s (%u) linked at its home 0x%08X (game buffer 0x%08X, bss 0x%08X)\n", m->name,
            m->id, m->image, header, gameBss);
    return true;
}

void LinkModule(CpuContext* ctx, bool fixed) {
    const uint32_t header = ctx->gpr[3];
    if (LinkAtHome(header, ctx->gpr[4])) {
        ctx->gpr[3] = 1;
        return;
    }
    ctx->gpr[5] = fixed ? 1u : 0u;
    func_801DD964(ctx);
    RT_LOGF(RT_TAG_OS, "REL module %u linked in place at 0x%08X (%s)\n", Memory::Read32(header), header,
            ctx->gpr[3] ? "ok" : "FAILED");
}

}  // namespace

// 0x801DDC44 OSLink(module, bss) = Link(module, bss, FALSE); 0x801DDC4C OSLinkFixed: version 3 only, then
// Link(module, bss, TRUE).
extern "C" void Brawl_OSLink_HLE(CpuContext* ctx) { LinkModule(ctx, false); }
PPC_NATIVE_OVERRIDE_VOID(801DDC44, Brawl_OSLink_HLE, (CpuContext* ctx), (ctx));

extern "C" void Brawl_OSLinkFixed_HLE(CpuContext* ctx)
{
    if (Memory::Read32(ctx->gpr[3] + 0x1Cu) != 3u) {
        ctx->gpr[3] = 0;
        return;
    }
    LinkModule(ctx, true);
}
PPC_NATIVE_OVERRIDE_VOID(801DDC4C, Brawl_OSLinkFixed_HLE, (CpuContext* ctx), (ctx));

// 0x801DDEAC OSUnlink(module): a home-linked module only leaves the module list (nothing else was relocated
// against it); anything else goes through the game's own OSUnlink.
extern "C" void Brawl_OSUnlink_HLE(CpuContext* ctx)
{
    const uint32_t header = ctx->gpr[3];
    {
        std::lock_guard<std::mutex> lock(g_modulesMutex);
        for (auto it = g_homeLinked.begin(); it != g_homeLinked.end(); ++it) {
            if (*it != header) continue;
            g_homeLinked.erase(it);
            const uint32_t next = Memory::Read32(header + 0x04u);
            const uint32_t prev = Memory::Read32(header + 0x08u);
            if (next == 0) Memory::Write32(kModuleListTail, prev); else Memory::Write32(next + 0x08u, prev);
            if (prev == 0) Memory::Write32(kModuleListHead, next); else Memory::Write32(prev + 0x04u, next);
            RT_LOGF(RT_TAG_OS, "REL module %u unlinked (game buffer 0x%08X)\n", Memory::Read32(header), header);
            ctx->gpr[3] = 1;
            return;
        }
    }
    func_801DDEAC(ctx);
}
PPC_NATIVE_OVERRIDE_VOID(801DDEAC, Brawl_OSUnlink_HLE, (CpuContext* ctx), (ctx));
