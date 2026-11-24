#!/usr/bin/env python3
"""Cell Broadband Engine SPU ELF disassembler & rodata/data dumper.

Decodes big-endian 32-bit SPU instructions (RR, RRR, RI7, RI8, RI10, RI16, RI18,
channel/DMA ops, branch hints) and dumps ELF sections + .rodata/.data as
hex/float32/uint32/ASCII.
"""

from __future__ import annotations

import argparse
import math
import struct
import sys
from dataclasses import dataclass
from pathlib import Path


def sxt(val: int, bits: int) -> int:
    sign = 1 << (bits - 1)
    return (val ^ sign) - sign


def reg_name(r: int) -> str:
    if r == 0:
        return "$lr"
    if r == 1:
        return "$sp"
    return f"${r}"


SPU_CHANNELS: dict[int, str] = {
    0: "SPU_RdEventStat",
    1: "SPU_WrEventMask",
    2: "SPU_WrEventAck",
    3: "SPU_RdSigNotify1",
    4: "SPU_RdSigNotify2",
    7: "SPU_WrDec",
    8: "SPU_RdDec",
    11: "SPU_RdEventMask",
    13: "SPU_RdMachStat",
    14: "SPU_WrSRR0",
    15: "SPU_RdSRR0",
    16: "MFC_LSA",
    17: "MFC_EAH",
    18: "MFC_EAL",
    19: "MFC_Size",
    20: "MFC_TagID",
    21: "MFC_Cmd",
    22: "MFC_WrTagMask",
    23: "MFC_WrTagUpdate",
    24: "MFC_RdTagStat",
    25: "MFC_RdListStallStat",
    26: "MFC_WrListStallAck",
    27: "MFC_RdAtomicStat",
    28: "SPU_WrOutMbox",
    29: "SPU_RdInMbox",
    30: "SPU_WrOutIntrMbox",
}

# 4-bit opcode (RRR format): op4(0..3), rt(4..10), rb(11..17), ra(18..24), rc(25..31)
RRR_OPS: dict[int, str] = {
    0x8: "selb",
    0xB: "shufb",
    0xC: "mpya",
    0xD: "fnms",
    0xE: "fma",
    0xF: "fms",
}

# 8-bit opcode (RI10 format): op8(0..7), i10(8..17), ra(18..24), rt(25..31)
RI10_OPS: dict[int, tuple[str, str]] = {
    0x04: ("ori", "s10"),
    0x05: ("orhi", "s10"),
    0x06: ("orbi", "s10"),
    0x0C: ("sfi", "s10"),
    0x0D: ("sfhi", "s10"),
    0x14: ("andi", "s10"),
    0x15: ("andhi", "s10"),
    0x16: ("andbi", "s10"),
    0x1C: ("ai", "s10"),
    0x1D: ("ahi", "s10"),
    0x24: ("stqd", "mem"),
    0x34: ("lqd", "mem"),
    0x44: ("xori", "s10"),
    0x45: ("xorhi", "s10"),
    0x46: ("xorbi", "s10"),
    0x4C: ("cgti", "s10"),
    0x4D: ("cgthi", "s10"),
    0x4E: ("cgtbi", "s10"),
    0x4F: ("hgti", "halt"),
    0x5C: ("clgti", "s10"),
    0x5D: ("clgthi", "s10"),
    0x5E: ("clgtbi", "s10"),
    0x5F: ("hlgti", "halt"),
    0x74: ("mpyi", "s10"),
    0x75: ("mpyui", "s10"),
    0x7C: ("ceqi", "s10"),
    0x7D: ("ceqhi", "s10"),
    0x7E: ("ceqbi", "s10"),
    0x7F: ("heqi", "halt"),
}

