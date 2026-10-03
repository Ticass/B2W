"""
CODIR material.

A Call of Duty material is a technique set plus a set of texture slots plus a
render state. The technique set name is engine-specific and worthless across
games -- T5 calls a lit bumped surface `mc_l_sm_b0c0n0s0`, T6 calls a similar
thing `mc_sw4_3d_char_cloth_4z8fq5wu`. Neither name can be translated by string
surgery.

What *is* portable is the behaviour:

    which texture semantics are bound
    how the surface blends
    whether it is alpha tested
    whether it is two sided
    whether it is a decal (polygon offset)
    which camera region it sorts into

That behaviour is what CODIR records, and it is what the BO2 technique-set
matcher scores candidates against. The original techset name is kept in
`source_techset` purely as evidence.
"""

from __future__ import annotations

import enum
import re
from dataclasses import dataclass, field
from typing import Any

from codport.codir.common import CodirAsset


class Semantic(enum.StrEnum):
    """
    Normalised texture roles.

    Every engine spells these differently; the parsers map onto this set so
    that "does this material have a normal map" is one question, not six.
    """

    COLOR = "color"
    NORMAL = "normal"
    SPECULAR = "specular"
    GLOSS = "gloss"
    OCCLUSION = "occlusion"
    DETAIL = "detail"
    DETAIL_NORMAL = "detail_normal"
    EMISSIVE = "emissive"
    ALPHA = "alpha"
    BLEND_MASK = "blend_mask"
    ENVIRONMENT = "environment"
    DISTORTION = "distortion"
    FLOW = "flow"
    HEIGHT = "height"
    LIGHTMAP = "lightmap"
    UNKNOWN = "unknown"

    @property
    def is_linear(self) -> bool:
        """
        True when the texture holds data, not colour.

        Treating a normal or specular map as sRGB is the single most common
        way a port ends up looking subtly wrong everywhere, so this drives
        colour-space handling in the image converter.
        """
        return self in (
            Semantic.NORMAL,
            Semantic.DETAIL_NORMAL,
            Semantic.SPECULAR,
            Semantic.GLOSS,
            Semantic.OCCLUSION,
            Semantic.ALPHA,
            Semantic.BLEND_MASK,
            Semantic.DISTORTION,
            Semantic.FLOW,
            Semantic.HEIGHT,
        )


#: Source slot name -> CODIR semantic. Covers IW3..T6 naming, which is mostly
#: consistent because the material system descends from a common ancestor.
SEMANTIC_ALIASES: dict[str, Semantic] = {
    "colormap": Semantic.COLOR,
    "colourmap": Semantic.COLOR,
    "diffusemap": Semantic.COLOR,
    "basecolormap": Semantic.COLOR,
    "color": Semantic.COLOR,
    "normalmap": Semantic.NORMAL,
    "bumpmap": Semantic.NORMAL,
    "normal": Semantic.NORMAL,
    "specularmap": Semantic.SPECULAR,
    "specmap": Semantic.SPECULAR,
    "specular": Semantic.SPECULAR,
    "glossmap": Semantic.GLOSS,
    "roughnessmap": Semantic.GLOSS,
    "occlusionmap": Semantic.OCCLUSION,
    "aomap": Semantic.OCCLUSION,
    "detailmap": Semantic.DETAIL,
    "detailnormalmap": Semantic.DETAIL_NORMAL,
    "emissivemap": Semantic.EMISSIVE,
    "glowmap": Semantic.EMISSIVE,
    "alphamap": Semantic.ALPHA,
    "opacitymap": Semantic.ALPHA,
    "blendmask": Semantic.BLEND_MASK,
    "layer0map": Semantic.COLOR,
    "envmap": Semantic.ENVIRONMENT,
    "cubemap": Semantic.ENVIRONMENT,
    "reflectionmap": Semantic.ENVIRONMENT,
    "distortionmap": Semantic.DISTORTION,
    "flowmap": Semantic.FLOW,
    "heightmap": Semantic.HEIGHT,
    "parallaxmap": Semantic.HEIGHT,
    "lightmap": Semantic.LIGHTMAP,
}


