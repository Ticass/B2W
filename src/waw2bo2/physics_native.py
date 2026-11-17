"""Request adapter for the native map physics stages."""
import json
from pathlib import Path
from tempfile import TemporaryDirectory

from . import native, weapons


def run(operation: str, **fields):
    with TemporaryDirectory(prefix='waw2bo2_physics_') as folder:
        request = Path(folder) / 'request.json'
        request.write_text(json.dumps({'operation': operation, **fields}), encoding='utf-8')
        response = json.loads(native.run('map-physics', str(request)))
    if 'weaponError' in response:
        raise weapons.WeaponError(response['weaponError'])
    if 'invalid' in response:
        raise ValueError(response['invalid'])
    return response['result']
