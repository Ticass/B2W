"""Locate shared converter resources in a checkout or portable application."""
from pathlib import Path
import sys


def resource_root() -> Path:
    if getattr(sys, 'frozen', False):
        return Path(sys._MEIPASS)
    return Path(__file__).resolve().parents[2]
