"""Retarget the runtime's hard-coded Super Paper Mario guest addresses to Brawl (RSBE01 rev 1), in place.
Mapped literals are rewritten; native hooks on unmapped functions are wrapped in #if 0; everything is logged.
Usage: remap_runtime.py <repo root> [--dry]   -> wiicompiled/projects/brawl/remap_log.csv"""
import csv, re, sys
from collections import Counter
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
from addr_map import Mapper

root = Path(sys.argv[1]).resolve()
dry = "--dry" in sys.argv
wc = root / "wiicompiled"
spm = root.parent / "SuperPaperMarioRecomp"
M = Mapper(spm / "game/main.dol.orig", root / "game/main.dol", wc / "projects/spm/MAP.txt",
           root / "extras/brawl-decomp/config/RSBE01_02/symbols.txt", wc / "projects/spm/pal/spm.us0.lst")
SPM_DOL_END, BRAWL_DOL_END = 0x80576720, 0x805A5160  # arena lo: bss end rounded up
LIT = re.compile(r"(?<![0-9A-Za-z_])(0x)([0-9A-Fa-f]{8})(?![0-9A-Fa-f])|(\(\s*)(80[0-9A-Fa-f]{6})(?![0-9A-Za-z_])|(_)(80[0-9A-Fa-f]{6})(?![0-9A-Fa-f])")
SDA = re.compile(r"(r13 - 0x|Sda1Offset = 0x|OffsetFromR13 = 0x)([0-9A-Fa-f]{4})(u?)")
HOOK = re.compile(r"PPC_NATIVE_OVERRIDE|PPC_NATIVE_|REGISTER_NATIVE|NATIVE_CPU_CALL")
SKIP_FILES = ("spm_map_sweep.cpp", "spm_rel_pin.cpp")  # SPM-only tooling: removed from the build by hand
# After the address pass: Brawl's identity and boot values (text, so the address pass never sees them).
TEXT = [("0x52385045", "0x52534245"),  # game code R8PE -> RSBE
        ("R8PE01 revision 0", "RSBE01 revision 1"), ("R8PE01, revision 0", "RSBE01, revision 1"), ("R8PE01", "RSBE01"),
        ("Super Paper Mario", "Super Smash Bros. Brawl"),
        ("constexpr int kRevision = 0;", "constexpr int kRevision = 1;"),
        ('{0x80000004u, 0x30310000u, "Disc maker/id"});  // "01", disc 0, version 1',
         '{0x80000004u, 0x30310001u, "Disc maker/id"});  // "01", disc 0, revision 1'),
        ("kDefaultEntryAddress = 0x80006124u", "kDefaultEntryAddress = 0x8000403Cu"),  # __start
        ("constexpr uint32_t kMem1ArenaLoDefault = 0x80576720u;  // NSMBW __ArenaLo (stack top, see OSInit)",
         "constexpr uint32_t kMem1ArenaLoDefault = 0x805B7160u;  // Brawl __ArenaLo (OSInit, above the 64 KiB boot stack)"),
        ("uint32_t mem1ArenaHiDefault = 0x817EF720u;  // MEM1 end - NSMBW FST (0x8B2C bytes, 32-aligned); verified via REL load addresses at boot",
         "uint32_t mem1ArenaHiDefault = 0x817DA5A0u;  // MEM1 end - Brawl FST (0x25A48 bytes, 32-aligned)")]
log, stats = [], Counter()


def map_addr(a):
    if 0x80F12640 <= a < 0x81800000 or SPM_DOL_END <= a < 0x80BF6840:
        return None, "heap/rel"  # SPM REL or heap objects: game-specific, reviewed by hand
    if 0x80BF6840 <= a < 0x80F12640:
        return None, "spm-rel"
    if a >= 0x80576280 and a < SPM_DOL_END:
        pass
    return M.map(a)


for path in sorted((wc / "runtime").rglob("*")):
    if path.suffix not in (".cpp", ".h", ".hpp", ".inc") or "third_party" in path.parts or path.name in SKIP_FILES:
        continue
    rel = str(path.relative_to(wc)).replace("\\", "/")
    text = path.read_text(encoding="utf-8", errors="surrogateescape")
    lines = text.split("\n")
    changed = False
    for i, line in enumerate(lines):
        drop = [False]

        def lit(m):
            hexpart = m.group(2) or m.group(4) or m.group(6)
            a = int(hexpart, 16)
            if not 0x80003100 <= a < 0x81800000 or a == 0x80808081:  # gx_dl colour constant
                return m.group(0)
            b, how = map_addr(a)
            if not m.group(6):
                stats[how.split("(")[0].split(":")[0]] += 1
                f = M.spm_func(a)
                log.append((rel, i + 1, f"{a:08X}", f"{b:08X}" if b is not None else "", how, f[2] if f else ""))
            if b is None:
                if HOOK.search(line) and M.S.is_text(a):
                    drop[0] = True
                return m.group(0)
            new = f"{b:08x}" if hexpart.islower() and any(c.isalpha() for c in hexpart) else f"{b:08X}"
            return (m.group(1) or m.group(3) or m.group(5)) + new

        def sda(m):
            off = int(m.group(2), 16)
            b, how = M.map(M.s13 - off)
            stats["sda-" + how.split("(")[0]] += 1
            log.append((rel, i + 1, f"r13-{off:04X}", f"r13-{M.b13 - b:04X}" if b else "", "sda:" + how, ""))
            if b is None or not 0 < M.b13 - b < 0x8000:
                return m.group(0)
            new = M.b13 - b
            return f"{m.group(1)}{new:04X}{m.group(3)}" if m.group(2).isupper() else f"{m.group(1)}{new:04x}{m.group(3)}"

        code, sep, comment = line.partition("//")  # comments keep the addresses of the game they were written for
        new = SDA.sub(sda, LIT.sub(lit, code)) + sep + comment
        for old, rep in TEXT:
            if old in new:
                new = new.replace(old, rep)
                stats["text"] += 1
        if drop[0] and not line.lstrip().startswith(("//", "#")):
            new = "#if 0  // BRAWL: no matching function in the Brawl dol\n" + new + "\n#endif"
        if new != line:
            lines[i] = new
            changed = True
    if changed and not dry:
        path.write_text("\n".join(lines), encoding="utf-8", errors="surrogateescape", newline="\n")
with open(wc / "projects/brawl/remap_log.csv", "w", newline="") as fh:
    w = csv.writer(fh)
    w.writerow(["file", "line", "spm", "brawl", "how", "spm_symbol"])
    w.writerows(log)
print(dict(stats))
