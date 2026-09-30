from __future__ import annotations

import json
from pathlib import Path


class MaterialError(ValueError):
    pass


def _safe_name(name: str) -> str:
    return "".join("_" if c in '/\\:*?\"<>|' else c for c in name)


def translate_materials(
    source_dir: Path,
    names: set[str],
    template: Path,
    output_dir: Path,
    image_dir: Path | None = None,
    allow_generated_aliases: bool = False,
    allow_missing_images: bool = False,
) -> int:
    """Translate only materials used by the extracted world.

    BO2's material compiler accepts the stock T6 JSON schema. We retain the
    stock technique/state layout and replace texture slots from the T4
    material. Missing source material or image data is an error; no fallback
    texture is invented.
    """
    base = json.loads(template.read_text(encoding="utf-8"))
    output_dir.mkdir(parents=True, exist_ok=True)
    count = 0
    missing_images: list[tuple[str, str]] = []
    generated_aliases: list[str] = []
    for name in sorted(names):
        source = source_dir / f"{name}.json"
        if not source.exists():
            source = source_dir / f"{_safe_name(name)}.json"
        if not source.exists() and name.startswith("*"):
            source = source_dir / "generated" / f"_{_safe_name(name[1:])}.json"
        if not source.exists() and name.startswith(",*"):
            source = source_dir / "generated" / f"_{_safe_name(name[2:])}.json"
        if not source.exists():
            if not (allow_generated_aliases and name.startswith("*")):
                raise MaterialError(f"missing extracted material: {name}")
            src = {"textures": [{"image": "$white", "semantic": "colorMap"}]}
            generated_aliases.append(name)
        else:
            src = json.loads(source.read_text(encoding="utf-8"))
        textures = [dict(t) for t in src.get("textures", []) if t.get("image")]
        if not textures:
            raise MaterialError(f"material has no usable texture slots: {name}")
        out = json.loads(json.dumps(base))
        out["_game"] = "t6"
        out["_type"] = "material"
        out["textures"] = textures
        if image_dir is not None:
            for texture in textures:
                image = texture["image"]
                candidates = [image_dir / f"{image}.dds", image_dir / f"{_safe_name(image)}.dds"]
                built_in = image.lower() in {"$identitynormalmap", "$black", "$white", "$gray", "$grey"}
                if not built_in and not any(p.exists() for p in candidates):
                    if allow_missing_images:
                        texture["image"] = "$white"
                    else:
                        missing_images.append((name, image))
        if any(material == name for material, _ in missing_images):
            continue
        relative = Path(*name.replace("\\", "/").split("/")) if "/" in name else Path(_safe_name(name))
        if any(part.startswith("*") or ":" in part for part in relative.parts):
            relative = Path(*[_safe_name(part) for part in relative.parts])
        out_path = output_dir / relative.with_suffix(".json")
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(
            json.dumps(out, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        count += 1
    if missing_images:
        preview = ", ".join(f"{m}:{i}" for m, i in missing_images[:12])
        suffix = " ..." if len(missing_images) > 12 else ""
        raise MaterialError(f"missing {len(missing_images)} referenced images ({preview}{suffix})")
    if generated_aliases:
        (output_dir / "generated_aliases.report.json").write_text(
            json.dumps(generated_aliases, indent=2) + "\n", encoding="utf-8"
        )
    return count
