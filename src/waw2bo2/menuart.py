"""Desktop-authored menu artwork, using the measured T6 menu canvases."""
from __future__ import annotations

import json
import struct
from functools import lru_cache
from pathlib import Path
from PIL import Image
from .iwi import FMT_RGBA, FLAG_NOMIPMAPS

SIZES = {'blit': (512, 256), 'large': (2048, 2048), 'blur': (2048, 2048)}


def read_art(path: str, role: str) -> Image.Image:
    with Image.open(path) as source:
        if source.size != SIZES[role]:
            w, h = SIZES[role]
            raise ValueError(f'{role.title()} requires {w} × {h} pixels; selected image is {source.width} × {source.height}.')
        image = source.convert('RGBA')
    if role == 'blit' and image.getextrema()[3][0] == 255:
        raise ValueError('Blit needs a transparent background. Choose a PNG or TGA with alpha transparency.')
    return image


def validate_art(settings) -> None:
    paths = [getattr(settings, 'menu_' + role) for role in SIZES]
    if any(paths):
        if not all(paths):
            raise ValueError('Choose all three images: Blit, Large, and Blur.')
        for role, path in zip(SIZES, paths):
            stat = Path(path).stat()
            _validate_file(path, role, stat.st_mtime_ns, stat.st_size)


@lru_cache(maxsize=24)
def _validate_file(path, role, modified, size):
    read_art(path, role)


def material(image: str) -> dict:
    # Native T6 2D material: straight alpha, clamped linear sampling, no mips.
    return {'_game': 't6', '_type': 'material', '_version': 1,
            'cameraRegion': 'none', 'constants': [], 'contents': 1, 'gameFlags': [],
            'layeredSurfaceTypes': 536870912, 'sortKey': 40,
            'stateBits': [{'alphaTest': 'gt0', 'blendOpAlpha': 'add', 'blendOpRgb': 'add',
                           'colorWriteAlpha': True, 'colorWriteRgb': True, 'cullFace': 'back',
                           'depthTest': 'disabled', 'depthWrite': False, 'dstBlendAlpha': 'one',
                           'dstBlendRgb': 'invsrcalpha', 'polygonOffset': 'offset0',
                           'polymodeLine': False, 'srcBlendAlpha': 'invdestalpha', 'srcBlendRgb': 'srcalpha'}],
            'stateBitsEntry': [-1, -1, 0] + [-1] * 33,
            'stateFlags': 0, 'surfaceFlags': 0, 'surfaceTypeBits': 0,
            'techniqueSet': 'trivial_9z33feqw', 'textureAtlas': {'columns': 1, 'rows': 1},
            'textures': [{'image': image, 'isMatureContent': False, 'name': 'colorMap',
                          'samplerState': {'clampU': True, 'clampV': True, 'clampW': True,
                                           'filter': 'linear', 'mipMap': 'disabled'}, 'semantic': '2D'}]}


def stage_art(settings, root: Path) -> None:
    validate_art(settings)
    metadata = root / 'menu.json'
    previous = json.loads(metadata.read_text(encoding='utf-8')) if metadata.is_file() else {}
    if previous.get('desktop_authored'):
        for name in previous.get('desktop_files', []):
            # Only direct generated image/material children may be removed.
            relative = Path(name)
            if len(relative.parts) == 2 and relative.parts[0] in ('images', 'materials') and relative.suffix in ('.iwi', '.json'):
                (root / relative).unlink(missing_ok=True)
    if not any((settings.menu_title, settings.menu_description, settings.menu_blit)):
        # Remove only metadata previously created by this launcher.
        if previous.get('desktop_authored'):
            metadata.unlink()
        return
    root.mkdir(parents=True, exist_ok=True)
    data = {'title': settings.menu_title or settings.project,
            'description': settings.menu_description or f'{settings.project}: World at War custom map',
            'desktop_authored': True}
    generated = []
    if settings.menu_blit:
        images = {role: read_art(getattr(settings, 'menu_' + role), role) for role in SIZES}
        # The three-upload workflow derives lobby and loading artwork from Large.
        images['icon'] = images['large'].resize((256, 256), Image.Resampling.LANCZOS)
        bindings = {'map': 'large', 'map_blur': 'blur', 'blit': 'blit', 'icon': 'icon',
                    'zclassic_default': 'large'}
        bindings.update({'loadscreen:' + suffix: 'large' for suffix in ('zclassic_default', 'zclassic_')})
        for folder in ('materials', 'images'):
            (root / folder).mkdir(parents=True, exist_ok=True)
        for role, image in images.items():
            pixels = image.tobytes('raw', 'RGBA')
            header = b'IWi\x1b' + struct.pack('<bbHHHf', FMT_RGBA, FLAG_NOMIPMAPS, image.width, image.height, 1, 0.0)
            header += bytes(16) + struct.pack('<8I', 64 + len(pixels), *([0] * 7))
            (root / 'images' / f'menu_{settings.project}_{role}.iwi').write_bytes(header + pixels)
            generated.append(f'images/menu_{settings.project}_{role}.iwi')
        for suffix, role in bindings.items():
            name = (f'loadscreen_{settings.project}_' + suffix.split(':')[1] if ':' in suffix
                    else f'menu_{settings.project}_{suffix}')
            (root / 'materials' / (name + '.json')).write_text(json.dumps(material(f'menu_{settings.project}_{role}'), indent=2), encoding='utf-8')
            generated.append(f'materials/{name}.json')
        data.update(icon=f'menu_{settings.project}_icon', blit=f'menu_{settings.project}_blit')
    data['desktop_files'] = generated
    metadata.write_text(json.dumps(data, indent=2) + '\n', encoding='utf-8')
