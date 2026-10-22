"""Colours for the in-game FPS, volume and fast-forward HUDs.

The bridge draws these itself, so it is handed the launcher's palette as
RGDS_HUD_THEME=panel,ink,muted,accent,line (RRGGBB): the user's light or dark
mode from themes/, recoloured by the favourite colour chosen in settings.
"""
from pathlib import Path

from fields import read_fields

# The sixteen DS favourite colours, in firmware order (src/launcher/accent.hpp).
DS_COLOURS = (
    "61829a", "ba4900", "fb0018", "fb8afb", "fb9200", "f3e300", "aafb00", "00fb00", "00a238", "49db8a", "30baf3",
    "0059f3", "000092", "8a00d3", "d300eb", "fb0092"
)
KEYS = ("panel", "ink", "muted", "accent", "line")


def _rgb(text):
    return tuple(int(text[i:i + 2], 16) for i in (0, 2, 4))


def _luminance(colour):
    def linear(value):
        x = value / 255
        return x / 12.92 if x <= 0.04045 else ((x + 0.055) / 1.055)**2.4

    r, g, b = (linear(v) for v in colour)
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


def _contrast(a, b):
    x, y = _luminance(a) + 0.05, _luminance(b) + 0.05
    return max(x, y) / min(x, y)


def _mix(a, b, amount):
    return tuple(round(f + (t - f) * amount) for f, t in zip(a, b))


def hud_theme(app, data):
    """Return the RGDS_HUD_THEME value, or None to keep the built-in dark palette."""
    try:
        mode = "dark" if read_fields(Path(data) / "preferences.ini").get("mode") == "dark" else "light"
        theme = read_fields(Path(app) / "themes/default" / f"{mode}.ini")
        colours = {key: _rgb(theme[key]) for key in KEYS + ("background",)}
        ui = read_fields(Path(data) / "ui.ini")
        index = int(ui.get("accent", "-1"))
        if 0 <= index < len(DS_COLOURS):
            toward = (255, 255, 255) if mode == "dark" else (0, 0, 0)
            accent = _rgb(DS_COLOURS[index])
            for _ in range(40):
                if all(_contrast(accent, colours[surface]) >= 4.5 for surface in ("background", "panel")):
                    break
                accent = _mix(accent, toward, 0.05)
            colours["accent"] = accent
        return ",".join("%02x%02x%02x" % colours[key] for key in KEYS)
    except (KeyError, ValueError, OSError):
        return None
