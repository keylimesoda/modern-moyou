#!/usr/bin/env python3
"""
PS3 RSX Cg Binary Shader Decoder (.vpo / .fpo)

Parses Sony PS3 CgBinaryProgram files (profiles 0x1b5b SCE_VP_RSX and
0x1b5c SCE_FP_RSX), including:
  - Header & Vertex/Fragment program metadata
  - Parameter table (attributes, uniforms, samplers, types, registers,
    default values, embedded constant offsets)
  - NV40 / RSX Vertex Program (VP) 128-bit microcode disassembly
  - NV40 / RSX Fragment Program (FP) 128-bit microcode disassembly
    (including inline 128-bit float4 constants mapped back to parameter names)

Usage:
  python3 decode_rsx_shaders.py <file_or_dir> [...] [--raw]
"""

import argparse
import os
import struct
import sys
from typing import Dict, List, Optional, Tuple

# ---------------------------------------------------------------------------
# Cg Binary Enums (PS3 SDK / libcg)
# ---------------------------------------------------------------------------

CG_PROFILES = {
    0x1B5B: "SCE_VP_RSX",
    0x1B5C: "SCE_FP_RSX",
}

CG_TYPES = {
    0x415: "float",
    0x416: "float2",
    0x417: "float3",
    0x418: "float4",
    0x419: "float1x1",
    0x41A: "float1x2",
    0x41B: "float1x3",
    0x41C: "float1x4",
    0x41D: "float2x1",
    0x41E: "float2x2",
    0x41F: "float2x3",
    0x420: "float2x4",
    0x421: "float3x1",
    0x422: "float3x2",
    0x423: "float3x3",
    0x424: "float3x4",
    0x425: "float4x1",
    0x426: "float4x2",
    0x427: "float4x3",
    0x428: "float4x4",
    0x429: "sampler1D",
    0x42A: "sampler2D",
    0x42B: "sampler3D",
    0x42C: "samplerRECT",
    0x42D: "samplerCUBE",
    0x42E: "half",
    0x42F: "half2",
    0x430: "half3",
    0x431: "half4",
    0x43E: "half3x3",
    0x443: "half4x4",
    0x444: "fixed",
    0x445: "fixed2",
    0x446: "fixed3",
    0x447: "fixed4",
    0x456: "int",
    0x457: "int2",
    0x458: "int3",
    0x459: "int4",
    0x46C: "bool",
    0x46D: "bool2",
    0x46E: "bool3",
    0x46F: "bool4",
}

CG_VARIABILITY = {
    0x1005: "VARYING",
    0x1006: "UNIFORM",
    0x1007: "CONSTANT",
    0x1008: "MIXED",
}

CG_DIRECTION = {
    0x1001: "IN",
    0x1002: "OUT",
    0x1003: "INOUT",
}

CG_RESOURCES = {
    0x800: "TEXUNIT0",
    0x801: "TEXUNIT1",
    0x802: "TEXUNIT2",
    0x803: "TEXUNIT3",
    0x804: "TEXUNIT4",
    0x805: "TEXUNIT5",
    0x806: "TEXUNIT6",
    0x807: "TEXUNIT7",
    0x808: "TEXUNIT8",
    0x809: "TEXUNIT9",
    0x80A: "TEXUNIT10",
    0x80B: "TEXUNIT11",
    0x80C: "TEXUNIT12",
    0x80D: "TEXUNIT13",
    0x80E: "TEXUNIT14",
    0x80F: "TEXUNIT15",
    0x841: "ATTR0",
    0x842: "ATTR1",
    0x843: "ATTR2",
    0x844: "ATTR3",
    0x845: "ATTR4",
    0x846: "ATTR5",
    0x847: "ATTR6",
    0x848: "ATTR7",
    0x849: "ATTR8",
    0x84A: "ATTR9",
    0x84B: "ATTR10",
    0x84C: "ATTR11",
    0x84D: "ATTR12",
    0x84E: "ATTR13",
    0x84F: "ATTR14",
    0x850: "ATTR15",
    0x882: "C",
    0x8C3: "HPOS",
    0x8C5: "COL0",
    0x8C6: "COL1",
    0x8C7: "BFC0",
    0x8C8: "BFC1",
    0x8C9: "FOGC",
    0x8CA: "PSIZ",
    0x945: "WPOS",
    0xAC5: "COL0",
    0xAC6: "COL1",
    0xAC7: "COL2",
    0xAC8: "COL3",
    0xAC9: "DEPR",
    0xC94: "TEX0",
    0xC95: "TEX1",
    0xC96: "TEX2",
    0xC97: "TEX3",
    0xC98: "TEX4",
    0xC99: "TEX5",
    0xC9A: "TEX6",
    0xC9B: "TEX7",
    0xC9C: "TEX8",
    0xC9D: "TEX9",
    0xCB8: "UNDEFINED",
    0xF64: "WPOS",
    0xF65: "FACE",
}