# 9-bit opcode (RI16 format): op9(0..8), i16(9..24), rt(25..31)
RI16_OPS: dict[int, tuple[str, str]] = {
    0x040: ("brz", "br_rt"),
    0x041: ("stqa", "abs_mem"),
    0x042: ("brnz", "br_rt"),
    0x044: ("brhz", "br_rt"),
    0x046: ("brhnz", "br_rt"),
    0x047: ("stqr", "rel_mem"),
    0x060: ("bra", "br_abs"),
    0x061: ("lqa", "abs_mem"),
    0x062: ("brasl", "br_abs_rt"),
    0x064: ("br", "br_rel"),
    0x065: ("fsmbi", "u16"),
    0x066: ("brsl", "br_rel_rt"),
    0x067: ("lqr", "rel_mem"),
    0x081: ("il", "s16"),
    0x082: ("ilhu", "u16"),
    0x083: ("ilh", "u16"),
    0x0C1: ("iohl", "u16"),
}

# 10-bit opcode (RI8 format): op10(0..9), i8(10..17), ra(18..24), rt(25..31)
RI8_OPS: dict[int, tuple[str, int]] = {
    0x1D8: ("cflts", 173),
    0x1D9: ("cfltu", 173),
    0x1DA: ("csflt", 155),
    0x1DB: ("cuflt", 155),
}

