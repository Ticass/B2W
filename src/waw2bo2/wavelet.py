"""WaW IWI6 wavelets, measured against AssetViewer's sub_4A1940.

LSB-first Huffman coefficients feed a reversible 2x2 integer Haar transform.
Colour differences use a separate codebook; the optional per-level residual
updates the parent used for reconstruction (not the already-uploaded mip).
Codebooks are extracted from the user's executable, never shipped here.
DDS output uses the pipeline's OAT-compatible mip-major cubemap ordering.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass
from functools import lru_cache
from pathlib import Path

from .iwi import IwiError, iwi6_to_dds

_SIGNATURES = (
    "0c000600000003000a0006000700060004000500020005000900060001000500",
    "1e000a00020004000100030000000200040005001f000b00ffff030000000200",
    "0080040000000100010004000000010007000700000001000b00080000000100",
)


@lru_cache(maxsize=8)
def _tables(path: str, size: int, mtime: int) -> tuple:
    blob = Path(path).read_bytes()
    result = []
    for signature in _SIGNATURES:
        sig = bytes.fromhex(signature)
        candidates = []
        pos = blob.find(sig)
        while pos >= 0:
            if pos + 16384 <= len(blob):
                table = tuple(struct.iter_unpack("<hh", blob[pos:pos + 16384]))
                if all(1 <= bits <= 12 and table[index & ((1 << bits) - 1)] == (value, bits)
                       for index, (value, bits) in enumerate(table)):
                    candidates.append(table)
            pos = blob.find(sig, pos + 1)
        if len(candidates) != 1:
            raise IwiError(f"missing/ambiguous WaW wavelet codebook in {path}")
        result.append(candidates[0])
    return tuple(result)


def load_tables(binary: Path) -> tuple:
    stat = binary.stat()
    return _tables(str(binary.resolve()), stat.st_size, stat.st_mtime_ns)


class _Bits:
    def __init__(self, data: bytes):
        self.data = data
        self.pos = 0

    def peek(self, count: int) -> int:
        offset = self.pos >> 3
        return (int.from_bytes(self.data[offset:offset + 4], "little") >> (self.pos & 7)) & ((1 << count) - 1)

    def read(self, count: int) -> int:
        if self.pos + count > len(self.data) * 8:
            raise IwiError("truncated WaW wavelet bitstream")
        value = self.peek(count)
        self.pos += count
        return value

    def coefficient(self, table: tuple, raw_bits: int = 9, bias: int = 255) -> int:
        value, bits = table[self.peek(12)]
        self.read(bits)
        return self.read(raw_bits) - bias if value == -32768 else value


@dataclass
class WaveletImage:
    width: int
    height: int
    format: int
    flags: int
    # full resolution first; faces of each mip contiguous, D3D BGR(A) order
    levels: list[bytes]

    @property
    def faces(self) -> int:
        return 6 if self.flags & 4 else 1

    @property
    def stride(self) -> int:
        return {6: 4, 7: 4, 8: 2, 9: 1, 10: 1}[self.format]

    def dds(self) -> bytes:
        fmt = self.format
        pf = {
            6: (0x41, 32, 0xff0000, 0xff00, 0xff, 0xff000000),
            7: (0x40, 32, 0xff0000, 0xff00, 0xff, 0),
            8: (0x20001, 16, 0xff, 0, 0, 0xff00),
            9: (0x20000, 8, 0xff, 0, 0, 0),
            10: (0x2, 8, 0, 0, 0, 0xff),
        }[fmt]
        head = bytearray(128)
        head[:4] = b"DDS "
        mips = len(self.levels)
        struct.pack_into("<7I", head, 4, 124, 0x100f | (0x20000 if mips > 1 else 0),
                         self.height, self.width, self.width * self.stride, 0, mips)
        struct.pack_into("<2I4s5I", head, 76, 32, pf[0], b"\0" * 4, *pf[1:])
        caps = 0x1000 | (0x400008 if mips > 1 else 0) | (8 if self.faces == 6 else 0)
        struct.pack_into("<2I", head, 108, caps, 0xfe00 if self.faces == 6 else 0)
        return bytes(head) + b"".join(self.levels)


def decode(blob: bytes, tables: tuple) -> WaveletImage:
    if len(blob) < 28 or blob[:4] != b"IWi\x06" or blob[4] not in range(6, 11):
        raise IwiError("not a WaW wavelet IWI6")
    fmt, flags, width, height, depth = struct.unpack_from("<BB3H", blob, 4)
    if flags & 8 or depth != 1:
        raise IwiError("volume wavelet IWI is not supported")
    if not width or not height or width & (width - 1) or height & (height - 1):
        raise IwiError("wavelet dimensions must be positive powers of two")
    # Avoid allocating untrusted multi-gigabyte output before consuming input.
    if width * height > 16_777_216:
        raise IwiError("wavelet image exceeds 16 megapixel safety limit")
    channels = {6: 4, 7: 3, 8: 2, 9: 1, 10: 1}[fmt]
    stride = 4 if channels == 3 else channels
    faces = 6 if flags & 4 else 1
    count = 1 if flags & 2 else max(width, height).bit_length()
    reader = _Bits(blob[28:])
    parents = [bytearray() for _ in range(faces)]
    levels = [b""] * count
    base, delta, alpha = tables
    for level in reversed(range(count)):
        w, h = max(1, width >> level), max(1, height >> level)
        outputs = []
        for face in range(faces):
            out = bytearray(w * h * stride)
            if w == 1 or h == 1:
                for pixel in range(w * h):
                    for c in range(channels):
                        out[pixel * stride + c] = reader.read(8)
                    if channels == 3:
                        out[pixel * stride + 3] = 255
            else:
                parent = parents[face]
                if not parent:
                    raise IwiError("wavelet 2D level has no parent mip (NOMIPMAPS)")
                if reader.read(1):
                    for pixel in range(w * h // 4):
                        for c in range(channels):
                            p = pixel * stride + c
                            parent[p] = (parent[p] + reader.coefficient(alpha)) & 255
                for y in range(0, h, 2):
                    for x in range(0, w, 2):
                        coarse = ((y // 2) * (w // 2) + x // 2) * stride
                        target = (y * w + x) * stride

                        def write(c: int, parity: int, a: int, b: int, d: int) -> None:
                            mean = 2 * parent[coarse + c]
                            out[target + c] = (parity + ((mean + a + b + d) >> 1)) & 255
                            out[target + stride + c] = ((mean + a - b - d) >> 1) & 255
                            out[target + w * stride + c] = ((mean + b - a - d) >> 1) & 255
                            out[target + (w + 1) * stride + c] = ((mean + d - a - b) >> 1) & 255

                        if channels > 1:
                            parity = reader.read(1)
                            a, b, d = (reader.coefficient(base) for _ in range(3))
                            write(0, parity, a, b, d)
                            if channels >= 3:
                                for c in (1, 2):
                                    parity = reader.read(1)
                                    aa, bb, dd = (reader.coefficient(delta, 10, 510) for _ in range(3))
                                    write(c, parity, a + aa, b + bb, d + dd)
                        if channels == 3:
                            for p in (target, target + stride, target + w * stride, target + (w + 1) * stride):
                                out[p + 3] = 255
                        else:
                            parity = reader.read(1)
                            a, b, d = (reader.coefficient(alpha) for _ in range(3))
                            write(channels - 1, parity, a, b, d)
            parents[face] = out
            outputs.append(bytes(out))
        levels[level] = b"".join(outputs)
    return WaveletImage(width, height, fmt, flags, levels)


def to_dds(blob: bytes, binary: Path) -> bytes:
    return decode(blob, load_tables(binary)).dds()


class IwdRecovery:
    """Recover missing DDS pixels from the first matching IWD, never stock over mod.

    No disk cache is trusted: each run decodes from the current source IWD.
    The small in-memory index avoids repeatedly walking every archive.
    """
    def __init__(self, directories: list[Path], binary: Path | None, root: Path):
        import zipfile
        self.binary, self.root = binary, root
        self.entries: dict[str, tuple[Path, str]] = {}
        self.recovered: dict[str, dict] = {}
        for directory in directories:
            for archive in sorted(directory.glob("*.iwd")):
                with zipfile.ZipFile(archive) as z:
                    for name in z.namelist():
                        if name.lower().startswith("images/") and name.lower().endswith(".iwi"):
                            self.entries.setdefault(name.lower(), (archive, name))

    def recover(self, stem: str) -> Path | None:
        import zipfile
        if stem in self.recovered:
            return self.root / "images" / f"{stem}.dds"
        hit = self.entries.get(f"images/{stem.lower()}.iwi")
        if hit is None:
            return None
        archive, entry = hit
        with zipfile.ZipFile(archive) as z:
            blob = z.read(entry)
        plain = iwi6_to_dds(blob)
        if plain is not None:
            # A plain DXT image the zone dump did not write (measured: Asylum
            # V2's coffee_machine_col, "Dumped" by the unlinker, no file).
            dst = self.root / "images" / f"{stem}.dds"
            if Path(f"{stem}.dds").is_absolute() or ".." in Path(stem).parts:
                raise IwiError(f"unsafe image name {stem!r}")
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_bytes(plain)
            self.recovered[stem] = {"archive": str(archive), "entry": entry, "iwi_format": blob[4]}
            return dst
        if len(blob) < 5 or blob[:4] != b"IWi\x06" or blob[4] not in range(6, 11):
            return None
        if self.binary is None or not self.binary.is_file():
            raise IwiError("wavelet image needs WaW bin/AssetViewer.exe (--waw-mod-tools)")
        # Asset names may have folders, but must not escape the output directory.
        relative = Path(f"{stem}.dds")
        if relative.is_absolute() or ".." in relative.parts:
            raise IwiError(f"unsafe wavelet image name {stem!r}")
        dst = self.root / "images" / relative
        data = to_dds(blob, self.binary)
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_bytes(data)
        self.recovered[stem] = {"archive": str(archive), "entry": entry, "iwi_format": blob[4],
                                "decoder_tables": str(self.binary)}
        return dst