# ---------------------------------------------------------------------------
# NV40 / RSX Vertex Program (VP) ISA Tables
# ---------------------------------------------------------------------------

VP_VEC_OPCODES = {
    0x00: "NOP",
    0x01: "MOV",
    0x02: "MUL",
    0x03: "ADD",
    0x04: "MAD",
    0x05: "DP3",
    0x06: "DPH",
    0x07: "DP4",
    0x08: "DST",
    0x09: "MIN",
    0x0A: "MAX",
    0x0B: "SLT",
    0x0C: "SGE",
    0x0D: "ARL",
    0x0E: "FRC",
    0x0F: "FLR",
    0x10: "SEQ",
    0x11: "SFL",
    0x12: "SGT",
    0x13: "SLE",
    0x14: "SNE",
    0x15: "STR",
    0x16: "SSG",
    0x17: "ARR",
    0x18: "ARA",
    0x19: "TXL",
}

VP_SCA_OPCODES = {
    0x00: "NOP",
    0x01: "MOV",
    0x02: "RCP",
    0x03: "RCC",
    0x04: "RSQ",
    0x05: "EXP",
    0x06: "LOG",
    0x07: "LIT",
    0x08: "BRA",
    0x09: "BRI",
    0x0A: "CAL",
    0x0B: "CLI",
    0x0C: "RET",
    0x0D: "LG2",
    0x0E: "EX2",
    0x0F: "SIN",
    0x10: "COS",
    0x11: "BRB",
    0x12: "CLB",
    0x13: "PSH",
    0x14: "POP",
}

VP_OUTPUT_NAMES = {
    0: "o[HPOS]",
    1: "o[COL0]",
    2: "o[COL1]",
    3: "o[BFC0]",
    4: "o[BFC1]",
    5: "o[FOGC]",
    6: "o[PSIZ]",
    7: "o[TEX0]",
    8: "o[TEX1]",
    9: "o[TEX2]",
    10: "o[TEX3]",
    11: "o[TEX4]",
    12: "o[TEX5]",
    13: "o[TEX6]",
    14: "o[TEX7]",
    15: "o[TEX8]",
    16: "o[TEX9]",
    17: "o[CLP0]",
    18: "o[CLP1]",
    19: "o[CLP2]",
    20: "o[CLP3]",
    21: "o[CLP4]",
}

VP_COND_NAMES = ["FL", "LT", "EQ", "LE", "GT", "NE", "GE", "TR"]
SWZ_CHARS = "xyzw"

# ---------------------------------------------------------------------------
# NV40 / RSX Fragment Program (FP) ISA Tables
# ---------------------------------------------------------------------------

FP_OPCODES = {
    0x00: "NOP",
    0x01: "MOV",
    0x02: "MUL",
    0x03: "ADD",
    0x04: "MAD",
    0x05: "DP3",
    0x06: "DP4",
    0x07: "DST",
    0x08: "MIN",
    0x09: "MAX",
    0x0A: "SLT",
    0x0B: "SGE",
    0x0C: "SLE",
    0x0D: "SGT",
    0x0E: "SNE",
    0x0F: "SEQ",
    0x10: "FRC",
    0x11: "FLR",
    0x12: "KIL",
    0x13: "PK4",
    0x14: "UP4",
    0x15: "DDX",
    0x16: "DDY",
    0x17: "TEX",
    0x18: "TXP",
    0x19: "TXD",
    0x1A: "RCP",
    0x1B: "RSQ",
    0x1C: "EX2",
    0x1D: "LG2",
    0x1E: "LIT",
    0x1F: "LRP",
    0x20: "STR",
    0x21: "SFL",
    0x22: "COS",
    0x23: "SIN",
    0x24: "PK2",
    0x25: "UP2",
    0x26: "POW",
    0x27: "PKB",
    0x28: "UPB",
    0x29: "PK16",
    0x2A: "UP16",
    0x2B: "BEM",
    0x2C: "PKG",
    0x2D: "UPG",
    0x2E: "DP2A",
    0x2F: "TXL",
    0x31: "TXB",
    0x33: "TEXBEM",
    0x34: "TXPBEM",
    0x35: "BEMLUM",
    0x36: "REFL",
    0x37: "TIMESWTEX",
    0x38: "DP2",
    0x39: "NRM",
    0x3A: "DIV",
    0x3B: "DIVSQ",
    0x3C: "LIF",
    0x3D: "FENCT",
    0x3E: "FENCB",
    0x3F: "BRK",
}