# 11-bit opcode (RR / RI7 / special): op11(0..10), rb(11..17), ra(18..24), rt(25..31)
RR_OPS: dict[int, tuple[str, str]] = {
    # Control / special
    0x000: ("stop", "stop"),
    0x001: ("lnop", "none"),
    0x002: ("sync", "sync"),
    0x003: ("dsync", "none"),
    0x00C: ("mfspr", "ra_rt"),
    0x00D: ("rdch", "ch"),
    0x00F: ("rchcnt", "ch"),
    0x10C: ("mtspr", "ra_rt"),
    0x10D: ("wrch", "ch"),
    0x140: ("stopd", "rr"),
    0x201: ("nop", "none"),
    0x398: ("fscrrd", "rt"),
    0x3BA: ("fscrwr", "ra"),

    # Branch indirect
    0x1A8: ("bi", "bi"),
    0x1A9: ("bisl", "bisl"),
    0x1AA: ("iret", "bi"),
    0x1AB: ("bisled", "bisl"),
    0x1AC: ("hbr", "hbr"),
    0x128: ("biz", "biz"),
    0x129: ("binz", "biz"),
    0x12A: ("bihz", "biz"),
    0x12B: ("bihnz", "biz"),

    # Load/store x-form & generate controls
    0x144: ("stqx", "memx"),
    0x1C4: ("lqx", "memx"),
    0x1F4: ("cbd", "ctl_i7"),
    0x1F5: ("chd", "ctl_i7"),
    0x1F6: ("cwd", "ctl_i7"),
    0x1F7: ("cdd", "ctl_i7"),
    0x1D4: ("cbx", "rr"),
    0x1D5: ("chx", "rr"),
    0x1D6: ("cwx", "rr"),
    0x1D7: ("cdx", "rr"),

    # Integer arithmetic
    0x040: ("sf", "rr"),
    0x048: ("sfh", "rr"),
    0x0C0: ("a", "rr"),
    0x0C8: ("ah", "rr"),
    0x0C1: ("and", "rr"),
    0x041: ("or", "rr"),
    0x241: ("xor", "rr"),
    0x2C1: ("andc", "rr"),
    0x2C9: ("orc", "rr"),
    0x049: ("nor", "rr"),
    0x249: ("eqv", "rr"),
    0x0C9: ("nand", "rr"),
    0x0C2: ("cg", "rr"),
    0x042: ("bg", "rr"),
    0x340: ("addx", "rr"),
    0x341: ("sfx", "rr"),
    0x342: ("cgx", "rr"),
    0x343: ("bgx", "rr"),
    0x3C4: ("mpy", "rr"),
    0x3CC: ("mpyu", "rr"),
    0x3C5: ("mpyh", "rr"),
    0x3C6: ("mpys", "rr"),
    0x3C7: ("mpyhh", "rr"),
    0x3CE: ("mpyhhu", "rr"),
    0x346: ("mpyhha", "rr"),
    0x34E: ("mpyhhau", "rr"),
    0x347: ("mpyh", "rr"),  # alias guard
    0x2A5: ("clz", "ra_rt"),
    0x2B4: ("cntb", "ra_rt"),
    0x1B4: ("fsmb", "ra_rt"),
    0x1B5: ("fsmh", "ra_rt"),
    0x1B6: ("fsm", "ra_rt"),
    0x1B0: ("gbb", "ra_rt"),
    0x1B1: ("gbh", "ra_rt"),
    0x1B2: ("gb", "ra_rt"),
    0x0D3: ("avgb", "rr"),
    0x053: ("absdb", "rr"),
    0x253: ("sumb", "rr"),
    0x2B6: ("xsbh", "ra_rt"),
    0x2AE: ("xshw", "ra_rt"),
    0x2A6: ("xswd", "ra_rt"),
    0x1F0: ("orx", "ra_rt"),

    # Compare / halt
    0x240: ("cgt", "rr"),
    0x248: ("cgth", "rr"),
    0x250: ("cgtb", "rr"),
    0x2C0: ("clgt", "rr"),
    0x2C8: ("clgth", "rr"),
    0x2D0: ("clgtb", "rr"),
    0x3C0: ("ceq", "rr"),
    0x3C8: ("ceqh", "rr"),
    0x3D0: ("ceqb", "rr"),
    0x258: ("hgt", "halt_rr"),
    0x2D8: ("hlgt", "halt_rr"),
    0x3D8: ("heq", "halt_rr"),

    # Shift / rotate
    0x058: ("rot", "rr"),
    0x059: ("rotm", "rr"),
    0x05A: ("rotma", "rr"),
    0x05B: ("shl", "rr"),
    0x05C: ("roth", "rr"),
    0x05D: ("rothm", "rr"),
    0x05E: ("rotmah", "rr"),
    0x05F: ("shlh", "rr"),
    0x078: ("roti", "ri7"),
    0x079: ("rotmi", "ri7"),
    0x07A: ("rotmai", "ri7"),
    0x07B: ("shli", "ri7"),
    0x07C: ("rothi", "ri7"),
    0x07D: ("rothmi", "ri7"),
    0x07E: ("rotmahi", "ri7"),
    0x07F: ("shlhi", "ri7"),
    0x1D8: ("rotqbi", "rr"),
    0x1D9: ("rotqmbi", "rr"),
    0x1DB: ("shlqbi", "rr"),
    0x1DC: ("rotqby", "rr"),
    0x1DD: ("rotqmby", "rr"),
    0x1DF: ("shlqby", "rr"),
    0x1CC: ("rotqbybi", "rr"),
    0x1CD: ("rotqmbybi", "rr"),
    0x1CF: ("shlqbybi", "rr"),
    0x1F8: ("rotqbii", "ri7"),
    0x1F9: ("rotqmbii", "ri7"),
    0x1FB: ("shlqbii", "ri7"),
    0x1FC: ("rotqbyi", "ri7"),
    0x1FD: ("rotqmbyi", "ri7"),
    0x1FF: ("shlqbyi", "ri7"),

    # Floating point
    0x2C4: ("fa", "rr"),
    0x2C5: ("fs", "rr"),
    0x2C6: ("fm", "rr"),
    0x2CC: ("dfa", "rr"),
    0x2CD: ("dfs", "rr"),
    0x2CE: ("dfm", "rr"),
    0x35C: ("dfma", "rr"),
    0x35D: ("dfms", "rr"),
    0x35E: ("dfnms", "rr"),
    0x35F: ("dfnma", "rr"),
    0x1B8: ("frest", "ra_rt"),
    0x1B9: ("frsqest", "ra_rt"),
    0x3D4: ("fi", "rr"),
    0x3B9: ("frds", "ra_rt"),
    0x3B8: ("fesd", "ra_rt"),
    0x3C2: ("fceq", "rr"),
    0x3CA: ("fcmeq", "rr"),
    0x2C2: ("fcgt", "rr"),
    0x2CA: ("fcmgt", "rr"),
    0x3C3: ("dfceq", "rr"),
    0x3CB: ("dfcmeq", "rr"),
    0x2C3: ("dfcgt", "rr"),
    0x2CB: ("dfcmgt", "rr"),
    0x357: ("dftsv", "ri7"),
}