def normalise_semantic(raw: str | None) -> Semantic:
    if not raw:
        return Semantic.UNKNOWN
    key = re.sub(r"[^a-z0-9]", "", str(raw).lower())
    if key in SEMANTIC_ALIASES:
        return SEMANTIC_ALIASES[key]
    # Fall back to substring sniffing: slots like "colorMap0" or "normalMap2".
    for alias, sem in SEMANTIC_ALIASES.items():
        if key.startswith(alias):
            return sem
    for token, sem in (
        ("normal", Semantic.NORMAL), ("bump", Semantic.NORMAL),
        ("spec", Semantic.SPECULAR), ("gloss", Semantic.GLOSS),
        ("color", Semantic.COLOR), ("diffuse", Semantic.COLOR),
        ("glow", Semantic.EMISSIVE), ("emiss", Semantic.EMISSIVE),
        ("detail", Semantic.DETAIL), ("env", Semantic.ENVIRONMENT),
        ("alpha", Semantic.ALPHA), ("opac", Semantic.ALPHA),
    ):
        if token in key:
            return sem
    return Semantic.UNKNOWN


class BlendMode(enum.StrEnum):
    OPAQUE = "opaque"
    ALPHA_TEST = "alpha_test"
    BLEND = "blend"
    ADD = "add"
    MULTIPLY = "multiply"
    SCREEN = "screen"
    UNKNOWN = "unknown"


#: Blend factors are compared in OpenAssetTools' stateBits spelling -- what both games' dumped
#: materials carry (JsonMaterial.h.template: srcalpha, invsrcalpha, destcolor, ...). This rule
#: was written against D3D-style names that no dumped material uses, so every blended material
#: with an alpha test, and every srcalpha/one additive, was misread.
_BLEND_FACTOR_ALIASES: dict[str, str] = {
    "src_alpha": "srcalpha", "one_minus_src_alpha": "invsrcalpha",
    "src_color": "srccolor", "one_minus_src_color": "invsrccolor",
    "dst_alpha": "destalpha", "one_minus_dst_alpha": "invdestalpha",
    "dst_color": "destcolor", "one_minus_dst_color": "invdestcolor",
}


def classify_blend(
    src_rgb: str,
    dst_rgb: str,
    blend_op: str,
    alpha_test: str,
) -> BlendMode:
    """
    Turn raw render state into an intent.

    This is the "behaviour analysis" step: BO2 has no way to reproduce an
    arbitrary blend equation, but it does have a material for each of these
    intents, so classifying first and matching second is what makes the
    conversion survivable.
    """
    src = _BLEND_FACTOR_ALIASES.get((src_rgb or "").lower(), (src_rgb or "").lower())
    dst = _BLEND_FACTOR_ALIASES.get((dst_rgb or "").lower(), (dst_rgb or "").lower())
    op = (blend_op or "disabled").lower()
    at = (alpha_test or "disabled").lower()

    blending = op not in ("", "disabled") or not (src == "one" and dst == "zero")

    if not blending:
        return BlendMode.ALPHA_TEST if at not in ("", "disabled") else BlendMode.OPAQUE

    # Blending decides the intent even when an alpha test is also set: Black Ops' decals and
    # blend layers are srcalpha/invsrcalpha with alphaTest gt0, and so are Black Ops II's own
    # (zm_nuked mc/dec_glass02_shatter). Read as alpha-tested, zombie_coast's snow and ice blend
    # layers matched opaque `t0` sets and drew solid over the icebergs (v50).
    # `invdestcolor`/`one` is additive that saturates -- Black Ops' `effect_*_add` glows, light rays
    # and embers, Black Ops II's `effect_ej431eje`. Read as a blend it matched alpha-blended sets, and
    # textures that carry their shape in colour drew as solid squares (zombie_coast v108: the
    # Pack-a-Punch spotlight's glow, the lighthouse rays).
    if src in ("one", "srcalpha", "invdestcolor") and dst == "one":
        return BlendMode.ADD
    if src in ("destcolor", "zero") and dst in ("srccolor", "zero", "invsrcalpha"):
        return BlendMode.MULTIPLY
    if src == "one" and dst == "invsrccolor":
        return BlendMode.SCREEN
    if src in ("srcalpha", "one") and dst == "invsrcalpha":
        return BlendMode.BLEND
    if at not in ("", "disabled"):
        return BlendMode.ALPHA_TEST
    return BlendMode.BLEND if dst else BlendMode.UNKNOWN


