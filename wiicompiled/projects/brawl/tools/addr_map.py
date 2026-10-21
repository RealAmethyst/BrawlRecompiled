"""Map Super Paper Mario (R8PE01 rev 0) main.dol addresses to Super Smash Bros. Brawl (RSBE01 rev 1) main.dol.

Functions: identical masked instruction bodies (unique match), else the same name in the Brawl decomp's symbols.txt.
Data: the matched functions' references (lis/addi pairs, r13/r2 small-data offsets) at the same instruction index.
Imported by remap_runtime.py; run alone for a summary:  addr_map.py <spm main.dol> <brawl main.dol> <spm MAP.txt> <brawl symbols.txt>"""
import bisect, collections, re, struct, sys, hashlib


class Dol:
    def __init__(self, path):
        b = open(path, "rb").read()
        h = struct.unpack(">18I18I18IIII", b[:0xE4])
        self.b = b
        self.secs = [(h[18 + i], h[36 + i], h[i], i < 7) for i in range(18) if h[36 + i]]
        self.text = [(a, a + s) for a, s, o, x in self.secs if x]

    def read(self, a, n):
        for addr, size, off, _ in self.secs:
            if addr <= a and a + n <= addr + size:
                return self.b[off + a - addr: off + a - addr + n]
        return None

    def words(self, a, size):
        raw = self.read(a, size)
        return list(struct.unpack(">%dI" % (size // 4), raw)) if raw else []

    def is_text(self, a):
        return any(s <= a < e for s, e in self.text)


def r13_r2(dol):
    """__init_registers: lis/ori r2 then r13 (whichever order); scans .init."""
    regs = {}
    s, e = dol.text[0]
    w = dol.words(s, e - s)
    for i in range(len(w) - 1):
        op, rd, ra = w[i] >> 26, (w[i] >> 21) & 31, (w[i] >> 16) & 31
        if op == 15 and ra == 0 and rd in (1, 2, 13):
            n = w[i + 1]
            if n >> 26 == 24 and (n >> 21) & 31 == rd and (n >> 16) & 31 == rd:  # ori rd,rd,lo
                regs.setdefault(rd, ((w[i] & 0xFFFF) << 16) | (n & 0xFFFF))
    return regs.get(13), regs.get(2)


def sext(v):
    return v - 0x10000 if v & 0x8000 else v


def skeleton(w):
    """Mask relocated fields: branch targets, immediates of address-forming and memory ops."""
    out = []
    for x in w:
        op = x >> 26
        if op == 18:
            out.append(x & 0xFC000003)
        elif op in (14, 15, 24, 25) or 32 <= op <= 55 or op in (56, 57, 60, 61):  # addi/lis/ori/oris, loads/stores, psq
            out.append(x & 0xFFFF0000)
        else:
            out.append(x)
    return tuple(out)


def refs(dol, a, size, r13, r2):
    """[(instruction index, absolute address)] for lis+lo pairs and r13/r2-relative accesses."""
    w = dol.words(a, size)
    hi = {}
    out = []
    for i, x in enumerate(w):
        op, rd, ra, imm = x >> 26, (x >> 21) & 31, (x >> 16) & 31, x & 0xFFFF
        if op == 15 and ra == 0:
            hi[rd] = imm << 16
            continue
        if op in (14, 24) or 32 <= op <= 55 or op in (56, 57, 60, 61):
            lo = imm if op == 24 else sext(imm if op not in (56, 57, 60, 61) else (imm & 0xFFF) | (0xF000 if imm & 0x800 else 0))
            if ra in hi:
                out.append((i, (hi[ra] + lo) & 0xFFFFFFFF))
            elif ra == 13 and r13:
                out.append((i, (r13 + sext(imm)) & 0xFFFFFFFF))
            elif ra == 2 and r2:
                out.append((i, (r2 + sext(imm)) & 0xFFFFFFFF))
        if op in (14, 15, 24, 25, 31) and rd in hi and op != 15:
            if not (op in (14, 24) and ra == rd):
                hi.pop(rd, None)
        if op == 18 and x & 1:  # bl clobbers volatiles
            for r in list(hi):
                if r <= 12:
                    hi.pop(r)
    return out


class Mapper:
    def __init__(self, spm_dol, brawl_dol, spm_map, brawl_syms, spm_lst=None):
        self.S, self.B = Dol(spm_dol), Dol(brawl_dol)
        self.s13, self.s2 = r13_r2(self.S)
        self.b13, self.b2 = r13_r2(self.B)
        rows = [l.split(None, 1) for l in open(spm_map).read().splitlines() if l.strip()]
        sf = sorted((int(a, 16), n.strip()) for a, n in rows)
        self.sstart = [a for a, _ in sf]
        self.sname = [n for _, n in sf]
        if spm_lst:
            for l in open(spm_lst).read().splitlines():
                a, _, n = l.partition(":")
                if re.fullmatch(r"[0-9A-Fa-f]{8}", a):
                    a = int(a, 16)
                    k = bisect.bisect_left(self.sstart, a)
                    if k < len(self.sstart) and self.sstart[k] == a and self.sname[k].startswith(("fn_", "func_")):
                        self.sname[k] = n.strip()
        self.bfunc, self.bobj, self.bname = [], [], {}
        for l in open(brawl_syms).read().splitlines():
            m = re.match(r"(\S+) = (\S+):0x([0-9A-Fa-f]+); // type:(\w+)(?: size:0x([0-9A-Fa-f]+))?", l)
            if not m or m[2] not in (".init", ".text", ".data", ".bss", ".sdata", ".sbss", ".sdata2", ".sbss2", ".rodata", ".ctors"):
                continue
            a, size = int(m[3], 16), int(m[5] or "0", 16)
            if m[4] == "function":
                self.bfunc.append((a, size, m[1]))
            self.bname.setdefault(m[1], a)
        self.bfunc.sort()
        self.bstart = [a for a, _, _ in self.bfunc]
        idx = collections.defaultdict(list)
        for a, size, n in self.bfunc:
            if size >= 8:
                idx[(size, hashlib.sha1(repr(skeleton(self.B.words(a, size))).encode()).digest())].append(a)
        self.bidx = idx
        self._fcache = {}

    def spm_func(self, a):
        k = bisect.bisect_right(self.sstart, a) - 1
        if k < 0:
            return None
        end = self.sstart[k + 1] if k + 1 < len(self.sstart) else a + 4
        return self.sstart[k], end - self.sstart[k], self.sname[k]

    def brawl_func_size(self, a):
        k = bisect.bisect_left(self.bstart, a)
        return self.bfunc[k][1] if k < len(self.bstart) and self.bstart[k] == a else None

    def map_func(self, start):
        """SPM function start -> (brawl start, how) or (None, why)."""
        if start in self._fcache:
            return self._fcache[start]
        f = self.spm_func(start)
        res = (None, "nofunc")
        if f and f[0] == start:
            _, size, name = f
            w = self.S.words(start, size)
            # Trailing alignment padding belongs to no function in symbols.txt.
            while w and w[-1] == 0:
                w.pop()
            key = (len(w) * 4, hashlib.sha1(repr(skeleton(w)).encode()).digest())
            hits = self.bidx.get(key, [])
            byname = self.bname.get(name) if not name.startswith(("fn_", "func_")) else None
            if len(hits) == 1:
                res = (hits[0], "body")
            elif byname is not None and (not hits or byname in hits):
                res = (byname, "name" + ("+body" if hits else ""))
            elif hits:
                res = (None, "ambiguous(%d)" % len(hits))
            else:
                res = (None, "unmatched:" + name)
        self._fcache[start] = res
        return res

    def map_code(self, a):
        f = self.spm_func(a)
        if not f:
            return None, "nofunc"
        b, how = self.map_func(f[0])
        if b is None:
            return None, how
        off = a - f[0]
        bs = self.brawl_func_size(b)
        if bs is not None and off >= max(bs, 4):
            return None, "offset-out"
        return b + off, how

    def _ref_index(self):
        if hasattr(self, "_refs"):
            return self._refs
        by_target = collections.defaultdict(list)
        for k, s in enumerate(self.sstart):
            if not self.S.is_text(s):
                continue
            size = (self.sstart[k + 1] if k + 1 < len(self.sstart) else s + 4) - s
            if size > 0x8000:
                continue
            for i, t in refs(self.S, s, size, self.s13, self.s2):
                if 0x80000000 <= t < 0x81800000 and not self.S.is_text(t):
                    by_target[t].append((s, size, i))
        self._refs = (sorted(by_target), by_target)
        return self._refs

    def map_data(self, d):
        """Vote over SPM references to d (or the nearest symbol below it within 0x400)."""
        keys, by_target = self._ref_index()
        k = bisect.bisect_right(keys, d) - 1
        votes = collections.Counter()
        tried = 0
        while k >= 0 and d - keys[k] < 0x400 and tried < 6:
            t = keys[k]
            for s, size, i in by_target[t]:
                b, how = self.map_func(s)
                if b is None:
                    continue
                bsize = self.brawl_func_size(b) or size
                for j, bt in refs(self.B, b, bsize, self.b13, self.b2):
                    if j == i:
                        votes[bt + (d - t)] += 1 if how.startswith("body") else 0.5
            if votes:
                break
            k -= 1
            tried += 1
        if not votes:
            return None, "noref"
        (best, n), = votes.most_common(1)
        total = sum(votes.values())
        return best, "data(%g/%g)" % (n, total)

    def map(self, a):
        if a < 0x80003100:  # low memory globals are the same on every Wii game
            return a, "lowmem"
        if self.S.is_text(a):
            return self.map_code(a)
        return self.map_data(a)


if __name__ == "__main__":
    m = Mapper(*sys.argv[1:5], *(sys.argv[5:6]))
    print("spm r13/r2", hex(m.s13), hex(m.s2), "brawl r13/r2", hex(m.b13), hex(m.b2))
    c = collections.Counter()
    for s in m.sstart:
        if m.S.is_text(s) and s < 0x80300000:
            c[m.map_func(s)[1].split(":")[0]] += 1
    print(c)
