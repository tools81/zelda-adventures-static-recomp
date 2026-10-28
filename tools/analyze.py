"""Control-flow discovery for OS-9 program modules.

Produces a set of decoded instructions reachable from the entry point and
other code pointers, plus a listing for inspection.
"""
import struct
import sys
from m68k import decode, DecodeError, CC
from os9mod import Module

MODULE_BASE = 0x00100000   # default; per-module bases come from game.json

# os9 calls that never return
NORETURN_OS9 = {0x06, 0x1e}  # F$Exit, F$RTE


class Analysis:
    def __init__(self, mod, extra_entries=(), extra_tables=(), a5=True, a6=False):
        self.mod = mod
        self.base = mod.base
        self.code_lo = self.base + 0x48
        self.code_hi = self.base + mod.idataoff
        self.insns = {}
        self.funcs = set()
        self.labels = set()
        self.errors = {}
        self.jumptables = {}   # insn addr -> list of targets
        self.indirect = set()  # unresolved indirect jumps/calls
        self.code_ptrs = set()
        self.extra_tables = list(extra_tables)
        if a5 == 'auto':
            a5 = uses_a5_table(mod)
        self.uses_a5 = bool(a5)
        self.a5table = a5_jumptable(mod) if a5 else {}
        # jump table register: 5 (MPW) or 6 (a6-relative stub table, e.g.
        # Zelda's Adventure).  The table maps displacement -> target.
        self.jt_reg = 5 if self.uses_a5 else None
        if not self.uses_a5 and a6:
            t = a6_jumptable(mod)
            if a6 != 'auto' or len(t) >= 8:
                self.a5table = t
                self.jt_reg = 6
        self.pcrel_calls = {}  # jsr/jmp d8(pc,dn.l) with dn loaded by move.l #imm -> target
        work = [self.base + mod.execoff]
        for t in set(self.a5table.values()):
            work.append(t)
            self.funcs.add(t)
        self.funcs.add(self.base + mod.execoff)
        if mod.excptoff:
            work.append(self.base + mod.excptoff)
            self.funcs.add(self.base + mod.excptoff)
        for e in extra_entries:
            work.append(e)
            self.funcs.add(e)
        self.work = work
        self.data_code_ptrs(mod)
        self.run()
        # function pointers taken with lea/pea x(pc): accept those that start
        # with a function prologue (link / movem to the stack)
        while True:
            added = False
            for p in sorted(self.code_ptrs):
                if p in self.insns or p in self.errors or p & 1:
                    continue
                try:
                    i = decode(self.mod.data, self.base, p)
                except DecodeError:
                    continue
                if i.op == 'link' or (i.op == 'movem' and i.dst is not None and i.dst.kind == 'predec'):
                    self.add(p, func=True)
                    added = True
            if not added:
                break
            self.run()
        # functions nothing refers to statically (dead library code, or code
        # reached through pointers computed at run time): a gap between
        # discovered code that starts with a function prologue is a function
        while self.sweep_gaps():
            self.run()
        self.names = macsbug_names(mod, self.funcs)

    def sweep_gaps(self):
        added = False
        ends = sorted(a + i.length for a, i in self.insns.items())
        for g in ends:
            if g in self.insns or g in self.errors or not self.in_code(g):
                continue
            prev = None
            for back in (2, 4, 6, 8, 10):
                p = self.insns.get(g - back)
                if p is not None and p.addr + p.length == g:
                    prev = p
                    break
            if prev is None or prev.op not in ('rts', 'rte', 'rtr', 'jmp', 'bra'):
                continue
            try:
                i = decode(self.mod.data, self.base, g)
            except DecodeError:
                continue
            if i.op == 'link' or (i.op == 'movem' and i.dst is not None and i.dst.kind == 'predec'):
                self.add(g, func=True)
                added = True
        return added

    def data_code_ptrs(self, mod):
        """function pointers in initialized data: longwords that the loader
        relocates by the module base (code-relative irefs) and that point into
        code.  Accepted like lea/pea pointers (prologue check below)."""
        try:
            doff, b = mod.idata()
            code_refs, _ = mod.irefs()
        except Exception:
            return
        for off in code_refs:
            p = off - doff
            if 0 <= p <= len(b) - 4:
                t = self.base + struct.unpack('>I', b[p:p + 4])[0]
                if self.in_code(t):
                    self.code_ptrs.add(t)

    def in_code(self, a):
        return self.code_lo <= a < self.code_hi

    def add(self, a, func=False):
        if not self.in_code(a) or a & 1:
            return
        if func:
            self.funcs.add(a)
        self.labels.add(a)
        if a not in self.insns:
            self.work.append(a)

    def run(self):
        while self.work:
            a = self.work.pop()
            self.trace(a)

    def trace(self, a):
        while True:
            if a in self.insns or a in self.errors:
                return
            if not self.in_code(a):
                self.errors[a] = 'outside code'
                return
            try:
                i = decode(self.mod.data, self.base, a)
            except DecodeError as e:
                self.errors[a] = str(e)
                return
            self.insns[a] = i
            nxt = a + i.length
            op = i.op
            # code pointers via lea/pea pc-relative
            for ea in (i.src, i.dst):
                if ea is not None and ea.kind == 'pcdisp' and op in ('lea', 'pea'):
                    if self.in_code(ea.addr):
                        self.code_ptrs.add(ea.addr)
            if op in ('bra',):
                self.add(i.target)
                return
            if op == 'b' or op == 'db':
                self.add(i.target)
                self.labels.add(nxt)
                a = nxt
                continue
            if op == 'bsr':
                self.add(i.target, func=True)
                self.labels.add(nxt)
                a = nxt
                continue
            if op in ('jsr', 'jmp'):
                ea = i.src
                if ea.kind == 'pcdisp':
                    self.add(ea.addr, func=(op == 'jsr'))
                elif ea.kind == 'disp' and ea.reg == self.jt_reg and ea.disp in self.a5table:
                    ea.addr = self.a5table[ea.disp]
                    self.add(ea.addr, func=True)
                elif ea.kind == 'pcindex':
                    t = self.pcrel_const(i)
                    if t is not None:
                        self.pcrel_calls[a] = t
                        self.add(t, func=(op == 'jsr'))
                    else:
                        self.jumptable(i)
                else:
                    self.indirect.add(a)
                if op == 'jmp':
                    return
                self.labels.add(nxt)
                a = nxt
                continue
            if op in ('rts', 'rte', 'rtr', 'illegal', 'stop', 'reset'):
                return
            if op == 'os9':
                self.labels.add(nxt)
                # a blocking call interrupted by a signal is restarted: the
                # intercept routine's F$RTE resumes at the os9 instruction
                self.labels.add(a)
                if i.extra == 0x09:
                    # F$Icpt: the intercept routine is usually loaded with lea x(pc),a0
                    for back in range(2, 24, 2):
                        p = self.insns.get(a - back)
                        if p is not None and p.op == 'lea' and p.dst.reg == 0 and p.src.kind == 'pcdisp':
                            self.add(p.src.addr, func=True)
                            break
                if i.extra in NORETURN_OS9:
                    return
            if op == 'trap':
                self.labels.add(nxt)
            if op in ('linea', 'linef'):
                return
            a = nxt

    def pcrel_const(self, i):
        """Microware large-model call: move.l #imm,dN ; jsr d8(pc,dN.l)"""
        ea = i.src
        if not ea.xlong:
            return None
        for back in (6, 2):
            p = self.insns.get(i.addr - back)
            if p is None or p.addr + p.length != i.addr:
                continue
            if p.src is None or p.src.kind != 'imm' or p.size != 'l':
                continue
            if ea.xreg < 8 and p.op in ('move', 'moveq') and p.dst.kind == 'dn' and p.dst.reg == ea.xreg:
                return (ea.addr + p.src.value) & 0xffffffff
            if ea.xreg >= 8 and p.op == 'movea' and p.dst.kind == 'an' and p.dst.reg == ea.xreg - 8:
                return (ea.addr + p.src.value) & 0xffffffff
        return None

    def unrolled_loop(self, i):
        """Jump into an unrolled loop (Duff's device):
              move.l #N,dX ; sub.l dY,dX ; lsl.l #s,dX ; jmp T(pc,dX.l)
           Targets are T + (k << s) for k = 0..N."""
        ea = i.src
        if i.op != 'jmp' or ea.xreg is None or ea.xreg >= 8:
            return False
        r = ea.xreg
        seq = []
        a = i.addr
        for _ in range(3):
            p = next((self.insns[x] for x in (a - 2, a - 4, a - 6) if x in self.insns and
                      self.insns[x].addr + self.insns[x].length == a), None)
            if p is None:
                return False
            seq.append(p)
            a = p.addr
        lsl, sub, mov = seq
        if not (lsl.op == 'lsl' and lsl.dst is not None and lsl.dst.kind == 'dn' and lsl.dst.reg == r and
                lsl.src is not None and lsl.src.kind == 'imm'):
            return False
        if not (sub.op == 'sub' and sub.dst.kind == 'dn' and sub.dst.reg == r):
            return False
        if not (mov.op == 'move' and mov.src.kind == 'imm' and mov.dst.kind == 'dn' and mov.dst.reg == r):
            return False
        n, sh = mov.src.value, lsl.src.value
        if n > 1024:
            return False
        targets = [ea.addr + (k << sh) for k in range(n + 1)]
        for t in targets:
            self.add(t)
        self.jumptables[i.addr] = targets
        return True

    def jumptable(self, i):
        """Compiler switch idiom:
              cmpi.x #N,d0 ; b(hi|gt) default ; ... add d0,d0
              move.w T(pc,d0),d0 ; jmp T-2(pc,d0.w)
           Table T holds N+1 word offsets relative to the jmp's ea base."""
        if self.unrolled_loop(i):
            return
        prev = self.insns.get(i.addr - 4)
        if prev is None or prev.op != 'move' or prev.src.kind != 'pcindex':
            self.indirect.add(i.addr)
            return
        tbl = prev.src.addr
        base = i.src.addr
        # find the bound
        n = None
        a = prev.addr
        for _ in range(8):
            cands = [x for x in (a - 2, a - 4, a - 6, a - 8) if x in self.insns and
                     self.insns[x].addr + self.insns[x].length == a]
            if not cands:
                break
            a = cands[0]
            p = self.insns[a]
            if p.op == 'cmpi':
                n = p.src.value
                if p.size == 'b' and n & 0x80: n = None
                break
        if n is None or n > 512:
            self.indirect.add(i.addr)
            return
        targets = []
        for k in range(n + 1):
            off = self.mod.w(tbl - self.base + 2 * k)
            if off & 0x8000: off -= 0x10000
            t = base + off
            targets.append(t)
            self.add(t)
        self.jumptables[i.addr] = targets
        self.tables_data = getattr(self, 'tables_data', [])
        self.tables_data.append((tbl, 2 * (n + 1)))