class SurfaceClass(enum.StrEnum):
    """
    What kind of thing the material is painted on.

    Inferred from the source name and techset, because BO2's technique sets are
    grouped the same way and this narrows the candidate list enormously.
    """

    WORLD = "world"
    MODEL = "model"
    CHARACTER = "character"
    WEAPON = "weapon"
    EFFECT = "effect"
    SKY = "sky"
    WATER = "water"
    DECAL = "decal"
    UI = "ui"
    FOLIAGE = "foliage"
    GLASS = "glass"
    UNKNOWN = "unknown"


@dataclass(slots=True)
class TextureBinding:
    """One texture slot on a material."""

    semantic: Semantic
    image: str
    slot_name: str = ""
    clamp_u: bool = False
    clamp_v: bool = False
    filter: str = "aniso2x"
    mip_map: str = "linear"
    nomipmaps: bool = False

    def to_dict(self) -> dict[str, Any]:
        return {
            "semantic": str(self.semantic),
            "image": self.image,
            "slot_name": self.slot_name,
            "clamp_u": self.clamp_u,
            "clamp_v": self.clamp_v,
            "filter": self.filter,
            "mip_map": self.mip_map,
        }

    @classmethod
    def from_dict(cls, d: dict) -> "TextureBinding":
        return cls(
            semantic=Semantic(d.get("semantic", "unknown")),
            image=d.get("image", ""),
            slot_name=d.get("slot_name", ""),
            clamp_u=d.get("clamp_u", False),
            clamp_v=d.get("clamp_v", False),
            filter=d.get("filter", "aniso2x"),
            mip_map=d.get("mip_map", "linear"),
        )


@dataclass(slots=True)
class RenderState:
    """The parts of the source render state that survive a port."""

    blend: BlendMode = BlendMode.OPAQUE
    alpha_test: str = "disabled"
    two_sided: bool = False
    depth_write: bool = True
    depth_test: str = "less_equal"
    polygon_offset: str = "offset0"
    sort_key: int = 0
    camera_region: str = "lit"
    #: The exact colour blend, as the source wrote it: one intent covers several equations
    #: (premultiplied `one`/`invsrcalpha` and straight `srcalpha`/`invsrcalpha` are both a blend).
    blend_src: str = ""
    blend_dst: str = ""
    blend_op: str = ""

    @property
    def is_decal(self) -> bool:
        """
        Polygon offset is how every COD engine pushes decals forward.

        Except `offsetShadowmap`, the shadow-map depth bias opaque surfaces use against shadow acne:
        85 of zombie_coast's materials carry it -- skin, cloth, flags and the two-layer snow props --
        and none is a decal. Counting it made the iceberg a decal to the shader matcher.
        """
        return self.polygon_offset not in ("", "offset0", "offsetShadowmap")

    @property
    def is_transparent(self) -> bool:
        return self.blend in (BlendMode.BLEND, BlendMode.ADD, BlendMode.SCREEN, BlendMode.MULTIPLY)

    def to_dict(self) -> dict[str, Any]:
        return {
            "blend": str(self.blend),
            "alpha_test": self.alpha_test,
            "two_sided": self.two_sided,
            "depth_write": self.depth_write,
            "depth_test": self.depth_test,
            "polygon_offset": self.polygon_offset,
            "sort_key": self.sort_key,
            "camera_region": self.camera_region,
            "blend_src": self.blend_src,
            "blend_dst": self.blend_dst,
            "blend_op": self.blend_op,
            "is_decal": self.is_decal,
            "is_transparent": self.is_transparent,
        }

    @classmethod
    def from_dict(cls, d: dict | None) -> "RenderState":
        if not d:
            return cls()
        return cls(
            blend=BlendMode(d.get("blend", "opaque")),
            alpha_test=d.get("alpha_test", "disabled"),
            two_sided=d.get("two_sided", False),
            depth_write=d.get("depth_write", True),
            depth_test=d.get("depth_test", "less_equal"),
            polygon_offset=d.get("polygon_offset", "offset0"),
            sort_key=d.get("sort_key", 0),
            camera_region=d.get("camera_region", "lit"),
            blend_src=d.get("blend_src", ""),
            blend_dst=d.get("blend_dst", ""),
            blend_op=d.get("blend_op", ""),
        )


