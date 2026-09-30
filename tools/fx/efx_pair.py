"""Pair stock BO2 .efx sources with their compiled FxElemDefs (T6 OAT JSON dumps).

Used to learn the T6 compiled element flag layout from ground truth
(tools/fx/efx_pair.py <bo2 raw/fx> <t6 fx dump roots...>)."""
import collections, glob, json, os, re, sys

TYPE_KEYS = {"billboardSprite": 0, "orientedSprite": 1, "rotatedSprite": 2, "tail": 3, "line": 4, "trail": 5,
             "cloud": 6, "model": 7, "light": 8, "spotLight": 9, "sound": 10, "decal": 11, "runner": 12}


def parse_efx(text):
    elems = []
    for block in re.findall(r"\n\{(.*?)\n\}", text, re.S):
        e = {"flags": set(), "editor": set()}
        m = re.search(r"\n\s*flags([^;]*);", block)
        if m:
            e["flags"] = set(m.group(1).split())
        m = re.search(r"\n\s*editorFlags([^;]*);", block)
        if m:
            e["editor"] = set(m.group(1).split())
        for k in ("lifeSpanMsec", "spawnRange", "fadeInRange", "fadeOutRange", "spawnFrustumCullRadius"):
            m = re.search(r"\n\s*" + k + r"\s+([^;]*);", block)
            e[k] = [float(x) for x in m.group(1).split()] if m else None
        e["type"] = None
        for k, t in TYPE_KEYS.items():
            if re.search(r"\n\s*" + k + r"\s*\n\s*\{", block):
                e["type"] = t
        elems.append(e)
    return elems


def key(lst):
    return tuple(round(x, 2) for x in lst)


def pairs(raw_fx, dump_roots):
    seen = set()
    for root in dump_roots:
        for f in glob.glob(os.path.join(root, "**", "*.w2bfx.json"), recursive=True):
            d = json.load(open(f))
            if d["name"] in seen:
                continue
            src = os.path.join(raw_fx, d["name"] + ".efx")
            if not os.path.exists(src):
                continue
            seen.add(d["name"])
            efx = [e for e in parse_efx(open(src, errors="replace").read()) if "disabled" not in e["editor"]]
            pool = list(efx)
            for c in d["elemDefs"]:
                sig = (key(c["lifeSpanMsec"]), key(c["fadeInRange"]), key(c["fadeOutRange"]),
                       round(c["spawnFrustumCullRadius"], 2))
                cand = [e for e in pool if e["lifeSpanMsec"] and (key(e["lifeSpanMsec"]), key(e["fadeInRange"]),
                        key(e["fadeOutRange"]), round(e["spawnFrustumCullRadius"][0], 2)) == sig
                        and (e["type"] is None or e["type"] == c["elemType"])]
                if len(cand) >= 1 and all(x["flags"] == cand[0]["flags"] and x["editor"] == cand[0]["editor"] for x in cand):
                    pool.remove(cand[0])
                    yield d["name"], cand[0], c


def main():
    raw_fx, roots = sys.argv[1], sys.argv[2:]
    ps = list(pairs(raw_fx, roots))
    print("paired elems", len(ps))
    tokens = collections.Counter()
    for _, e, _ in ps:
        tokens.update(e["flags"] | {"ed:" + x for x in e["editor"]})
    for tok, n in tokens.most_common():
        if n < 3:
            continue
        best = []
        for b in range(32):
            agree = sum(1 for _, e, c in ps if ((tok in e["flags"] or tok[3:] in e["editor"]) == bool(c["flags"] >> b & 1)))
            best.append((agree, b))
        best.sort(reverse=True)
        a, b = best[0]
        print(f"{tok:28s} n={n:5d} bit 0x{1 << b:08x} agree {a}/{len(ps)}  next 0x{1 << best[1][1]:08x} {best[1][0]}")
    print("types", collections.Counter((e["type"], c["elemType"]) for _, e, c in ps))


if __name__ == "__main__":
    main()