def a5_jumptable(mod):
    """MPW-style jump table: idata = [tblsize, a5off, entries...]; entries are
    (dc.w seg, jmp abs.l) copied to a5+0x20.  Returns {a5 displacement: target}."""
    doff, b = mod.idata()
    tsize, a5off = struct.unpack('>II', b[0:8])
    tbl = {}
    for k in range((tsize - 0x20) // 8):
        e = b[8 + k * 8: 16 + k * 8]
        if len(e) < 8 or e[2:4] != bytes([0x4e, 0xf9]):
            continue
        tbl[0x20 + k * 8 + 2] = mod.base + struct.unpack('>I', e[4:8])[0]
    return tbl


def a6_jumptable(mod):
    """Jump table of 'jmp abs.l' stubs inside the initialized data, reached
    with 'jsr d16(a6)' where a6 = static storage + 0x8000.  A stub is accepted
    when its absolute address is patched by the code-relative irefs (so the
    loader relocates it to the module).  Returns {a6 displacement: target}."""
    doff, b = mod.idata()
    try:
        code_refs, _ = mod.irefs()
    except Exception:
        return {}
    tbl = {}
    for off in code_refs:
        p = off - doff - 2          # offset of the stub within idata
        if p < 0 or p + 6 > len(b) or b[p:p + 2] != bytes([0x4e, 0xf9]):
            continue
        off_t = struct.unpack('>I', b[p + 2:p + 6])[0]
        if not (0x48 <= off_t < mod.idataoff):
            continue        # unfilled stub (target 0): leave it to the runtime
        t = mod.base + off_t
        disp = doff + p - 0x8000
        if -0x8000 <= disp < 0x8000:
            tbl[disp] = t
    return tbl


def uses_a5_table(mod):
    """Heuristic: the initialized data is an MPW jump table (every entry a
    'dc.w seg; jmp abs.l' stub) as produced by Apple MPW / compatible
    compilers.  Microware C programs do not have one."""
    try:
        doff, b = mod.idata()
        tsize = struct.unpack('>I', b[0:4])[0]
    except Exception:
        return False
    n = (tsize - 0x20) // 8
    if n < 3 or 8 + n * 8 > len(b):
        return False
    return len(a5_jumptable(mod)) == n


def macsbug_names(mod, funcs):
    """Functions end with rts/jmp followed by a MacsBug symbol: byte 0x80+len, name."""
    import bisect
    d = mod.data
    fl = sorted(funcs)
    names = {}
    for o in range(0x48, mod.idataoff - 2):
        if d[o] & 0x80 and 0x82 <= d[o] <= 0x9f and o >= 2 and d[o-2:o] in (bytes([0x4e, 0x75]), bytes([0x4e, 0xd0]), bytes([0x4e, 0x73])):
            n = d[o] & 0x7f
            nm = d[o + 1:o + 1 + n]
            if all(32 < c < 127 for c in nm):
                a = mod.base + o
                k = bisect.bisect_right(fl, a) - 1
                if k >= 0 and fl[k] not in names:
                    names[fl[k]] = nm.decode()
    return names


def load(path=None, base=None):
    """load a module; defaults to the main program from game.json"""
    if path is None or base is None:
        import config
        m = config.load()['modules'][0]
        path = path or 'extracted/' + config.module_file(m)
        base = base if base is not None else m['base']
    data = open(path, 'rb').read()
    return Module(data, base)


def analyze_module(m):
    """Analysis for a game.json module entry"""
    import config
    mod = load('extracted/' + config.module_file(m), m['base'])
    return Analysis(mod, extra_entries=m['extra_entries'], a5=m['a5_table'], a6=m.get('a6_table', 'auto'))


def listing(an, out):
    mod = an.mod
    addrs = sorted(an.insns)
    prev_end = None
    for a in addrs:
        i = an.insns[a]
        if prev_end is not None and a != prev_end:
            out.write('\n; ---- gap %x-%x\n' % (prev_end, a) if a > prev_end else '; ---- overlap\n')
        if a in an.funcs:
            out.write('\n;================ func_%06x %s\n' % (a, an.names.get(a, '')))
        lab = 'L%06x:' % a if a in an.labels else ''
        raw = mod.data[a - an.base:a - an.base + i.length].hex()
        out.write('%-9s %06x  %-20s %s\n' % (lab, a, raw, i))
        prev_end = a + i.length


if __name__ == '__main__':
    # python tools/analyze.py [module index]  -> build/listing_<file>.asm
    import os
    import config
    cfg = config.load()
    idx = int(sys.argv[1]) if len(sys.argv) > 1 else 0
    m = cfg['modules'][idx]
    an = analyze_module(m)
    os.makedirs('build', exist_ok=True)
    out = 'build/listing_%s.asm' % config.module_file(m)
    with open(out, 'w') as f:
        listing(an, f)
    print('listing written to', out, '(jump table register: %s, %d entries)' % (an.jt_reg, len(an.a5table)))
    print('insns', len(an.insns), 'funcs', len(an.funcs), 'errors', len(an.errors), 'indirect', len(an.indirect))
    for a, e in sorted(an.errors.items())[:40]:
        print('err %x %s' % (a, e))
    print('jumptables', len(an.jumptables))
    extra = sorted(p for p in an.code_ptrs if p not in an.insns)
    print('lea/pea code ptrs not decoded:', ' '.join('%x' % x for x in extra))
    print('indirect:', ' '.join('%x' % x for x in sorted(an.indirect)))
    covered = sum(i.length for i in an.insns.values())
    print('code bytes covered %d of %d' % (covered, an.code_hi - an.code_lo))