def decode_insn(word: int, pc: int) -> tuple[str, int | None]:
    """Decode one 32-bit big-endian SPU instruction.

    Returns (disassembly_text, branch_or_mem_target_addr_or_None).
    """
    # 1. RRR format (4-bit opcode)
    op4 = (word >> 28) & 0xF
    if op4 in RRR_OPS:
        mnem = RRR_OPS[op4]
        rt = (word >> 21) & 0x7F
        rb = (word >> 14) & 0x7F
        ra = (word >> 7) & 0x7F
        rc = word & 0x7F
        return f"{mnem:<9s} {reg_name(rt)}, {reg_name(ra)}, {reg_name(rb)}, {reg_name(rc)}", None

    # 2. RI18 format (7-bit opcode) + hbra/hbrr
    op7 = (word >> 25) & 0x7F
    if op7 == 0x21:  # ila
        i18 = (word >> 7) & 0x3FFFF
        rt = word & 0x7F
        return f"{'ila':<9s} {reg_name(rt)}, 0x{i18:x}", i18
    if op7 in (0x08, 0x09):  # hbra / hbrr
        mnem = "hbra" if op7 == 0x08 else "hbrr"
        ro_hi = (word >> 23) & 0x3
        i16 = sxt((word >> 7) & 0xFFFF, 16)
        ro_lo = word & 0x7F
        ro = sxt((ro_hi << 7) | ro_lo, 9) << 2
        branch_addr = (pc + ro) & 0x3FFFF
        if op7 == 0x08:
            target = (i16 << 2) & 0x3FFFF
        else:
            target = (pc + (i16 << 2)) & 0x3FFFF
        return f"{mnem:<9s} 0x{branch_addr:x}, 0x{target:x}", target

    # 3. RI8 format (10-bit opcode: csflt, cuflt, cflts, cfltu)
    op10 = (word >> 22) & 0x3FF
    if op10 in RI8_OPS:
        mnem, bias = RI8_OPS[op10]
        i8 = (word >> 14) & 0xFF
        ra = (word >> 7) & 0x7F
        rt = word & 0x7F
        scale = bias - i8
        return f"{mnem:<9s} {reg_name(rt)}, {reg_name(ra)}, {scale}", None

    # 4. RI10 format (8-bit opcode)
    op8 = (word >> 24) & 0xFF
    if op8 in RI10_OPS:
        mnem, kind = RI10_OPS[op8]
        s10 = sxt((word >> 14) & 0x3FF, 10)
        ra = (word >> 7) & 0x7F
        rt = word & 0x7F
        if kind == "mem":
            off = s10 << 4
            return f"{mnem:<9s} {reg_name(rt)}, {off}({reg_name(ra)})", None
        if kind == "halt":
            return f"{mnem:<9s} {reg_name(ra)}, {s10}", None
        return f"{mnem:<9s} {reg_name(rt)}, {reg_name(ra)}, {s10}", None

    # 5. RI16 format (9-bit opcode)
    op9 = (word >> 23) & 0x1FF
    if op9 in RI16_OPS:
        mnem, kind = RI16_OPS[op9]
        u16 = (word >> 7) & 0xFFFF
        s16 = sxt(u16, 16)
        rt = word & 0x7F
        if kind == "br_rt":
            target = (pc + (s16 << 2)) & 0x3FFFF
            return f"{mnem:<9s} {reg_name(rt)}, 0x{target:x}", target
        if kind == "br_rel":
            target = (pc + (s16 << 2)) & 0x3FFFF
            return f"{mnem:<9s} 0x{target:x}", target
        if kind == "br_rel_rt":
            target = (pc + (s16 << 2)) & 0x3FFFF
            return f"{mnem:<9s} {reg_name(rt)}, 0x{target:x}", target
        if kind == "br_abs":
            target = (s16 << 2) & 0x3FFFF
            return f"{mnem:<9s} 0x{target:x}", target
        if kind == "br_abs_rt":
            target = (s16 << 2) & 0x3FFFF
            return f"{mnem:<9s} {reg_name(rt)}, 0x{target:x}", target
        if kind == "abs_mem":
            target = (s16 << 2) & 0x3FFFF
            return f"{mnem:<9s} {reg_name(rt)}, 0x{target:x}", target
        if kind == "rel_mem":
            target = (pc + (s16 << 2)) & 0x3FFFF
            return f"{mnem:<9s} {reg_name(rt)}, 0x{target:x}", target
        if kind == "s16":
            return f"{mnem:<9s} {reg_name(rt)}, {s16} (0x{u16:04x})", None
        return f"{mnem:<9s} {reg_name(rt)}, 0x{u16:04x}", None

    # 6. RR / RI7 / special (11-bit opcode)
    op11 = (word >> 21) & 0x7FF
    if op11 in RR_OPS:
        mnem, kind = RR_OPS[op11]
        rb = (word >> 14) & 0x7F
        ra = (word >> 7) & 0x7F
        rt = word & 0x7F
        if kind == "none":
            return mnem, None
        if kind == "sync":
            c = (word >> 20) & 1
            return (f"{mnem}c" if c else mnem), None
        if kind == "stop":
            code = word & 0x3FFF
            return f"{mnem:<9s} 0x{code:x}", None
        if kind == "rt":
            return f"{mnem:<9s} {reg_name(rt)}", None
        if kind == "ra":
            return f"{mnem:<9s} {reg_name(ra)}", None
        if kind == "ra_rt":
            return f"{mnem:<9s} {reg_name(rt)}, {reg_name(ra)}", None
        if kind == "ch":
            ch_name = SPU_CHANNELS.get(ra, f"$ch{ra}")
            return f"{mnem:<9s} {reg_name(rt)}, {ch_name}", None
        if kind == "rr":
            return f"{mnem:<9s} {reg_name(rt)}, {reg_name(ra)}, {reg_name(rb)}", None
        if kind == "halt_rr":
            return f"{mnem:<9s} {reg_name(ra)}, {reg_name(rb)}", None
        if kind == "memx":
            return f"{mnem:<9s} {reg_name(rt)}, {reg_name(ra)}, {reg_name(rb)}", None
        if kind == "ri7":
            s7 = sxt(rb, 7)
            return f"{mnem:<9s} {reg_name(rt)}, {reg_name(ra)}, {s7}", None
        if kind == "ctl_i7":
            s7 = sxt(rb, 7)
            return f"{mnem:<9s} {reg_name(rt)}, {s7}({reg_name(ra)})", None
        if kind == "bi":
            de = ((word >> 19) & 1, (word >> 18) & 1)
            sfx = ("d" if de[0] else "") + ("e" if de[1] else "")
            return f"{mnem + sfx:<9s} {reg_name(ra)}", None
        if kind == "bisl":
            de = ((word >> 19) & 1, (word >> 18) & 1)
            sfx = ("d" if de[0] else "") + ("e" if de[1] else "")
            return f"{mnem + sfx:<9s} {reg_name(rt)}, {reg_name(ra)}", None
        if kind == "biz":
            de = ((word >> 19) & 1, (word >> 18) & 1)
            sfx = ("d" if de[0] else "") + ("e" if de[1] else "")
            return f"{mnem + sfx:<9s} {reg_name(rt)}, {reg_name(ra)}", None
        if kind == "hbr":
            p = (word >> 18) & 1
            ro = sxt((((word >> 14) & 0x3) << 7) | rt, 9) << 2
            branch_addr = (pc + ro) & 0x3FFFF
            m = "hbrp" if p else "hbr"
            return f"{m:<9s} 0x{branch_addr:x}, {reg_name(ra)}", None

    return f".word     0x{word:08x}  # UNKNOWN op4=0x{op4:x} op8=0x{op8:02x} op9=0x{op9:03x} op11=0x{op11:03x}", None


