"""
CODIR image.

Texture pixels stay on disk; CODIR holds the description. The description is
what decides the conversion, and getting one field wrong here is how a port
ends up with inverted lighting or mushy normals:

    `semantic` decides colour space. A normal map pushed through an sRGB path
    comes back subtly wrong on every surface that uses it.

    `format` decides whether the data can be handed to ImageConverter at all.
    BO2's IWI accepts a specific set of block formats; anything else must be
    re-encoded, and re-encoding a normal map as DXT1 destroys it.
"""

from __future__ import annotations

import enum
from dataclasses import dataclass, field
from typing import Any

from codport.codir.common import CodirAsset
from codport.codir.material import Semantic


class PixelFormat(enum.StrEnum):
    """Formats we can reason about. Anything else is UNKNOWN and gets re-encoded."""

    DXT1 = "dxt1"
    DXT3 = "dxt3"
    DXT5 = "dxt5"
    BC4 = "bc4"
    BC5 = "bc5"        # two-channel, the correct home for normal maps
    ATI2 = "ati2"      # BC5 under its DirectX 9 FourCC
    RGBA8 = "rgba8"
    RGB8 = "rgb8"
    A8 = "a8"
    UNKNOWN = "unknown"

    @property
    def block_compressed(self) -> bool:
        return self in (
            PixelFormat.DXT1, PixelFormat.DXT3, PixelFormat.DXT5,
            PixelFormat.BC4, PixelFormat.BC5, PixelFormat.ATI2,
        )

    @property
    def has_alpha(self) -> bool:
        return self in (PixelFormat.DXT3, PixelFormat.DXT5, PixelFormat.RGBA8, PixelFormat.A8)

    @property
    def bytes_per_block(self) -> int:
        """
        Bytes for one 4x4 block, or for one pixel when uncompressed.

        DXT1 and BC4 carry half the data of the other block formats, which is
        what makes a size check able to tell a full mip chain from a lone top
        level.
        """
        if self in (PixelFormat.DXT1, PixelFormat.BC4):
            return 8
        if self.block_compressed:
            return 16
        if self is PixelFormat.A8:
            return 1
        if self is PixelFormat.RGB8:
            return 3
        return 4


