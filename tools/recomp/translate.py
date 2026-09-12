#!/usr/bin/env python3
"""Static x86 -> C recompiler for the supported Populous executable.

Reads the Ghidra listings in analysis/decompiled/D3DPopTB.exe/functions/*.asm
plus the PE itself and emits one C function `void fn_XXXXXXXX(X86 *c)` per
original function into build/recomp/gen/chunk_NNN.c, together with funcs.h
(prototypes) and table.c (sorted address table + recomp_call dispatch).

Usage:
    .venv/bin/python tools/recomp/translate.py [--out build/recomp/gen]
    .venv/bin/python tools/recomp/translate.py --only 00401000 00586000
    .venv/bin/python tools/recomp/translate.py --check-flags   # liveness study

The semantics live in tools/recomp/runtime/x86.h, which the generated code
includes. Instructions use general translations. The explicitly audited visual
clock reads below expose an identity-by-default runtime seam; timing changes
are enabled by the native host, never by the baseline/differential build.
"""

import argparse
import json
import os
import re
import sys
import time
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LISTINGS = os.path.join(ROOT, "analysis/decompiled/D3DPopTB.exe/functions")
FUNCS_TSV = os.path.join(ROOT, "analysis/decompiled/D3DPopTB.exe/functions.tsv")
BINARY = os.path.join(ROOT, "original/gog/D3DPopTB.exe")

FUNCS_PER_CHUNK = 200

# Reads of sprite_animation_counter that select a visual phase or blink.
# Do NOT include interpolation, simulation stamps, FPS measurement or input
# timing. The native host provides a separate elapsed-time animation clock.
# Each address is an instruction in the pinned D3DPopTB.exe .asm listing.
VISUAL_ANIMATION_READS = frozenset((
    0x468f27, 0x4690ad, 0x46922b, 0x469327,  # spell/particle textures
    0x4758b0, 0x4758c4, 0x4758d4,           # selection pulse
    0x525c05, 0x525c15,                     # rectangle pulse
    0x49d2eb, 0x49d386, 0x49d890, 0x4a2581, # UI blinking
    0x47a76d, 0x47a8f5,                     # palette animation
    0x50f45c,                              # effect sprite phase
))


def visual_animation_read(addr, body):
    if addr not in VISUAL_ANIMATION_READS:
        return body
    # Operate on the translated read, not the guest global: nested draws and
    # yielded guest threads must always see the original frame ID in memory.
    found = 0
    result = []
    pattern = r"rd(8|32)\(0x897981u\)"
    def replace(match):
        return "((uint%s_t)recomp_visual_animation_tick(rd32(0x897981u)))" % match[1]
    for line in body:
        line, count = re.subn(pattern, replace, line)
        found += count
        result.append(line)
    if found != 1:
        raise TranslateError("visual animation read %08x no longer matches its audited operand" % addr)
    return result

#: How a recovered block was found, and whether that is a structural fact or
#: a guess.
#:
#: A jump-table slot and an __initterm entry NAME the address: the program
#: itself will load and call it, so the block is established code and must
#: never be withdrawn.  If its callee cannot be resolved that is a translator
#: gap and has to fail the build.
#:
#: A data pointer and an instruction immediate are guesses.  Any dword that
#: happens to look like an address is a candidate, and three of the four
#: blocks that reach nowhere - 00540360, 00540500, 00d18bd0 - are exactly
#: that: they pass only the weak "16-aligned and decodes" gate.  A guess whose
#: own dispatch goes nowhere may be withdrawn, and every withdrawal is
#: reported with its provenance and the target that failed, so a real callback
#: with an unresolvable callee is visible rather than silently dropped.
STRUCTURAL_PROVENANCE = ("table", "initterm")


def note_structural(provenance, owner, t, why):
    """Record that a jump table or `__initterm` names `t` outright.

    Being named by one of those makes an address code by construction, whatever
    route it arrived by and whether or not it was already known.  That has to be
    recorded at the point of discovery: a target the decoder accepts because it
    is already an instruction boundary never reaches `unlisted_targets` (see
    `want_target`, which returns at its first check), so a block first found by
    a guess would keep its guessed provenance and stay prunable even after a
    table had named it.

    When the named address is an alternate entry into a body, the body is
    promoted too: pruning is keyed on the body's own address, so protecting the
    entry alone would still let the block it lives in be withdrawn out from
    under the table."""
    if t is None or why not in STRUCTURAL_PROVENANCE:
        return
    provenance[t] = why
    named = owner.get(t)
    if named is not None:
        provenance[named.addr] = why


# What makes an alternate entry a named place a mod may hook, as opposed to an
# internal block of the function it sits in.  A dword that merely reads as an
# instruction address is not on this list: the byte-wise scan finds those by
# the thousand and a coincidence is indistinguishable from a pointer, so the
# scan feeds dispatch recovery only.
HOOK_EVIDENCE = ("initterm", "reloc", "immediate", "curated")


def read_curated(path):
    """The curated globals file. Line-oriented on purpose: Python 3.9 has
    no tomllib, and this file is written to be read by twenty lines."""
    out, section = {}, None
    with open(path) as fh:
        for raw in fh:
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            if line.startswith("[") and line.endswith("]"):
                section = line[1:-1]
                out.setdefault(section, {})
                continue
            if "=" not in line or section is None:
                raise TranslateError("globals.toml: cannot parse %r" % raw)
            k, v = (t.strip() for t in line.split("=", 1))
            out[section][k] = v[1:-1] if v.startswith('"') else int(v, 0)
    return out


def hook_kind(addr, listed, alt_owner, provenance, evidence, intrinsics):
    """The symbol kind for `addr`, and whether a mod may hook it.

    Dispatch and eligibility are different questions.  Every entry in the
    three tables can be dispatched to, because the program reaches it; only a
    place the program *names* has a meaning stable enough to hook, and the
    exclusions come first:

      intrinsic     a substituted body (setjmp, longjmp).  Its address is an
                    implementation detail of this translator.
      block         a recovered block, or an alternate entry a jump table
                    names.  A jump-table target is an internal block of its
                    owner however else it is reached, so the table wins over
                    any pointer evidence.
      continuation  an alternate entry that only branch or fallthrough
                    discovery reached - a listing gap continued, not a name.

    Nothing here consults the order in which discovery ran: `evidence` is
    recorded wherever an address is named, whether or not that pass was the
    one that added it.
    """
    if addr in intrinsics:
        return "intrinsic", False
    # A recovered whole function can share a jump-table provenance. Explicit
    # entry curation requires a verified prologue/calling convention; ordinary
    # alternate curation still cannot promote internal jump-table blocks.
    if addr in listed or "curated_entry" in evidence.get(addr, ()):
        return "entry", True
    if addr not in alt_owner:
        return "block", False
    if provenance.get(addr) == "table":
        return "block", False
    if any(e in HOOK_EVIDENCE for e in evidence.get(addr, ())):
        return "alternate", True
    return "continuation", False


def prunable_blocks(recovered_addrs, provenance):
    """The recovered blocks the withdraw pass is allowed to drop: the guesses,
    never the ones a table or `__initterm` named."""
    return {a for a in recovered_addrs
            if provenance.get(a, "branch") not in STRUCTURAL_PROVENANCE}

#: The import-shim trampoline range from x86.h; a literal dispatch into it is
#: an import, not a missing function.
GUEST_SHIM_BASE = 0x0FF00000
GUEST_SHIM_END = 0x10000000

# Runtime intrinsics: guest addresses whose translated body is replaced by a
# call into src/recomp/runtime/intrinsics.h.
INTRINSIC_LONGJMP = 0x0055DB78          # _longjmp
INTRINSIC_SETJMP  = 0x0055DAFC          # __setjmp3, buffer at ESP+4
INTRINSIC_BODY = {
    INTRINSIC_LONGJMP: "recomp_longjmp(c);",
    INTRINSIC_SETJMP:  "recomp_setjmp(c);",   # single-call fallback, indirect only
}

# --------------------------------------------------------------- registers --

REG32 = ["EAX", "ECX", "EDX", "EBX", "ESP", "EBP", "ESI", "EDI"]
REG16 = ["AX", "CX", "DX", "BX", "SP", "BP", "SI", "DI"]
REG8L = ["AL", "CL", "DL", "BL"]
REG8H = ["AH", "CH", "DH", "BH"]

R_EAX, R_ECX, R_EDX, R_EBX, R_ESP, R_EBP, R_ESI, R_EDI = range(8)

ALL_FLAGS = frozenset(("cf", "zf", "sf", "of", "pf", "af"))
NO_FLAGS = frozenset()


class TranslateError(Exception):
    pass


def hexlit(v):
    return "0x%xu" % (v & 0xFFFFFFFF)


def mask_of(size):
    return {8: 0xFF, 16: 0xFFFF, 32: 0xFFFFFFFF}[size]


def utype(size):
    return {8: "uint8_t", 16: "uint16_t", 32: "uint32_t"}[size]


def stype(size):
    return {8: "int8_t", 16: "int16_t", 32: "int32_t"}[size]


# ---------------------------------------------------------------- operands --

class Op(object):
    __slots__ = ("kind", "size", "reg", "part", "imm",
                 "base", "index", "scale", "disp", "seg", "sti", "addr_c")

    def __init__(self, kind, **kw):
        self.kind = kind
        self.size = kw.get("size")
        self.reg = kw.get("reg")
        self.part = kw.get("part")
        self.imm = kw.get("imm")
        self.base = kw.get("base")
        self.index = kw.get("index")
        self.scale = kw.get("scale", 1)
        self.disp = kw.get("disp", 0)
        self.seg = kw.get("seg")
        self.sti = kw.get("sti")
        self.addr_c = kw.get("addr_c")   # pre-computed address expression

    def __repr__(self):
        return "Op(%s,%s)" % (self.kind, self.size)


PTR_SIZE = {"byte": 8, "word": 16, "dword": 32, "qword": 64,
            "float": 32, "double": 64, "extended double": 80}

MEM_RE = re.compile(
    r"^(?:(byte|word|dword|qword|float|double|extended double) ptr )?"
    r"(?:([CDEFGS]S):)?"
    r"\[([^\]]*)\]$")

IMM_RE = re.compile(r"^-?0x[0-9a-fA-F]+$|^-?[0-9]+$")
ST_RE = re.compile(r"^ST([0-7])$")


def parse_reg(text):
    """Return (index, size, part) or None."""
    if text in REG32:
        return (REG32.index(text), 32, None)
    if text in REG16:
        return (REG16.index(text), 16, None)
    if text in REG8L:
        return (REG8L.index(text), 8, "l")
    if text in REG8H:
        return (REG8H.index(text), 8, "h")
    return None


def parse_imm(text):
    if text.startswith("-"):
        return -int(text[1:], 0)
    return int(text, 0)


def parse_mem_expr(expr):
    """`EAX*0x4 + 0x1234` -> (base, index, scale, disp)."""
    base = index = None
    scale = 1
    disp = 0
    for term in [t.strip() for t in expr.split("+")]:
        if not term:
            continue
        if "*" in term:
            rname, sc = term.split("*", 1)
            r = parse_reg(rname.strip())
            if r is None or r[1] != 32:
                raise TranslateError("bad index register %r" % term)
            index, scale = r[0], parse_imm(sc.strip())
        else:
            r = parse_reg(term)
            if r is not None:
                if r[1] != 32:
                    raise TranslateError("bad base register %r" % term)
                if base is None:
                    base = r[0]
                elif index is None:
                    index, scale = r[0], 1
                else:
                    raise TranslateError("too many registers in %r" % expr)
            elif IMM_RE.match(term):
                disp += parse_imm(term)
            else:
                raise TranslateError("bad memory term %r" % term)
    return base, index, scale, disp


def parse_operand(text):
    text = text.strip()
    r = parse_reg(text)
    if r is not None:
        return Op("reg", reg=r[0], size=r[1], part=r[2])
    m = ST_RE.match(text)
    if m:
        return Op("st", sti=int(m.group(1)))
    if IMM_RE.match(text):
        return Op("imm", imm=parse_imm(text))
    m = MEM_RE.match(text)
    if m:
        sz = PTR_SIZE[m.group(1)] if m.group(1) else None
        base, index, scale, disp = parse_mem_expr(m.group(3))
        return Op("mem", size=sz, base=base, index=index, scale=scale,
                  disp=disp, seg=m.group(2))
    raise TranslateError("unparsed operand %r" % text)


# ------------------------------------------------------------ C generation --

def addr_expr(op):
    if op.addr_c is not None:
        return op.addr_c
    terms = []
    if op.seg == "FS":
        terms.append("c->fs_base")
    if op.base is not None:
        terms.append("c->r[%d]" % op.base)
    if op.index is not None:
        if op.scale == 1:
            terms.append("c->r[%d]" % op.index)
        else:
            terms.append("c->r[%d] * %du" % (op.index, op.scale))
    if op.disp or not terms:
        terms.append(hexlit(op.disp))
    if len(terms) == 1:
        return terms[0]
    return "(" + " + ".join(terms) + ")"


def reg_read(idx, size, part):
    if size == 32:
        return "c->r[%d]" % idx
    if size == 16:
        return "(uint16_t)c->r[%d]" % idx
    if part == "h":
        return "(uint8_t)(c->r[%d] >> 8)" % idx
    return "(uint8_t)c->r[%d]" % idx


def reg_write(idx, size, part, value):
    if size == 32:
        return "c->r[%d] = %s;" % (idx, value)
    if size == 16:
        return "c->r[%d] = (c->r[%d] & 0xffff0000u) | ((%s) & 0xffffu);" % (idx, idx, value)
    if part == "h":
        return ("c->r[%d] = (c->r[%d] & 0xffff00ffu) | (((%s) & 0xffu) << 8);"
                % (idx, idx, value))
    return "c->r[%d] = (c->r[%d] & 0xffffff00u) | ((%s) & 0xffu);" % (idx, idx, value)


def read_op(op, size):
    if op.kind == "reg":
        return reg_read(op.reg, op.size, op.part)
    if op.kind == "imm":
        return hexlit(op.imm) if size == 32 else "0x%xu" % (op.imm & mask_of(size))
    if op.kind == "mem":
        return "rd%d(%s)" % (size, addr_expr(op))
    raise TranslateError("cannot read operand %r" % op.kind)


def write_op(op, size, value):
    if op.kind == "reg":
        return reg_write(op.reg, op.size, op.part, value)
    if op.kind == "mem":
        return "wr%d(%s, (%s)(%s));" % (size, addr_expr(op), utype(size), value)
    raise TranslateError("cannot write operand %r" % op.kind)


def operand_size(ops, hint=None):
    """Infer the operation width from a register operand or an explicit ptr."""
    for o in ops:
        if o.kind == "reg":
            return o.size
    for o in ops:
        if o.kind == "mem" and o.size:
            return o.size
    if hint:
        return hint
    raise TranslateError("ambiguous operand size")


# ----------------------------------------------------------------- parsing --

LINE_RE = re.compile(r"^([0-9a-f]{8})  (\S+)(?: (.*))?$")


class Insn(object):
    __slots__ = ("addr", "mnem", "rep", "ops", "text", "raw")

    def __init__(self, addr, mnem, rep, opstrs, raw):
        self.addr = addr
        self.mnem = mnem
        self.rep = rep
        self.ops = opstrs
        self.raw = raw

    def __repr__(self):
        return "%08x %s %s" % (self.addr, self.mnem, ",".join(self.ops))


def parse_listing(path):
    with open(path) as fh:
        return parse_listing_text(fh.read())


def parse_listing_text(text):
    """Parse listing text.  Callers that build a listing in memory use this
    directly so they never have to write a temporary file."""
    insns = []
    if True:
        for line in text.splitlines():
            line = line.rstrip("\n")
            if not line.strip():
                continue
            m = LINE_RE.match(line)
            if not m:
                raise TranslateError("bad listing line %r" % line)
            addr = int(m.group(1), 16)
            mnem = m.group(2)
            rep = None
            if "." in mnem:
                mnem, rep = mnem.split(".", 1)
            opstr = m.group(3) or ""
            ops = [o for o in (x.strip() for x in opstr.split(",")) if o]
            insns.append(Insn(addr, mnem, rep, ops, line))
    insns.sort(key=lambda i: i.addr)
    return insns


# ------------------------------------------------------------------- flags --