@dataclass
class ElfSection:
    name: str
    sh_type: int
    sh_flags: int
    sh_addr: int
    sh_offset: int
    sh_size: int
    sh_addralign: int
    data: bytes


def parse_elf32_be(blob: bytes) -> tuple[int, list[ElfSection]]:
    if blob[:4] != b"\x7fELF" or blob[4] != 1 or blob[5] != 2:
        raise ValueError("Not a big-endian 32-bit ELF file")
    e_entry, e_phoff, e_shoff = struct.unpack_from(">III", blob, 24)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from(">HHH", blob, 46)

    raw_sh = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        fields = struct.unpack_from(">IIIIIIIIII", blob, off)
        raw_sh.append(fields)

    shstr_off = raw_sh[e_shstrndx][4]
    shstr_size = raw_sh[e_shstrndx][5]
    shstr = blob[shstr_off : shstr_off + shstr_size]

    def get_name(idx: int) -> str:
        end = shstr.find(b"\x00", idx)
        return shstr[idx:end].decode("ascii", errors="replace")

    sections: list[ElfSection] = []
    for sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size, _lk, _inf, sh_al, _es in raw_sh:
        data = b"" if sh_type == 8 else blob[sh_offset : sh_offset + sh_size]
        sections.append(
            ElfSection(
                name=get_name(sh_name),
                sh_type=sh_type,
                sh_flags=sh_flags,
                sh_addr=sh_addr,
                sh_offset=sh_offset,
                sh_size=sh_size,
                sh_addralign=sh_al,
                data=data,
            )
        )
    return e_entry, sections