def level_sizes(width: int, height: int, fmt: PixelFormat) -> list[int]:
    """
    Byte size of every mip level of a full chain, largest first.

    A block format never goes below one 4x4 block, so the tail of the chain
    flattens out rather than shrinking to nothing.
    """
    sizes: list[int] = []
    w, h = max(1, width), max(1, height)
    while True:
        if fmt.block_compressed:
            sizes.append(max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * fmt.bytes_per_block)
        else:
            sizes.append(w * h * fmt.bytes_per_block)
        if w == 1 and h == 1:
            return sizes
        w, h = max(1, w // 2), max(1, h // 2)


#: Formats BO2's IWI container accepts directly.
#: Anything outside this set has to be re-encoded before ImageConverter will
#: take it -- Pillow's default DDS output is uncompressed ARGB with no FourCC,
#: which fails with "Unknown IWI format: 0".
#: Verified against IWI files produced by OpenAssetTools' ImageConverter --t6:
#: format codes 2 (uncompressed), 4 (A8), 11 (DXT1), 13 (DXT5), 14 (ATI2/DXN).
BO2_IWI_FORMATS: frozenset[PixelFormat] = frozenset(
    {
        PixelFormat.DXT1,
        PixelFormat.DXT3,
        PixelFormat.DXT5,
        PixelFormat.ATI2,
        PixelFormat.RGBA8,
        PixelFormat.A8,
    }
)


#: Which format each semantic should end up as for BO2.
#: DXT5 for colour keeps alpha; ATI2/BC5 for normals keeps both channels at
#: full precision, which DXT1 does not.
PREFERRED_FORMAT: dict[Semantic, PixelFormat] = {
    Semantic.COLOR: PixelFormat.DXT5,
    Semantic.NORMAL: PixelFormat.DXT5,
    Semantic.DETAIL_NORMAL: PixelFormat.DXT5,
    Semantic.SPECULAR: PixelFormat.DXT5,
    Semantic.GLOSS: PixelFormat.DXT5,
    Semantic.EMISSIVE: PixelFormat.DXT5,
    Semantic.ALPHA: PixelFormat.DXT5,
    Semantic.DETAIL: PixelFormat.DXT1,
    Semantic.OCCLUSION: PixelFormat.DXT1,
    Semantic.ENVIRONMENT: PixelFormat.DXT5,
    # A light bake is smooth gradients, which is the worst case for a block
    # format: DXT gives each 4x4 block two endpoints, so shading bands visibly.
    # Stock Black Ops II lightmaps are uncompressed for that reason.
    Semantic.LIGHTMAP: PixelFormat.RGBA8,
}


class ColorSpace(enum.StrEnum):
    SRGB = "srgb"
    LINEAR = "linear"


@dataclass
class CodirImage(CodirAsset):
    kind: str = "image"

    width: int = 0
    height: int = 0
    depth: int = 1
    mip_levels: int = 1
    format: PixelFormat = PixelFormat.UNKNOWN
    semantic: Semantic = Semantic.UNKNOWN
    color_space: ColorSpace = ColorSpace.SRGB
    is_cubemap: bool = False
    has_alpha: bool = False
    file_path: str = ""
    """Workspace-relative path to the .dds/.iwi/.png holding the pixels."""
    byte_size: int = 0

    def __post_init__(self) -> None:
        # Colour space follows the semantic unless a parser overrode it.
        if self.semantic.is_linear:
            self.color_space = ColorSpace.LINEAR

    # ---- queries ---------------------------------------------------------

    @property
    def is_power_of_two(self) -> bool:
        return _pot(self.width) and _pot(self.height)

    @property
    def megapixels(self) -> float:
        return (self.width * self.height) / 1_000_000.0

    @property
    def bo2_compatible(self) -> bool:
        return self.format in BO2_IWI_FORMATS

    def target_format(self) -> PixelFormat:
        """
        What this image should be encoded as for BO2.

        A semantic that names an uncompressed format means it, and wins over
        passing the source format through: a lightmap that arrives already block
        compressed still has to end up uncompressed, because that is what the
        light bake needs to survive.
        """
        preferred = PREFERRED_FORMAT.get(self.semantic, PixelFormat.DXT5)
        if not preferred.block_compressed:
            return preferred
        if self.format in BO2_IWI_FORMATS and self.format is not PixelFormat.RGBA8:
            return self.format
        return preferred

    def problems(self) -> list[str]:
        issues: list[str] = []
        if self.width <= 0 or self.height <= 0:
            issues.append("zero or unknown dimensions")
        if not self.is_power_of_two:
            issues.append(f"{self.width}x{self.height} is not power-of-two")
        if not self.bo2_compatible:
            issues.append(f"format {self.format} is not a BO2 IWI format")
        if self.semantic is Semantic.NORMAL and self.format is PixelFormat.DXT1:
            issues.append("normal map stored as DXT1; the 5:6:5 colour endpoints destroy it")
        if self.semantic.is_linear and self.color_space is ColorSpace.SRGB:
            issues.append(f"{self.semantic} data flagged sRGB; it should be linear")
        if self.mip_levels <= 1 and max(self.width, self.height) > 64:
            issues.append("no mipmaps; will alias badly at distance")
        return issues

    def to_dict(self) -> dict[str, Any]:
        d = self.base_dict()
        d.update(
            {
                "width": self.width,
                "height": self.height,
                "depth": self.depth,
                "mip_levels": self.mip_levels,
                "format": str(self.format),
                "semantic": str(self.semantic),
                "color_space": str(self.color_space),
                "is_cubemap": self.is_cubemap,
                "has_alpha": self.has_alpha,
                "file_path": self.file_path,
                "byte_size": self.byte_size,
                "target_format": str(self.target_format()),
                "problems": self.problems(),
            }
        )
        return d

    @classmethod
    def from_dict(cls, d: dict[str, Any]) -> "CodirImage":
        img = cls()
        img.load_base(d)
        img.width = d.get("width", 0)
        img.height = d.get("height", 0)
        img.depth = d.get("depth", 1)
        img.mip_levels = d.get("mip_levels", 1)
        img.format = PixelFormat(d.get("format", "unknown"))
        img.semantic = Semantic(d.get("semantic", "unknown"))
        img.color_space = ColorSpace(d.get("color_space", "srgb"))
        img.is_cubemap = d.get("is_cubemap", False)
        img.has_alpha = d.get("has_alpha", False)
        img.file_path = d.get("file_path", "")
        img.byte_size = d.get("byte_size", 0)
        return img


def _pot(n: int) -> bool:
    return n > 0 and (n & (n - 1)) == 0