FP_BRANCH_OPCODES = {
    0x00: "CAL",
    0x01: "IFE",
    0x02: "LOOP",
    0x03: "REP",
    0x04: "RET",
}

FP_ATTR_NAMES = {
    0: "WPOS",
    1: "COL0",
    2: "COL1",
    3: "FOGC",
    4: "TEX0",
    5: "TEX1",
    6: "TEX2",
    7: "TEX3",
    8: "TEX4",
    9: "TEX5",
    10: "TEX6",
    11: "TEX7",
    12: "TEX8",
    13: "TEX9",
    14: "SSA",
    15: "FACE",
}

FP_PREC_SUFFIX = {0: "R", 1: "H", 2: "X", 3: "?"}
FP_SCALE_STR = {
    0: "",
    1: "*2",
    2: "*4",
    3: "*8",
    4: "",
    5: "/2",
    6: "/4",
    7: "/8",
}

# Number of source operands per FP opcode
FP_SRC_COUNT = {
    "NOP": 0, "KIL": 0, "FENCT": 0, "FENCB": 0, "BRK": 0, "RET": 0,
    "MOV": 1, "FRC": 1, "FLR": 1, "DDX": 1, "DDY": 1, "RCP": 1, "RSQ": 1,
    "EX2": 1, "LG2": 1, "LIT": 1, "COS": 1, "SIN": 1, "NRM": 1, "LIF": 1,
    "PK4": 1, "UP4": 1, "PK2": 1, "UP2": 1, "PKB": 1, "UPB": 1,
    "PK16": 1, "UP16": 1, "PKG": 1, "UPG": 1,
    "TEX": 1, "TXP": 1,
    "MUL": 2, "ADD": 2, "DP3": 2, "DP4": 2, "DP2": 2, "DST": 2,
    "MIN": 2, "MAX": 2, "SLT": 2, "SGE": 2, "SLE": 2, "SGT": 2,
    "SNE": 2, "SEQ": 2, "STR": 2, "SFL": 2, "POW": 2, "DIV": 2,
    "DIVSQ": 2, "REFL": 2, "TXB": 2, "TXL": 2, "BEM": 2, "TEXBEM": 2,
    "TXPBEM": 2, "BEMLUM": 2,
    "MAD": 3, "LRP": 3, "DP2A": 3, "TXD": 3,
}

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def read_cstr(data: bytes, offset: int) -> str:
    if not offset or offset >= len(data):
        return ""
    end = data.find(b"\x00", offset)
    if end == -1:
        end = len(data)
    return data[offset:end].decode("ascii", errors="replace")


def fmt_swz(sx: int, sy: int, sz: int, sw: int) -> str:
    s = SWZ_CHARS[sx] + SWZ_CHARS[sy] + SWZ_CHARS[sz] + SWZ_CHARS[sw]
    if s == "xyzw":
        return ""
    if s[0] == s[1] == s[2] == s[3]:
        return "." + s[0]
    return "." + s


def fmt_mask_vp(mask: int) -> str:
    # VP mask: bit 3 = x, bit 2 = y, bit 1 = z, bit 0 = w
    if mask == 0xF:
        return ""
    s = ""
    if mask & 8: s += "x"
    if mask & 4: s += "y"
    if mask & 2: s += "z"
    if mask & 1: s += "w"
    return "." + s if s else ".none"


def fmt_mask_fp(mask: int) -> str:
    # FP mask: bit 0 = x, bit 1 = y, bit 2 = z, bit 3 = w
    if mask == 0xF:
        return ""
    s = ""
    if mask & 1: s += "x"
    if mask & 2: s += "y"
    if mask & 4: s += "z"
    if mask & 8: s += "w"
    return "." + s if s else ".none"


