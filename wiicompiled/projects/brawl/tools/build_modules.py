"""Every Brawl .rel module at a fixed home address, pre-linked, as one synthetic REL for the base translation.

Each of the 126 modules (files/module/*.rel) gets its own home in MEM2 above the 64 MiB the game sees (0x94004000 up),
so modules the game loads at the same heap address (all stages, all fighters) never share code addresses. Every
module is relocated for its home with the imports from the dol and from every other module resolved, so the whole
set translates together with main.dol. At run time hle/brawl_modules.cpp links a module by copying its pristine
pre-linked image to its home instead of relocating it in the game's buffer.

Usage: build_modules.py <files/module dir> <decomp config/RSBE01_02 dir> <out .rel> <wiicompiled dir>
Writes: <out .rel>, projects/brawl/MAP_modules.txt (function starts), projects/brawl/module_layout.json,
runtime/src/hle/brawl_module_layout.inc (the runtime's table)."""
import json, re, struct, sys, zlib
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
from prelink_rels import Rel, link

HOME_BASE = 0x94000000        # synthetic REL load address
FIRST_HOME = 0x94004000       # room for the synthetic header and section table
HOME_LIMIT = 0x97C00000       # the runtime maps MEM2 to 0x98000000; the game's arena ends at 0x94000000


def align(v, a=0x20):
    return (v + a - 1) & ~(a - 1)


def fix_size(m):
    return struct.unpack(">I", m.raw[0x48:0x4C])[0] if m.ver >= 3 else len(m.raw)


def main():
    mod_dir, cfg_dir, out_rel, wc = map(Path, sys.argv[1:5])
    mods = sorted((Rel(p) for p in mod_dir.glob("*.rel")), key=lambda m: m.id)
    assert len({m.id for m in mods}) == len(mods), "duplicate module ids"
    addr = FIRST_HOME
    for m in mods:
        image = addr
        bss = align(image + fix_size(m))
        veneers = align(bss + max(m.bss_size, 4))
        addr = align(veneers + 16 * len(m.far_call_targets()))
        m.place(image, bss, veneers)
    assert addr <= HOME_LIMIT, hex(addr)
    linked = link({m.id: m for m in mods}, 0)

    # The synthetic REL: the home region as one image, every module section as a section at its absolute offset.
    end = addr
    mem = bytearray(end - HOME_BASE)
    sections = [(0, False, 0)]
    for m in mods:
        base, image, _unresolved = linked[m.name]
        n = fix_size(m)
        mem[m.image - HOME_BASE:m.image - HOME_BASE + n] = image[m.image - base:m.image - base + n]
        for i, (o, e, s) in enumerate(m.secs):
            if i == 0 or not s:
                continue
            sections.append((m.sec_addr(i) - HOME_BASE, e, s))
        if m.veneers:
            v = 16 * len(m.veneers)
            mem[m.veneer_base - HOME_BASE:m.veneer_base - HOME_BASE + v] = image[m.veneer_base - base:m.veneer_base - base + v]
            sections.append((m.veneer_base - HOME_BASE, True, v))
    hdr_size = 0x4C + 8 * len(sections)
    assert hdr_size <= FIRST_HOME - HOME_BASE, hex(hdr_size)
    size = len(mem)
    hdr = struct.pack(">IIIIIIIIIIIIBBBBIII", 0x7E, 0, 0, len(sections), 0x4C, 0, 0, 3, 0, size, size, 0, 0, 0, 0, 0,
                      0, 0, 0) + struct.pack(">III", 0x20, 0x20, size)
    mem[0:hdr_size] = hdr + b"".join(struct.pack(">II", a | (1 if e else 0), s) for a, e, s in sections)
    out_rel.write_bytes(mem)

    # Function starts from the decomp's per-module symbols (section-relative), at the homes.
    funcs = []
    for m in mods:
        sym = cfg_dir / "rels" / m.name / "symbols.txt"
        splits = cfg_dir / "rels" / m.name / "splits.txt"
        names = [l.split()[0] for l in splits.read_text().split("Sections:")[1].split("\n\n")[0].splitlines() if l.strip()]
        index = {n: i + 1 for i, n in enumerate(names)}  # REL section index by the order splits.txt lists them
        for l in sym.read_text().splitlines():
            r = re.match(r"(\S+) = (\.\w+):0x([0-9A-Fa-f]+); // type:function", l)
            if r and r[2] in index and m.secs[index[r[2]]][1]:
                funcs.append((m.sec_addr(index[r[2]]) + int(r[3], 16), f"{m.name}_{r[1]}"))
        funcs += [(at, f"{m.name}_veneer_{t:08X}") for t, at in m.veneers.items()]
    funcs.sort()
    (wc / "projects/brawl/MAP_modules.txt").write_text("".join(f"{a:08X} {n}\n" for a, n in funcs))
    # The translator takes one function map: main.dol's (MAP.txt) and the modules'.
    (wc / "projects/brawl/MAP_full.txt").write_text((wc / "projects/brawl/MAP.txt").read_text() +
                                                    (wc / "projects/brawl/MAP_modules.txt").read_text())

    layout = {}
    rows = []
    for m in mods:
        n = fix_size(m)
        crc = zlib.crc32(m.raw[:n]) & 0xFFFFFFFF  # the unrelocated file as the game reads it: a modded module differs
        layout[m.name] = {"id": m.id, "image": hex(m.image), "bss": hex(m.bss), "bss_size": hex(m.bss_size),
                          "veneers": hex(m.veneer_base), "veneer_count": len(m.veneers),
                          "fix_size": hex(n), "crc32": hex(crc)}
        rows.append(f'    {{{m.id}u, 0x{m.image:08X}u, 0x{m.bss:08X}u, 0x{m.bss_size:X}u, 0x{n:X}u, 0x{crc:08X}u, "{m.name}"}},')
    (wc / "projects/brawl/module_layout.json").write_text(json.dumps(layout, indent=1))
    (wc / "runtime/src/hle/brawl_module_layout.inc").write_text(
        "// Generated by projects/brawl/tools/build_modules.py: every Brawl module's home (pre-linked image and bss).\n"
        "// {id, image, bss, bss size, fixed image size, crc32 of the unrelocated file's first fixed-size bytes, name}\n"
        + "\n".join(rows) + "\n")
    print(f"{len(mods)} modules, homes 0x{FIRST_HOME:08X}..0x{end:08X} ({(end - HOME_BASE) / 1048576:.1f} MiB), "
          f"{len(sections) - 1} sections, {len(funcs)} functions")


if __name__ == "__main__":
    main()
