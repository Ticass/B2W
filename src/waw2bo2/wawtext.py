"""Read text that WaW's tools or map authors wrote.

WaW's tools write scripts, weapon files and vision files in the Windows code
page; map authors' comments and display names may hold bytes such as 0xD7
that are not UTF-8. Files the converter writes itself are UTF-8.
"""
from __future__ import annotations

from pathlib import Path


def read(path: Path) -> str:
    """Like Path.read_text: universal newlines, so a file this converter
    wrote on Windows (CRLF) reads back unchanged instead of gaining a CR on
    every read/write cycle."""
    data = Path(path).read_bytes()
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError:
        text = data.decode("cp1252", errors="replace")
    return text.replace("\r\n", "\n").replace("\r", "\n")