#: Tokens in a material or techset name that reveal what it is for.
_SURFACE_TOKENS: tuple[tuple[str, SurfaceClass], ...] = (
    ("sky", SurfaceClass.SKY),
    ("water", SurfaceClass.WATER),
    ("ocean", SurfaceClass.WATER),
    ("decal", SurfaceClass.DECAL),
    ("dec_", SurfaceClass.DECAL),
    ("glass", SurfaceClass.GLASS),
    ("window", SurfaceClass.GLASS),
    ("foliage", SurfaceClass.FOLIAGE),
    ("tree", SurfaceClass.FOLIAGE),
    ("grass", SurfaceClass.FOLIAGE),
    ("leaf", SurfaceClass.FOLIAGE),
    ("effect", SurfaceClass.EFFECT),
    ("fx", SurfaceClass.EFFECT),
    ("particle", SurfaceClass.EFFECT),
    # Treyarch names character materials `mtl_c_*` (characters are `c_*` models; see
    # _CHARACTER_NAME); a bare "zombie", "head" or "body" is not a character in a zombies map --
    # they classed the mystery box, perk machines, a desk "body" and a filing cabinet "body"
    # as skin (v50).
    ("char", SurfaceClass.CHARACTER),
    ("skin", SurfaceClass.CHARACTER),
    ("wpn", SurfaceClass.WEAPON),
    ("weapon", SurfaceClass.WEAPON),
    ("viewmodel", SurfaceClass.WEAPON),
    ("hud", SurfaceClass.UI),
    ("menu", SurfaceClass.UI),
    ("ui_", SurfaceClass.UI),
    ("2d", SurfaceClass.UI),
)


_CHARACTER_NAME = re.compile(r"(?:^|/)(?:mtl_)?c_")


def classify_surface(name: str, techset: str = "", state: RenderState | None = None) -> SurfaceClass:
    """Best-effort surface classification from names plus render state."""
    haystack = f"{name} {techset}".lower()

    if state is not None and state.is_decal:
        return SurfaceClass.DECAL

    if _CHARACTER_NAME.search(name.lower()):
        return SurfaceClass.CHARACTER

    for token, cls in _SURFACE_TOKENS:
        if token in haystack:
            return cls

    # The material-name prefix convention is stable across Treyarch/IW titles:
    # wc_ = world  mc_ = model  sc_ = sky/special.
    if haystack.startswith("wc") or "/wc/" in haystack:
        return SurfaceClass.WORLD
    if haystack.startswith("mc") or "/mc/" in haystack:
        return SurfaceClass.MODEL
    return SurfaceClass.UNKNOWN


