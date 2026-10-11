from __future__ import annotations

import json
import struct
from pathlib import Path

from PySide6.QtCore import QPoint, QRect, QSize, Qt
from PySide6.QtGui import QColor, QImage, QPainter

WALLPAPER_WIDTH = 480
WALLPAPER_HEIGHT = 320
WALLPAPER_BYTE_COUNT = WALLPAPER_WIDTH * WALLPAPER_HEIGHT * 2
WALLPAPER_FILENAME = "WALLPAPER.RGB565"

WALLPAPER_ZONES = {
    "home": [
        {"id": "header", "label": "Nazwa terminala i tryb", "rect": (12, 12, 456, 42)},
        {"id": "clock", "label": "Godzina", "rect": (40, 62, 396, 77)},
        {"id": "date", "label": "Data", "rect": (70, 145, 336, 40)},
        {"id": "quote", "label": "Hasło motywacyjne i prompt RFID", "rect": (24, 195, 432, 77)},
        {"id": "status", "label": "Status urządzeń i przycisk ustawień", "rect": (12, 270, 456, 40)},
    ],
    "attendance": [
        {"id": "message", "label": "Komunikat wejścia / wyjścia", "rect": (40, 94, 400, 55)},
        {"id": "employee", "label": "Imię i nazwisko pracownika", "rect": (40, 153, 400, 52)},
    ],
}


def make_wallpaper_image(source_path: str | Path, sizing: str = "crop") -> QImage:
    source = QImage(str(source_path))
    if source.isNull():
        raise ValueError(f"Nie można odczytać obrazu: {source_path}")
    if sizing not in {"crop", "fit"}:
        raise ValueError("Nieznany sposób dopasowania obrazu.")

    aspect_mode = (
        Qt.AspectRatioMode.KeepAspectRatioByExpanding
        if sizing == "crop"
        else Qt.AspectRatioMode.KeepAspectRatio
    )
    scaled = source.scaled(
        QSize(WALLPAPER_WIDTH, WALLPAPER_HEIGHT),
        aspect_mode,
        Qt.TransformationMode.SmoothTransformation,
    )
    image = QImage(
        WALLPAPER_WIDTH,
        WALLPAPER_HEIGHT,
        QImage.Format.Format_RGB16,
    )
    image.fill(QColor(10, 17, 33))
    x = (WALLPAPER_WIDTH - scaled.width()) // 2
    y = (WALLPAPER_HEIGHT - scaled.height()) // 2
    painter = QPainter(image)
    painter.drawImage(QPoint(x, y), scaled)
    painter.end()

    if sizing == "crop" and (scaled.width() != WALLPAPER_WIDTH or
                             scaled.height() != WALLPAPER_HEIGHT):
        image = image.copy(QRect(0, 0, WALLPAPER_WIDTH, WALLPAPER_HEIGHT))
    return image


def wallpaper_rgb565_bytes(image: QImage) -> bytes:
    if image.size() != QSize(WALLPAPER_WIDTH, WALLPAPER_HEIGHT):
        raise ValueError("Tapeta musi mieć dokładnie 480 × 320 pikseli.")
    rgb565 = image.convertToFormat(QImage.Format.Format_RGB16)
    raw = bytes(rgb565.constBits())
    expected = WALLPAPER_BYTE_COUNT
    if len(raw) < expected:
        raise ValueError("Nie udało się odczytać pełnych danych obrazu RGB565.")
    return raw[:expected]


def wallpaper_c_header(image: QImage) -> bytes:
    raw = wallpaper_rgb565_bytes(image)
    values = struct.unpack(f"<{WALLPAPER_WIDTH * WALLPAPER_HEIGHT}H", raw)
    lines = [
        "#pragma once",
        "#include <Arduino.h>",
        "",
        f"constexpr uint16_t kWallpaperWidth = {WALLPAPER_WIDTH};",
        f"constexpr uint16_t kWallpaperHeight = {WALLPAPER_HEIGHT};",
        f"const uint16_t kWallpaperRgb565[{len(values)}] PROGMEM = {{",
    ]
    for offset in range(0, len(values), 12):
        row = ", ".join(f"0x{value:04X}" for value in values[offset:offset + 12])
        lines.append(f"    {row},")
    lines.extend(["};", ""])
    return "\n".join(lines).encode("ascii")


def wallpaper_layout_json() -> bytes:
    document = {
        "schema_version": 1,
        "display": {"width": WALLPAPER_WIDTH, "height": WALLPAPER_HEIGHT},
        "screens": WALLPAPER_ZONES,
        "note": "Overlay zones are placement guides; the firmware uses fixed UI positions.",
    }
    return (json.dumps(document, ensure_ascii=False, indent=2) + "\n").encode("utf-8")


def save_wallpaper_assets(directory: str | Path, image: QImage) -> tuple[Path, Path, Path]:
    output_dir = Path(directory)
    output_dir.mkdir(parents=True, exist_ok=True)
    raw_path = output_dir / WALLPAPER_FILENAME
    header_path = output_dir / "wallpaper_rgb565.h"
    layout_path = output_dir / "wallpaper_layout.json"

    raw_path.write_bytes(wallpaper_rgb565_bytes(image))
    header_path.write_bytes(wallpaper_c_header(image))
    layout_path.write_bytes(wallpaper_layout_json())
    return raw_path, header_path, layout_path
