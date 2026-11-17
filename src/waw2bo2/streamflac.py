"""Opt-in lossless storage for the streamed side of a converted sound bank."""
from __future__ import annotations

import json
from pathlib import Path
from tempfile import TemporaryDirectory

from . import audio, native, parallel


def compress_rows(rows: list[dict], pcm_root: Path) -> dict:
    # A source used by both banks keeps one asset ID in T6's shared pool.
    loaded = {row["FileSource"].lower() for row in rows if row["Storage"] == "loaded"}
    sources = sorted({row["FileSource"] for row in rows if row["Storage"] == "streamed"
                      and row["FileSource"].endswith(".wav") and row["FileSource"].lower() not in loaded})
    if not sources:
        return {"files": 0, "original_bytes": 0, "output_bytes": 0}
    encoder = audio.find_decoder()
    if encoder is None:
        raise audio.SoundError("Streamed FLAC encoding needs FFmpeg")
    pcm_root = pcm_root.resolve()
    targets = {source: source.removesuffix(".wav") + ".flac" for source in sources}
    with TemporaryDirectory(prefix="waw2bo2_stream_", dir=pcm_root) as folder:
        scratch = Path(folder)
        tasks = []
        for index, name in enumerate(sources):
            source, target = (pcm_root / name).resolve(), (pcm_root / targets[name]).resolve()
            if not source.is_relative_to(pcm_root) or not target.is_relative_to(pcm_root):
                raise audio.SoundError(f"unsafe streamed audio path: {name}")
            tasks.append({"source": str(source), "target": str(target), "scratch": str(scratch / str(index))})
        request = scratch / "request.json"
        request.write_text(json.dumps({"encoder": str(encoder), "tasks": tasks,
                                       "workers": min(8, parallel.worker_count(len(tasks)))}), encoding="utf-8")
        results = json.loads(native.run("encode-streamed-flac", str(request)))
    encoded = {name: targets[name] for name, result in zip(sources, results, strict=True) if result["encoded"]}
    for row in rows:
        if row["Storage"] == "streamed" and row["FileSource"] in encoded:
            row["FileSource"] = encoded[row["FileSource"]]
    return {"files": len(encoded), "original_bytes": sum(r["original_bytes"] for r in results),
            "output_bytes": sum(r["output_bytes"] for r in results)}