# ---------------------------------------------------------------------------
# VP Disassembler
# ---------------------------------------------------------------------------

def decode_vp_src(bits: int, input_src: int, const_src: int, abs_bit: int,
                  indexed_const: bool, addr_reg: int, addr_swz: int,
                  const_names: Dict[int, str], attr_names: Dict[int, str]) -> str:
    reg_type = bits & 0x3
    tmp_src = (bits >> 2) & 0x3F
    swz_w = (bits >> 8) & 0x3
    swz_z = (bits >> 10) & 0x3
    swz_y = (bits >> 12) & 0x3
    swz_x = (bits >> 14) & 0x3
    neg = (bits >> 16) & 0x1

    if reg_type == 0:
        return "NONE"
    elif reg_type == 1:
        base = f"R{tmp_src}"
    elif reg_type == 2:
        aname = attr_names.get(input_src)
        base = f"v[{input_src}:{aname}]" if aname else f"v[{input_src}]"
    elif reg_type == 3:
        if indexed_const:
            idx_str = f"A{addr_reg}.{SWZ_CHARS[addr_swz]}"
            cname = const_names.get(const_src)
            if cname:
                base = f"c[{const_src}+{idx_str}:{cname}]"
            else:
                base = f"c[{const_src}+{idx_str}]"
        else:
            cname = const_names.get(const_src)
            base = f"c[{const_src}:{cname}]" if cname else f"c[{const_src}]"
    else:
        base = "?"

    swz = fmt_swz(swz_x, swz_y, swz_z, swz_w)
    expr = base + swz
    if abs_bit:
        expr = f"|{expr}|"
    if neg:
        expr = f"-{expr}"
    return expr