COND = {
    "Z":  ("c->eflags_zf", ("zf",)),
    "NZ": ("!c->eflags_zf", ("zf",)),
    "C":  ("c->eflags_cf", ("cf",)),
    "B":  ("c->eflags_cf", ("cf",)),
    "NC": ("!c->eflags_cf", ("cf",)),
    "AE": ("!c->eflags_cf", ("cf",)),
    "A":  ("(!c->eflags_cf && !c->eflags_zf)", ("cf", "zf")),
    "BE": ("(c->eflags_cf || c->eflags_zf)", ("cf", "zf")),
    "S":  ("c->eflags_sf", ("sf",)),
    "NS": ("!c->eflags_sf", ("sf",)),
    "O":  ("c->eflags_of", ("of",)),
    "NO": ("!c->eflags_of", ("of",)),
    "P":  ("c->eflags_pf", ("pf",)),
    "NP": ("!c->eflags_pf", ("pf",)),
    "G":  ("(!c->eflags_zf && c->eflags_sf == c->eflags_of)", ("zf", "sf", "of")),
    "GE": ("c->eflags_sf == c->eflags_of", ("sf", "of")),
    "L":  ("c->eflags_sf != c->eflags_of", ("sf", "of")),
    "LE": ("(c->eflags_zf || c->eflags_sf != c->eflags_of)", ("zf", "sf", "of")),
}
# capstone spells several conditions differently from Ghidra
for _dst, _src in (("E", "Z"), ("NE", "NZ"), ("NAE", "C"), ("NB", "NC"),
                   ("NBE", "A"), ("NA", "BE"), ("NG", "LE"), ("NGE", "L"),
                   ("NL", "GE"), ("NLE", "G"), ("PE", "P"), ("PO", "NP")):
    COND[_dst] = COND[_src]

JCC = {"J" + k: v for k, v in COND.items()}
JCC["JECXZ"] = ("c->r[1] == 0", ())
SETCC = {"SET" + k: v for k, v in COND.items()}
CMOVCC = {"CMOV" + k: v for k, v in COND.items()}

ARITH_ALL = ALL_FLAGS
LOGIC_DEF = ALL_FLAGS           # AF is architecturally undefined; treat as killed
INCDEC_DEF = frozenset(("zf", "sf", "of", "pf", "af"))

#: A shift or rotate with a zero count leaves every flag untouched, so it only
#: kills flags when the count is a non-zero constant.  MAYDEF is what it writes
#: when it does shift, and drives the choice of the flag-setting helper.
SHIFT_MAYDEF = {
    "SHL": ALL_FLAGS, "SHR": ALL_FLAGS, "SAR": ALL_FLAGS,
    "SHLD": ALL_FLAGS, "SHRD": ALL_FLAGS,
    "ROL": frozenset(("cf", "of")), "ROR": frozenset(("cf", "of")),
    "RCL": frozenset(("cf", "of")), "RCR": frozenset(("cf", "of")),
}
SHIFT_COUNT_INDEX = {"SHL": 1, "SHR": 1, "SAR": 1, "ROL": 1, "ROR": 1,
                     "RCL": 1, "RCR": 1, "SHLD": 2, "SHRD": 2}

# mnemonic -> (defs, uses)
FLAG_EFFECT = {
    "ADD": (ARITH_ALL, NO_FLAGS),
    "SUB": (ARITH_ALL, NO_FLAGS),
    "CMP": (ARITH_ALL, NO_FLAGS),
    "NEG": (ARITH_ALL, NO_FLAGS),
    "ADC": (ARITH_ALL, frozenset(("cf",))),
    "SBB": (ARITH_ALL, frozenset(("cf",))),
    "AND": (LOGIC_DEF, NO_FLAGS),
    "OR": (LOGIC_DEF, NO_FLAGS),
    "XOR": (LOGIC_DEF, NO_FLAGS),
    "TEST": (LOGIC_DEF, NO_FLAGS),
    "INC": (INCDEC_DEF, NO_FLAGS),
    "DEC": (INCDEC_DEF, NO_FLAGS),
    "SHL": (ALL_FLAGS, NO_FLAGS),
    "SHR": (ALL_FLAGS, NO_FLAGS),
    "SAR": (ALL_FLAGS, NO_FLAGS),
    "SHLD": (ALL_FLAGS, NO_FLAGS),
    "SHRD": (ALL_FLAGS, NO_FLAGS),
    "ROL": (frozenset(("cf", "of")), NO_FLAGS),
    "ROR": (frozenset(("cf", "of")), NO_FLAGS),
    "RCL": (frozenset(("cf", "of")), frozenset(("cf",))),
    "RCR": (frozenset(("cf", "of")), frozenset(("cf",))),
    "MUL": (ALL_FLAGS, NO_FLAGS),
    "IMUL": (ALL_FLAGS, NO_FLAGS),
    "DIV": (ALL_FLAGS, NO_FLAGS),
    "IDIV": (ALL_FLAGS, NO_FLAGS),
    "BT": (frozenset(("cf",)), NO_FLAGS),
    "BTS": (frozenset(("cf",)), NO_FLAGS),
    "BSR": (frozenset(("zf",)), NO_FLAGS),
    "BSF": (frozenset(("zf",)), NO_FLAGS),
    "SAHF": (frozenset(("cf", "pf", "af", "zf", "sf")), NO_FLAGS),
    "POPFD": (ALL_FLAGS, NO_FLAGS),
    "PUSHFD": (NO_FLAGS, ALL_FLAGS),
    "SCASB": (ARITH_ALL, NO_FLAGS),
    "CMPSB": (ARITH_ALL, NO_FLAGS),
    "NOT": (NO_FLAGS, NO_FLAGS),
}


def shift_count_const(insn):
    """The shift count if it is a constant, else None (a CL count is unknown)."""
    idx = SHIFT_COUNT_INDEX[insn.mnem]
    if len(insn.ops) <= idx:
        return 1                      # the shift-by-one encoding
    try:
        o = parse_operand(insn.ops[idx])
    except TranslateError:
        return None
    return (o.imm & 31) if o.kind == "imm" else None


def flag_effect(insn):
    """(defs, uses) of x86 arithmetic flags, ignoring DF which is tracked apart.

    `defs` is the kill set for liveness, so it only lists flags the
    instruction writes on every path.  A shift or rotate whose count might be
    zero, and a REP-prefixed compare whose count might be zero, write nothing
    and must therefore not kill the incoming flags."""
    m = insn.mnem
    if m in JCC:
        return (NO_FLAGS, frozenset(JCC[m][1]))
    if m in SETCC:
        return (NO_FLAGS, frozenset(SETCC[m][1]))
    if m in CMOVCC:
        return (NO_FLAGS, frozenset(CMOVCC[m][1]))
    if insn.rep and m in ("SCASB", "SCASW", "SCASD", "CMPSB", "CMPSW", "CMPSD"):
        return (NO_FLAGS, NO_FLAGS)
    if m in SHIFT_MAYDEF:
        uses = frozenset(("cf",)) if m in ("RCL", "RCR") else NO_FLAGS
        cnt = shift_count_const(insn)
        if cnt is None or cnt == 0:
            return (NO_FLAGS, uses)
        return (SHIFT_MAYDEF[m], uses)
    return FLAG_EFFECT.get(m, (NO_FLAGS, NO_FLAGS))


# ------------------------------------------------------------------- image --

class Image(object):
    def __init__(self, path):
        import pefile
        pe = pefile.PE(path, fast_load=True)
        self.base = pe.OPTIONAL_HEADER.ImageBase
        self.size = pe.OPTIONAL_HEADER.SizeOfImage
        data = pe.get_memory_mapped_image()
        # get_memory_mapped_image() stops at the last section's raw end, which
        # is short of SizeOfImage; the architectural image runs to
        # base + SizeOfImage (0xd4c000 here) and those bytes read as zero.
        if len(data) < self.size:
            data = data + bytes(self.size - len(data))
        self.data = data[:self.size]
        self.end = self.base + self.size
        rel = pe.OPTIONAL_HEADER.DATA_DIRECTORY[5]      # IMAGE_DIRECTORY_BASERELOC
        self.reloc_dir = (rel.VirtualAddress, rel.Size)
        pe.close()
        # Section map: executable ranges are where code can live, initialized
        # data is where a pointer to it can be stored.  .reloc is the
        # relocation table and .rsrc is resource blobs; neither is data the
        # program dereferences, and both produce nothing but false matches.
        self.recover_errors = []
        self.string_candidates = set()
        self.thunk_candidates = set()
        self.weak_candidates = set()
        self.interior_candidates = set()
        self.exec_ranges = []
        self.data_ranges = []
        for sec in pe.sections:
            name = sec.Name.rstrip(b"\0").decode("ascii", "replace")
            lo = self.base + sec.VirtualAddress
            hi = lo + max(sec.Misc_VirtualSize, sec.SizeOfRawData)
            if sec.Characteristics & 0x20000000:
                self.exec_ranges.append((lo, hi, name))
            if (sec.Characteristics & 0x40) and name not in (".reloc", ".rsrc"):
                self.data_ranges.append((lo, hi, name))
        try:
            import capstone
            self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        except ImportError:            # length checking is optional
            self.md = None

    def relocated_pointers(self):
        """Every address named by a dword the loader rewrites, and where.

        A base relocation is the linker's own record that the four bytes at
        that location are an address, not data that happens to read as one.
        That is the difference between a pointer and a coincidence, and it is
        why this and not the byte-wise scan decides hook eligibility: the scan
        reads every offset in every data section, so any dword whose value
        lands on an instruction boundary passes it.

        HIGHLOW (type 3) is the only relocation a 32-bit x86 image uses for a
        full address; ABSOLUTE (type 0) is block padding and names nothing.
        """
        rva, size = self.reloc_dir
        out = {}
        if not rva or not size:
            return out
        off, end = rva, rva + size
        while off + 8 <= end:
            page = int.from_bytes(self.data[off:off + 4], "little")
            blk = int.from_bytes(self.data[off + 4:off + 8], "little")
            if blk < 8 or off + blk > end:
                break
            for k in range(off + 8, off + blk, 2):
                e = int.from_bytes(self.data[k:k + 2], "little")
                if e >> 12 != 3:
                    continue
                site = page + (e & 0xfff)
                if site + 4 > self.size:
                    continue
                va = int.from_bytes(self.data[site:site + 4], "little")
                out.setdefault(va, self.base + site)
            off += blk
        return out

    def is_exec(self, va):
        return any(lo <= va < hi for lo, hi, _ in self.exec_ranges)

    def looks_like_function(self, va):
        """Does `va` look like the start of an MSVC function?

        Three signals, all of which the compiler's own output satisfies: the
        address is 16-byte aligned, the byte before it is padding or the end
        of the previous function, and it decodes to a real instruction."""
        if self.md is None or va % 16 or not self.is_exec(va):
            return False
        if va <= self.base or self.data[va - self.base - 1] not in (0xCC, 0x90, 0xC3):
            return False
        got = list(self.md.disasm(self.data[va - self.base:va - self.base + 16], va, count=1))
        return bool(got) and got[0].mnemonic not in ("int3", "hlt", "(bad)")

    def looks_like_code_start(self, va):
        """A weaker signal for an address something in the image points AT.

        The pointer is itself the evidence that the address matters; all this
        adds is that it could be the start of a function.  The padding test
        `looks_like_function` applies is too strict for a pointed-to address,
        because it only recognises `RET`, `NOP` and `int3` as the end of the
        previous function: `005781c0` follows a `ret 8` (`c2 08 00`) and so was
        rejected, even though a vtable slot at `005729d6` names it.  What keeps
        this honest is that every recovered block still has to translate.
        """
        if self.md is None or va % 16 or not self.is_exec(va):
            return False
        got = list(self.md.disasm(self.data[va - self.base:va - self.base + 16],
                                  va, count=1))
        return bool(got) and got[0].mnemonic not in ("int3", "hlt", "(bad)")

    #: A thunk is short and ends by transferring control somewhere else.
    THUNK_LIMIT = 8

    def looks_like_thunk(self, va):
        """Does `va` look like a thunk?

        Thunks are neither 16-byte aligned nor preceded by padding - they are
        packed one after another - so the function-start signals miss them.
        What they do have is shape: a handful of instructions that all decode
        and convert, ending in a jump or a return."""
        if self.md is None or not self.is_exec(va):
            return False
        pos = va - self.base
        got = list(self.md.disasm(self.data[pos:pos + 16 * self.THUNK_LIMIT], va,
                                  count=self.THUNK_LIMIT))
        if not got or got[0].mnemonic in ("int3", "hlt", "(bad)"):
            return False
        for ci in got:
            if ci.mnemonic in ("int3", "hlt", "(bad)"):
                return False
            if ci.mnemonic in ("ret", "retn"):
                return True
            if ci.mnemonic in ("jmp", "call"):
                # A thunk jumps somewhere real.  Without this, the three-byte
                # NOP padding `8d 49 00` that MSVC puts between functions
                # decodes at its last byte as a lone JMP to a wild address and
                # is taken for a thunk: 0040fc44 jmp 0xf5413d44.
                if ci.op_str.startswith("0x"):
                    return self.is_exec(int(ci.op_str, 16))
                return True          # indirect: the target is not static
        return False

    @staticmethod
    def _looks_like_string_tail(blob, off):
        """Is the dword at `off` the last four bytes of a C string?

        Strings are the one systematic source of in-range values that is not a
        pointer at all, because a NUL terminator supplies the 0x00 top byte an
        image address needs.  But three printable bytes and a NUL is far too
        weak on its own: an address in this image has 0x00 on top by
        construction and 0x40..0x58 in the next byte, every one of which is
        printable, so the test fired for any pointer whose low two bytes
        happened to be printable as well - about one .text pointer in seven.
        It cost 21 of the 113 CRT static initializers.

        A real string tail has text running into it, so the byte before the
        dword has to be printable too.  A pointer stored after a NUL, a count
        or any other non-text byte is no longer mistaken for one."""
        if off < 1:
            return False
        quad = blob[off:off + 4]
        return (len(quad) == 4 and quad[3] == 0
                and all(0x20 <= b < 0x7F for b in quad[:3])
                and 0x20 <= blob[off - 1] < 0x7F)

    #: The body of the CRT's __initterm (0055d6a0 here): it walks an array of
    #: function pointers between its two arguments and calls each non-null
    #: one.  Matching the shape rather than the address keeps this from
    #: depending on one build's layout.
    INITTERM_BODY = ("CALL EAX", "ADD ESI,0x4")

    def initterm_tables(self, functions):
        """Ranges passed to __initterm, and every function pointer in them.

        These are the C++ static initializers.  Nothing points at the table
        entries except the table itself, and nothing points at the functions
        except those entries, so they are invisible to a cross-reference and
        Ghidra lists none of them.  The call site names the range outright -
        `PUSH end; PUSH start; CALL __initterm` - so the entries are entry
        points by construction and are taken ahead of every heuristic: no
        alignment, padding, string or shape test gets a say.
        """
        walkers = set()
        for fn in functions:
            text = {ins.mnem + " " + ",".join(ins.ops) for ins in fn.insns}
            if all(sig in text for sig in self.INITTERM_BODY) and \
                    any(i.mnem == "MOV" and i.ops[1:] == ["dword ptr [ESI]"]
                        for i in fn.insns):
                walkers.add(fn.addr)
        if not walkers:
            return [], set()

        ranges, entries = [], set()
        for fn in functions:
            for i, ins in enumerate(fn.insns):
                if ins.mnem != "CALL" or not ins.ops:
                    continue
                if not ins.ops[0].startswith("0x") or int(ins.ops[0], 16) not in walkers:
                    continue
                if i < 2 or fn.insns[i - 1].mnem != "PUSH" or fn.insns[i - 2].mnem != "PUSH":
                    continue
                try:            # cdecl: the push nearest the call is the first arg
                    start = parse_operand(fn.insns[i - 1].ops[0])
                    end = parse_operand(fn.insns[i - 2].ops[0])
                except TranslateError:
                    continue
                if start.kind != "imm" or end.kind != "imm":
                    continue
                lo, hi = start.imm, end.imm
                if not (lo < hi and hi - lo <= 0x100000 and lo % 4 == 0):
                    continue
                if not any(a <= lo and hi <= b for a, b, _ in self.data_ranges):
                    continue
                ranges.append((lo, hi, ins.addr))
                for va in range(lo, hi, 4):
                    target = self.rd32(va)
                    if target and self.is_exec(target):
                        entries.add(target)
        return ranges, entries

    def code_pointers(self, covered, interior_bytes=None, exclude=()):
        """Dwords stored anywhere in the image that name executable addresses.

        Returns (starts, interior).  `starts` are addresses nothing has
        translated that look like function entries or thunks; `interior` are
        addresses that already are instruction boundaries inside a translated
        function, which need an alternate entry so a call through the pointer
        can be dispatched rather than aborting.

        The order of the tests matters.  A known instruction boundary is
        accepted first, whatever its bytes look like: rejecting it for
        resembling text would throw away a pointer whose destination is
        already proven to be code.  An address that falls strictly inside a
        translated instruction is rejected outright, because it is not a place
        execution can begin.

        Ghidra's function list is not complete: `0049e800` is reached only
        through `CALL EAX` after the pointer is loaded from a packed struct in
        .data, so no cross-reference names it and no listing was produced.  The
        pointers are not 4-aligned - the array's stride is 0x42 - so the scan
        looks at every byte offset.
        """
        starts, interior = set(), set()
        skip = list(exclude)

        def excluded(va):
            return any(lo <= va < hi for lo, hi in skip)

        for lo, hi, _ in self.data_ranges + self.exec_ranges:
            blob = self.data[lo - self.base:hi - self.base]
            # len(blob) - 3 so the section's final dword is examined too.
            for off in range(len(blob) - 3):
                quad = blob[off:off + 4]
                va = int.from_bytes(quad, "little")
                if va in starts or va in interior:
                    continue
                # Skip both a pointer stored inside decoded table storage and
                # a candidate whose value points at table storage: 004784f0 is
                # the four-entry table behind `JMP [EAX*4 + 0x4784f0]` at
                # 004783e2, it is 16-aligned, and the byte before it is the
                # `RET` ending the previous function, so every static signal
                # says function start.
                if not self.is_exec(va):
                    continue
                if va in covered:
                    interior.add(va)          # a proven instruction boundary
                    continue
                if excluded(va) or excluded(lo + off):
                    continue
                if interior_bytes is not None and interior_bytes[va - self.base]:
                    self.interior_candidates.add(va)
                    continue                  # inside an instruction, not a start
                if self._looks_like_string_tail(blob, off):
                    self.string_candidates.add(va)
                    continue
                if self.looks_like_function(va):
                    starts.add(va)
                elif self.looks_like_thunk(va):
                    self.thunk_candidates.add(va)
                    starts.add(va)
                elif self.looks_like_code_start(va):
                    self.weak_candidates.add(va)
                    starts.add(va)
        return starts, interior


    def rd8(self, va):
        if not (self.base <= va < self.end):
            return None
        return self.data[va - self.base]

    def rd32(self, va):
        if not (self.base <= va and va + 4 <= self.end):
            return None
        o = va - self.base
        return int.from_bytes(self.data[o:o + 4], "little")

    #: x87 mnemonics whose memory operand is an integer or a control/status
    #: word rather than a float, so its width names itself (`word ptr`) instead
    #: of being spelled `float`/`double`/`extended double`.
    X87_INT = frozenset(("FILD", "FIST", "FISTP", "FIADD", "FISUB", "FISUBR",
                         "FIMUL", "FIDIV", "FIDIVR", "FICOM", "FICOMP",
                         "FNSTSW", "FSTSW", "FNSTCW", "FSTCW", "FLDCW",
                         "FNSTENV", "FLDENV", "FNSAVE", "FRSTOR"))
    REP_PREFIX = {"rep": "REP", "repe": "REPE", "repz": "REPE",
                  "repne": "REPNE", "repnz": "REPNE"}
    #: Mnemonics capstone spells differently from the listing grammar.
    MNEM_ALIAS = {"POPAL": "POPAD", "PUSHAL": "PUSHAD",
                  "POPFL": "POPFD", "PUSHFL": "PUSHFD",
                  "CWTL": "CWDE", "CLTD": "CDQ", "CBTW": "CBW",
                  "IRETD": "IRET"}

    def _size_word(self, nbytes, mnem):
        if mnem.startswith("F") and mnem not in self.X87_INT:
            return {4: "float", 8: "double", 10: "extended double"}.get(nbytes)
        return {1: "byte", 2: "word", 4: "dword", 8: "qword",
                10: "extended double"}.get(nbytes)

    def _op_text(self, ci, op, mnem):
        import capstone.x86_const as X
        if op.type == X.X86_OP_REG:
            name = ci.reg_name(op.reg).upper()
            if name.startswith("ST(") and name.endswith(")"):
                return "ST" + name[3:-1]        # capstone st(1) -> ST1
            return name
        if op.type == X.X86_OP_IMM:
            return "-0x%x" % -op.imm if op.imm < 0 else "0x%x" % op.imm
        if op.type == X.X86_OP_MEM:
            word = self._size_word(op.size, mnem)
            if word is None:
                raise TranslateError("unknown operand width %d" % op.size)
            parts = []
            if op.mem.base:
                parts.append(ci.reg_name(op.mem.base).upper())
            if op.mem.index:
                parts.append("%s*0x%x" % (ci.reg_name(op.mem.index).upper(), op.mem.scale))
            if op.mem.disp or not parts:
                parts.append("-0x%x" % -op.mem.disp if op.mem.disp < 0
                             else "0x%x" % op.mem.disp)
            inner = " + ".join(parts)
            seg = ci.reg_name(op.mem.segment).upper() if op.mem.segment else ""
            return "%s ptr %s[%s]" % (word, seg + ":" if seg else "", inner)
        raise TranslateError("unknown capstone operand type %d" % op.type)

    def to_insn(self, ci):
        """Build a listing Insn from a capstone instruction."""
        words = ci.mnemonic.split()
        rep = None
        if words[0] in self.REP_PREFIX:
            rep = self.REP_PREFIX[words[0]]
            words = words[1:]
        mnem = words[-1].upper()
        mnem = self.MNEM_ALIAS.get(mnem, mnem)
        ops = [self._op_text(ci, o, mnem) for o in ci.operands]
        if rep or mnem in Translator.STRING_MNEM:
            ops = []            # implicit ES:EDI / ESI operands carry nothing
        text = "%08x  %s%s%s" % (ci.address, mnem, "." + rep if rep else "",
                                 (" " + ",".join(ops)) if ops else "")
        return Insn(ci.address, mnem, rep, ops, text)

    #: How far a linear sweep may run before giving up.  Large enough that no
    #: real function reaches it - the longest in this binary is 4,058
    #: instructions - so exhausting it is reported as an error rather than
    #: quietly deciding the address was data.
    RECOVER_LIMIT = 65536

    #: How far from the entry a recovered block may follow a branch.  The
    #: largest function in this binary is 14,816 bytes, so 32 KB covers a
    #: whole one without letting a far jump merge two unrelated regions:
    #: 00430ad2 was reaching back 60 KB to 0041fc70.
    RECOVER_WINDOW = 0x8000

    def recover(self, start, listed, limit=None):
        """Decode the block at `start`, following its branches.

        Recursive descent rather than a linear sweep.  A sweep stops at the
        first RET, which cuts a real function into pieces at every early
        return; each piece then has to be discovered and validated on its own,
        and one piece failing takes out everything that branches to it.  That
        is what dropped 0053fd30, a perfectly ordinary function, because a
        fragment several branches away decoded badly.

        Following the branches keeps a function whole: its internal targets
        become labels inside one block.  Targets outside the window, or into
        code already translated, are left for the discovery loop to resolve as
        separate entries, exactly as before.
        """
        if limit is None:
            limit = self.RECOVER_LIMIT
        if self.md is None or not (self.base <= start < self.end):
            return []
        self.md.detail = True
        seen = {}
        pending = [start]
        truncated = False
        while pending and len(seen) < limit:
            va = pending.pop()
            while len(seen) < limit:
                if va in seen or va in listed or not (self.base <= va < self.end):
                    break
                got = list(self.md.disasm(self.data[va - self.base:va - self.base + 16],
                                          va, count=1))
                if not got:
                    break
                ci = got[0]
                if ci.mnemonic in ("int3", "hlt", "(bad)"):
                    break
                try:
                    seen[va] = self.to_insn(ci)
                except TranslateError as e:
                    self.recover_errors.append(
                        "%08x: recovery abandoned at %08x: %s" % (start, ci.address, e))
                    self.md.detail = False
                    return []
                nxt = ci.address + ci.size
                if ci.op_str.startswith("0x") and (
                        ci.mnemonic == "jmp" or ci.mnemonic.startswith("j")):
                    t = int(ci.op_str, 16)
                    if (abs(t - start) <= self.RECOVER_WINDOW and t not in listed
                            and self.is_exec(t)):
                        pending.append(t)
                if ci.mnemonic in ("ret", "retn", "jmp"):
                    break
                va = nxt
            else:
                truncated = True
        self.md.detail = False
        if truncated or len(seen) >= limit:
            self.recover_errors.append(
                "%08x: recovery hit the %d-instruction budget; block not emitted"
                % (start, limit))
            return []
        return [seen[a] for a in sorted(seen)]

    def insn_end(self, va, mnem):
        """Address just past the instruction at `va`, or None if unknown.

        The Ghidra listing folds a WAIT prefix into the following x87
        mnemonic; capstone reports the two separately, so skip a leading
        WAIT when the listing calls the instruction something else.
        """
        if self.md is None or not (self.base <= va and va + 16 <= self.end):
            return None
        o = va - self.base
        got = list(self.md.disasm(self.data[o:o + 16], va, count=2))
        if not got:
            return None
        if (got[0].mnemonic in ("wait", "fwait") and mnem != "WAIT"
                and mnem.startswith("F")):
            return got[1].address + got[1].size if len(got) > 1 else None
        return got[0].address + got[0].size


