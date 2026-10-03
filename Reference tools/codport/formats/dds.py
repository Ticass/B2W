"""
DDS header reading and re-encoding.

DDS is the pivot format between everything else and IWI: OpenAssetTools'
ImageConverter takes a DDS and produces a BO2 IWI.

The one trap that matters, learned the hard way: **Pillow's default DDS output
is uncompressed ARGB with no FourCC**, and ImageConverter rejects it with
"Unknown IWI format: 0". A DDS destined for BO2 must be block compressed, so
`ensure_block_compressed` exists and every path that writes a DDS goes through
it.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from pathlib import Path

from codport.codir.image import PixelFormat, level_sizes

_DDS_MAGIC = b"DDS "
_HEADER_SIZE = 124

# DDSD / DDPF flags we care about.
_DDPF_FOURCC = 0x4
_DDPF_RGB = 0x40
_DDPF_ALPHAPIXELS = 0x1
_DDSCAPS2_CUBEMAP = 0x200
#: One bit per cube face; all six together is what a complete cubemap sets.
_DDSCAPS2_CUBEMAP_ALLFACES = 0xFC00

# Flags a mipmapped DDS must advertise, or readers stop at level 0.
_DDSD_CAPS = 0x1
_DDSD_HEIGHT = 0x2
_DDSD_WIDTH = 0x4
_DDSD_PIXELFORMAT = 0x1000
_DDSD_MIPMAPCOUNT = 0x20000
_DDSD_LINEARSIZE = 0x80000
_DDSCAPS_COMPLEX = 0x8
_DDSCAPS_MIPMAP = 0x400000
_DDSCAPS_TEXTURE = 0x1000

_FOURCC_TO_FORMAT: dict[bytes, PixelFormat] = {
    b"DXT1": PixelFormat.DXT1,
    b"DXT2": PixelFormat.DXT3,
    b"DXT3": PixelFormat.DXT3,
    b"DXT4": PixelFormat.DXT5,
    b"DXT5": PixelFormat.DXT5,
    b"ATI1": PixelFormat.BC4,
    b"BC4U": PixelFormat.BC4,
    b"ATI2": PixelFormat.ATI2,
    b"BC5U": PixelFormat.BC5,
}

#: What Pillow wants in `pixel_format=` for each target.
PILLOW_PIXEL_FORMAT: dict[PixelFormat, str] = {
    PixelFormat.DXT1: "DXT1",
    PixelFormat.DXT3: "DXT3",
    PixelFormat.DXT5: "DXT5",
    PixelFormat.ATI2: "DXT5",   # Pillow cannot write BC5; DXT5 is the fallback
    PixelFormat.BC5: "DXT5",
}


@dataclass(slots=True)
class DdsInfo:
    path: str = ""
    width: int = 0
    height: int = 0
    depth: int = 1
    mip_levels: int = 1
    format: PixelFormat = PixelFormat.UNKNOWN
    fourcc: str = ""
    has_alpha: bool = False
    is_cubemap: bool = False
    #: 6 for a cubemap, 1 otherwise. A sky material samples a cube, so losing
    #: five of the faces renders the sky black rather than merely wrong.
    face_count: int = 1
    byte_size: int = 0
    valid: bool = False
    error: str = ""

    @property
    def block_compressed(self) -> bool:
        return self.format.block_compressed

    def to_dict(self) -> dict[str, object]:
        return {
            "path": self.path,
            "width": self.width,
            "height": self.height,
            "mip_levels": self.mip_levels,
            "format": str(self.format),
            "fourcc": self.fourcc,
            "has_alpha": self.has_alpha,
            "is_cubemap": self.is_cubemap,
            "byte_size": self.byte_size,
            "valid": self.valid,
            "error": self.error,
        }


def read_dds_header(path: str | Path) -> DdsInfo:
    """Read a DDS header. Never raises."""
    p = Path(path)
    info = DdsInfo(path=str(p))
    try:
        info.byte_size = p.stat().st_size
        with p.open("rb") as fh:
            blob = fh.read(148)
    except OSError as exc:
        info.error = str(exc)
        return info

    if len(blob) < 128 or blob[:4] != _DDS_MAGIC:
        info.error = "not a DDS file"
        return info

    size, flags, height, width, _pitch, depth, mipcount = struct.unpack_from("<7I", blob, 4)
    if size != _HEADER_SIZE:
        info.error = f"unexpected DDS header size {size}"
        return info

    info.width, info.height = width, height
    info.depth = max(1, depth)
    info.mip_levels = max(1, mipcount)

    # Pixel format block starts at offset 4 + 72.
    pf_flags = struct.unpack_from("<I", blob, 80)[0]
    fourcc = blob[84:88]
    rgb_bits = struct.unpack_from("<I", blob, 88)[0]
    # caps1 is at header offset 104 and caps2 at 108, so caps2 is 4 + 108 into
    # the file. Reading four bytes further lands on caps3, which is always zero
    # -- and a cubemap that reports itself flat gets flattened to one face.
    caps2 = struct.unpack_from("<I", blob, 4 + 108)[0]

    info.is_cubemap = bool(caps2 & _DDSCAPS2_CUBEMAP)
    info.face_count = 6 if info.is_cubemap else 1
    info.has_alpha = bool(pf_flags & _DDPF_ALPHAPIXELS)

    if pf_flags & _DDPF_FOURCC:
        info.fourcc = fourcc.decode("ascii", "replace")
        info.format = _FOURCC_TO_FORMAT.get(fourcc, PixelFormat.UNKNOWN)
        if fourcc == b"DX10":
            # DXGI format lives in the extended header; treat as unknown and
            # let the re-encoder handle it rather than guessing.
            info.error = "DX10 extended header; will be re-encoded"
    elif pf_flags & _DDPF_RGB:
        info.format = PixelFormat.RGBA8 if rgb_bits == 32 else PixelFormat.RGB8
        info.fourcc = ""
    else:
        info.format = PixelFormat.A8 if rgb_bits == 8 else PixelFormat.UNKNOWN

    info.valid = info.width > 0 and info.height > 0
    return info


def ensure_block_compressed(
    source: str | Path,
    dest: str | Path,
    target: PixelFormat = PixelFormat.DXT5,
    *,
    generate_mips: bool = True,
) -> tuple[bool, str]:
    """
    Produce a BO2-ready DDS at `dest`.

    Returns (ok, message). Uses Pillow, which is always available. An
    uncompressed DDS here is not a cosmetic problem: ImageConverter reports
    "Unknown IWI format: 0" and the texture never reaches the game.

    With `generate_mips` the full chain is built level by level, because a
    texture that ships with only its top level renders untextured once the
    engine asks for a mip it does not have.
    """
    try:
        from PIL import Image
    except ImportError:  # pragma: no cover - Pillow is a hard dependency
        return False, "Pillow is not installed"

    src = Path(source)
    dst = Path(dest)
    dst.parent.mkdir(parents=True, exist_ok=True)

    pillow_format = PILLOW_PIXEL_FORMAT.get(target, "DXT5")

    # A cubemap has to be re-encoded face by face. Opening one with Pillow gives
    # back only the first face, so going through the normal path would turn a
    # sky into a single direction and render it black.
    source_info = read_dds_header(src)
    if source_info.valid and source_info.is_cubemap and not source_info.block_compressed:
        try:
            faces = read_faces(src, source_info)
        except Exception as exc:
            return False, f"could not split cubemap faces: {type(exc).__name__}: {exc}"
        return write_cubemap_dds(faces, dst, target)

    try:
        with Image.open(src) as img:
            # RGBA everywhere: DXT1 drops alpha itself, and forcing the mode
            # first keeps palette and grayscale inputs from surprising us.
            rgba = img.convert("RGBA")

            # Block formats need multiple-of-4 dimensions.
            w, h = rgba.size
            pad_w, pad_h = _round_up4(w), _round_up4(h)
            if (pad_w, pad_h) != (w, h):
                padded = Image.new("RGBA", (pad_w, pad_h), (0, 0, 0, 0))
                padded.paste(rgba, (0, 0))
                rgba = padded

            if generate_mips:
                ok, message = write_mipped_dds(rgba, dst, target)
                if not ok:
                    return False, message
            else:
                rgba.save(dst, format="DDS", pixel_format=pillow_format)
    except Exception as exc:  # Pillow raises a wide variety here
        return False, f"{type(exc).__name__}: {exc}"

    check = read_dds_header(dst)
    if not check.block_compressed:
        return False, (
            f"wrote {dst.name} but it is {check.format}, not block compressed; "
            "ImageConverter will reject it"
        )

    msg = f"{check.width}x{check.height} {check.fourcc}"
    if generate_mips:
        msg += f" with {check.mip_levels} mip levels"
    return True, msg


def read_faces(path: str | Path, info: DdsInfo | None = None) -> list["Image.Image"]:  # noqa: F821
    """
    Every face of a DDS as an RGBA image, in cube order (+X -X +Y -Y +Z -Z).

    Pillow opens a cubemap as its first face only, which silently reduces a sky
    to one direction. The payload is split by hand instead, which is exact for
    an uncompressed DDS and is the case that needs re-encoding.
    """
    from PIL import Image

    p = Path(path)
    info = info or read_dds_header(p)
    data = p.read_bytes()[4 + _HEADER_SIZE:]

    if info.block_compressed:
        raise ValueError("block compressed faces cannot be decoded here; copy them instead")

    bytes_per_pixel = max(1, info.format.bytes_per_block)
    face_bytes = info.width * info.height * bytes_per_pixel
    faces: list[Image.Image] = []
    for index in range(info.face_count):
        chunk = data[index * face_bytes:(index + 1) * face_bytes]
        if len(chunk) < face_bytes:
            break
        # DDS stores 32-bit colour as BGRA, which Pillow calls "BGRA" on read.
        mode = "BGRA" if bytes_per_pixel == 4 else "RGBA"
        img = Image.frombytes("RGBA", (info.width, info.height), chunk, "raw", mode)
        faces.append(img)
    return faces


def write_cubemap_dds(
    faces: list["Image.Image"],  # noqa: F821
    dest: str | Path,
    target: PixelFormat,
) -> tuple[bool, str]:
    """
    Write a six-face block-compressed cubemap.

    Mip chains are deliberately not generated: a cubemap's levels interleave
    per face, and a sky is never sampled at a distance that would need them.
    """
    dst = Path(dest)
    dst.parent.mkdir(parents=True, exist_ok=True)

    if len(faces) != 6:
        return False, f"a cubemap needs 6 faces, got {len(faces)}"

    pillow_format = PILLOW_PIXEL_FORMAT.get(target, "DXT5")
    width, height = faces[0].size
    face_size = level_sizes(width, height, target)[0]

    payloads = []
    for index, face in enumerate(faces):
        try:
            data = _encode_level(face.convert("RGBA"), pillow_format)
        except Exception as exc:
            return False, f"face {index} failed: {type(exc).__name__}: {exc}"
        payloads.append(data[:face_size].ljust(face_size, b"\0"))

    fourcc = {
        PixelFormat.DXT1: b"DXT1",
        PixelFormat.DXT3: b"DXT3",
        PixelFormat.DXT5: b"DXT5",
    }.get(target, b"DXT5")

    flags = _DDSD_CAPS | _DDSD_HEIGHT | _DDSD_WIDTH | _DDSD_PIXELFORMAT | _DDSD_LINEARSIZE
    pitch = max(1, (width + 3) // 4) * target.bytes_per_block

    header = bytearray(_HEADER_SIZE)
    struct.pack_into("<IIIIIII", header, 0, _HEADER_SIZE, flags, height, width, pitch, 1, 1)
    struct.pack_into("<II4s", header, 72, 32, _DDPF_FOURCC, fourcc)
    struct.pack_into("<I", header, 104, _DDSCAPS_TEXTURE | _DDSCAPS_COMPLEX)
    # The cubemap bit plus one bit per face present, which is what marks this as
    # a cube rather than six unrelated images.
    struct.pack_into("<I", header, 108, _DDSCAPS2_CUBEMAP | _DDSCAPS2_CUBEMAP_ALLFACES)

    try:
        with dst.open("wb") as fh:
            fh.write(_DDS_MAGIC)
            fh.write(header)
            for payload in payloads:
                fh.write(payload)
    except OSError as exc:
        return False, str(exc)

    return True, f"{width}x{height} {fourcc.decode()} cubemap, 6 faces"


def ensure_uncompressed(
    source: str | Path,
    dest: str | Path,
    target: PixelFormat = PixelFormat.RGBA8,
) -> tuple[bool, str]:
    """
    Write an uncompressed DDS at `dest`, whatever the source was.

    Needed for lightmaps. A light bake is smooth gradients, and a block format
    gives every 4x4 block just two colour endpoints, so shading bands visibly;
    stock Black Ops II ships its lightmaps uncompressed for that reason. Black
    Ops stores them as R5G6B5, which is neither block compressed nor a format
    Black Ops II's IWI container accepts, so it has to be widened here.

    No mip chain is written: a lightmap is sampled by explicit UV over the whole
    atlas, and stock Black Ops II lightmaps carry a single level.
    """
    from PIL import Image

    src = Path(source)
    dst = Path(dest)
    dst.parent.mkdir(parents=True, exist_ok=True)

    try:
        with Image.open(src) as img:
            rgba = img.convert("RGBA")
    except Exception as exc:
        return False, f"{type(exc).__name__}: {exc}"

    width, height = rgba.size

    # **Channel order is not cosmetic here.** ImageConverter maps a DDS to an IWI
    # format by its masks, and its T6 writer knows R8G8B8A8 but not B8G8R8A8 --
    # the everyday DDS order. Writing BGRA yields "Unknown IWI format: 0" and the
    # texture never reaches the game, so this writes true RGBA.
    header = bytearray(_HEADER_SIZE)
    flags = _DDSD_CAPS | _DDSD_HEIGHT | _DDSD_WIDTH | _DDSD_PIXELFORMAT | 0x8  # pitch
    struct.pack_into("<IIIIIII", header, 0, _HEADER_SIZE, flags, height, width, width * 4, 1, 1)
    struct.pack_into("<IIII", header, 72, 32, _DDPF_RGB | _DDPF_ALPHAPIXELS, 0, 32)
    struct.pack_into(
        "<IIII", header, 88,
        0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000,
    )
    struct.pack_into("<I", header, 104, _DDSCAPS_TEXTURE)

    try:
        with dst.open("wb") as fh:
            fh.write(_DDS_MAGIC)
            fh.write(header)
            fh.write(rgba.tobytes())   # Pillow's RGBA order is already correct
    except OSError as exc:
        return False, str(exc)

    return True, f"{width}x{height} uncompressed RGBA"


def add_mip_chain(source: str | Path, dest: str | Path) -> tuple[bool, str]:
    """
    Copy a block-compressed DDS to `dest`, giving it a mip chain if it lacks one.

    Textures dumped straight out of a fastfile often carry only their top level
    -- notably a world's lightmap, which every surface samples. Level 0 is kept
    verbatim and only the smaller levels are encoded, so the texture the player
    stands next to is bit-identical to the source.
    """
    import shutil

    from PIL import Image

    src, dst = Path(source), Path(dest)
    info = read_dds_header(src)
    full = len(level_sizes(info.width, info.height, info.format))

    if not info.valid or info.mip_levels >= full or info.is_cubemap:
        shutil.copy2(src, dst)
        return True, f"copied as-is ({info.mip_levels} levels)"

    if info.format not in PILLOW_PIXEL_FORMAT:
        shutil.copy2(src, dst)
        return False, f"cannot add mips to {info.format}; copied unchanged"

    try:
        with Image.open(src) as img:
            rgba = img.convert("RGBA")
        top = src.read_bytes()[4 + _HEADER_SIZE:]
        ok, message = write_mipped_dds(rgba, dst, info.format, top_level=top)
    except Exception as exc:
        shutil.copy2(src, dst)
        return False, f"{type(exc).__name__}: {exc}; copied unchanged"

    if not ok:
        shutil.copy2(src, dst)
        return False, f"{message}; copied unchanged"
    return True, f"added mip chain: {message}"


def _round_up4(n: int) -> int:
    return (n + 3) & ~3


def _encode_level(img: "Image.Image", pillow_format: str) -> bytes:  # noqa: F821
    """
    Block-encode one image and return just its payload.

    Pillow will only hand back compressed blocks by way of a file, so this
    writes one to memory and strips the header off the front.
    """
    import io

    buf = io.BytesIO()
    img.save(buf, format="DDS", pixel_format=pillow_format)
    return buf.getvalue()[4 + _HEADER_SIZE:]


def write_mipped_dds(
    source_rgba: "Image.Image",  # noqa: F821
    dest: str | Path,
    target: PixelFormat,
    *,
    top_level: bytes | None = None,
) -> tuple[bool, str]:
    """
    Write a block-compressed DDS with a full mip chain.

    Pillow cannot write mipmaps, so each level is encoded separately and the
    payloads are concatenated behind a header that declares the chain. This
    matters most for a lightmap: every world surface samples it, and a distant
    surface asking for a mip that is not there renders untextured.

    Pass `top_level` to keep an already-compressed level 0 byte for byte, so
    adding a chain to an existing texture does not cost it a re-encode.
    """
    from PIL import Image

    dst = Path(dest)
    dst.parent.mkdir(parents=True, exist_ok=True)
    pillow_format = PILLOW_PIXEL_FORMAT.get(target, "DXT5")

    width, height = source_rgba.size
    expected = level_sizes(width, height, target)

    payloads: list[bytes] = []
    w, h = width, height
    for index, want in enumerate(expected):
        level = source_rgba if index == 0 else source_rgba.resize((w, h), Image.LANCZOS)
        # Below 4x4 a block format still stores one whole block, so the image
        # handed to the encoder is padded up rather than shrunk further.
        if w < 4 or h < 4:
            padded = Image.new("RGBA", (max(4, w), max(4, h)), (0, 0, 0, 0))
            padded.paste(level, (0, 0))
            level = padded
        if index == 0 and top_level is not None and len(top_level) >= want:
            data = top_level
        else:
            try:
                data = _encode_level(level, pillow_format)
            except Exception as exc:
                return False, f"level {index} ({w}x{h}) failed: {type(exc).__name__}: {exc}"

        # Trust the computed size over whatever Pillow padded to, so the chain
        # stays exactly the length a reader will compute from the dimensions.
        payloads.append(data[:want].ljust(want, b"\0"))
        if w == 1 and h == 1:
            break
        w, h = max(1, w // 2), max(1, h // 2)

    fourcc = {
        PixelFormat.DXT1: b"DXT1",
        PixelFormat.DXT3: b"DXT3",
        PixelFormat.DXT5: b"DXT5",
    }.get(target, b"DXT5")

    flags = (
        _DDSD_CAPS | _DDSD_HEIGHT | _DDSD_WIDTH
        | _DDSD_PIXELFORMAT | _DDSD_MIPMAPCOUNT | _DDSD_LINEARSIZE
    )
    # Not the top level's size, despite the DDSD_LINEARSIZE flag: the textures
    # ImageConverter already accepts carry the pitch of a single row of blocks,
    # and feeding it anything larger sends it striding past the end of the file.
    pitch = max(1, (width + 3) // 4) * target.bytes_per_block

    header = bytearray(_HEADER_SIZE)
    struct.pack_into(
        "<IIIIIII", header, 0,
        _HEADER_SIZE, flags, height, width, pitch, 1, len(payloads),
    )
    # Pixel format block at offset 72 within the header.
    struct.pack_into("<II4s", header, 72, 32, _DDPF_FOURCC, fourcc)
    struct.pack_into(
        "<I", header, 104, _DDSCAPS_TEXTURE | _DDSCAPS_COMPLEX | _DDSCAPS_MIPMAP
    )

    try:
        with dst.open("wb") as fh:
            fh.write(_DDS_MAGIC)
            fh.write(header)
            for payload in payloads:
                fh.write(payload)
    except OSError as exc:
        return False, str(exc)

    return True, f"{width}x{height} {fourcc.decode()} with {len(payloads)} mip levels"