def disasm_vp_instruction(idx: int, d0: int, d1: int, d2: int, d3: int,
                          const_names: Dict[int, str],
                          attr_names: Dict[int, str],
                          out_names: Dict[int, str]) -> str:
    # d0 fields
    addr_swz = d0 & 0x3
    cond_swz_w = (d0 >> 2) & 0x3
    cond_swz_z = (d0 >> 4) & 0x3
    cond_swz_y = (d0 >> 6) & 0x3
    cond_swz_x = (d0 >> 8) & 0x3
    cond_func = (d0 >> 10) & 0x7
    cond_test_en = (d0 >> 13) & 0x1
    cond_reg_sel = (d0 >> 14) & 0x1
    dst_tmp = (d0 >> 15) & 0x3F
    src0_abs = (d0 >> 21) & 0x1
    src1_abs = (d0 >> 22) & 0x1
    src2_abs = (d0 >> 23) & 0x1
    addr_reg = (d0 >> 24) & 0x1
    cond_upd_en_1 = (d0 >> 25) & 0x1
    cond_upd_en_0 = (d0 >> 26) & 0x1
    # bits 27..29 = st_sat / tex_unit / etc., bit 30 = vec_result_is_output_only
    vec_out_only = (d0 >> 30) & 0x1

    # d1 fields
    sca_op = (d1 >> 27) & 0x1F
    vec_op = (d1 >> 22) & 0x1F
    const_src = (d1 >> 12) & 0x3FF
    input_src = (d1 >> 8) & 0xF

    # sources
    src0_bits = ((d1 & 0xFF) << 9) | ((d2 >> 23) & 0x1FF)
    src1_bits = (d2 >> 6) & 0x1FFFF
    src2_bits = ((d2 & 0x3F) << 11) | ((d3 >> 21) & 0x7FF)

    # d3 fields
    sca_mask = (d3 >> 17) & 0xF
    vec_mask = (d3 >> 13) & 0xF
    sca_dst_tmp = (d3 >> 7) & 0x3F
    out_dst = (d3 >> 2) & 0x1F
    indexed_const = bool((d3 >> 1) & 0x1)
    end_bit = d3 & 0x1

    s0 = decode_vp_src(src0_bits, input_src, const_src, src0_abs, indexed_const, addr_reg, addr_swz, const_names, attr_names)
    s1 = decode_vp_src(src1_bits, input_src, const_src, src1_abs, indexed_const, addr_reg, addr_swz, const_names, attr_names)
    s2 = decode_vp_src(src2_bits, input_src, const_src, src2_abs, indexed_const, addr_reg, addr_swz, const_names, attr_names)

    cond_str = ""
    if cond_test_en:
        cswz = fmt_swz(cond_swz_x, cond_swz_y, cond_swz_z, cond_swz_w)
        cond_str = f" ({VP_COND_NAMES[cond_func]}{cond_reg_sel}{cswz})"

    parts = []

    if vec_op != 0:
        opname = VP_VEC_OPCODES.get(vec_op, f"VEC_{vec_op:#x}")
        if cond_upd_en_0 or cond_upd_en_1:
            opname += "C"
        dsts = []
        if out_dst != 0x1F and vec_mask:
            oname = out_names.get(out_dst, VP_OUTPUT_NAMES.get(out_dst, f"o[{out_dst}]"))
            dsts.append(oname + fmt_mask_vp(vec_mask))
        if dst_tmp != 0x3F and not vec_out_only and vec_mask:
            dsts.append(f"R{dst_tmp}" + fmt_mask_vp(vec_mask))
        if opname.startswith("ARL") or opname.startswith("ARR"):
            dst_str = f"A{addr_reg}" + fmt_mask_vp(vec_mask)
        else:
            dst_str = ", ".join(dsts) if dsts else "RC" + fmt_mask_vp(vec_mask)

        if opname.startswith(("MOV", "FRC", "FLR", "ARL", "ARR", "SSG")):
            srcs = [s0]
        elif opname.startswith("ADD"):
            srcs = [s0, s2]
        elif opname.startswith("MAD"):
            srcs = [s0, s1, s2]
        elif opname.startswith("TXL"):
            srcs = [s0, f"tex{const_src & 3}"]
        elif opname.startswith(("SFL", "STR")):
            srcs = []
        else:
            srcs = [s0, s1]
        parts.append(f"{opname:<5} {dst_str}{cond_str}" + (f", {', '.join(srcs)}" if srcs else ""))

    if sca_op != 0:
        opname = VP_SCA_OPCODES.get(sca_op, f"SCA_{sca_op:#x}")
        if opname in ("BRA", "BRI", "CAL", "CLI", "BRB", "CLB"):
            target = ((d2 & 0x3F) << 3) | ((d3 >> 29) & 0x7)
            parts.append(f"{opname:<5} L{target:03d}{cond_str}")
        elif opname == "RET":
            parts.append(f"RET{cond_str}")
        else:
            dsts = []
            if sca_dst_tmp != 0x3F and sca_mask:
                dsts.append(f"R{sca_dst_tmp}" + fmt_mask_vp(sca_mask))
            if out_dst != 0x1F and vec_op == 0 and sca_mask:
                oname = out_names.get(out_dst, VP_OUTPUT_NAMES.get(out_dst, f"o[{out_dst}]"))
                dsts.append(oname + fmt_mask_vp(sca_mask))
            dst_str = ", ".join(dsts) if dsts else "RC" + fmt_mask_vp(sca_mask)
            parts.append(f"{opname:<5} {dst_str}{cond_str}, {s2}")

    if not parts:
        parts.append("NOP")

    line = f"  {idx:03d}: " + "  |  ".join(parts)
    if end_bit:
        line += "  [END]"
    return line


# ---------------------------------------------------------------------------
# FP Disassembler
# ---------------------------------------------------------------------------

def fp_swap_words(data: bytes, offset: int) -> Tuple[int, int, int, int]:
    w = struct.unpack_from(">4I", data, offset)
    return tuple(((x & 0xFFFF) << 16) | ((x >> 16) & 0xFFFF) for x in w)


def fp_read_const_f4(data: bytes, offset: int) -> Tuple[float, float, float, float]:
    w = fp_swap_words(data, offset)
    raw = struct.pack(">4I", *w)
    return struct.unpack(">4f", raw)


