"""DDS -> Black Ops II IWI (version 27) conversion.

The T6 OAT bridge (``bsp-compilation-2``) only loads images from
``images/<name>.iwi`` (see ``LoaderImageT6.cpp``); DDS files in the search path
are ignored. This module converts the DDS files written by the T4 unlinker into
the exact layout OAT's ``iwi::LoadIwi27`` accepts:

* 4-byte version tag ``IWi`` + 27
* 60-byte header (format, flags, w/h/d, gamma, maxGlossForMip[16],
  fileSizeForPicmip[8])
* mip levels stored smallest-first, all faces of a mip contiguous

Only formats OAT maps for IWI27 are produced. Anything else raises
``IwiError``; nothing is silently re-encoded or replaced.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass
from pathlib import Path

# iwi27::IwiFormat
FMT_RGBA = 0x1
FMT_RGB = 0x2
FMT_LUMINANCE_ALPHA = 0x3
FMT_LUMINANCE = 0x4
FMT_ALPHA = 0x5
FMT_DXT1 = 0xB
FMT_DXT3 = 0xC
FMT_DXT5 = 0xD
FMT_DXN = 0xE

# iwi27::IwiFlags
FLAG_NOMIPMAPS = 1 << 1
FLAG_CUBEMAP = 1 << 2
FLAG_VOLMAP = 1 << 3

# DDS constants
_DDPF_ALPHAPIXELS = 0x1
_DDPF_ALPHA = 0x2
_DDPF_FOURCC = 0x4
_DDPF_RGB = 0x40
_DDPF_LUMINANCE = 0x20000
_DDSCAPS2_CUBEMAP = 0x200
_DDSCAPS2_VOLUME = 0x200000

# DXGI formats that may appear behind a DX10 header
_DXGI_TO_IWI = {
    71: FMT_DXT1, 72: FMT_DXT1,   # BC1
    74: FMT_DXT3, 75: FMT_DXT3,   # BC2
    77: FMT_DXT5, 78: FMT_DXT5,   # BC3
    83: FMT_DXN, 84: FMT_DXN,     # BC5
    28: FMT_RGBA, 29: FMT_RGBA,   # R8G8B8A8
    65: FMT_ALPHA,                # A8
    61: FMT_LUMINANCE,            # R8
    49: FMT_LUMINANCE_ALPHA,      # R8G8
}
_FOURCC_TO_IWI = {
    b"DXT1": FMT_DXT1,
    b"DXT2": FMT_DXT3,
    b"DXT3": FMT_DXT3,
    b"DXT4": FMT_DXT5,
    b"DXT5": FMT_DXT5,
    b"ATI2": FMT_DXN,
    b"BC5U": FMT_DXN,
}
_BLOCK_BYTES = {FMT_DXT1: 8, FMT_DXT3: 16, FMT_DXT5: 16, FMT_DXN: 16}
_PIXEL_BYTES = {FMT_RGBA: 4, FMT_RGB: 3, FMT_LUMINANCE_ALPHA: 2, FMT_LUMINANCE: 1, FMT_ALPHA: 1}


class IwiError(ValueError):
    pass


@dataclass
class DdsImage:
    width: int
    height: int
    depth: int
    mip_count: int
    faces: int
    volume: bool
    iwi_format: int
    # pixel conversion applied to uncompressed data before writing
    swizzle: str | None
    src_bpp: int
    data: bytes


def full_mip_count(width: int, height: int, depth: int = 1) -> int:
    """Mirror of OAT Texture*::GetMipMapCount (bit length of the max dim)."""
    return max(width, height, depth).bit_length()


def mip_size(fmt: int, width: int, height: int, depth: int, level: int) -> int:
    w = max(1, width >> level)
    h = max(1, height >> level)
    d = max(1, depth >> level)
    if fmt in _BLOCK_BYTES:
        return ((w + 3) // 4) * ((h + 3) // 4) * d * _BLOCK_BYTES[fmt]
    return w * h * d * _PIXEL_BYTES[fmt]


def _src_mip_size(img: DdsImage, level: int) -> int:
    w = max(1, img.width >> level)
    h = max(1, img.height >> level)
    d = max(1, img.depth >> level)
    if img.iwi_format in _BLOCK_BYTES:
        return mip_size(img.iwi_format, img.width, img.height, img.depth, level)
    return w * h * d * img.src_bpp // 8


def _mask_shift(mask: int) -> int:
    return (mask & -mask).bit_length() - 1 if mask else -1


def read_dds(blob: bytes) -> DdsImage:
    if blob[:4] != b"DDS ":
        raise IwiError("not a DDS file")
    (hsize, flags, height, width, _pitch, depth, mips) = struct.unpack_from("<7I", blob, 4)
    if hsize != 124:
        raise IwiError(f"unexpected DDS header size {hsize}")
    pf_size, pf_flags, fourcc, bpp, rmask, gmask, bmask, amask = struct.unpack_from("<2I4s5I", blob, 76)
    _caps, caps2, _c3, _c4 = struct.unpack_from("<4I", blob, 108)
    offset = 128
    mips = max(1, mips)
    depth = max(1, depth)
    cube = bool(caps2 & _DDSCAPS2_CUBEMAP)
    volume = bool(caps2 & _DDSCAPS2_VOLUME) and depth > 1
    faces = 6 if cube else 1
    swizzle = None
    src_bpp = bpp

    if pf_flags & _DDPF_FOURCC:
        if fourcc == b"DX10":
            dxgi, dim, misc, array_size, _m2 = struct.unpack_from("<5I", blob, 128)
            offset += 20
            if dxgi not in _DXGI_TO_IWI:
                raise IwiError(f"unsupported DXGI format {dxgi}")
            fmt = _DXGI_TO_IWI[dxgi]
            if misc & 0x4:
                cube, faces = True, 6
            if array_size not in (0, 1) and not cube:
                raise IwiError("texture arrays are not supported by IWI")
            src_bpp = {FMT_RGBA: 32, FMT_ALPHA: 8, FMT_LUMINANCE: 8, FMT_LUMINANCE_ALPHA: 16}.get(fmt, 0)
        elif fourcc in _FOURCC_TO_IWI:
            fmt = _FOURCC_TO_IWI[fourcc]
        else:
            raise IwiError(f"unsupported FourCC {fourcc!r}")
    else:
        has_alpha = bool(pf_flags & (_DDPF_ALPHAPIXELS | _DDPF_ALPHA)) and amask
        if pf_flags & _DDPF_LUMINANCE:
            if bpp == 8 and not has_alpha:
                fmt = FMT_LUMINANCE
            elif bpp == 16 and has_alpha and rmask == 0xFF and amask == 0xFF00:
                fmt = FMT_LUMINANCE_ALPHA
            else:
                raise IwiError(f"unsupported luminance layout bpp={bpp}")
        elif (pf_flags & _DDPF_ALPHA) and not (pf_flags & _DDPF_RGB) and bpp == 8:
            fmt = FMT_ALPHA
        elif pf_flags & _DDPF_RGB and bpp in (24, 32):
            shifts = tuple(_mask_shift(m) for m in (rmask, gmask, bmask))
            if any(s < 0 or s % 8 for s in shifts):
                raise IwiError(f"unsupported RGB masks {rmask:#x}/{gmask:#x}/{bmask:#x}")
            ashift = _mask_shift(amask) if has_alpha else -1
            fmt = FMT_RGBA if ashift >= 0 else FMT_RGB
            # swizzle string: byte index in source for R,G,B(,A)
            order = [s // 8 for s in shifts] + ([ashift // 8] if ashift >= 0 else [])
            identity = [0, 1, 2, 3][: len(order)]
            if bpp == 24 and fmt == FMT_RGB and order == identity:
                swizzle = None
            elif bpp == 32 and fmt == FMT_RGBA and order == identity:
                swizzle = None
            else:
                swizzle = ",".join(str(i) for i in order)
        else:
            raise IwiError(f"unsupported uncompressed pixel format flags={pf_flags:#x} bpp={bpp}")

    img = DdsImage(width, height, depth, mips, faces, volume, fmt, swizzle, src_bpp, b"")
    needed = sum(_src_mip_size(img, lvl) for lvl in range(mips)) * faces
    data = blob[offset:offset + needed]
    if len(data) != needed:
        raise IwiError(f"truncated DDS pixel data ({len(data)} of {needed} bytes)")
    img.data = data
    return img


def _convert_pixels(img: DdsImage, chunk: bytes) -> bytes:
    if img.swizzle is None:
        return chunk
    order = [int(i) for i in img.swizzle.split(",")]
    stride = img.src_bpp // 8
    out_px = len(order)
    count = len(chunk) // stride
    out = bytearray(count * out_px)
    for dst, src in enumerate(order):
        out[dst::out_px] = chunk[src::stride]
    return bytes(out)


def _split_levels(img: DdsImage) -> list[bytes]:
    """Return per-mip data with all faces contiguous (OAT/IWI ordering).

    OAT's DdsWriter already writes mip-major (all faces per mip). Standard DDS
    cubemaps are face-major; we detect OAT output by assuming mip-major, which
    is what every file in the unlinker output uses. A face-major cube can be
    requested with ``face_major=True`` via ``dds_to_iwi``.
    """
    levels = []
    pos = 0
    for lvl in range(img.mip_count):
        size = _src_mip_size(img, lvl) * img.faces
        levels.append(img.data[pos:pos + size])
        pos += size
    return levels


def _split_levels_face_major(img: DdsImage) -> list[bytes]:
    per_face = []
    pos = 0
    for _face in range(img.faces):
        mips = []
        for lvl in range(img.mip_count):
            size = _src_mip_size(img, lvl)
            mips.append(img.data[pos:pos + size])
            pos += size
        per_face.append(mips)
    return [b"".join(per_face[f][lvl] for f in range(img.faces)) for lvl in range(img.mip_count)]


def dds_to_iwi(blob: bytes, face_major: bool = False) -> bytes:
    img = read_dds(blob)
    levels = _split_levels_face_major(img) if (face_major and img.faces > 1) else _split_levels(img)
    levels = [_convert_pixels(img, lvl) for lvl in levels]

    full = full_mip_count(img.width, img.height, img.depth if img.volume else 1)
    flags = 0
    if img.mip_count >= full:
        levels = levels[:full]
    else:
        # OAT's IWI27 loader only understands "full chain" or "base level only".
        flags |= FLAG_NOMIPMAPS
        levels = levels[:1]
    if img.faces == 6:
        flags |= FLAG_CUBEMAP
    if img.volume:
        flags |= FLAG_VOLMAP

    depth = img.depth if img.volume else 1
    for lvl, data in enumerate(levels):
        expected = mip_size(img.iwi_format, img.width, img.height, depth, lvl) * img.faces
        if len(data) != expected:
            raise IwiError(f"mip {lvl}: {len(data)} bytes, expected {expected}")

    header_size = 4 + 60
    file_sizes = [0] * 8
    total = header_size
    for lvl in range(len(levels) - 1, -1, -1):
        total += len(levels[lvl])
        if lvl < 8:
            file_sizes[lvl] = total

    out = bytearray()
    out += b"IWi" + bytes([27])
    out += struct.pack("<bbHHHf", img.iwi_format, flags, img.width, img.height, depth, 0.0)
    out += bytes(16)
    out += struct.pack("<8I", *file_sizes)
    assert len(out) == header_size
    for lvl in range(len(levels) - 1, -1, -1):
        out += levels[lvl]
    return bytes(out)


def solid_rgba_iwi(width: int, height: int, rgba: tuple[int, int, int, int]) -> bytes:
    """Uncompressed single-level RGBA IWI27 filled with one colour.

    Only used for images whose content is defined rather than extracted
    (the flat bridge lightmap, specular maps whose source no longer exists).
    """
    out = bytearray()
    out += b"IWi" + bytes([27])
    out += struct.pack("<bbHHHf", FMT_RGBA, FLAG_NOMIPMAPS, width, height, 1, 0.0)
    out += bytes(16)
    total = 64 + width * height * 4
    out += struct.pack("<8I", total, *([0] * 7))
    out += bytes(rgba) * (width * height)
    return bytes(out)


def paged_rgba_iwi(page_width: int, page_height: int, pages: list[tuple[int, int, int, int]]) -> bytes:
    """Uncompressed RGBA IWI27 of vertically stacked solid-colour pages
    (T6 lightmaps stack their pages this way: height = pages * page height)."""
    width, height = page_width, page_height * len(pages)
    out = bytearray()
    out += b"IWi" + bytes([27])
    out += struct.pack("<bbHHHf", FMT_RGBA, FLAG_NOMIPMAPS, width, height, 1, 0.0)
    out += bytes(16)
    out += struct.pack("<8I", 64 + width * height * 4, *([0] * 7))
    for rgba in pages:
        out += bytes(rgba) * (page_width * page_height)
    return bytes(out)


def solid_cube_rgba_iwi(size: int, rgba: tuple[int, int, int, int]) -> bytes:
    """Uncompressed single-level RGBA IWI27 cube map, every face one colour
    (the bridge reflection probe: stock probes are tiny 4x4 cubes too)."""
    out = bytearray()
    out += b"IWi" + bytes([27])
    out += struct.pack("<bbHHHf", FMT_RGBA, FLAG_NOMIPMAPS | FLAG_CUBEMAP, size, size, 1, 0.0)
    out += bytes(16)
    out += struct.pack("<8I", 64 + size * size * 4 * 6, *([0] * 7))
    out += bytes(rgba) * (size * size * 6)
    return bytes(out)


def read_iwi_header(blob: bytes) -> dict:
    if blob[:3] != b"IWi" or blob[3] != 27:
        raise IwiError("not an IWI v27 file")
    fmt, flags, w, h, d, gamma = struct.unpack_from("<bbHHHf", blob, 4)
    sizes = struct.unpack_from("<8I", blob, 32)
    return {"format": fmt, "flags": flags, "width": w, "height": h, "depth": d, "fileSizeForPicmip": list(sizes)}


def convert_file(src: Path, dst: Path) -> dict:
    blob = dds_to_iwi(src.read_bytes())
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_bytes(blob)
    return read_iwi_header(blob)