@dataclass
class CodirMaterial(CodirAsset):
    """A material, expressed as behaviour rather than as a shader name."""

    kind: str = "material"

    source_techset: str = ""
    surface_class: SurfaceClass = SurfaceClass.UNKNOWN
    textures: list[TextureBinding] = field(default_factory=list)
    state: RenderState = field(default_factory=RenderState)
    constants: dict[str, list[float]] = field(default_factory=dict)
    game_flags: list[str] = field(default_factory=list)
    surface_type: str = ""
    """Footstep / bullet-impact material, e.g. "brick", "metal". Gameplay-visible."""
    atlas_rows: int = 1
    atlas_columns: int = 1

    # ---- queries the converter and matcher rely on -----------------------

    def image_for(self, semantic: Semantic) -> str | None:
        for t in self.textures:
            if t.semantic is semantic:
                return t.image
        return None

    def semantics(self) -> set[Semantic]:
        return {t.semantic for t in self.textures}

    def signature(self) -> "MaterialSignature":
        """
        The comparable form used to rank BO2 technique sets.

        UNKNOWN semantics are excluded: a slot we could not classify carries no
        information, and letting it into the signature makes two materials look
        different when all we actually know is that we failed to read them.
        `texture_count` is kept separately so "no textures at all" stays
        distinguishable from "textures we could not classify" -- the matcher
        must not treat those as equally informative.
        """
        known = {s for s in self.semantics() if s is not Semantic.UNKNOWN}
        # A slot the reader could not name was labelled from the source's semantic byte alone,
        # and Black Ops' custom shaders label nearly everything colour: the two-layer snow props
        # bind their own specular map (`p_zom_iceberg_s`) as "colour". The filename convention
        # is the better evidence there, and only there.
        from codport.plugins.assetdump import semantic_from_filename

        for binding in self.textures:
            if not binding.slot_name and binding.semantic is Semantic.COLOR:
                named = semantic_from_filename(binding.image or "")
                if named not in (Semantic.UNKNOWN, Semantic.COLOR):
                    known.add(named)
        return MaterialSignature(
            semantics=frozenset(known),
            blend=self.state.blend,
            two_sided=self.state.two_sided,
            decal=self.state.is_decal,
            camera_region=self.state.camera_region,
            surface_class=self.surface_class,
            has_atlas=self.atlas_rows > 1 or self.atlas_columns > 1,
            texture_count=len(self.textures),
        )

    # ---- serialisation ---------------------------------------------------

    def to_dict(self) -> dict[str, Any]:
        d = self.base_dict()
        d.update(
            {
                "source_techset": self.source_techset,
                "surface_class": str(self.surface_class),
                "textures": [t.to_dict() for t in self.textures],
                "state": self.state.to_dict(),
                "constants": self.constants,
                "game_flags": self.game_flags,
                "surface_type": self.surface_type,
                "atlas_rows": self.atlas_rows,
                "atlas_columns": self.atlas_columns,
            }
        )
        return d

    @classmethod
    def from_dict(cls, d: dict[str, Any]) -> "CodirMaterial":
        m = cls()
        m.load_base(d)
        m.source_techset = d.get("source_techset", "")
        m.surface_class = SurfaceClass(d.get("surface_class", "unknown"))
        m.textures = [TextureBinding.from_dict(t) for t in d.get("textures", [])]
        m.state = RenderState.from_dict(d.get("state"))
        m.constants = dict(d.get("constants") or {})
        m.game_flags = list(d.get("game_flags") or [])
        m.surface_type = d.get("surface_type", "")
        m.atlas_rows = d.get("atlas_rows", 1)
        m.atlas_columns = d.get("atlas_columns", 1)
        return m


@dataclass(slots=True, frozen=True)
class MaterialSignature:
    """
    Hashable behaviour fingerprint.

    Two materials with the same signature can use the same BO2 technique set,
    which is what lets the matcher cache results across a map with 800
    materials and maybe 40 distinct behaviours.
    """

    semantics: frozenset[Semantic]
    blend: BlendMode
    two_sided: bool
    decal: bool
    camera_region: str
    surface_class: SurfaceClass
    has_atlas: bool = False
    texture_count: int = 0

    @property
    def semantics_known(self) -> bool:
        """
        False when the material binds textures we could not classify.

        This is the difference between "this surface genuinely uses no
        textures" and "we do not know what this surface uses". The matcher
        must not be confident in the second case.
        """
        return bool(self.semantics) or self.texture_count == 0

    def describe(self) -> str:
        slots = "+".join(sorted(str(s) for s in self.semantics)) or (
            f"{self.texture_count} unclassified" if self.texture_count else "no textures"
        )
        bits = [f"{self.surface_class}", slots, str(self.blend)]
        if self.two_sided:
            bits.append("two-sided")
        if self.decal:
            bits.append("decal")
        return " / ".join(bits)

    def to_dict(self) -> dict[str, Any]:
        return {
            "semantics": sorted(str(s) for s in self.semantics),
            "semantics_known": self.semantics_known,
            "texture_count": self.texture_count,
            "blend": str(self.blend),
            "two_sided": self.two_sided,
            "decal": self.decal,
            "camera_region": self.camera_region,
            "surface_class": str(self.surface_class),
            "has_atlas": self.has_atlas,
        }