def decode_fp_src(w: int, src_attr: int, const_val: Optional[Tuple[float, float, float, float]],
                  const_name: Optional[str], attr_names: Dict[int, str],
                  abs_bit: int) -> str:
    reg_type = w & 0x3
    tmp_idx = (w >> 2) & 0x3F
    fp16 = (w >> 8) & 0x1
    swz_x = (w >> 9) & 0x3
    swz_y = (w >> 11) & 0x3
    swz_z = (w >> 13) & 0x3
    swz_w = (w >> 15) & 0x3
    neg = (w >> 17) & 0x1

    swz = fmt_swz(swz_x, swz_y, swz_z, swz_w)
    if reg_type == 0:
        base = f"{'H' if fp16 else 'R'}{tmp_idx}"
    elif reg_type == 1:
        aname = attr_names.get(src_attr, FP_ATTR_NAMES.get(src_attr, f"ATTR{src_attr}"))
        base = f"f[{aname}]"
    elif reg_type == 2:
        if const_name and const_val is not None:
            vstr = ",".join(f"{v:.5g}" for v in const_val)
            base = f"const({const_name}=<{vstr}>)"
        elif const_name:
            base = f"const({const_name})"
        elif const_val is not None:
            vstr = ",".join(f"{v:.5g}" for v in const_val)
            base = f"<{vstr}>"
        else:
            base = "const(?)"
    else:
        base = f"unk{reg_type}"

    expr = base + swz
    if abs_bit:
        expr = f"|{expr}|"
    if neg:
        expr = f"-{expr}"
    return expr


def disasm_fp(data: bytes, uoff: int, usize: int,
              embedded_map: Dict[int, str],
              attr_names: Dict[int, str],
              tex_names: Dict[int, str],
              raw: bool = False) -> List[str]:
    lines: List[str] = []
    pos = 0
    inst_idx = 0
    while pos < usize:
        w0, w1, w2, w3 = fp_swap_words(data, uoff + pos)
        end_bit = w0 & 0x1
        dst_reg = (w0 >> 1) & 0x3F
        dst_fp16 = (w0 >> 7) & 0x1
        no_dest = (w0 >> 8) & 0x1
        mask = (w0 >> 9) & 0xF
        src_attr = (w0 >> 13) & 0xF
        tex_num = (w0 >> 17) & 0xF
        prec = (w0 >> 22) & 0x3
        opcode_lo = (w0 >> 24) & 0x3F
        set_cond = (w0 >> 30) & 0x1
        saturate = (w0 >> 31) & 0x1

        is_branch = bool((w1 >> 31) & 0x1)
        cond_func = (w1 >> 18) & 0x7
        cond_swz_x = (w1 >> 21) & 0x3
        cond_swz_y = (w1 >> 23) & 0x3
        cond_swz_z = (w1 >> 25) & 0x3
        cond_swz_w = (w1 >> 27) & 0x3
        s0_abs = (w1 >> 29) & 0x1
        s1_abs = (w2 >> 29) & 0x1
        s2_abs = (w3 >> 29) & 0x1
        scale = (w2 >> 28) & 0x7  # actually bits 28..30 in w2? Wait: bit 29 is abs, bits 28..30:
        # In NV40 FP: w2 bits 28..30 = scale (0..7) ONLY when not abs; actually w2 bit 18 = s1_abs, w1 bit 18..20 = cond!
        # Let's use the standard NV40 FP bit positions:
        # w1: bit 29 = s0_abs, bits 18..20 = cond_func, bits 21..28 = cond_swz
        # w2: bit 18 = s1_abs, bits 28..30 = scale
        # w3: bit 18 = s2_abs
        s1_abs = (w2 >> 18) & 0x1
        s2_abs = (w3 >> 18) & 0x1
        scale = (w2 >> 28) & 0x7

        if is_branch:
            opname = FP_BRANCH_OPCODES.get(opcode_lo, f"BR_{opcode_lo:#x}")
            raw_str = f"  [{w0:08x} {w1:08x} {w2:08x} {w3:08x}]" if raw else ""
            lines.append(f"  {pos:04x}: {opname} (cond={VP_COND_NAMES[cond_func]}){raw_str}")
            pos += 16
            inst_idx += 1
            if end_bit:
                break
            continue

        opname = FP_OPCODES.get(opcode_lo, f"OP_{opcode_lo:#x}")
        nsrc = FP_SRC_COUNT.get(opname, 3)

        uses_const = any(((w & 0x3) == 2) for w in (w1, w2, w3)[:nsrc])
        const_val = None
        const_name = None
        next_pos = pos + 16
        if uses_const and next_pos < usize:
            const_val = fp_read_const_f4(data, uoff + next_pos)
            const_name = embedded_map.get(next_pos)
            next_pos += 16

        s_words = [w1, w2, w3]
        s_abs = [s0_abs, s1_abs, s2_abs]
        srcs = [
            decode_fp_src(s_words[i], src_attr, const_val, const_name, attr_names, s_abs[i])
            for i in range(nsrc)
        ]

        if opname in ("TEX", "TXP", "TXB", "TXL", "TXD", "TEXBEM", "TXPBEM"):
            tname = tex_names.get(tex_num)
            srcs.append(f"tex{tex_num}({tname})" if tname else f"tex{tex_num}")

        # Build mnemonic
        mnem = opname + FP_PREC_SUFFIX.get(prec, "")
        if set_cond:
            mnem += "C"
        if saturate:
            mnem += "_SAT"
        if scale and scale in FP_SCALE_STR:
            mnem += FP_SCALE_STR[scale]

        if no_dest or opname in ("NOP", "KIL"):
            dst_str = "RC" if set_cond else ""
        else:
            dst_str = f"{'H' if dst_fp16 else 'R'}{dst_reg}{fmt_mask_fp(mask)}"

        cond_str = ""
        if cond_func != 7:
            cswz = fmt_swz(cond_swz_x, cond_swz_y, cond_swz_z, cond_swz_w)
            cond_str = f" ({VP_COND_NAMES[cond_func]}{cswz})"

        if dst_str and srcs:
            body = f"{mnem:<10} {dst_str}{cond_str}, {', '.join(srcs)}"
        elif dst_str:
            body = f"{mnem:<10} {dst_str}{cond_str}"
        elif srcs:
            body = f"{mnem:<10} {', '.join(srcs)}{cond_str}"
        else:
            body = f"{mnem}{cond_str}"

        if end_bit:
            body += "  [END]"
        if raw:
            body += f"  ; {w0:08x} {w1:08x} {w2:08x} {w3:08x}"

        lines.append(f"  {pos:04x}: {body}")
        pos = next_pos
        inst_idx += 1
        if end_bit:
            break

    return lines


