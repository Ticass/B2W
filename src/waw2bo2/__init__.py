"""World at War to Black Ops II map conversion tools."""

__version__ = "0.2.15"


def _harden_platform_wmi() -> None:
    """CPython's Windows platform.machine()/uname() read WMI and fall back to
    the PROCESSOR_* environment on OSError, but a WMI answer without the field
    (a damaged WMI repository) raises KeyError('Architecture') instead.
    imageio_ffmpeg's decoder lookup hit it: a Subway build failed at audio
    decoding. A missing field takes CPython's own fallback."""
    import platform
    query = getattr(platform, "_wmi_query", None)
    if query is None or getattr(query, "_waw2bo2_hardened", False):
        return

    def hardened(table, *keys):
        try:
            return tuple(query(table, *keys))
        except KeyError as missing:
            raise OSError(f"WMI {table} answer lacks {missing}") from None

    hardened._waw2bo2_hardened = True
    platform._wmi_query = hardened


_harden_platform_wmi()