def format_f32(u: int) -> str:
    f = struct.unpack(">f", struct.pack(">I", u))[0]
    if math.isnan(f) or math.isinf(f):
        return f"0x{u:08x}"
    if u == 0:
        return "0.0"
    if u == 0x80000000:
        return "-0.0"
    if abs(f) >= 1e-6 and abs(f) < 1e7:
        return f"{f:.6g}"
    return f"0x{u:08x}"


def dump_rodata_map(sections: list[ElfSection]) -> dict[int, bytes]:
    mem: dict[int, bytes] = {}
    for sec in sections:
        if sec.name in (".rodata", ".data") and sec.data:
            for off in range(0, len(sec.data), 16):
                chunk = sec.data[off : off + 16]
                if len(chunk) == 16:
                    mem[sec.sh_addr + off] = chunk
    return mem


def format_qword_comment(qw: bytes) -> str:
    # Check if looks like printable ASCII string
    printable = sum(1 for b in qw if 32 <= b < 127)
    if printable >= 6 and all(b == 0 or 32 <= b < 127 or b in (9, 10, 13) for b in qw):
        s = qw.split(b"\x00", 1)[0].decode("ascii", errors="replace")
        return f'str="{s}"'
    u = struct.unpack(">IIII", qw)
    # Check if looks like a shuffle mask (all bytes < 0x20 or >= 0x80)
    if all((b < 0x20 or b in (0x80, 0xC0, 0xE0)) for b in qw) and any(b != 0 for b in qw) and not all(b < 0x20 for b in qw[:4]):
        return "shuf=" + " ".join(f"{b:02x}" for b in qw)
    fs = [format_f32(x) for x in u]
    return f"f32=({', '.join(fs)}) u32=({u[0]:08x} {u[1]:08x} {u[2]:08x} {u[3]:08x})"