# ---------------------------------------------------------------------------
# Top-level Parser
# ---------------------------------------------------------------------------

def parse_cg_binary(path: str, raw: bool = False) -> str:
    with open(path, "rb") as f:
        data = f.read()

    if len(data) < 32:
        raise ValueError(f"{path}: file too small ({len(data)} bytes)")

    profile, rev, total_size, param_count, param_arr, prog_off, ucode_size, ucode_off = (
        struct.unpack_from(">8I", data, 0)
    )
    prof_name = CG_PROFILES.get(profile, f"UNKNOWN_0x{profile:x}")
    out: List[str] = []
    out.append(f"========================================================================")
    out.append(f"FILE: {path}")
    out.append(f"Profile: {prof_name} (0x{profile:04x}), Rev: {rev}, Size: {total_size}B, "
               f"Params: {param_count}, Ucode: {ucode_size}B ({ucode_size // 16} slots)")

    if profile == 0x1B5B:
        icnt, islot, rcnt, ain, aout, uclip = struct.unpack_from(">6I", data, prog_off)
        out.append(f"VP Meta: inst={icnt}, slot={islot}, regs={rcnt}, "
                   f"attrIn=0x{ain:04x}, attrOut=0x{aout:08x}, clip=0x{uclip:x}")
    elif profile == 0x1B5C:
        icnt, ain, ptex, tc_in, tc_2d, tc_cent, rcnt, out_h0, drep, pkill = (
            struct.unpack_from(">3I3H4B", data, prog_off)
        )
        out.append(f"FP Meta: inst={icnt}, regs={rcnt}, outH0={out_h0}, "
                   f"depthRep={drep}, kill={pkill}, attrIn=0x{ain:04x}, "
                   f"texIn=0x{tc_in:04x}, tex2D=0x{tc_2d:04x}")

    const_names: Dict[int, str] = {}
    attr_names: Dict[int, str] = {}
    out_names: Dict[int, str] = {}
    tex_names: Dict[int, str] = {}
    embedded_map: Dict[int, str] = {}

    out.append("\nParameters:")
    for i in range(param_count):
        off = param_arr + i * 48
        typ, res, var, ridx, name_o, def_o, emb_o, sem_o, direc, pno, ref, shared = (
            struct.unpack_from(">3Ii8I", data, off)
        )
        tname = CG_TYPES.get(typ, f"0x{typ:x}")
        rname = CG_RESOURCES.get(res, f"0x{res:x}")
        vname = CG_VARIABILITY.get(var, f"0x{var:x}")
        dname = CG_DIRECTION.get(direc, f"0x{direc:x}")
        name = read_cstr(data, name_o)
        sem = read_cstr(data, sem_o)

        emb_offsets: List[int] = []
        if emb_o:
            ecnt = struct.unpack_from(">I", data, emb_o)[0]
            emb_offsets = list(struct.unpack_from(f">{ecnt}I", data, emb_o + 4))
            for eo in emb_offsets:
                embedded_map[eo] = name

        dval_str = ""
        if def_o:
            vals = struct.unpack_from(">4f", data, def_o)
            dval_str = " default=(" + ", ".join(f"{v:.5g}" for v in vals) + ")"

        # Populate symbol tables for disassembly
        if ref:
            if res == 0x882 and ridx >= 0:
                # Prefer element name like _MVM[0] over matrix _MVM
                if ridx not in const_names or "[" in name:
                    const_names[ridx] = name
            elif 0x800 <= res <= 0x80F:
                tex_names[res - 0x800] = name
            elif 0x841 <= res <= 0x850 and direc == 0x1001:
                attr_names[res - 0x841] = name
            elif profile == 0x1B5B and direc == 0x1002:
                # VP outputs
                vp_out_map = {
                    0x8C3: 0, 0x8C5: 1, 0x8C6: 2, 0x8C7: 3, 0x8C8: 4, 0x8C9: 5, 0x8CA: 6,
                    0xC94: 7, 0xC95: 8, 0xC96: 9, 0xC97: 10, 0xC98: 11, 0xC99: 12,
                    0xC9A: 13, 0xC9B: 14, 0xC9C: 15, 0xC9D: 16,
                }
                if res in vp_out_map:
                    out_names[vp_out_map[res]] = f"o[{rname}:{name}]"
            elif profile == 0x1B5C and direc == 0x1001:
                fp_in_map = {
                    0x945: 0, 0xF64: 0, 0xAC5: 1, 0xAC6: 2, 0x8C9: 3,
                    0xC94: 4, 0xC95: 5, 0xC96: 6, 0xC97: 7, 0xC98: 8,
                    0xC99: 9, 0xC9A: 10, 0xC9B: 11, 0xC9C: 12, 0xC9D: 13,
                    0xF65: 15,
                }
                if res in fp_in_map:
                    attr_names[fp_in_map[res]] = f"{rname}:{name}"

        reg_desc = f"{rname}[{ridx}]" if ridx >= 0 else rname
        emb_str = f" emb={[hex(x) for x in emb_offsets]}" if emb_offsets else ""
        sem_str = f" : {sem}" if sem else ""
        ref_str = "" if ref else " [UNUSED]"
        out.append(f"  {dname:<5} {vname:<8} {tname:<10} {name:<24} @ {reg_desc:<12}{sem_str}{dval_str}{emb_str}{ref_str}")

    out.append("\nDisassembly:")
    if profile == 0x1B5B:
        for i in range(0, ucode_size, 16):
            d0, d1, d2, d3 = struct.unpack_from(">4I", data, ucode_off + i)
            line = disasm_vp_instruction(i // 16, d0, d1, d2, d3, const_names, attr_names, out_names)
            if raw:
                line += f"  ; {d0:08x} {d1:08x} {d2:08x} {d3:08x}"
            out.append(line)
    elif profile == 0x1B5C:
        out.extend(disasm_fp(data, ucode_off, ucode_size, embedded_map, attr_names, tex_names, raw=raw))
    else:
        out.append("  (unsupported profile)")

    return "\n".join(out)


def main() -> int:
    ap = argparse.ArgumentParser(description="Decode PS3 RSX Cg binary shaders (.vpo/.fpo)")
    ap.add_argument("paths", nargs="+", help="Shader files or directories")
    ap.add_argument("--raw", action="store_true", help="Include raw 128-bit hex words")
    args = ap.parse_args()

    files: List[str] = []
    for p in args.paths:
        if os.path.isdir(p):
            for root, _, fnames in os.walk(p):
                for fn in sorted(fnames):
                    if fn.endswith((".vpo", ".fpo")):
                        files.append(os.path.join(root, fn))
        else:
            files.append(p)

    for fp in files:
        print(parse_cg_binary(fp, raw=args.raw))
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
