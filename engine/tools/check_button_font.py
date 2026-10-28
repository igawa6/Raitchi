#!/usr/bin/env python3
"""Check the small generic SharedFontType::NintendoExt substitute font."""
import argparse
from pathlib import Path
import struct
import sys

MAX_BYTES = 64 * 1024
# NintendoExt is a button/icon font. Its icon codepoints are the contract; the
# generic missing-glyph square is checked for Standard/CJK fonts separately.
REQUIRED = (
    0x20, 0x2B,
    0xE090, 0xE091, 0xE092, 0xE093, 0xE094, 0xE095, 0xE096, 0xE097,
    0xE0A0, 0xE0A1, 0xE0A2, 0xE0A3, 0xE0A4, 0xE0A5, 0xE0A6, 0xE0A7, 0xE0A8, 0xE0A9,
    0xE0AB, 0xE0AC, 0xE0AD, 0xE0AE,
    0xE0E0, 0xE0E1, 0xE0E2, 0xE0E3, 0xE0E4, 0xE0E5,
    0xE0E6, 0xE0E7, 0xE0E8, 0xE0E9, 0xE0EA, 0xE0EF, 0xE0F0,
    0xE100, 0xE101, 0xE145, 0xE146,
)


def _u16(data, offset):
    if offset < 0 or offset + 2 > len(data):
        raise ValueError("truncated uint16")
    return struct.unpack_from(">H", data, offset)[0]


def _u32(data, offset):
    if offset < 0 or offset + 4 > len(data):
        raise ValueError("truncated uint32")
    return struct.unpack_from(">I", data, offset)[0]


def _mapped_format4(data, base, cp):
    if cp > 0xFFFF or base + 14 > len(data):
        return False
    length = _u16(data, base + 2)
    end = base + length
    seg_count = _u16(data, base + 6) // 2
    if length < 16 or end > len(data) or not seg_count:
        return False
    ends = base + 14
    starts = ends + 2 * seg_count + 2
    deltas = starts + 2 * seg_count
    ranges = deltas + 2 * seg_count
    if ranges + 2 * seg_count > end:
        return False
    for i in range(seg_count):
        last, first = _u16(data, ends + 2 * i), _u16(data, starts + 2 * i)
        if cp > last:
            continue
        if cp < first:
            return False
        delta = _u16(data, deltas + 2 * i)
        ro = _u16(data, ranges + 2 * i)
        if not ro:
            return ((cp + delta) & 0xFFFF) != 0
        glyph_offset = ranges + 2 * i + ro + 2 * (cp - first)
        if glyph_offset + 2 > end:
            return False
        glyph = _u16(data, glyph_offset)
        return glyph != 0 and ((glyph + delta) & 0xFFFF) != 0
    return False


def _mapped_format12(data, base, cp):
    if base + 16 > len(data):
        return False
    end = base + _u32(data, base + 4)
    count = _u32(data, base + 12)
    if end > len(data) or base + 16 + count * 12 > end:
        return False
    for i in range(count):
        pos = base + 16 + 12 * i
        first, last, glyph = _u32(data, pos), _u32(data, pos + 4), _u32(data, pos + 8)
        if first <= cp <= last:
            return glyph + cp - first != 0
        if cp < first:
            return False
    return False


def has_codepoint(data, cp):
    if len(data) < 12 or data[:4] not in (b"\x00\x01\x00\x00", b"OTTO", b"true"):
        return False
    tables = _u16(data, 4)
    cmap = None
    for i in range(tables):
        record = 12 + 16 * i
        if record + 16 > len(data):
            return False
        if data[record:record + 4] == b"cmap":
            offset, length = _u32(data, record + 8), _u32(data, record + 12)
            if offset + length > len(data) or length < 4:
                return False
            cmap = (offset, length)
            break
    if cmap is None:
        return False
    base, length = cmap
    count = _u16(data, base + 2)
    if 4 + count * 8 > length:
        return False
    for i in range(count):
        record = base + 4 + 8 * i
        platform, encoding = _u16(data, record), _u16(data, record + 2)
        if platform != 0 and not (platform == 3 and encoding in (1, 10)):
            continue
        subtable = base + _u32(data, record + 4)
        if subtable + 2 > base + length:
            continue
        fmt = _u16(data, subtable)
        if fmt == 4 and _mapped_format4(data, subtable, cp):
            return True
        if fmt == 12 and _mapped_format12(data, subtable, cp):
            return True
    return False


def check_font(data, label="font"):
    if len(data) > MAX_BYTES:
        raise ValueError(f"{label}: {len(data)} bytes exceeds {MAX_BYTES}-byte package limit")
    missing = [f"U+{cp:04X}" for cp in REQUIRED if not has_codepoint(data, cp)]
    if missing:
        raise ValueError(f"{label}: missing required cmap glyphs: {', '.join(missing)}")
    return {"bytes": len(data), "required_codepoints": [f"U+{cp:04X}" for cp in REQUIRED]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("font", type=Path)
    args = parser.parse_args()
    try:
        result = check_font(args.font.read_bytes(), str(args.font))
    except (OSError, ValueError) as exc:
        print(exc, file=sys.stderr)
        return 1
    print(f"checked {result['bytes']} bytes; cmap covers {', '.join(result['required_codepoints'])}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