# -------------------------------------------------------------- translator --

class Function(object):
    def __init__(self, addr, name, size, insns):
        self.addr = addr
        self.name = name
        self.size = size
        self.insns = insns
        self.addrs = {i.addr for i in insns}
        self.end = max(addr + size, insns[-1].addr + 1) if insns else addr
        # filled in by measure(): contiguous[i] is True when insn i+1 in the
        # listing really is insn i's fall-through successor.
        self.contiguous = [True] * len(insns)
        self.fallthrough = [None] * len(insns)

    def measure(self, image):
        """Find listing gaps: places where the next listed instruction is not
        where the current one actually ends."""
        for i, ins in enumerate(self.insns):
            e = image.insn_end(ins.addr, ins.mnem)
            self.fallthrough[i] = e
            nxt = self.insns[i + 1].addr if i + 1 < len(self.insns) else self.end
            if e is not None and e != nxt:
                self.contiguous[i] = False


TERMINATORS = frozenset(("RET", "JMP"))


class Translator(object):
    def __init__(self, image, func_addrs, opts):
        self.image = image
        self.func_addrs = func_addrs      # set of all known function entry points
        self.opts = opts
        self.stats = defaultdict(int)
        self.notes = []
        self.jumptables = {}
        self.unlisted_targets = set()
        self.all_insn_addrs = set()
        #: byte ranges holding decoded jump tables; an address inside one is
        #: table storage, not code, however much it looks like a function.
        self.table_ranges = set()
        #: Every table base seen, so one table's extent can be capped by the
        #: next one's start even before that table has been decoded.
        self.table_bases = set()
        #: alternate entries that turned out not to be in their block
        self.stale_entries = set()
        #: (function, jmp address) -> table base, for every constant-
        #: displacement indirect jump, decoded or not
        self.table_sites = {}
        #: sites whose extent is inferred rather than derived from a bound
        self.table_sites_inferred = set()
        self.jmp_index = 0
        self.strict = False

    # -- control flow ------------------------------------------------------

    def successors(self, fn, i):
        """Indices reachable from insn i, and whether flags escape the function."""
        ins = fn.insns[i]
        m = ins.mnem
        nxt = i + 1 if (i + 1 < len(fn.insns) and fn.contiguous[i]) else None
        if m == "RET":
            return []
        if m in JCC:
            out = [nxt] if nxt is not None else []
            t = self.branch_target(ins)
            if t is not None and t in fn.index:
                out.append(fn.index[t])
            return out
        if m == "JMP":
            if ins.ops and ins.ops[0].startswith("0x"):
                t = int(ins.ops[0], 16)
                return [fn.index[t]] if t in fn.index else []
            tgts = self.jumptables.get((fn.addr, ins.addr), [])
            return [fn.index[t] for t in tgts if t in fn.index]
        return [nxt] if nxt is not None else []

    @staticmethod
    def branch_target(ins):
        if ins.ops and ins.ops[0].startswith("0x"):
            return int(ins.ops[0], 16)
        return None

    #: instructions that end a basic block; flags are live across all of them
    BOUNDARY = frozenset(("CALL", "RET", "JMP"))

    def liveness(self, fn):
        """Flag liveness within straight-line code only.

        Plan correction 6: flags are live at every control-flow boundary, so a
        CALL, RET, JMP or conditional jump has all six live on the way out and
        elimination only happens between them.  What this still removes is a
        flag write that a later instruction in the same run overwrites before
        anything reads it, which is most of them.
        """
        n = len(fn.insns)
        live_out = [ALL_FLAGS] * n
        for i in range(n - 2, -1, -1):
            ins = fn.insns[i]
            # A listing gap is a control-flow boundary too: emission inserts a
            # goto or a tail call there, so the next listed instruction is not
            # the fall-through successor.
            if (ins.mnem in self.BOUNDARY or ins.mnem in JCC
                    or not fn.contiguous[i]):
                live_out[i] = ALL_FLAGS
                continue
            d, u = flag_effect(fn.insns[i + 1])
            live_out[i] = (live_out[i + 1] - d) | u
        return live_out

    def liveness_cfg(self, fn, call_transparent=False):
        """CFG-wide liveness, used only by --check-flags to measure how often
        a flag would cross a call or return boundary."""
        n = len(fn.insns)
        live_in = [NO_FLAGS] * n
        live_out = [NO_FLAGS] * n
        succ = [self.successors(fn, i) for i in range(n)]
        changed = True
        while changed:
            changed = False
            for i in range(n - 1, -1, -1):
                ins = fn.insns[i]
                lo = NO_FLAGS
                for s in succ[i]:
                    lo = lo | live_in[s]
                if ins.mnem == "CALL" and not call_transparent:
                    li = NO_FLAGS
                elif ins.mnem == "CALL":
                    li = lo
                else:
                    d, u = flag_effect(ins)
                    li = (lo - d) | u
                if li != live_in[i] or lo != live_out[i]:
                    live_in[i], live_out[i] = li, lo
                    changed = True
        return live_in, live_out

    # -- jump tables -------------------------------------------------------

    def decode_jumptable(self, fn, i):
        """Decode the switch behind `JMP dword ptr [reg*4 + base]`.

        Plan correction 8.  The bound comes from dataflow on the index
        register, which is not always the register the JMP indexes with: the
        common MSVC shape bounds one register with `CMP r,N` + `JA`, remaps it
        through a byte table, and indexes the dword table with the byte.  Both
        shapes are decoded exactly; only when neither is recognised does this
        fall back to reading entries until one stops being an instruction
        boundary.  In a bounded table an entry that is not an instruction
        boundary is an error, not a place to stop.
        """
        ins = fn.insns[i]
        try:
            op = parse_operand(ins.ops[0])
        except TranslateError:
            return None
        if op.kind != "mem" or op.index is None or op.scale != 4 or not op.disp:
            return None
        # Every indirect jump through a constant displacement is a table read,
        # recorded whether or not the decode below succeeds: a site that
        # decodes nothing at all is the worst kind of gap and would otherwise
        # be invisible to the coverage check.
        self.table_sites[(fn.addr, ins.addr)] = op.disp
        if op.base is not None:
            self.table_bases.add(op.disp)
            return self.two_index_table(fn, i, ins, op)
        dword_base = op.disp

        self.jmp_index = i
        kind, info = self.index_source(fn, i, op.index)
        if kind == "byte_table":
            byte_base, count = info
            targets = []
            for k in range(count):
                b = self.image.rd8(byte_base + k)
                if b is None:
                    raise TranslateError("jump table at %08x: byte table runs off "
                                         "the image at %08x" % (ins.addr, byte_base + k))
                t = self.image.rd32(dword_base + 4 * b)
                self.want_target(fn, ins, t, k)
                targets.append(t)
            self.table_ranges.add((byte_base, byte_base + count))
            self.table_ranges.add((dword_base, dword_base + 4 * (max(
                self.image.rd8(byte_base + k) for k in range(count)) + 1)))
            self.stats["_jmp_table_two_level"] += 1
            return targets
        if kind == "bounded":
            targets = []
            for k in range(info):
                t = self.image.rd32(dword_base + 4 * k)
                self.want_target(fn, ins, t, k)
                targets.append(t)
            self.table_ranges.add((dword_base, dword_base + 4 * info))
            self.stats["_jmp_table_bounded"] += 1
            return targets

        # No bound recovered.  Read while the entries stay plausible code and
        # hand each one to the recovery machinery rather than stopping at the
        # first that is not yet a block entry: the FMV decoder's table at
        # 00d184b0 has an unrecovered address in slot 0, so stopping there
        # dropped the whole table.  The extent is capped by the next known
        # table's base.
        self.stats["_jmp_table_unbounded"] += 1
        self.table_sites_inferred.add((fn.addr, ins.addr))
        targets = []
        for k in range(4096):
            at = dword_base + 4 * k
            t = self.image.rd32(at)
            if t is None or t == 0:
                break
            if not self.is_block_entry(fn, t) and not self.plausible_code(t):
                break
            self.want_target(fn, ins, t, k, strict=False)
            targets.append(t)
        if targets:
            self.table_ranges.add((dword_base, dword_base + 4 * len(targets)))
        return targets or None

    def plausible_code(self, va):
        """Could `va` be an address in this program's code?"""
        return self.image.is_exec(va) and self.image.looks_like_code_start(va) or (
            self.image.is_exec(va) and self.image.md is not None
            and bool(list(self.image.md.disasm(
                self.image.data[va - self.image.base:va - self.image.base + 16],
                va, count=1))))

    #: How many values a byte can take at the jump, given the guards in front
    #: of it.  `OR CL,CL; JS` sends the negative half elsewhere.
    BYTE_ROWS = 256
    BYTE_ROWS_SIGNED = 128

    def two_index_table(self, fn, i, ins, op):
        """`JMP dword ptr [row + col*4 + base]`: a two-dimensional table.

        The hand-written decoders at 00588917 and 0058ac4f dispatch on a byte
        and a two-bit phase at once:

            MOV CL,byte ptr [ESI] / OR CL,CL / JS ... / JE ...
            MOV EAX,ECX / SHL EAX,4 / MOV EBP,EDX / AND EBP,3
            JMP dword ptr [EAX + EBP*4 + 0x58d368]

        There is no bounding CMP, so the extent comes from the shapes instead:
        the `SHL` by k makes the row stride 1 << k bytes, its operand is a
        byte so there are at most 256 rows and the `JS` halves that, and the
        `AND` with m leaves m+1 columns.  A zero entry is a hole rather than
        the end - row 0 is all zeros here, because the `JE` diverts the zero
        byte before the jump is reached.
        """
        rows = self.shift_rows(fn, i, op.base)
        cols = self.and_columns(fn, i, op.index)
        if not rows or not cols:
            return None
        stride, count = rows
        # The extent is exactly what the geometry proves: rows the guard makes
        # reachable, times the masked column count.  The next table's base is
        # an upper bound on where this one could end, not evidence that the
        # space between is table storage, and treating it as evidence both
        # swallows intervening data as targets and hides it from the pointer
        # scan.
        targets, holes = [], 0
        for r in range(count):
            for col in range(cols):
                at = op.disp + r * stride + col * op.scale
                t = self.image.rd32(at)
                if t is None:
                    break
                if t == 0:
                    holes += 1
                    continue
                self.want_target(fn, ins, t, r * cols + col, strict=False)
                targets.append(t)
        if not targets:
            return None
        self.table_ranges.add((op.disp, op.disp + count * stride))
        self.table_sites_inferred.add((fn.addr, ins.addr))
        self.stats["_jmp_table_two_index"] += 1
        self.stats["_jmp_table_holes"] += holes
        return targets

    def shift_rows(self, fn, i, reg):
        """(row stride in bytes, row count) from the `SHL reg,k` feeding it."""
        signed_guard = False
        for j in range(i - 1, max(-1, i - 20), -1):
            ins = fn.insns[j]
            if ins.mnem == "JS":
                signed_guard = True
            if ins.mnem != "SHL" or len(ins.ops) != 2:
                continue
            try:
                d, k = parse_operand(ins.ops[0]), parse_operand(ins.ops[1])
            except TranslateError:
                return None
            if d.kind != "reg" or d.reg != reg or k.kind != "imm":
                continue
            if not (0 < k.imm < 16):
                return None
            # Look a little further back for the sign guard on the byte.
            for j2 in range(j - 1, max(-1, j - 12), -1):
                if fn.insns[j2].mnem == "JS":
                    signed_guard = True
            rows = self.BYTE_ROWS_SIGNED if signed_guard else self.BYTE_ROWS
            return (1 << k.imm, rows)
        return None

    def and_columns(self, fn, i, reg):
        """Column count from the `AND reg,m` masking the index."""
        for j in range(i - 1, max(-1, i - 20), -1):
            ins = fn.insns[j]
            if ins.mnem != "AND" or len(ins.ops) != 2:
                continue
            try:
                d, m = parse_operand(ins.ops[0]), parse_operand(ins.ops[1])
            except TranslateError:
                return None
            if d.kind != "reg" or d.reg != reg or m.kind != "imm":
                continue
            if not (0 < m.imm < 256) or (m.imm & (m.imm + 1)):
                return None          # not a low-bit mask
            return m.imm + 1
        return None

    def is_block_entry(self, fn, target):
        """A jump-table target is usable if it is an instruction in this
        function, a known entry point, or an instruction inside another
        function (which then gets its own alternate entry)."""
        return (target in fn.addrs or target in self.func_addrs
                or target in self.all_insn_addrs)

    def want_target(self, fn, ins, target, k, strict=None):
        """Validate a jump-table entry, deferring recovery on pass 1."""
        if target is not None and self.is_block_entry(fn, target):
            return
        if strict is False:
            if target is not None and self.image.is_exec(target):
                self.unlisted_targets.add(target)
            return
        # Ghidra's recorded function size is sometimes far short of the real
        # body, so bound recovery by a generous window past the entry rather
        # than by fn.end.
        if (not self.strict and target is not None
                and fn.addr <= target < fn.addr + 0x10000):
            self.unlisted_targets.add(target)
            return
        raise TranslateError(
            "jump table at %08x: entry %d -> %08x is not an instruction boundary"
            % (ins.addr, k, target or 0))

    def index_source(self, fn, i, idx_reg):
        """How the JMP's index register was produced.

        ("bounded", n)             `CMP idx,n-1` + `JA` guards it directly
        ("byte_table", (base, n))  idx was loaded from `[other + base]` as a
                                   byte, with `other` bounded to n values
        (None, None)               neither
        """
        for j in range(i - 1, max(-1, i - 16), -1):
            ins = fn.insns[j]
            if ins.mnem == "CMP" and len(ins.ops) == 2:
                b = self.cmp_bound_here(fn, j, idx_reg)
                return ("bounded", b) if b is not None else (None, None)
            if not self.writes_reg32(ins, idx_reg):
                continue
            if ins.mnem in ("MOV", "MOVZX", "MOVSX") and len(ins.ops) == 2:
                try:
                    dst, src = parse_operand(ins.ops[0]), parse_operand(ins.ops[1])
                except TranslateError:
                    return (None, None)
                if (dst.kind == "reg" and dst.reg == idx_reg and src.kind == "mem"
                        and src.size == 8 and src.index is None
                        and src.base is not None and src.disp):
                    inner = self.index_source(fn, j, src.base)
                    if inner[0] == "bounded":
                        return ("byte_table", (src.disp, inner[1]))
            return (None, None)
        return (None, None)

    #: How many index values reach the table, given `CMP idx,N` and the guard
    #: that follows it.  Only a guard that branches AWAY on out-of-range values
    #: bounds the fall-through path the JMP sits on: JA leaves 0..N (N+1
    #: values) and JAE/JNC leave 0..N-1 (N).  JBE/JB/JC/JNA branch away on
    #: in-range values, so reaching the JMP by falling through them means the
    #: index is out of range and the compare bounds nothing.
    GUARD_BOUND = {"JA": 1, "JNBE": 1, "JAE": 0, "JNC": 0, "JNB": 0}

    def cmp_bound_here(self, fn, j, idx_reg):
        """`CMP idx,N` guarded by an away-branching JA/JAE -> reachable count."""
        ins = fn.insns[j]
        try:
            a, b = parse_operand(ins.ops[0]), parse_operand(ins.ops[1])
        except TranslateError:
            return None
        if not (a.kind == "reg" and a.reg == idx_reg and b.kind == "imm"):
            return None
        if b.imm < 0 or b.imm > 0xFFFF:
            return None
        # The guard is the first later instruction that reads the flags, not
        # necessarily the next one: MSVC schedules unrelated moves in between.
        for k in range(j + 1, min(len(fn.insns), j + 10)):
            nxt = fn.insns[k]
            d, u = flag_effect(nxt)
            if u:
                delta = self.GUARD_BOUND.get(nxt.mnem)
                if delta is None:
                    return None
                # The guard has to leave the dispatch, not re-enter it: a
                # branch back into the compare-to-JMP range would mean the
                # out-of-range path runs the table lookup anyway.
                t = self.branch_target(nxt)
                if t is None or (ins.addr <= t <= fn.insns[self.jmp_index].addr):
                    return None
                n = b.imm + delta
                return n if n > 0 else None
            if d:
                return None
        return None

    @staticmethod
    def writes_reg32(ins, reg):
        if not ins.ops:
            return False
        if ins.mnem in ("CMP", "TEST", "PUSH", "JMP") or ins.mnem in JCC:
            return False
        try:
            d = parse_operand(ins.ops[0])
        except TranslateError:
            return True
        return d.kind == "reg" and d.reg == reg

    # -- emission ----------------------------------------------------------

    def prepare(self, fn, strict=False):
        """Index the function and decode its jump tables (needed before both
        liveness and entry-point discovery).

        The first pass is lenient: a bounded table entry that is not a listed
        instruction goes into `unlisted_targets` for the driver to recover from
        the PE, because the Ghidra export drops instructions.  The second pass
        is strict and a target that is still not an instruction boundary is an
        error."""
        fn.index = {ins.addr: k for k, ins in enumerate(fn.insns)}
        self.strict = strict
        for i, ins in enumerate(fn.insns):
            if ins.mnem == "JMP" and ins.ops and not ins.ops[0].startswith("0x"):
                t = self.decode_jumptable(fn, i)
                if t:
                    self.jumptables[(fn.addr, ins.addr)] = t

    def translate(self, fn, entries=()):
        """Emit fn_ADDR, plus one thin wrapper per alternate entry point.

        A function is entered anywhere its address appears in the block-entry
        table: its own start, a jump-table target inside it, or a jump from
        another function that Ghidra split at a different boundary.  With any
        of those, the body becomes `static void body_ADDR(X86 *, uint32_t)`
        preceded by a dispatch switch, and every entry is a three-line wrapper,
        so no code is duplicated."""
        entries = sorted(set(entries))
        # An alternate entry must be an instruction in THIS body.  A block can
        # be re-recovered between the discovery rounds and the final pass -
        # recursive descent makes them grow - so an entry registered against
        # an earlier, shorter version has to be dropped rather than emitted as
        # a goto to a label that was never placed.
        fn.index = {ins.addr: k for k, ins in enumerate(fn.insns)}
        dropped = [e for e in entries if e not in fn.index]
        if dropped:
            self.stale_entries.update(dropped)
            entries = [e for e in entries if e in fn.index]
        if self.opts.eager_flags:
            live_out = [ALL_FLAGS] * len(fn.insns)
        else:
            live_out = self.liveness(fn)

        labels = set()
        for i, ins in enumerate(fn.insns):
            if ins.mnem in JCC or ins.mnem == "JMP":
                t = self.branch_target(ins)
                if t is not None and t in fn.index:
                    labels.add(t)
            for t in self.jumptables.get((fn.addr, ins.addr), []):
                if t in fn.index:
                    labels.add(t)
            # A listing gap or an end-of-block fall-through emits a transfer
            # too, and its target needs a label just as much as a branch's.
            if ins.mnem not in TERMINATORS and not fn.contiguous[i]:
                t = fn.fallthrough[i]
                if t in fn.index:
                    labels.add(t)
        if fn.insns[-1].mnem not in TERMINATORS:
            t = fn.fallthrough[-1] or fn.end
            if t in fn.index:
                labels.add(t)

        for e in entries:
            labels.add(e)

        out = []
        if fn.addr in INTRINSIC_BODY:
            self.stats["_intrinsic_body"] += 1
            return ["/* runtime intrinsic */",
                    "void fn_%08x(X86 *c) { %s }" % (fn.addr, INTRINSIC_BODY[fn.addr])]
        # Following branches can pull in addresses BELOW the entry, so the
        # first instruction in address order is not necessarily where this
        # function starts.  Jump to the entry explicitly rather than falling
        # into whatever sorts first: fn_00565e1e began at 00565dd0, a POP EAX,
        # which ate the caller's return address.
        head = fn.insns[0].addr != fn.addr
        if head:
            labels.add(fn.addr)
        prologue = 0                 # lines before the first instruction
        if entries:
            out.append("static void body_%08x(X86 *c, uint32_t entry_) {" % fn.addr)
            out.append("    switch (entry_) {")
            for e in entries:
                out.append("    case %s: goto L_%08x;" % (hexlit(e), e))
            out.append("    default: goto L_%08x;" % fn.addr if head else
                       "    default: break;")
            out.append("    }")
        else:
            out.append("void fn_%08x(X86 *c) {" % fn.addr)
            if head:
                out.append("    goto L_%08x;" % fn.addr)
        prologue = len(out)          # everything emitted so far is dispatch
        for i, ins in enumerate(fn.insns):
            if ins.addr in labels:
                out.append("L_%08x: ;" % ins.addr)
            body = self.emit(fn, i, live_out[i])
            for line in body:
                out.append("    " + line)
            if not fn.contiguous[i] and ins.mnem not in TERMINATORS:
                t = fn.fallthrough[i]
                self.stats["_listing_gap"] += 1
                self.notes.append(
                    "%08x: listing gap, %s falls through to %08x" % (ins.addr, ins.mnem, t))
                for line in self.goto_target(fn, t, ins):
                    out.append("    " + line)
        # A function whose last listed instruction is not a terminator falls
        # through into the next function.
        last = fn.insns[-1]
        if last.mnem not in TERMINATORS:
            t = fn.fallthrough[-1] or fn.end
            self.stats["_fallthrough_exit"] += 1
            out.append("    " + " ".join(self.goto_target(fn, t, last)))
        # Invariant: the first thing fn_ADDR does is reach ADDR.  Checked
        # here rather than trusted, because getting it wrong corrupts the
        # guest stack silently and only shows up several calls away.  The
        # whole dispatch prologue is searched - `out[:prologue]` - not a fixed
        # number of lines: with four alternate entries the default arm sits
        # past any such window, and slicing rejected correct output.
        if fn.addr not in fn.index:
            raise TranslateError("fn_%08x does not contain its own entry" % fn.addr)
        if head:
            want = ("goto L_%08x;" % fn.addr, "default: goto L_%08x;" % fn.addr)
            if not any(line.strip() in want for line in out[:prologue]):
                raise TranslateError(
                    "fn_%08x starts at %08x, not at its own address; prologue "
                    "was %s" % (fn.addr, fn.insns[0].addr,
                                " | ".join(l.strip() for l in out[:prologue])))
        out.append("}")
        if entries:
            out.append("void fn_%08x(X86 *c) { body_%08x(c, 0u); }" % (fn.addr, fn.addr))
            for e in entries:
                out.append("void fn_%08x(X86 *c) { body_%08x(c, %s); }"
                           % (e, fn.addr, hexlit(e)))
        return out

    # ---- helpers used by emit -------------------------------------------

    def flags_arith(self, kind, size, live, a="a_", b="b_", r="r_", rf="rf_"):
        """Flag stores for ADD/ADC/SUB/SBB/CMP/INC/DEC."""
        bits = size
        lines = []
        if "cf" in live and kind in ("add", "sub"):
            lines.append("c->eflags_cf = (uint32_t)((%s >> %d) & 1u);" % (rf, bits))
        if "of" in live:
            if kind == "add":
                lines.append("c->eflags_of = (uint32_t)((~(%s ^ %s) & (%s ^ %s)) >> %d) & 1u;"
                             % (a, b, a, r, bits - 1))
            else:
                lines.append("c->eflags_of = (uint32_t)(((%s ^ %s) & (%s ^ %s)) >> %d) & 1u;"
                             % (a, b, a, r, bits - 1))
        if "af" in live:
            lines.append("c->eflags_af = ((%s ^ %s ^ %s) >> 4) & 1u;" % (a, b, r))
        if "zf" in live:
            lines.append("c->eflags_zf = (%s == 0);" % r)
        if "sf" in live:
            lines.append("c->eflags_sf = (%s >> %d) & 1u;" % (r, bits - 1))
        if "pf" in live:
            lines.append("c->eflags_pf = parity8(%s);" % r)
        return lines

    def flags_logic(self, size, live, r="r_"):
        lines = []
        if "cf" in live:
            lines.append("c->eflags_cf = 0;")
        if "of" in live:
            lines.append("c->eflags_of = 0;")
        if "zf" in live:
            lines.append("c->eflags_zf = (%s == 0);" % r)
        if "sf" in live:
            lines.append("c->eflags_sf = (%s >> %d) & 1u;" % (r, size - 1))
        if "pf" in live:
            lines.append("c->eflags_pf = parity8(%s);" % r)
        return lines

    def rmw(self, op, size, lines):
        """For a memory destination, hoist the address into ad_ and return a
        substitute operand that reuses it."""
        if op.kind != "mem":
            return op
        lines.append("uint32_t ad_ = %s;" % addr_expr(op))
        return Op("mem", size=op.size, addr_c="ad_")

    # ---- the instruction dispatcher --------------------------------------

    def emit(self, fn, i, live):
        ins = fn.insns[i]
        m = ins.mnem
        nxt = fn.insns[i + 1].addr if i + 1 < len(fn.insns) else fn.end
        try:
            body = self._emit(fn, i, ins, m, nxt, live)
            body = visual_animation_read(ins.addr, body)
        except TranslateError as e:
            raise TranslateError("%08x %s: %s" % (ins.addr, ins.raw.split("  ", 1)[1], e))
        self.stats[m] += 1
        declares = any(body[0].startswith(t) for t in
                       ("uint8_t ", "uint16_t ", "uint32_t ", "uint64_t ", "double "))
        if len(body) == 1 and not declares:
            return ["%s  /* %08x %s */" % (body[0], ins.addr, ins.raw.split("  ", 1)[1])]
        return (["{   /* %08x %s */" % (ins.addr, ins.raw.split("  ", 1)[1])]
                + ["    " + b for b in body] + ["}"])

    STRING_MNEM = frozenset(("STOSB", "STOSW", "STOSD", "MOVSB", "MOVSW", "MOVSD",
                             "LODSB", "LODSW", "LODSD", "SCASB", "SCASW", "SCASD",
                             "CMPSB", "CMPSW", "CMPSD"))

    def _emit(self, fn, i, ins, m, nxt, live):
        if m in self.STRING_MNEM:
            # Ghidra prints the implicit ES:EDI / ESI operands; they carry no
            # information the mnemonic does not already imply.
            return self.emit_string(ins, m)
        ops = [parse_operand(o) for o in ins.ops] if ins.ops else []
        L = []

        # ---------------------------------------------------------- data --
        if m == "MOV":
            size = operand_size(ops)
            dst, src = ops
            L.append(write_op(dst, size, read_op(src, size)))
            return L

        if m == "LEA":
            dst, src = ops
            if src.kind != "mem":
                raise TranslateError("LEA without memory operand")
            L.append(write_op(dst, 32, addr_expr(src)))
            return L

        if m in ("MOVSX", "MOVZX"):
            dst, src = ops
            ssize = src.size if src.kind == "mem" and src.size else (
                src.size if src.kind == "reg" else None)
            if ssize is None:
                raise TranslateError("%s: unknown source size" % m)
            if m == "MOVSX":
                val = "(uint32_t)(int32_t)(%s)(%s)" % (stype(ssize), read_op(src, ssize))
            else:
                val = "(uint32_t)(%s)" % read_op(src, ssize)
            L.append(write_op(dst, dst.size, val))
            return L

        if m == "XCHG":
            size = operand_size(ops)
            a, b = ops
            a = self.rmw(a, size, L)
            L.append("uint32_t t_ = %s;" % read_op(a, size))
            L.append(write_op(a, size, read_op(b, size)))
            L.append(write_op(b, size, "t_"))
            return L

        if m == "PUSH":
            size = operand_size(ops, hint=32)
            if size != 32:
                raise TranslateError("non-32-bit PUSH")
            L.append("uint32_t v_ = %s;" % read_op(ops[0], 32))
            L.append("c->r[4] -= 4; wr32(c->r[4], v_);")
            return L

        if m == "POP":
            size = operand_size(ops, hint=32)
            if size != 32:
                raise TranslateError("non-32-bit POP")
            L.append("uint32_t v_ = rd32(c->r[4]); c->r[4] += 4;")
            L.append(write_op(ops[0], 32, "v_"))
            return L

        if m == "PUSHAD":
            return ["x86_pushad(c);"]
        if m == "POPAD":
            return ["x86_popad(c);"]
        if m == "PUSHFD":
            return ["c->r[4] -= 4; wr32(c->r[4], x86_get_eflags(c));"]
        if m == "POPFD":
            return ["x86_set_eflags(c, rd32(c->r[4])); c->r[4] += 4;"]
        if m == "LEAVE":
            return ["c->r[4] = c->r[5]; c->r[5] = rd32(c->r[4]); c->r[4] += 4;"]
        if m == "SAHF":
            return ["x86_sahf(c);"]
        if m == "XLAT":
            return ["xlat(c);"]
        if m == "BSWAP":
            return [write_op(ops[0], 32, "bswap32(%s)" % read_op(ops[0], 32))]

        if m == "CDQ":
            return ["c->r[2] = (uint32_t)((int32_t)c->r[0] >> 31);"]
        if m == "CWD":
            return ["c->r[2] = (c->r[2] & 0xffff0000u) | "
                    "((uint32_t)((int16_t)c->r[0] >> 15) & 0xffffu);"]
        if m == "CBW":
            return ["c->r[0] = (c->r[0] & 0xffff0000u) | "
                    "((uint32_t)(int32_t)(int8_t)c->r[0] & 0xffffu);"]

        # ------------------------------------------------------- arith ----
        if m in ("ADD", "ADC", "SUB", "SBB", "CMP"):
            size = operand_size(ops)
            dst, src = ops
            dst = self.rmw(dst, size, L)
            L.append("uint32_t a_ = %s, b_ = %s;" % (read_op(dst, size), read_op(src, size)))
            if m in ("ADD", "ADC"):
                carry = " + c->eflags_cf" if m == "ADC" else ""
                L.append("uint64_t rf_ = (uint64_t)a_ + b_%s;" % carry)
                kind = "add"
            else:
                carry = " - c->eflags_cf" if m == "SBB" else ""
                L.append("uint64_t rf_ = (uint64_t)a_ - b_%s;" % carry)
                kind = "sub"
            L.append("uint32_t r_ = (uint32_t)rf_ & %s;" % hexlit(mask_of(size)))
            if m != "CMP":
                L.append(write_op(dst, size, "r_"))
            L += self.flags_arith(kind, size, live)
            return L

        if m in ("INC", "DEC"):
            size = operand_size(ops)
            dst = self.rmw(ops[0], size, L)
            L.append("uint32_t a_ = %s, b_ = 1u;" % read_op(dst, size))
            L.append("uint64_t rf_ = (uint64_t)a_ %s b_;" % ("+" if m == "INC" else "-"))
            L.append("uint32_t r_ = (uint32_t)rf_ & %s;" % hexlit(mask_of(size)))
            L.append(write_op(dst, size, "r_"))
            L += self.flags_arith("add" if m == "INC" else "sub", size, live - {"cf"})
            return L

        if m == "NEG":
            size = operand_size(ops)
            dst = self.rmw(ops[0], size, L)
            L.append("uint32_t b_ = %s, a_ = 0u;" % read_op(dst, size))
            L.append("uint64_t rf_ = (uint64_t)a_ - b_;")
            L.append("uint32_t r_ = (uint32_t)rf_ & %s;" % hexlit(mask_of(size)))
            L.append(write_op(dst, size, "r_"))
            L += self.flags_arith("sub", size, live)
            return L

        if m == "NOT":
            size = operand_size(ops)
            dst = self.rmw(ops[0], size, L)
            L.append(write_op(dst, size, "~%s" % read_op(dst, size)))
            return L

        if m in ("AND", "OR", "XOR", "TEST"):
            size = operand_size(ops)
            dst, src = ops
            cop = {"AND": "&", "OR": "|", "XOR": "^", "TEST": "&"}[m]
            dst2 = self.rmw(dst, size, L) if m != "TEST" else dst
            L.append("uint32_t r_ = (%s %s %s) & %s;"
                     % (read_op(dst2, size), cop, read_op(src, size), hexlit(mask_of(size))))
            if m != "TEST":
                L.append(write_op(dst2, size, "r_"))
            L += self.flags_logic(size, live)
            return L

        # ------------------------------------------------- shift / rotate --
        if m in ("SHL", "SHR", "SAR", "ROL", "ROR", "RCL", "RCR"):
            size = ops[0].size or operand_size(ops, hint=32)
            dst = self.rmw(ops[0], size, L)
            cnt = read_op(ops[1], 32) if len(ops) > 1 else "1u"
            if len(ops) > 1 and ops[1].kind == "reg":
                cnt = "(uint32_t)%s" % read_op(ops[1], ops[1].size)
            fn_base = m.lower() + str(size)
            wants_flags = bool(live & SHIFT_MAYDEF[m]) or m in ("RCL", "RCR")
            call = "%s%s(%s%s, %s)" % (fn_base, "_f" if wants_flags else "",
                                       "c, " if wants_flags else "",
                                       read_op(dst, size), cnt)
            L.append(write_op(dst, size, call))
            return L

        if m in ("SHLD", "SHRD"):
            size = operand_size(ops[:2])
            if size != 32:
                raise TranslateError("%s at width %d" % (m, size))
            dst = self.rmw(ops[0], 32, L)
            cnt = ("(uint32_t)%s" % read_op(ops[2], ops[2].size)
                   if ops[2].kind == "reg" else read_op(ops[2], 32))
            L.append(write_op(dst, 32, "%s32_f(c, %s, %s, %s)"
                              % (m.lower(), read_op(dst, 32), read_op(ops[1], 32), cnt)))
            return L

        # -------------------------------------------------- mul  /  div ---
        if m == "IMUL" and len(ops) >= 2:
            size = operand_size(ops[:2])
            if len(ops) == 2:
                a, b = read_op(ops[0], size), read_op(ops[1], size)
            else:
                a, b = read_op(ops[1], size), read_op(ops[2], size)
            wants = bool(live & frozenset(("cf", "of")))
            helper = "imul2_%d%s" % (size, "_f" if wants else "")
            args = ("c, " if wants else "") + "%s, %s" % (a, b)
            L.append(write_op(ops[0], size, "%s(%s)" % (helper, args)))
            return L

        if m in ("MUL", "IMUL", "DIV", "IDIV"):
            size = operand_size(ops)
            src = read_op(ops[0], size)
            base = m.lower() + str(size)
            if m in ("DIV", "IDIV"):
                L.append("%s(c, (%s)(%s), %s);" % (base, utype(size), src, hexlit(ins.addr)))
            else:
                L.append("%s(c, (%s)(%s));" % (base, utype(size), src))
            return L

        # ------------------------------------------------------- bit ops --
        if m in ("BT", "BTS", "BTR", "BTC"):
            size = operand_size(ops, hint=32)
            dst, src = ops
            idx = read_op(src, size)
            if dst.kind == "mem":
                L.append("uint32_t bi_ = %s;" % idx)
                L.append("uint32_t ad_ = %s + 4u * (bi_ >> 5);" % addr_expr(dst))
                dst = Op("mem", size=32, addr_c="ad_")
                bit = "(bi_ & 31u)"
            else:
                L.append("uint32_t bi_ = (%s) & %du;" % (idx, size - 1))
                bit = "bi_"
            L.append("uint32_t v_ = %s;" % read_op(dst, size))
            if "cf" in live or m != "BT":
                L.append("c->eflags_cf = (v_ >> %s) & 1u;" % bit)
            if m == "BTS":
                L.append(write_op(dst, size, "v_ | (1u << %s)" % bit))
            elif m == "BTR":
                L.append(write_op(dst, size, "v_ & ~(1u << %s)" % bit))
            elif m == "BTC":
                L.append(write_op(dst, size, "v_ ^ (1u << %s)" % bit))
            return L

        if m in ("BSR", "BSF"):
            # With a zero source the destination is architecturally undefined;
            # unicorn (and AMD) leave it unchanged, so pass the old value in.
            L.append(write_op(ops[0], 32, "%s32_f(c, %s, %s)"
                              % (m.lower(), read_op(ops[0], 32), read_op(ops[1], 32))))
            return L

        if m in SETCC:
            cond = SETCC[m][0]
            L.append(write_op(ops[0], 8, "(%s) ? 1u : 0u" % cond))
            return L

        if m in CMOVCC:
            size = operand_size(ops)
            cond = CMOVCC[m][0]
            L.append("if (%s) { %s }" % (cond, write_op(ops[0], size,
                                                        read_op(ops[1], size))))
            return L

        if m == "CLD":
            return ["c->eflags_df = 0;"]
        if m == "STD":
            return ["c->eflags_df = 1;"]

        # ---------------------------------------------------- control flow --
        if m in JCC:
            cond = JCC[m][0]
            t = self.branch_target(ins)
            if t is None:
                raise TranslateError("indirect conditional jump")
            return ["if (%s) { %s }" % (cond, " ".join(self.goto_target(fn, t, ins)))]

        if m == "JMP":
            t = self.branch_target(ins)
            if t is not None:
                return self.goto_target(fn, t, ins)
            return self.emit_indirect_jump(fn, i, ins, ops[0])

        if m == "CALL":
            if ins.ops and ins.ops[0].startswith("0x"):
                t = int(ins.ops[0], 16)
                L.append("c->r[4] -= 4; wr32(c->r[4], %s);" % hexlit(nxt))
                if t == INTRINSIC_SETJMP:
                    # The host jmp_buf has to belong to a frame that is still
                    # live when _longjmp fires, so setjmp is taken here rather
                    # than inside the runtime (src/recomp/runtime/intrinsics.h).
                    self.stats["_intrinsic_setjmp"] += 1
                    # C11 7.13.2.1 allows setjmp only as a whole controlling
                    # expression, or compared against an integer constant in
                    # one; inside another call's argument list it is undefined.
                    L.append("jmp_buf *b_ = recomp_setjmp_prepare(c);")
                    L.append("if (setjmp(*b_) == 0) recomp_setjmp_return(c, 0);")
                    L.append("else recomp_setjmp_return(c, 1);")
                elif t in self.func_addrs:
                    L.append("CALL_FN(%08x);" % t)
                else:
                    self.stats["_call_unknown"] += 1
                    L.append("recomp_call(c, %s);" % hexlit(t))
                return L
            L.append("uint32_t t_ = %s;" % read_op(ops[0], 32))
            L.append("c->r[4] -= 4; wr32(c->r[4], %s);" % hexlit(nxt))
            L.append("recomp_call(c, t_);")
            self.stats["_call_indirect"] += 1
            return L

        if m == "RET":
            n = parse_imm(ins.ops[0]) if ins.ops else 0
            return ["c->eip = rd32(c->r[4]); c->r[4] += %du; return;" % (4 + n)]

        # --------------------------------------------------------- system --
        if m == "RDTSC":
            return ["recomp_rdtsc(c);"]
        if m == "CPUID":
            return ["recomp_cpuid(c);"]
        if m == "CLI":
            return ["recomp_cli(c);"]
        if m == "STI":
            return ["recomp_sti(c);"]
        if m == "HLT":
            return ["recomp_hlt(c);"]
        if m == "INT":
            return ["recomp_int(c, %s);" % hexlit(parse_imm(ins.ops[0]))]
        if m == "IN":
            size = operand_size(ops[:1], hint=32)
            port = read_op(ops[1], 32) if len(ops) > 1 else "0u"
            L.append(write_op(ops[0], size, "recomp_in(c, %s, %d)" % (port, size // 8)))
            return L
        if m == "OUT":
            size = operand_size(ops[1:], hint=8)
            port = read_op(ops[0], 32) if ops[0].kind == "reg" else read_op(ops[0], 32)
            L.append("recomp_out(c, %s, %s, %d);" % (port, read_op(ops[1], size), size // 8))
            return L
        if m in ("NOP", "WAIT"):
            return [";"]
        if m == "FNCLEX":
            return ["c->fpu_sw &= (uint16_t)~0x80ffu;"]

        # ------------------------------------------------------------ x87 --
        if m.startswith("F"):
            return self.emit_x87(fn, ins, m, ops)

        raise TranslateError("unhandled mnemonic %s" % m)

    # ---- control-flow helpers -------------------------------------------

    def goto_target(self, fn, t, ins):
        if t in fn.index:
            return ["goto L_%08x;" % t]
        if t in self.func_addrs:
            self.stats["_tailcall"] += 1
            return ["CALL_FN(%08x); return;" % t]
        self.stats["_tailcall_unknown"] += 1
        self.notes.append("%08x: jump to %08x outside any known function" % (ins.addr, t))
        return ["c->eip = %s; recomp_jump(c, %s); return;" % (hexlit(ins.addr), hexlit(t))]

    def emit_indirect_jump(self, fn, i, ins, op):
        targets = self.jumptables.get((fn.addr, ins.addr))
        L = ["uint32_t t_ = %s;" % read_op(op, 32)]
        if not targets:
            # Not a switch: an indirect tail call (import thunk, vtable jump).
            self.stats["_jmp_indirect_tail"] += 1
            L.append("c->eip = %s; recomp_jump(c, t_); return;" % hexlit(ins.addr))
            return L
        self.stats["_jmp_table"] += 1
        self.stats["_jmp_table_entries"] += len(targets)
        L.append("switch (t_) {")
        for t in sorted(set(targets)):
            if t in fn.index:
                L.append("case %s: goto L_%08x;" % (hexlit(t), t))
            else:
                L.append("case %s: CALL_FN(%08x); return;" % (hexlit(t), t))
        L.append("default: c->eip = %s; recomp_jump(c, t_); return;" % hexlit(ins.addr))
        L.append("}")
        return L

    def emit_string(self, ins, m):
        suf = {"B": "b", "W": "w", "D": "d"}[m[-1]]
        kind = m[:-1].lower()
        if kind in ("scas", "cmps"):
            if suf != "b":
                raise TranslateError("%s at width %s" % (m, suf))
            if ins.rep == "REPE" or ins.rep == "REP":
                return ["repe_%s(c);" % m.lower()]
            if ins.rep == "REPNE":
                return ["repne_%s(c);" % m.lower()]
            if ins.rep:
                raise TranslateError("prefix %s on %s" % (ins.rep, m))
            return ["%s(c);" % m.lower()]
        if ins.rep in ("REP", "REPE"):
            return ["rep_%s%s(c);" % (kind, suf)]
        if ins.rep:
            raise TranslateError("prefix %s on %s" % (ins.rep, m))
        return ["%s%s(c);" % (kind, suf)]

    def cross_check_switches(self, funcs):
        """Plan correction 8: compare each decoded jump table against the case
        labels in Ghidra's own decompilation of the same function.  Ghidra
        merges cases that share a body, so its label count is a lower bound on
        the number of selector values; a decoded table with fewer distinct
        targets than Ghidra has distinct case labels means entries were missed."""
        case_re = re.compile(r"^\s*case (-?(?:0x[0-9a-fA-F]+|\d+)):", re.M)
        by_fn = defaultdict(list)
        for (fn_addr, at), targets in self.jumptables.items():
            by_fn[fn_addr].append((at, targets))
        for fn in funcs:
            if fn.addr not in by_fn:
                continue
            path = os.path.join(LISTINGS, "%08x.c" % fn.addr)
            if not os.path.exists(path):
                continue
            try:
                cases = set(case_re.findall(open(path).read()))
            except OSError:
                continue
            if not cases:
                continue
            entries = sum(len(t) for _at, t in by_fn[fn.addr])
            self.stats["_switch_checked"] += 1
            # Cases that share a body collapse to one target, so distinct
            # targets can legitimately be fewer than case labels; what cannot
            # happen is fewer table entries than selector values.
            if entries < len(cases):
                self.stats["_switch_short"] += 1
                self.notes.append(
                    "%08x: decoded %d jump-table entries but Ghidra's .c has %d "
                    "case labels" % (fn.addr, entries, len(cases)))

    # ---- x87 -------------------------------------------------------------

    X87_MEM_INT = {"FILD", "FISTP", "FIST", "FIADD", "FISUB", "FISUBR",
                   "FIMUL", "FIDIV", "FIDIVR", "FICOM", "FICOMP"}

    def x87_mem_value(self, op):
        """Value of an x87 memory source as a C double expression."""
        if op.size == 32:
            return "(double)rdf32(%s)" % addr_expr(op)
        if op.size == 64:
            return "rdf64(%s)" % addr_expr(op)
        if op.size == 80:
            return "rdf80(%s)" % addr_expr(op)
        raise TranslateError("bad x87 memory size %r" % op.size)

    def x87_int_value(self, op):
        if op.size == 16:
            return "(double)(int16_t)rd16(%s)" % addr_expr(op)
        if op.size == 32:
            return "(double)(int32_t)rd32(%s)" % addr_expr(op)
        if op.size == 64:
            return "(double)(int64_t)rd64(%s)" % addr_expr(op)
        raise TranslateError("bad x87 integer size %r" % op.size)

    def emit_x87(self, fn, ins, m, ops):
        L = []
        st = lambda n: "ST(c, %d)" % n
        # Writing a register goes through fset so its tag follows the value.
        setst = lambda n, v: "fset(c, %d, %s);" % (n, v)

        if m == "FLD":
            if ops[0].kind == "st":
                L.append("double v_ = %s;" % st(ops[0].sti))
                L.append("fpush(c, v_);")
            else:
                L.append("fpush(c, %s);" % self.x87_mem_value(ops[0]))
            return L
        if m == "FLD1":
            return ["fpush(c, 1.0);"]
        if m == "FLDZ":
            return ["fpush(c, 0.0);"]
        if m == "FLDPI":
            return ["fpush(c, 3.14159265358979323846);"]
        if m == "FILD":
            return ["fpush(c, %s);" % self.x87_int_value(ops[0])]

        if m in ("FST", "FSTP"):
            if ops and ops[0].kind == "st":
                if ops[0].sti != 0:
                    L.append(setst(ops[0].sti, st(0)))
            elif ops:
                op = ops[0]
                if op.size == 32:
                    L.append("wrf32(%s, fto_float(c, %s));" % (addr_expr(op), st(0)))
                elif op.size == 64:
                    L.append("wrf64(%s, %s);" % (addr_expr(op), st(0)))
                elif op.size == 80:
                    L.append("wrf80(%s, %s);" % (addr_expr(op), st(0)))
                else:
                    raise TranslateError("bad %s size" % m)
            if m == "FSTP":
                L.append("fdrop(c);")
            return L

        if m in ("FIST", "FISTP"):
            op = ops[0]
            if op.size == 16:
                L.append("wr16(%s, (uint16_t)fto_i16(c, %s));" % (addr_expr(op), st(0)))
            elif op.size == 32:
                L.append("wr32(%s, (uint32_t)fto_i32(c, %s));" % (addr_expr(op), st(0)))
            elif op.size == 64:
                L.append("wr64(%s, (uint64_t)fto_i64(c, %s));" % (addr_expr(op), st(0)))
            else:
                raise TranslateError("bad %s size" % m)
            if m == "FISTP":
                L.append("fdrop(c);")
            return L

        # arithmetic ------------------------------------------------------
        ARITH = {"FADD": "+", "FSUB": "-", "FMUL": "*", "FDIV": "/",
                 "FSUBR": "-", "FDIVR": "/"}

        def combine(lhs, op, rhs):
            """`lhs op rhs`, routing division through the #Z check."""
            if op == "/":
                return "fdivz(c, %s, %s)" % (lhs, rhs)
            return "%s %s %s" % (lhs, op, rhs)
        base = m[:-1] if m.endswith("P") and m[:-1] in ARITH else None
        if m in ARITH:                       # non-popping
            rev = m in ("FSUBR", "FDIVR")
            o = ARITH[m]
            if not ops:
                raise TranslateError("%s without operand" % m)
            if len(ops) == 2:
                d, s = ops[0].sti, ops[1].sti
                lhs, rhs = (st(s), st(d)) if rev else (st(d), st(s))
                L.append(setst(d, "fx87(c, %s)" % combine(lhs, o, rhs)))
            elif ops[0].kind == "st":
                # D8 /n form: ST(0) op= ST(i)
                s = st(ops[0].sti)
                lhs, rhs = (s, st(0)) if rev else (st(0), s)
                L.append(setst(0, "fx87(c, %s)" % combine(lhs, o, rhs)))
            else:
                v = (self.x87_mem_value(ops[0]))
                L.append("double v_ = %s;" % v)
                lhs, rhs = ("v_", st(0)) if rev else (st(0), "v_")
                L.append(setst(0, "fx87(c, %s)" % combine(lhs, o, rhs)))
            return L

        if base:                             # FADDP/FSUBP/FMULP/FDIVP/...
            rev = base in ("FSUBR", "FDIVR")
            o = ARITH[base]
            d = ops[0].sti if ops else 1
            lhs, rhs = (st(0), st(d)) if rev else (st(d), st(0))
            L.append(setst(d, "fx87(c, %s)" % combine(lhs, o, rhs)))
            L.append("fdrop(c);")
            return L

        IARITH = {"FIADD": "+", "FISUB": "-", "FIMUL": "*", "FIDIV": "/",
                  "FISUBR": "-", "FIDIVR": "/"}
        if m in IARITH:
            rev = m in ("FISUBR", "FIDIVR")
            o = IARITH[m]
            L.append("double v_ = %s;" % self.x87_int_value(ops[0]))
            lhs, rhs = ("v_", st(0)) if rev else (st(0), "v_")
            L.append(setst(0, "fx87(c, %s)" % combine(lhs, o, rhs)))
            return L

        # comparison ------------------------------------------------------
        if m in ("FCOM", "FCOMP", "FUCOM", "FUCOMP"):
            # ops[-1] is the source: a two-operand form is `FCOM ST0,STi`, so
            # taking ops[0] would compare ST(0) with itself.
            if not ops:
                other = st(1)
            elif ops[-1].kind == "st":
                other = st(ops[-1].sti)
            else:
                other = self.x87_mem_value(ops[-1])
            # FUCOM raises the invalid exception only for a signalling NaN.
            L.append("%s(c, %s, %s);" % ("fucom" if m.startswith("FU") else "fcom",
                                         st(0), other))
            if m.endswith("P"):
                L.append("fdrop(c);")
            return L
        if m in ("FCOMPP", "FUCOMPP"):
            L.append("%s(c, %s, %s);" % ("fucom" if m.startswith("FU") else "fcom",
                                         st(0), st(1)))
            L.append("fdrop(c); fdrop(c);")
            return L
        if m in ("FICOM", "FICOMP"):
            L.append("fcom(c, %s, %s);" % (st(0), self.x87_int_value(ops[0])))
            if m == "FICOMP":
                L.append("fdrop(c);")
            return L
        if m in ("FCOMI", "FCOMIP", "FUCOMI", "FUCOMIP"):
            other = st(ops[-1].sti) if ops else st(1)
            L.append("%s(c, %s, %s);" % ("fucomi" if m.startswith("FU") else "fcomi",
                                         st(0), other))
            if m.endswith("IP"):
                L.append("fdrop(c);")
            return L
        if m == "FTST":
            return ["fcom(c, %s, 0.0);" % st(0)]
        if m == "FXAM":
            return ["fxam(c);"]

        # transcendental / misc --------------------------------------------
        # Per the SDM, precision control affects only FADD/FSUB/FMUL/FDIV
        # (with their integer and popping forms) and FSQRT.  FRNDINT and the
        # transcendentals must keep the register's full precision, otherwise
        # PC=00 turns an exact 16777217 into 16777216.
        UNARY = {"FABS": "fabs(%s)", "FCHS": "-(%s)",
                 "FSQRT": "fx87(c, sqrt(%s))",
                 "FSIN": "fx87_exact(c, sin(%s))",
                 "FCOS": "fx87_exact(c, cos(%s))",
                 "FRNDINT": "fx87_exact(c, fround_cw(c, %s))"}
        if m in UNARY:
            return [setst(0, UNARY[m] % st(0))]
        if m == "FSINCOS":
            L.append("double v_ = %s;" % st(0))
            L.append(setst(0, "fx87_exact(c, sin(v_))"))
            L.append("fpush(c, fx87_exact(c, cos(v_)));")
            return L
        if m == "FSCALE":
            return [setst(0, "fx87_exact(c, fscale(%s, %s))" % (st(0), st(1)))]
        if m == "FPATAN":
            L.append(setst(1, "fx87_exact(c, atan2(%s, %s))" % (st(1), st(0))))
            L.append("fdrop(c);")
            return L
        if m == "FYL2X":
            L.append(setst(1, "fx87_exact(c, %s * log2(%s))" % (st(1), st(0))))
            L.append("fdrop(c);")
            return L
        if m == "FYL2XP1":
            # log2(x+1) loses every significant bit for small x; log1p keeps
            # them.  With ST(0) = 2^-54 and ST(1) = 1 the naive form returns 0.
            L.append(setst(1, "fx87_exact(c, %s * log1p(%s) / M_LN2)"
                           % (st(1), st(0))))
            L.append("fdrop(c);")
            return L
        if m == "F2XM1":
            # Likewise 2^x - 1: expm1(x * ln 2) is the same value without the
            # cancellation.
            return [setst(0, "fx87_exact(c, expm1(%s * M_LN2))" % st(0))]
        if m in ("FPREM", "FPREM1"):
            return [setst(0, "fprem_common(c, %s, %s, %d)"
                          % (st(0), st(1), 1 if m == "FPREM1" else 0))]
        if m == "FXTRACT":
            L.append("double v_ = %s;" % st(0))
            L.append(setst(0, "fxtract_exponent(v_)"))
            L.append("fpush(c, fxtract_significand(v_));")
            return L
        if m == "FDECSTP":
            # Both rotate TOP and clear C1.
            return ["c->fpu_top = (c->fpu_top - 1u) & 7u;",
                    "c->fpu_sw &= (uint16_t)~0x0200u;"]
        if m == "FINCSTP":
            return ["c->fpu_top = (c->fpu_top + 1u) & 7u;",
                    "c->fpu_sw &= (uint16_t)~0x0200u;"]
        if m == "FNOP":
            return [";"]
        if m == "FXCH":
            # Capstone renders D9 C9 with the implicit ST(0) present, as
            # `FXCH ST0,ST1`, where the listing writes `FXCH ST1`.  Taking
            # ops[0] there selects ST(0) and swaps the register with itself.
            return ["fxch(c, %d);" % (ops[-1].sti if ops else 1)]
        if m in ("FNSTSW", "FSTSW"):
            if ops and ops[0].kind == "reg":
                return ["c->r[0] = (c->r[0] & 0xffff0000u) | fstsw(c);"]
            return ["wr16(%s, fstsw(c));" % addr_expr(ops[0])]
        if m in ("FSTCW", "FNSTCW"):
            return ["wr16(%s, c->fpu_cw);" % addr_expr(ops[0])]
        if m == "FLDCW":
            return ["x87_set_cw(c, rd16(%s));" % addr_expr(ops[0])]
        if m == "FFREE":
            return [";"]
        if m in ("FNSTENV", "FLDENV", "FNSAVE", "FRSTOR", "FINIT", "FNINIT"):
            raise TranslateError("unsupported x87 environment op %s" % m)

        raise TranslateError("unhandled x87 mnemonic %s" % m)


# --------------------------------------------------------------- driver ----

def load_functions(only=None):
    funcs = []
    with open(FUNCS_TSV) as fh:
        rows = [l.rstrip("\n").split("\t") for l in fh][1:]
    for row in rows:
        addr = int(row[0], 16)
        name, nbytes = row[1], int(row[2])
        if only and "%08x" % addr not in only:
            continue
        path = os.path.join(LISTINGS, "%08x.asm" % addr)
        if not os.path.exists(path) or os.path.getsize(path) == 0:
            continue
        funcs.append((addr, name, nbytes, path))
    funcs.sort()
    return funcs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(ROOT, "build/recomp/gen"))
    ap.add_argument("--only", nargs="*", default=None)
    ap.add_argument("--eager-flags", action="store_true",
                    help="compute every flag at every instruction (debug)")
    ap.add_argument("--check-flags", action="store_true",
                    help="report flags that would cross CALL/RET boundaries")
    ap.add_argument("--report", default=None, help="write a JSON stats file")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--allow-table-gaps", metavar="REASON", default=None,
                    help="accept jump-table entries that dispatch nowhere, "
                         "recording the reason in the report")
    args = ap.parse_args()

    t0 = time.time()
    entries = load_functions(set(args.only) if args.only else None)
    all_addrs = set()
    with open(FUNCS_TSV) as fh:
        for l in list(fh)[1:]:
            a = int(l.split("\t")[0], 16)
            p = os.path.join(LISTINGS, "%08x.asm" % a)
            if os.path.exists(p) and os.path.getsize(p) > 0:
                all_addrs.add(a)

    image = Image(BINARY)
    curated = read_curated(os.path.join(ROOT, "tools/recomp/symbols/globals.toml"))
    # Addresses named by a dword the loader relocates: a vtable slot, a
    # function-pointer table, a stored callback.  Read once, before discovery,
    # so the evidence does not depend on which pass reaches an address first.
    relocated = image.relocated_pointers()
    hook_evidence = defaultdict(set)
    for name, addr in sorted(curated.get("alternates", {}).items()):
        hook_evidence[addr].add("curated")
    for name, addr in sorted(curated.get("entries", {}).items()):
        hook_evidence[addr].add("curated_entry")
    parsed = []
    failures = []
    for addr, name, nbytes, path in entries:
        try:
            insns = parse_listing(path)
        except TranslateError as e:
            failures.append((addr, "parse: %s" % e))
            continue
        fn = Function(addr, name, nbytes, insns)
        fn.measure(image)
        parsed.append(fn)

    # The Ghidra export is not complete: control lands on addresses it never
    # listed, and functions get split at boundaries other code jumps past.
    # Resolve both to a fixpoint - an address inside another function becomes
    # an alternate entry, an address in no listing is decoded from the PE.
    owner = {}
    # One byte per image byte: set where a byte belongs to a translated
    # instruction but is not its first.  A pointer landing there names no
    # place execution can begin, so it is neither an entry nor a candidate.
    interior_bytes = bytearray(image.size)

    def register(fn):
        for k, ins in enumerate(fn.insns):
            owner.setdefault(ins.addr, fn)
            end = fn.fallthrough[k]
            if end is None:
                end = (fn.insns[k + 1].addr if k + 1 < len(fn.insns) else fn.end)
            if not (image.base <= ins.addr and end <= image.end):
                continue
            for b in range(ins.addr + 1, min(end, image.end)):
                interior_bytes[b - image.base] = 1

    for fn in parsed:
        register(fn)

    extra = {}
    recovered = []
    discovered_by_scan = [0]
    provenance = {}
    interior_entries = [0]
    initterm_found = [0]
    immediate_entries = [0]
    initterm_tables = []
    rejected = set()
    tr = Translator(image, all_addrs, args)

    if args.check_flags:
        for fn in parsed:
            tr.prepare(fn)
        check_flags(tr, parsed)
        return 0

    os.makedirs(args.out, exist_ok=True)
    chunks = []

    def accepts(fn):
        """Can the emitter translate every instruction of this block?

        A candidate address that is really data decodes into instructions this
        compiler never emits - `POP ES`, `DAS`, `LJMP` - and the emitter says
        so.  Using it as the filter means the vocabulary check can never drift
        from what the translator actually supports."""
        notes, stats = len(tr.notes), dict(tr.stats)
        try:
            tr.prepare(fn)
            tr.translate(fn)
            return True
        except TranslateError:
            return False
        except Exception:                       # noqa: BLE001
            return False
        finally:
            del tr.notes[notes:]
            tr.stats.clear()
            tr.stats.update(stats)

    def resolve(t, listed, home=None, validate=True, why="branch"):
        """Make `t` an entry point.  Returns True if that changed anything."""
        if t is None:
            return False
        # Before any of the early returns below.  See note_structural.
        note_structural(provenance, owner, t, why)
        if t in all_addrs:
            return False
        if home is not None and t in home.addrs:
            return False
        if t in owner:
            extra[t] = owner[t]
            all_addrs.add(t)
            # Why this address is an entry, which symbols.json turns into hook
            # eligibility.  A data pointer or an __initterm table naming it is
            # a stronger statement than its merely falling out of branch
            # following, so it replaces that; a structural naming has already
            # been recorded above and is never weakened here.
            if why != "branch" and provenance.get(t, "branch") == "branch":
                provenance[t] = why
            return True
        insns = image.recover(t, listed)
        if not insns:
            return False
        new_fn = Function(t, "recovered_%08x" % t, insns[-1].addr + 1 - t, insns)
        new_fn.measure(image)
        # A block reached from an established one is established too.
        provenance[t] = why if why != "branch" else provenance.get(
            home.addr if home is not None else None, "branch")
        if validate and not accepts(new_fn):
            # Decoded into something this compiler never emits, so the address
            # is data.  Dropping it leaves any jump to it aborting at runtime
            # with the address printed, which beats emitting nonsense.
            rejected.add(t)
            return False
        parsed.append(new_fn)
        recovered.append(new_fn)
        all_addrs.add(t)
        register(new_fn)
        return True

    scanned_pointers = False
    converged = False
    for _round in range(64):
        tr.func_addrs = all_addrs
        tr.all_insn_addrs = set(owner)
        tr.jumptables.clear()
        tr.unlisted_targets.clear()
        for fn in parsed:
            try:
                tr.prepare(fn, strict=False)
            except TranslateError:
                pass                      # reported by the strict pass below
        listed = set(owner)
        changed = False
        for fn in list(parsed):
            for i, ins in enumerate(fn.insns):
                # Direct CALL targets are followed too.  Every one in the
                # Ghidra corpus was already a listed function, but code found
                # by the data-pointer scan calls functions Ghidra never listed
                # either: 0049e800 calls 004c3110 and 004c31e0.
                if ins.mnem in ("JMP", "CALL") or ins.mnem in JCC:
                    changed |= resolve(Translator.branch_target(ins), listed, fn)
                if ins.mnem in TERMINATORS:
                    continue
                if not fn.contiguous[i]:
                    changed |= resolve(fn.fallthrough[i], listed, fn)
            if fn.insns[-1].mnem not in TERMINATORS:
                changed |= resolve(fn.fallthrough[-1] or fn.end, listed, fn)
        for t in sorted(tr.unlisted_targets):
            changed |= resolve(t, listed, why="table")
        # And from the tables directly, which is the only way targets that were
        # already known get promoted: those never enter unlisted_targets.
        for _targets in tr.jumptables.values():
            for t in _targets:
                note_structural(provenance, owner, t, "table")
        if not changed and not scanned_pointers:
            # Ghidra's list misses functions that are only ever reached through
            # a pointer loaded from data.  Scan the image for those once the
            # listed code has settled, then let the same loop follow whatever
            # they call and fall into.
            scanned_pointers = True
            before = len(all_addrs)
            # The CRT's static initializers first, and unconditionally: the
            # call site to __initterm names the table's bounds outright, so
            # every non-null pointer in it is an entry point by construction
            # and no heuristic gets a say.  21 of these were being dropped by
            # the string-tail test, leaving their C++ globals zero.
            init_ranges, init_entries = image.initterm_tables(parsed)
            initterm_tables[:] = init_ranges
            for t in sorted(init_entries):
                # The evidence first, and unconditionally: the CRT's table
                # names this address whether or not this is the pass that adds
                # it, and eligibility must not depend on that.
                hook_evidence[t].add("initterm")
                # Through resolve, which handles an address already covered by
                # a known body and, either way, records the provenance that
                # keeps it out of the pruning pass.
                if resolve(t, listed, why="initterm"):
                    changed = True
                    initterm_found[0] += 1
            # Addresses inside a decoded jump table are table storage, not
            # code: 0x4422c0 is 16-aligned and follows a RET, so it passes
            # every static signal there is.
            # Addresses that only ever appear as an instruction's immediate.
            # `atexit(00567570)` pushes its handler and nothing else in the
            # image names it, so neither the data scan nor call following sees
            # it and the handler is skipped at shutdown.
            immediates = set()
            for fn in parsed:
                for ins in fn.insns:
                    if ins.mnem not in ("PUSH", "MOV") or not ins.ops:
                        continue
                    try:
                        src = parse_operand(ins.ops[-1])
                    except TranslateError:
                        continue
                    if src.kind != "imm" or not image.is_exec(src.imm):
                        continue
                    if ins.mnem == "MOV":
                        try:
                            dst = parse_operand(ins.ops[0])
                        except TranslateError:
                            continue
                        if dst.kind != "reg" or dst.size != 32:
                            continue
                    immediates.add(src.imm)
            # Same gate as a data pointer, in the same order: a known
            # instruction boundary wins outright; an address inside an
            # instruction, or inside decoded table storage, is not a place
            # execution can begin whatever it looks like.
            for t in sorted(immediates):
                if t in owner:
                    if t not in all_addrs:
                        extra[t] = owner[t]
                        all_addrs.add(t)
                        changed = True
                        immediate_entries[0] += 1
                    continue
                if interior_bytes[t - image.base]:
                    image.interior_candidates.add(t)
                    continue
                if any(lo <= t < hi for lo, hi in tr.table_ranges):
                    continue
                if not (image.looks_like_function(t)
                        or image.looks_like_code_start(t)):
                    continue
                hook_evidence[t].add("immediate")
                if resolve(t, listed, why="immediate"):
                    changed = True
                    immediate_entries[0] += 1

            starts, interior = image.code_pointers(
                set(owner), interior_bytes=interior_bytes,
                exclude=tr.table_ranges)
            # A scan hit is evidence of nothing on its own.  It becomes
            # evidence when the same address is the value of a dword the
            # loader relocates, which is the linker saying that dword is a
            # pointer.  Recorded for both sets, before either is resolved.
            for t in starts | interior:
                if t in relocated:
                    hook_evidence[t].add("reloc")
            for t in sorted(starts):
                changed |= resolve(t, listed, why="data")
            # A pointer to an address that already is an instruction boundary
            # names a place the program can be entered, so it gets an
            # alternate entry rather than being dropped.  It came out of the
            # same data-pointer scan as `starts`, so it carries the same
            # provenance: the program names it, which is what makes it a
            # stable hook target rather than an internal block.
            for t in sorted(interior):
                if resolve(t, listed, why="data"):
                    changed = True
                    interior_entries[0] += 1
            discovered_by_scan[0] = (len(all_addrs) - before - interior_entries[0]
                                     - initterm_found[0] - immediate_entries[0])
            if not changed:
                converged = True
                break
        elif not changed:
            converged = True
            break
    if not converged:
        raise TranslateError(
            "entry-point discovery did not converge in 64 rounds; %d entry "
            "points and %d recovered blocks so far"
            % (len(all_addrs), len(recovered)))
    parsed.sort(key=lambda f: f.addr)

    # final, strict jump-table decode (stats from the discovery rounds are
    # discarded so the report counts each table once)
    for k in [k for k in tr.stats if k.startswith("_jmp_table")]:
        del tr.stats[k]
    tr.func_addrs = all_addrs
    tr.all_insn_addrs = set(owner)
    tr.jumptables.clear()
    tr.unlisted_targets.clear()
    for fn in parsed:
        try:
            tr.prepare(fn, strict=True)
        except TranslateError as e:
            failures.append((fn.addr, "jump table: %s" % e))

    # Plan correction 9: every decoded jump-table target inside a function is a
    # block entry, so recomp_jump can reach it.
    for (fn_addr, _at), targets in tr.jumptables.items():
        for t in targets:
            # This is the last word on which addresses the tables name, and it
            # runs after discovery, so promote here too: the withdraw pass
            # below reads provenance and must not drop a live target.
            note_structural(provenance, owner, t, "table")
            if t not in all_addrs and t in owner:
                extra[t] = owner[t]
                all_addrs.add(t)

    entries_by_fn = defaultdict(set)
    for t, fn in extra.items():
        entries_by_fn[fn.addr].add(t)

    bodies = {}
    for fn in parsed:
        if fn.addr in [a for a, _ in failures]:
            continue
        try:
            bodies[fn.addr] = tr.translate(fn, entries_by_fn.get(fn.addr, ()))
        except TranslateError as e:
            failures.append((fn.addr, str(e)))
        except Exception as e:              # noqa: BLE001 - report, never crash the build
            failures.append((fn.addr, "%s: %s" % (type(e).__name__, e)))

    tr.cross_check_switches([fn for fn in parsed if fn.addr in bodies])

    # A block recovered by the sweep whose own dispatch targets do not exist
    # was never code: real code calls real code.  Dropping such a block can
    # orphan whoever called it, so this repeats until it settles.  Blocks that
    # came from Ghidra's listing are never dropped - a dangling target there is
    # a translator gap, and the invariant below turns it into a build failure.
    fn_ref = re.compile(r"FN\(([0-9a-f]{8})\)")

    def dangling_targets(body, known):
        """Literal dispatch targets in this body that are not entry points.

        Both spellings count: `recomp_call`/`recomp_jump` with a constant, and
        a direct `FN(ADDR)` reference, which is a link error rather than a
        runtime one if the entry has gone."""
        out = []
        for line in body:
            for kind in ("recomp_call", "recomp_jump"):
                at = line.find(kind + "(c, 0x")
                if at < 0:
                    continue
                lit = line[at + len(kind) + 6:].split("u")[0]
                try:
                    target = int(lit, 16)
                except ValueError:
                    continue
                if target not in known and not (GUEST_SHIM_BASE <= target
                                                < GUEST_SHIM_END):
                    out.append((target, line.strip()))
            for hit in fn_ref.findall(line):
                target = int(hit, 16)
                if target not in known:
                    out.append((target, line.strip()))
        return out

    # Entries the emitter had to drop are not entry points either.
    for t in list(extra):
        if t in tr.stale_entries:
            del extra[t]

    # Structural provenance is never withdrawn; a guess may be.  See
    # STRUCTURAL_PROVENANCE for why the line is drawn there.
    withdrawn = []
    recovered_addrs = prunable_blocks({f.addr for f in recovered}, provenance)
    pruned = []
    while True:
        known = set(bodies) | set(t for t in extra if extra[t].addr in bodies)
        drop = [a for a in bodies
                if a in recovered_addrs and dangling_targets(bodies[a], known)]
        if not drop:
            break
        for a in drop:
            why = dangling_targets(bodies[a], known)
            withdrawn.append((a, provenance.get(a, "branch"),
                              why[0][0] if why else 0))
            del bodies[a]
            pruned.append(a)
        for t in [t for t, f in extra.items() if f.addr in drop]:
            del extra[t]

    ok = [fn for fn in parsed if fn.addr in bodies]
    entry_names = sorted(set([fn.addr for fn in ok])
                         | set(t for t, f in extra.items() if f.addr in bodies))

    # Table coverage, in two parts, both of them sound.  Every entry a table
    # decoded has to dispatch somewhere, and every constant-displacement
    # indirect jump has to decode a table at all - a site that decodes nothing
    # is the worst kind of gap and emits a `recomp_jump` on a computed target,
    # which no literal check can see.  Guessing at a table's real extent by
    # reading past what was decoded is not included: it flags whatever data
    # happens to follow a short table, and it disagreed with the decode's own
    # stopping rule on four sites.
    known_entries = set(bodies) | set(t for t in extra if extra[t].addr in bodies)
    table_gaps = []
    undecoded = [(f, a, b) for (f, a), b in sorted(tr.table_sites.items())
                 if f in bodies and (f, a) not in tr.jumptables]
    for fn_addr, at, base in undecoded:
        print("  jump table at %08x (in fn_%08x) reads %08x but decoded no "
              "entries at all" % (at, fn_addr, base), file=sys.stderr)
    for (fn_addr, at), targets in sorted(tr.jumptables.items()):
        if fn_addr not in bodies:
            continue
        missing = {t for t in targets if t not in known_entries}
        if missing:
            table_gaps.append((fn_addr, at, len(targets), sorted(missing)))
    for fn_addr, at, total, missing in table_gaps:
        print("  jump table at %08x (in fn_%08x): %d of %d entries have no "
              "block entry, first %s"
              % (at, fn_addr, len(missing), total,
                 " ".join("%08x" % m for m in missing[:6])), file=sys.stderr)
    if (table_gaps or undecoded) and not args.allow_table_gaps:
        raise TranslateError(
            "%d jump tables have entries that dispatch nowhere (%d slots in "
            "total) and %d table sites decoded nothing; pass "
            "--allow-table-gaps with a reason to accept them"
            % (len(table_gaps), sum(len(m) for _f, _a, _t, m in table_gaps),
               len(undecoded)))

    # Emitted-text invariant: every single-entry fn_ADDR must reach ADDR
    # before it does anything else.  Recursive descent can pull in addresses
    # below the entry, and falling into whichever sorts first made
    # fn_00565e1e start on a POP that ate its caller's return address.
    wrong_entry = []
    for fn in ok:
        body = bodies[fn.addr]
        if fn.addr in INTRINSIC_BODY:
            continue                      # a one-line call into the runtime
        if body and body[0].startswith("static void body_"):
            continue                      # multi-entry form, dispatched below
        lines = [l for l in body[1:] if l.strip()]
        if not lines:
            continue
        first = lines[0].strip()
        if (first.startswith("L_%08x:" % fn.addr)
                or first == "goto L_%08x;" % fn.addr
                or ("/* %08x " % fn.addr) in first):
            continue
        wrong_entry.append((fn.addr, first[:80]))
    if wrong_entry:
        for addr, first in wrong_entry[:20]:
            print("  fn_%08x does not begin at %08x; it begins with %s"
                  % (addr, addr, first), file=sys.stderr)
        raise TranslateError(
            "%d functions do not begin executing at their own address"
            % len(wrong_entry))

    # Build-time invariant, on what is left: a `recomp_call(c, <const>)` or
    # `recomp_jump(c, <const>)` whose target is not an entry point is a
    # translator gap, not a runtime condition.  It fails at run time deep in
    # whatever it was doing - the sprintf float path corrupted the x87 stack
    # and returned with EBP = 0 - so it has to fail here instead, by name.
    known = set(entry_names)
    dangling = []
    for fn in ok:
        for target, line in dangling_targets(bodies[fn.addr], known):
            dangling.append((fn.addr, target, line))
    if dangling:
        for caller, target, line in dangling[:20]:
            print("  fn_%08x dispatches to %08x, which is not an entry point: %s"
                  % (caller, target, line[:110]), file=sys.stderr)
        raise TranslateError(
            "%d literal dispatch targets are not entry points; every direct "
            "call and jump has to reach translated code" % len(dangling))

    # funcs.h ---------------------------------------------------------------
    with open(os.path.join(args.out, "funcs.h"), "w") as fh:
        fh.write("/* generated by tools/recomp/translate.py -- do not edit */\n")
        fh.write("#ifndef RECOMP_FUNCS_H\n#define RECOMP_FUNCS_H\n")
        fh.write('#include "x86.h"\n#include "intrinsics.h"\n')
        fh.write("/* Task 8 replacements: define FN_<addr> to a native function in a\n"
                 " * header named by RECOMP_OVERRIDE_HEADER and every direct call site,\n"
                 " * tail call and jump-table case for that address is redirected. */\n")
        fh.write("#ifdef RECOMP_OVERRIDE_HEADER\n#include RECOMP_OVERRIDE_HEADER\n#endif\n")
        fh.write("#define FN_CAT_(a, b) a##b\n#define FN_CAT(a, b) FN_CAT_(a, b)\n")
        fh.write("#define FN(a) FN_CAT(FN_, a)\n")
        fh.write("#define FIDX(a) FN_CAT(FIDX_, a)\n")
        # An unhooked call costs one acquire byte-load and a not-taken branch.
        # The load is an acquire, not a plain read, because the installer
        # publishes the pointer BEFORE the flag with a release store: the
        # acquire is what makes "flag set" imply "pointer visible" on arm64.
        fh.write("#ifdef RECOMP_NO_HOOKS\n"
                 "#define CALL_FN(a) do { if (recomp_profile_enabled) recomp_call(c, recomp_func_addrs[FIDX(a)]); else FN(a)(c); } while (0)\n"
                 "#else\n"
                 "#define CALL_FN(a) do {\\\n"
                 "    uint32_t i_ = FIDX(a);\\\n"
                 "    if (recomp_profile_enabled) { recomp_call(c, recomp_func_addrs[i_]); break; }\\\n"
                 "    if (__builtin_expect(__atomic_load_n(&recomp_hooked[i_],"
                 " __ATOMIC_ACQUIRE) != 0u, 0))\\\n"
                 "        recomp_hook_ptrs[i_](c, i_);\\\n"
                 "    else FN(a)(c);\\\n"
                 "} while (0)\n"
                 "#endif\n")
        for i, a in enumerate(entry_names):
            fh.write("#define FIDX_%08x %du\n" % (a, i))
        # Both spellings are declared: fn_ADDR because the chunk defines it,
        # and FN(ADDR) so an override header only has to #define FN_<addr> and
        # its replacement is declared here.  In the default case the two are
        # the same declaration, which C allows to repeat.
        for a in entry_names:
            fh.write("#ifndef FN_%08x\n#define FN_%08x fn_%08x\n#endif\n" % (a, a, a))
        for a in entry_names:
            fh.write("void fn_%08x(X86 *c);\nvoid FN(%08x)(X86 *c);\n" % (a, a))
        fh.write("#endif\n")

    # chunks ----------------------------------------------------------------
    nchunk = 0
    for start in range(0, len(ok), FUNCS_PER_CHUNK):
        group = ok[start:start + FUNCS_PER_CHUNK]
        path = os.path.join(args.out, "chunk_%03d.c" % nchunk)
        with open(path, "w") as fh:
            fh.write("/* generated by tools/recomp/translate.py -- do not edit */\n")
            fh.write('#include "funcs.h"\n\n')
            for fn in group:
                alt = sorted(entries_by_fn.get(fn.addr, ()))
                # The range is where the instructions lie, not a code size: a
                # block that follows a far branch spans the gap without
                # occupying it.
                fh.write("/* %s  %d insns  %08x..%08x%s */\n"
                         % (fn.name, len(fn.insns), fn.insns[0].addr,
                            fn.insns[-1].addr,
                            ("  entries: " + " ".join("%08x" % a for a in alt)) if alt else ""))
                fh.write("\n".join(bodies[fn.addr]))
                fh.write("\n\n")
        chunks.append(path)
        nchunk += 1

    # Canonical records shared by dispatch profiling and symbols.json.
    # The listing's own functions.  `parsed` is not that list here: recovered
    # blocks are appended to it as they are found, and a recovered block is not
    # a listed function.  `entries` is the functions.tsv rows themselves.
    listed = {a: n for a, n, _nb, _p in entries}
    alt_owner = {t: fn.addr for t, fn in extra.items()}

    functions = []
    for i, a in enumerate(entry_names):
        kind, hookable = hook_kind(a, listed, alt_owner, provenance,
                                   hook_evidence, INTRINSIC_BODY)
        if a in listed:
            name = listed[a]
        else:
            owner_addr = alt_owner.get(a)
            base = listed.get(owner_addr, "sub_%08x" % (owner_addr or a))
            name = "%s.%s_%08x" % (base, "alt" if kind == "alternate" else "blk", a)
        functions.append({"addr": "%08x" % a, "index": i, "name": name,
                          "kind": kind, "provenance": provenance.get(a, "listed"),
                          # Why this address is eligible, so the decision can be
                          # audited rather than taken on trust.
                          "evidence": sorted(hook_evidence.get(a, ())),
                          "hookable": hookable, "aliases": []})

    by_addr = {int(f["addr"], 16): f for f in functions}
    for alias, addr in sorted({**curated.get("entries", {}), **curated.get("aliases", {})}.items()):
        if addr in by_addr:
            by_addr[addr]["aliases"].append(alias)     # secondary, never primary

    # table.c ---------------------------------------------------------------
    with open(os.path.join(args.out, "table.c"), "w") as fh:
        fh.write("/* generated by tools/recomp/translate.py -- do not edit */\n")
        fh.write('#include <stdio.h>\n#include <stdlib.h>\n#include "funcs.h"\n\n')
        fh.write("/* Baseline and differential hosts retain exact guest timing. */\n"
                 "__attribute__((weak)) uint32_t recomp_visual_animation_tick(uint32_t tick) { return tick; }\n\n")
        fh.write("const uint32_t recomp_func_addrs[] = {\n")
        for a in entry_names:
            fh.write("    0x%08xu,\n" % a)
        fh.write("};\n")
        fh.write("const uint32_t recomp_func_count = %d;\n\n" % len(entry_names))
        fh.write("static const char *const profile_names[] = {\n")
        for symbol in functions:
            fh.write("    %s,\n" % json.dumps(symbol["name"]))
        fh.write("};\nconst char *recomp_profile_name(uint32_t i) { return i < recomp_func_count ? profile_names[i] : 0; }\n")
        fh.write("void (*const recomp_base_ptrs[])(X86 *) = {\n")
        for a in entry_names:
            fh.write("    FN(%08x),\n" % a)
        fh.write("};\n\n")
        fh.write("static void recomp_run_base(X86 *c, uint32_t i)\n"
                 "{\n    recomp_base_ptrs[i](c);\n}\n\n")
        # The untouched translations, beside the possibly-overridden ones: the
        # two differ exactly where this build defined an FN_<addr> override, so
        # the override set becomes a fact the binary states about itself.
        fh.write("void (*const recomp_raw_ptrs[])(X86 *) = {\n")
        for a in entry_names:
            fh.write("    fn_%08x,\n" % a)
        fh.write("};\n\n")
        fh.write("RecompHookFn recomp_hook_ptrs[] = {\n")
        for _a in entry_names:
            fh.write("    recomp_run_base,\n")
        fh.write("};\n")
        fh.write("uint8_t recomp_hooked[%d];\n\n" % len(entry_names))
        fh.write("""uint32_t recomp_override_count(void)
{
    uint32_t i, n = 0;
    for (i = 0; i < recomp_func_count; ++i)
        if (recomp_base_ptrs[i] != recomp_raw_ptrs[i]) ++n;
    return n;
}

uint64_t recomp_override_hash(void)
{
    uint64_t h = 1469598103934665603ull;
    uint32_t i, j;
    for (i = 0; i < recomp_func_count; ++i) {
        if (recomp_base_ptrs[i] == recomp_raw_ptrs[i]) continue;
        for (j = 0; j < 4; ++j) {
            h ^= (recomp_func_addrs[i] >> (8 * j)) & 0xffu;
            h *= 1099511628211ull;
        }
    }
    return h;
}

""")
        fh.write("""static int32_t recomp_lookup(uint32_t target)
{
    uint32_t lo = 0, hi = recomp_func_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (recomp_func_addrs[mid] < target) lo = mid + 1; else hi = mid;
    }
    return (lo < recomp_func_count && recomp_func_addrs[lo] == target)
           ? (int32_t)lo : -1;
}

int32_t recomp_index_of(uint32_t addr) { return recomp_lookup(addr); }

void recomp_call(X86 *c, uint32_t target)
{
    int32_t i = recomp_lookup(target);
    if (i >= 0) {
        if (recomp_profile_enabled) recomp_profile_push((uint32_t)i);
#ifdef RECOMP_NO_HOOKS
        recomp_base_ptrs[i](c);
#else
        if (__atomic_load_n(&recomp_hooked[i], __ATOMIC_ACQUIRE))
            recomp_hook_ptrs[i](c, (uint32_t)i);
        else
            recomp_base_ptrs[i](c);
#endif
        if (recomp_profile_enabled) recomp_profile_pop();
        return;
    }
    if (target >= GUEST_SHIM_BASE && target < GUEST_SHIM_END) {
        recomp_shim_call(c, target);
        return;
    }
    recomp_unknown_call(c, target);
}

/* Block-entry dispatch: the table holds every function start, every alternate
 * entry and every decoded jump-table target. */
void recomp_jump(X86 *c, uint32_t target)
{
    int32_t i = recomp_lookup(target);
    if (i >= 0) {
        if (recomp_profile_enabled) recomp_profile_push((uint32_t)i);
#ifdef RECOMP_NO_HOOKS
        recomp_base_ptrs[i](c);
#else
        if (__atomic_load_n(&recomp_hooked[i], __ATOMIC_ACQUIRE))
            recomp_hook_ptrs[i](c, (uint32_t)i);
        else
            recomp_base_ptrs[i](c);
#endif
        if (recomp_profile_enabled) recomp_profile_pop();
        return;
    }
    if (target >= GUEST_SHIM_BASE && target < GUEST_SHIM_END) {
        recomp_shim_call(c, target);
        return;
    }
    recomp_unknown_jump(c, target);
}

void recomp_unknown_jump(X86 *c, uint32_t target)
{
    fprintf(stderr, "recomp: no block entry for indirect jump to 0x%08x from 0x%08x\\n",
            target, c->eip);
    abort();
}
""")
    chunks.append(os.path.join(args.out, "table.c"))

    # ---- symbols.json -----------------------------------------------------
    import hashlib

    sha = hashlib.sha256(open(BINARY, "rb").read()).hexdigest()

    globals_out = []
    for section, body in sorted(curated.items()):
        if not section.startswith("globals."):
            continue
        e = {"name": section.split(".", 1)[1], "addr": "%08x" % body["addr"]}
        for k in ("size", "stride", "count"):
            if k in body:
                e[k] = body[k]
        globals_out.append(e)

    events = {k: "%08x" % v for k, v in sorted(curated.get("events", {}).items())}
    for name, addr in events.items():
        f = by_addr.get(int(addr, 16))
        if not f or not f["hookable"]:
            raise TranslateError("event %s (%s) is not a hookable entry symbol"
                                 % (name, addr))

    # Beside the generated sources, not straight into build/recomp: the build
    # publishes gen/ and the archive together only after the compile succeeds,
    # and the symbol index has to move with them or it will describe a build
    # that was never published.
    with open(os.path.join(args.out, "symbols.json"), "w") as fh:
        json.dump({"exe_sha256": sha, "image_base": "00400000",
                   "functions": functions, "globals": globals_out,
                   "events": events}, fh, indent=1)
    if not args.quiet:
        kinds = defaultdict(int)
        for f in functions:
            kinds[f["kind"]] += 1
        print("  symbols.json: %d symbols, %d hookable (%s), %d globals, "
              "%d events"
              % (len(functions), sum(1 for f in functions if f["hookable"]),
                 ", ".join("%d %s" % (n, k) for k, n in sorted(kinds.items())),
                 len(globals_out), len(events)), file=sys.stderr)

    dt = time.time() - t0
    if not args.quiet:
        print("translated %d/%d functions, %d entry points (+%d alternate, "
              "%d recovered from the PE of which %d found by the data-pointer "
              "scan, %d candidates rejected as data) in %.1fs -> %s (%d chunks)"
              % (len(ok), len(parsed), len(entry_names), len(extra),
                 len(recovered), discovered_by_scan[0], len(rejected),
                 dt, args.out, nchunk + 1))
        if withdrawn:
            print("  withdrew %d guessed blocks whose own dispatch went "
                  "nowhere: %s"
                  % (len(withdrawn),
                     ", ".join("%08x (from %s, wanted %08x)" % (a, w, t)
                               for a, w, t in sorted(withdrawn))))
        print("  jump tables: %d constant-displacement sites, %d decoded, %d "
              "entries, %d entries dispatch nowhere, %d sites decoded nothing"
              % (len(tr.table_sites), tr.stats.get("_jmp_table", 0),
                 tr.stats.get("_jmp_table_entries", 0),
                 sum(len(m) for _f, _a, _t, m in table_gaps), len(undecoded)))
        print("  discovery converged in %d rounds; %d CRT static initializers "
              "from %d __initterm tables, %d alternate entries from data "
              "pointers, %d entry points from instruction immediates, %d "
              "thunks and %d aligned targets accepted without the padding "
              "signal, %d string tails and %d instruction-interior addresses "
              "excluded, %d recovery errors"
              % (_round + 1, initterm_found[0], len(initterm_tables),
                 interior_entries[0], immediate_entries[0],
                 len(image.thunk_candidates), len(image.weak_candidates),
                 len(image.string_candidates), len(image.interior_candidates),
                 len(image.recover_errors)))
        if failures:
            print("FAILED %d functions:" % len(failures))
            for a, why in failures[:40]:
                print("  %08x  %s" % (a, why))
    if args.report:
        with open(args.report, "w") as fh:
            json.dump({
                "functions_total": len(parsed),
                "functions_ok": len(ok),
                "entry_points": len(entry_names),
                "alternate_entries": ["%08x" % a for a in sorted(extra)],
                "entry_points_from_data_pointers": discovered_by_scan[0],
                "initterm_tables": [["%08x" % a, "%08x" % b, "%08x" % c]
                                    for a, b, c in initterm_tables],
                "initterm_entries": initterm_found[0],
                "entry_points_from_immediates": immediate_entries[0],
                "alternate_entries_from_data_pointers": interior_entries[0],
                "data_pointer_candidates_rejected": len(rejected),
                "blocks_withdrawn": [["%08x" % a, why, "%08x" % t]
                                     for a, why, t in sorted(withdrawn)],
                "provenance": {k: sum(1 for v in provenance.values() if v == k)
                               for k in ("table", "initterm", "data",
                                         "immediate", "branch")},
                "table_gaps": [["%08x" % f, "%08x" % a, t,
                                ["%08x" % m for m in ms]]
                               for f, a, t, ms in table_gaps],
                "table_gaps_allowed": args.allow_table_gaps,
                "table_sites_undecoded": [["%08x" % f, "%08x" % a, "%08x" % b]
                                          for f, a, b in undecoded],
                "stale_alternate_entries": len(tr.stale_entries),
                "string_tail_candidates_excluded": len(image.string_candidates),
                "interior_candidates_excluded": len(image.interior_candidates),
                "thunk_candidates": len(image.thunk_candidates),
                "weak_candidates": len(image.weak_candidates),
                "recover_errors": image.recover_errors[:50],
                "discovery_rounds": _round + 1,
                "recovered_blocks": [["%08x" % f.addr, len(f.insns)]
                                     for f in sorted(recovered, key=lambda x: x.addr)],
                "seconds": dt,
                "failures": [["%08x" % a, w] for a, w in failures],
                "mnemonics": dict(tr.stats),
                "notes": tr.notes[:200],
            }, fh, indent=1)
    return 1 if failures else 0


def check_flags(tr, parsed):
    """Empirical check for the 'flags never cross a call' assumption."""
    entry_live = 0
    after_call = 0
    call_sites = 0
    examples = []
    for fn in parsed:
        li, lo = tr.liveness_cfg(fn, call_transparent=True)
        if li and li[0]:
            entry_live += 1
            if len(examples) < 10:
                examples.append("entry %08x live-in %s" % (fn.addr, sorted(li[0])))
        for i, ins in enumerate(fn.insns):
            if ins.mnem == "CALL":
                call_sites += 1
                if lo[i]:
                    after_call += 1
                    if len(examples) < 20:
                        examples.append("after call %08x live-out %s"
                                        % (ins.addr, sorted(lo[i])))
    print("functions with flags live at entry: %d / %d" % (entry_live, len(parsed)))
    print("call sites with flags live afterwards: %d / %d" % (after_call, call_sites))
    for e in examples:
        print("  " + e)


if __name__ == "__main__":
    sys.exit(main())
