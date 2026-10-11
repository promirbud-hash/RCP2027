import tempfile
import unittest
from pathlib import Path

from PySide6.QtGui import QColor, QImage

from rcp2027_app.data.wallpaper import (
    WALLPAPER_BYTE_COUNT,
    WALLPAPER_HEIGHT,
    WALLPAPER_WIDTH,
    make_wallpaper_image,
    save_wallpaper_assets,
    wallpaper_c_header,
    wallpaper_rgb565_bytes,
)


class WallpaperTests(unittest.TestCase):
    def test_scaling_modes_produce_rgb565_screen_size(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "source.png"
            image = QImage(160, 90, QImage.Format.Format_RGB32)
            image.fill(QColor("#ee3344"))
            self.assertTrue(image.save(str(source)))

            for mode in ("crop", "fit"):
                with self.subTest(mode=mode):
                    output = make_wallpaper_image(source, mode)
                    self.assertEqual(output.size().width(), WALLPAPER_WIDTH)
                    self.assertEqual(output.size().height(), WALLPAPER_HEIGHT)
                    self.assertEqual(len(wallpaper_rgb565_bytes(output)), WALLPAPER_BYTE_COUNT)

    def test_export_writes_raw_header_and_overlay_template(self) -> None:
        image = QImage(WALLPAPER_WIDTH, WALLPAPER_HEIGHT, QImage.Format.Format_RGB16)
        image.fill(QColor("#123456"))
        with tempfile.TemporaryDirectory() as temporary:
            raw_path, header_path, layout_path = save_wallpaper_assets(temporary, image)

            self.assertEqual(raw_path.name, "WALLPAPER.RGB565")
            self.assertEqual(raw_path.stat().st_size, WALLPAPER_BYTE_COUNT)
            self.assertIn(b"kWallpaperRgb565", wallpaper_c_header(image))
            self.assertIn(b'"attendance"', layout_path.read_bytes())
            self.assertTrue(header_path.is_file())

    def test_invalid_image_path_is_reported(self) -> None:
        with self.assertRaisesRegex(ValueError, "Nie można odczytać"):
            make_wallpaper_image("missing-wallpaper-image.png")
