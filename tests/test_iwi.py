import struct, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
from waw2bo2 import iwi

def oat_load_iwi27(blob):
    """Python mirror of OAT iwi::LoadIwi27 size validation."""
    assert blob[:3] == b"IWi" and blob[3] == 27
    fmt, flags, w, h, d, _g = struct.unpack_from("<bbHHHf", blob, 4)
    sizes = struct.unpack_from("<8I", blob, 32)
    assert fmt in (1, 2, 3, 4, 5, 0xB, 0xC, 0xD, 0xE), fmt
    has_mips = not (flags & iwi.FLAG_NOMIPMAPS)
    faces = 6 if flags & iwi.FLAG_CUBEMAP else 1
    vol = bool(flags & iwi.FLAG_VOLMAP)
    count = iwi.full_mip_count(w, h, d if vol else 1) if has_mips else 1
    cur = 64
    for lvl in range(count - 1, -1, -1):
        cur += iwi.mip_size(fmt, w, h, d if vol else 1, lvl) * faces
        if lvl < 8:
            assert sizes[lvl] == cur, (lvl, sizes[lvl], cur)
    assert cur == len(blob), (cur, len(blob))
    return dict(fmt=fmt, flags=flags, w=w, h=h, d=d, mips=count, faces=faces)

def main(folder):
    for p in sorted(Path(folder).glob("*.dds")):
        blob = iwi.dds_to_iwi(p.read_bytes())
        print(p.name, oat_load_iwi27(blob))

if __name__ == "__main__":
    main(sys.argv[1])