def disassemble_elf(elf_path: Path, out_path: Path | None) -> tuple[int, int]:
    blob = elf_path.read_bytes()
    entry, sections = parse_elf32_be(blob)
    ro_map = dump_rodata_map(sections)

    lines: list[str] = []
    lines.append(f"; SPU ELF Disassembly: {elf_path}")
    lines.append(f"; Entry point: 0x{entry:05x}")
    lines.append("; Sections:")
    for s in sections:
        if s.name:
            lines.append(
                f";   {s.name:<16s} addr=0x{s.sh_addr:05x} size=0x{s.sh_size:05x} align={s.sh_addralign}"
            )
    lines.append("")

    # Pass 1: collect call targets (brsl / brasl) to mark functions
    call_targets: set[int] = {entry}
    for sec in sections:
        if sec.name not in (".init", ".text", ".fini"):
            continue
        for off in range(0, len(sec.data), 4):
            pc = sec.sh_addr + off
            w = struct.unpack_from(">I", sec.data, off)[0]
            op9 = (w >> 23) & 0x1FF
            if op9 == 0x066:  # brsl
                s16 = sxt((w >> 7) & 0xFFFF, 16)
                call_targets.add((pc + (s16 << 2)) & 0x3FFFF)
            elif op9 == 0x062:  # brasl
                s16 = sxt((w >> 7) & 0xFFFF, 16)
                call_targets.add((s16 << 2) & 0x3FFFF)

    unknown_count = 0
    total_insns = 0

    for sec in sections:
        if sec.name not in (".init", ".text", ".fini"):
            continue
        lines.append(f"; ==================== SECTION {sec.name} (0x{sec.sh_addr:05x}..0x{sec.sh_addr + sec.sh_size:05x}) ====================")
        for off in range(0, len(sec.data), 4):
            pc = sec.sh_addr + off
            if pc in call_targets:
                tag = "ENTRY" if pc == entry else "FUNC"
                lines.append("")
                lines.append(f"sub_{pc:05x}:  ; [{tag}]")
            w = struct.unpack_from(">I", sec.data, off)[0]
            text, target = decode_insn(w, pc)
            total_insns += 1
            if "UNKNOWN" in text:
                unknown_count += 1
            comment = ""
            if target is not None and target in ro_map:
                comment = f"  ; [{target:05x}] {format_qword_comment(ro_map[target])}"
            elif target is not None and target in call_targets and (text.startswith("brsl") or text.startswith("brasl") or text.startswith("br ") or text.startswith("bra ")):
                comment = f"  ; -> sub_{target:05x}"
            lines.append(f"  {pc:05x}:  {w:08x}  {text}{comment}")
        lines.append("")

    # Dump .rodata and .data
    for sec in sections:
        if sec.name not in (".rodata", ".data", ".ctors", ".dtors"):
            continue
        lines.append(f"; ==================== SECTION {sec.name} (0x{sec.sh_addr:05x}..0x{sec.sh_addr + sec.sh_size:05x}) ====================")
        for off in range(0, len(sec.data), 16):
            addr = sec.sh_addr + off
            chunk = sec.data[off : off + 16]
            hex_str = " ".join(f"{b:02x}" for b in chunk)
            if len(chunk) == 16:
                u = struct.unpack(">IIII", chunk)
                f_str = ", ".join(f"{format_f32(x):>10s}" for x in u)
                ascii_str = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
                lines.append(f"  {addr:05x}:  {hex_str:<47s}  | f32=({f_str})  |{ascii_str}|")
            else:
                lines.append(f"  {addr:05x}:  {hex_str}")
        lines.append("")

    output = "\n".join(lines) + "\n"
    if out_path:
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(output)
    else:
        sys.stdout.write(output)

    return total_insns, unknown_count


def main() -> None:
    ap = argparse.ArgumentParser(description="Cell SPU ELF disassembler")
    ap.add_argument("elf", type=Path, help="Input SPU ELF file")
    ap.add_argument("-o", "--output", type=Path, default=None, help="Output .asm file")
    args = ap.parse_args()
    total, unk = disassemble_elf(args.elf, args.output)
    print(f"{args.elf.name}: {total} instructions decoded, {unk} unknown", file=sys.stderr)


if __name__ == "__main__":
    main()
