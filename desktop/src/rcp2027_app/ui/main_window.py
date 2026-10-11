from __future__ import annotations

import sys
from pathlib import Path
from typing import Callable

from PySide6.QtCore import QThread, QRect, Signal, Qt
from PySide6.QtGui import QColor, QFontDatabase, QImage, QPainter, QPen
from PySide6.QtWidgets import (
    QApplication,
    QCheckBox,
    QComboBox,
    QDialog,
    QFileDialog,
    QFormLayout,
    QHBoxLayout,
    QHeaderView,
    QLabel,
    QLineEdit,
    QListWidget,
    QListWidgetItem,
    QMainWindow,
    QMessageBox,
    QPushButton,
    QPlainTextEdit,
    QTableWidget,
    QTableWidgetItem,
    QTabWidget,
    QTextBrowser,
    QVBoxLayout,
    QWidget,
)

from rcp2027_app.data.schemas import (
    ATTENDANCE_FIELDS,
    ATTENDANCE_FILE,
    EMPLOYEE_FIELDS,
    EMPLOYEES_FILE,
    LOG_FIELDS,
    LOGS_FILE,
    DeviceConfig,
    apps_script_request,
    check_apps_script,
    generate_api_token,
    initialize_sd_csv_files,
    load_config,
    load_csv,
    load_csv_file,
    prepare_sd_card,
    save_config,
    save_csv_file,
    validate_attendance_rows,
    validate_apps_script_url,
    validate_employee_rows,
    validate_sheet_url,
)
from rcp2027_app.data.wallpaper import (
    WALLPAPER_HEIGHT,
    WALLPAPER_WIDTH,
    WALLPAPER_ZONES,
    make_wallpaper_image,
    save_wallpaper_assets,
)


class _WallpaperPreview(QWidget):
    def __init__(self) -> None:
        super().__init__()
        self.setMinimumSize(600, 400)
        self.image = QImage()
        self.zones: list[dict] = []
        self.visible_zone_ids: set[str] = set()

    def set_preview(self, image: QImage, zones: list[dict], visible_zone_ids: set[str]) -> None:
        self.image = image
        self.zones = zones
        self.visible_zone_ids = visible_zone_ids
        self.update()

    def paintEvent(self, event) -> None:
        del event
        painter = QPainter(self)
        painter.fillRect(self.rect(), QColor("#172033"))
        scale = min(
            self.width() / WALLPAPER_WIDTH,
            self.height() / WALLPAPER_HEIGHT,
        )
        preview_width = round(WALLPAPER_WIDTH * scale)
        preview_height = round(WALLPAPER_HEIGHT * scale)
        left = (self.width() - preview_width) // 2
        top = (self.height() - preview_height) // 2
        preview_rect = self.rect().adjusted(0, 0, 0, 0)
        preview_rect.setRect(left, top, preview_width, preview_height)
        if not self.image.isNull():
            painter.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)
            painter.drawImage(preview_rect, self.image)
        else:
            painter.setPen(QColor("#ffffff"))
            painter.drawText(preview_rect, Qt.AlignmentFlag.AlignCenter,
                             "Wybierz obraz, aby zobaczyć podgląd tapety")
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        for zone in self.zones:
            if zone["id"] not in self.visible_zone_ids:
                continue
            x, y, width, height = zone["rect"]
            rect = QRect(
                left + round(x * scale),
                top + round(y * scale),
                round(width * scale),
                round(height * scale),
            )
            painter.fillRect(rect, QColor(4, 12, 24, 145))
            painter.setPen(QPen(QColor("#ffd166"), 2, Qt.PenStyle.DashLine))
            painter.drawRect(rect)
            painter.setPen(QColor("#ffffff"))
            painter.drawText(
                rect.adjusted(6, 4, -4, -4),
                Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignTop,
                zone["label"],
            )


class _AppsScriptWorker(QThread):
    succeeded = Signal(object)
    failed = Signal(str)

    def __init__(self, operation: Callable[[], object], parent: QWidget) -> None:
        super().__init__(parent)
        self._operation = operation

    def run(self) -> None:
        try:
            result = self._operation()
        except (OSError, ValueError) as error:
            self.failed.emit(str(error))
            return
        self.succeeded.emit(result)


class MainWindow(QMainWindow):
    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("RCP2027 — Rejestrator czasu pracy")
        self.resize(1120, 760)
        self.folder: Path | None = None
        self.remote_revisions: dict[str, str] = {}
        self.csv_paths: dict[str, Path] = {}
        self._api_worker: _AppsScriptWorker | None = None
        self._api_buttons: list[QPushButton] = []

        tabs = QTabWidget()
        tabs.addTab(self._configuration_page(), "Konfiguracja i karta SD")
        tabs.addTab(self._wallpaper_page(), "Tapeta")
        self.employee_table = self._table_page(
            tabs, EMPLOYEE_FIELDS, EMPLOYEES_FILE, "Pracownicy", self._save_employees,
            "employees",
        )
        self.attendance_table = self._table_page(
            tabs, ATTENDANCE_FIELDS, ATTENDANCE_FILE, "Ewidencja czasu",
            self._save_attendance, "attendanceRecords", select_csv=True,
        )
        self.log_table = self._table_page(
            tabs, LOG_FIELDS, LOGS_FILE, "Logi", self._save_logs,
            "logs", editable=False, select_csv=True,
        )
        self.setCentralWidget(tabs)
        self.setStyleSheet(
            """
            QMainWindow, QWidget { background: #f4f6fa; color: #172033; }
            QTabWidget::pane { border: 1px solid #dce2ec; background: white; }
            QTabBar::tab { padding: 12px 18px; background: #e9edf4; }
            QTabBar::tab:selected { background: #ffffff; color: #2457d6; }
            QLineEdit, QComboBox, QTableWidget {
                background: white; border: 1px solid #cbd3df; padding: 7px;
            }
            QPushButton { background: #2457d6; color: white; padding: 9px 14px;
                          border: 0; border-radius: 5px; }
            QPushButton:hover { background: #1745b8; }
            """
        )

    def closeEvent(self, event) -> None:
        if self._api_worker is not None and self._api_worker.isRunning():
            self.status.setText("Poczekaj na zakończenie żądania do Google.")
            event.ignore()
            return
        super().closeEvent(event)

    def _configuration_page(self) -> QWidget:
        page = QWidget()
        layout = QVBoxLayout(page)
        heading = QLabel("Konfiguracja terminala RCP2027")
        heading.setStyleSheet("font-size: 22px; font-weight: 700; padding: 12px;")
        layout.addWidget(heading)

        folder_row = QHBoxLayout()
        self.folder_label = QLabel("Nie wybrano głównego katalogu karty SD (np. E:\\)")
        browse = QPushButton("Wybierz kartę SD")
        browse.clicked.connect(self._choose_folder)
        folder_row.addWidget(self.folder_label, 1)
        folder_row.addWidget(browse)
        layout.addLayout(folder_row)

        form = QFormLayout()
        self.mode = QComboBox()
        self.mode.addItem("Offline — karta SD", "offline")
        self.mode.addItem("Online — Arkusze Google", "online")
        self.language = QComboBox()
        for label, code in (
            ("Polski", "pl"),
            ("English", "en"),
            ("Español", "es"),
            ("Deutsch", "de"),
        ):
            self.language.addItem(label, code)
        self.ssid = QLineEdit()
        self.wifi_password = QLineEdit()
        self.wifi_password.setEchoMode(QLineEdit.EchoMode.Password)
        self.timezone_offset = QLineEdit("60")
        self.timezone_offset.setPlaceholderText("np. 60 dla UTC+01:00")
        self.apps_script = QLineEdit()
        self.api_token = QLineEdit()
        self.api_token.setEchoMode(QLineEdit.EchoMode.Password)
        token_row = QHBoxLayout()
        token_row.addWidget(self.api_token, 1)
        generate_token = QPushButton("Generuj token")
        generate_token.clicked.connect(lambda: self.api_token.setText(generate_api_token()))
        token_row.addWidget(generate_token)
        self.employees_sheet = QLineEdit()
        self.attendance_sheet = QLineEdit()
        self.logs_sheet = QLineEdit()
        self.camera_enabled = QCheckBox("Włącz wykonywanie zdjęć przy odbiciu")
        self.camera_enabled.setChecked(False)
        for label, widget in (
            ("Tryb pracy", self.mode),
            ("Język terminala", self.language),
            ("Nazwa sieci Wi-Fi", self.ssid),
            ("Hasło Wi-Fi", self.wifi_password),
            ("Przesunięcie strefy czasu (minuty)", self.timezone_offset),
            ("Google Apps Script URL (/exec)", self.apps_script),
            ("Arkusz pracowników URL", self.employees_sheet),
            ("Arkusz wejść/wyjść URL", self.attendance_sheet),
            ("Arkusz logów URL", self.logs_sheet),
        ):
            form.addRow(label, widget)
        form.addRow("Token API (min. 32 znaki)", token_row)
        form.addRow("", self.camera_enabled)
        layout.addLayout(form)

        self.status = QLabel(
            "Linki do arkuszy służą do identyfikacji i kontroli. Odczyt/zapis terminala "
            "online realizuje wdrożone API Google Apps Script."
        )
        self.status.setWordWrap(True)
        layout.addWidget(self.status)
        buttons = QHBoxLayout()
        verify_links = QPushButton("Sprawdź linki")
        verify_links.clicked.connect(self._verify_links)
        test_api = QPushButton("Testuj API")
        test_api.clicked.connect(self._test_api)
        self._api_buttons.append(test_api)
        save = QPushButton("Zapisz konfigurację")
        save.clicked.connect(self._save_configuration)
        prepare = QPushButton("Przygotuj kartę SD")
        prepare.clicked.connect(self._prepare_sd_card)
        help_button = QPushButton("Pomoc")
        help_button.clicked.connect(self._show_help)
        buttons.addWidget(verify_links)
        buttons.addWidget(test_api)
        buttons.addWidget(help_button)
        buttons.addStretch()
        buttons.addWidget(save)
        buttons.addWidget(prepare)
        layout.addLayout(buttons)
        layout.addStretch()
        return page

    def _wallpaper_page(self) -> QWidget:
        page = QWidget()
        layout = QVBoxLayout(page)
        self.wallpaper_image = QImage()
        self._wallpaper_source_path: str | None = None
        self.wallpaper_output_dir: Path | None = None

        controls = QHBoxLayout()
        choose_image = QPushButton("Wybierz obraz")
        choose_image.clicked.connect(self._choose_wallpaper_image)
        self.wallpaper_source_label = QLabel("Nie wybrano obrazu")
        self.wallpaper_sizing = QComboBox()
        self.wallpaper_sizing.addItem("Wypełnij ekran (kadruj)", "crop")
        self.wallpaper_sizing.addItem("Dopasuj cały obraz", "fit")
        self.wallpaper_sizing.currentIndexChanged.connect(self._refresh_wallpaper_preview)
        controls.addWidget(choose_image)
        controls.addWidget(self.wallpaper_source_label, 1)
        controls.addWidget(QLabel("Dopasowanie:"))
        controls.addWidget(self.wallpaper_sizing)
        layout.addLayout(controls)

        screen_row = QHBoxLayout()
        self.wallpaper_screen = QComboBox()
        self.wallpaper_screen.addItem("Ekran główny", "home")
        self.wallpaper_screen.addItem("Potwierdzenie odbicia", "attendance")
        self.wallpaper_screen.currentIndexChanged.connect(self._refresh_wallpaper_zones)
        screen_row.addWidget(QLabel("Podgląd oznaczeń dla:"))
        screen_row.addWidget(self.wallpaper_screen)
        screen_row.addStretch()
        layout.addLayout(screen_row)

        preview_row = QHBoxLayout()
        self.wallpaper_preview = _WallpaperPreview()
        preview_row.addWidget(self.wallpaper_preview, 1)
        self.wallpaper_zone_list = QListWidget()
        self.wallpaper_zone_list.setMaximumWidth(270)
        self.wallpaper_zone_list.itemChanged.connect(self._refresh_wallpaper_preview)
        preview_row.addWidget(self.wallpaper_zone_list)
        layout.addLayout(preview_row, 1)

        footer = QHBoxLayout()
        self.wallpaper_output_label = QLabel("Nie wybrano folderu karty SD")
        choose_output = QPushButton("Wybierz folder karty SD")
        choose_output.clicked.connect(self._choose_wallpaper_output)
        export = QPushButton("Eksportuj tapetę na SD")
        export.clicked.connect(self._export_wallpaper)
        footer.addWidget(self.wallpaper_output_label, 1)
        footer.addWidget(choose_output)
        footer.addWidget(export)
        layout.addLayout(footer)

        self.wallpaper_status = QLabel(
            "Obraz zostanie przeskalowany do 480 × 320 pikseli. "
            "Eksport tworzy plik RGB565 dla terminala, nagłówek C i szablon oznaczeń."
        )
        self.wallpaper_status.setWordWrap(True)
        layout.addWidget(self.wallpaper_status)
        self._refresh_wallpaper_zones()
        return page

    def _choose_wallpaper_image(self) -> None:
        filename, _ = QFileDialog.getOpenFileName(
            self,
            "Wybierz obraz tapety",
            "",
            "Obrazy (*.png *.jpg *.jpeg *.bmp *.webp)",
        )
        if not filename:
            return
        try:
            self.wallpaper_image = make_wallpaper_image(
                filename, str(self.wallpaper_sizing.currentData())
            )
        except (OSError, ValueError) as error:
            self._show_error(str(error))
            return
        self._wallpaper_source_path = filename
        self.wallpaper_source_label.setText(Path(filename).name)
        self._refresh_wallpaper_preview()

    def _refresh_wallpaper_preview(self, *_args) -> None:
        if self.wallpaper_image.isNull() or self._wallpaper_source_path is None:
            return
        try:
            image = make_wallpaper_image(
                self._wallpaper_source_path,
                str(self.wallpaper_sizing.currentData()),
            )
        except (OSError, ValueError) as error:
            self.wallpaper_status.setText(f"Nie można przygotować podglądu: {error}")
            return
        self.wallpaper_image = image
        zones = WALLPAPER_ZONES[str(self.wallpaper_screen.currentData())]
        visible_ids = {
            self.wallpaper_zone_list.item(index).data(Qt.ItemDataRole.UserRole)
            for index in range(self.wallpaper_zone_list.count())
            if self.wallpaper_zone_list.item(index).checkState()
            == Qt.CheckState.Checked
        }
        self.wallpaper_preview.set_preview(image, zones, visible_ids)

    def _refresh_wallpaper_zones(self, *_args) -> None:
        if not hasattr(self, "wallpaper_zone_list"):
            return
        self.wallpaper_zone_list.blockSignals(True)
        self.wallpaper_zone_list.clear()
        zones = WALLPAPER_ZONES[str(self.wallpaper_screen.currentData())]
        for zone in zones:
            item = QListWidgetItem(zone["label"])
            item.setData(Qt.ItemDataRole.UserRole, zone["id"])
            item.setFlags(item.flags() | Qt.ItemFlag.ItemIsUserCheckable)
            item.setCheckState(Qt.CheckState.Checked)
            self.wallpaper_zone_list.addItem(item)
        self.wallpaper_zone_list.blockSignals(False)
        self._refresh_wallpaper_preview()

    def _choose_wallpaper_output(self) -> None:
        folder = QFileDialog.getExistingDirectory(self, "Wybierz folder karty SD")
        if folder:
            self.wallpaper_output_dir = Path(folder)
            self.wallpaper_output_label.setText(str(self.wallpaper_output_dir))

    def _export_wallpaper(self) -> None:
        if self.wallpaper_image.isNull():
            self._show_error("Najpierw wybierz obraz tapety.")
            return
        if self.wallpaper_output_dir is None:
            self._show_error("Wybierz folder karty SD jako miejsce zapisu.")
            return
        try:
            paths = save_wallpaper_assets(self.wallpaper_output_dir, self.wallpaper_image)
        except (OSError, ValueError) as error:
            self._show_error(f"Nie można wyeksportować tapety: {error}")
            return
        self.wallpaper_status.setText(
            f"Zapisano: {paths[0].name} ({paths[0].stat().st_size} B), "
            f"{paths[1].name}, {paths[2].name}. Uruchom ponownie terminal."
        )

    def _table_page(
        self,
        tabs: QTabWidget,
        fields: tuple[str, ...],
        filename: str,
        title: str,
        save_handler,
        remote_action: str,
        editable: bool = True,
        select_csv: bool = False,
    ) -> QTableWidget:
        page = QWidget()
        layout = QVBoxLayout(page)
        table = QTableWidget(0, len(fields))
        table.setHorizontalHeaderLabels(fields)
        table.horizontalHeader().setSectionResizeMode(QHeaderView.ResizeMode.Stretch)
        table.setAlternatingRowColors(True)
        layout.addWidget(table)
        controls = QHBoxLayout()
        if select_csv:
            open_file = QPushButton("Wybierz plik z karty SD")
            open_file.clicked.connect(
                lambda: self._choose_csv_file(table, fields, filename)
            )
            controls.addWidget(open_file)
            refresh = QPushButton("Odśwież z Google Sheets")
            refresh.clicked.connect(
                lambda: self._refresh_table(table, fields, filename, remote_action)
            )
            self._api_buttons.append(refresh)
            controls.addWidget(refresh)
        else:
            refresh = QPushButton("Odśwież")
            refresh.clicked.connect(
                lambda: self._refresh_table(table, fields, filename, remote_action)
            )
            self._api_buttons.append(refresh)
            controls.addWidget(refresh)
        if editable:
            add = QPushButton("Dodaj rekord")
            add.clicked.connect(lambda: table.insertRow(table.rowCount()))
            remove = QPushButton("Usuń zaznaczone")
            remove.clicked.connect(lambda: self._remove_selected(table))
            save = QPushButton("Zapisz / wyślij zmiany")
            save.clicked.connect(save_handler)
            self._api_buttons.append(save)
            controls.addWidget(add)
            controls.addWidget(remove)
            controls.addStretch()
            controls.addWidget(save)
        layout.addLayout(controls)
        tabs.addTab(page, title)
        return table

    def _refresh_table(
        self,
        table: QTableWidget,
        fields: tuple[str, ...],
        filename: str,
        remote_action: str,
    ) -> None:
        if self.mode.currentData() == "online":
            endpoint = self.apps_script.text().strip()
            token = self.api_token.text()

            def apply_response(response: object) -> None:
                if not isinstance(response, dict):
                    self._show_error("API zwróciło nieprawidłową odpowiedź.")
                    return
                rows = response.get("rows")
                if not isinstance(rows, list):
                    self._show_error("API nie zwróciło listy rekordów.")
                    return
                converted_rows: list[dict[str, str]] = []
                for row in rows:
                    if not isinstance(row, dict):
                        self._show_error(
                            "API zwróciło rekord o nieprawidłowym formacie."
                        )
                        return
                    converted_rows.append(
                        {field: str(row.get(field, "")) for field in fields}
                    )
                self._set_table_rows(table, fields, converted_rows)
                revision = response.get("revision")
                if isinstance(revision, str):
                    self.remote_revisions[remote_action] = revision

                self.status.setText(
                    f"Pobrano dane z Google Apps Script: {filename}."
                )

            self._start_api_worker(
                lambda: apps_script_request(endpoint, token, remote_action),
                apply_response,
                self._show_error,
            )
            return
        self._load_table(table, fields, filename)

    def _choose_folder(self) -> None:
        selected = QFileDialog.getExistingDirectory(self, "Wybierz katalog karty SD")
        if not selected:
            return
        self.folder = Path(selected)
        self.remote_revisions.clear()
        self.csv_paths.clear()
        self.folder_label.setText(str(self.folder))
        try:
            created_csvs = initialize_sd_csv_files(self.folder)
            config = load_config(self.folder)
        except FileNotFoundError:
            self.status.setText("Utworzono brakujące pliki CSV z nagłówkami.")
            self._load_table(self.employee_table, EMPLOYEE_FIELDS, EMPLOYEES_FILE)
            self.attendance_table.setRowCount(0)
            self.log_table.setRowCount(0)
        except (OSError, ValueError) as error:
            self._show_error(str(error))
        else:
            self._put_config(config)
            created_message = (
                f" Utworzono: {', '.join(path.name for path in created_csvs)}."
                if created_csvs
                else ""
            )
            self.status.setText(f"Wczytano config.json.{created_message}")
            if config.mode == "offline":
                self._load_table(self.employee_table, EMPLOYEE_FIELDS, EMPLOYEES_FILE)
                self.attendance_table.setRowCount(0)
                self.log_table.setRowCount(0)
                self.status.setText(
                    "Konfigurację wczytano. Wskaż attendance.csv i logs.csv z karty SD."
                )
            else:
                self.employee_table.setRowCount(0)
                self.attendance_table.setRowCount(0)
                self.log_table.setRowCount(0)

    def _load_table(
        self, table: QTableWidget, fields: tuple[str, ...], filename: str
    ) -> None:
        if self.folder is None:
            return
        try:
            rows = load_csv(self.folder, filename, fields)
        except (OSError, ValueError) as error:
            self._show_error(str(error))
            return
        table.setRowCount(len(rows))
        for row_index, row in enumerate(rows):
            for column, field in enumerate(fields):
                table.setItem(row_index, column, QTableWidgetItem(row[field]))

    def _choose_csv_file(
        self, table: QTableWidget, fields: tuple[str, ...], filename: str
    ) -> None:
        if self.folder is None:
            self._show_error("Najpierw wybierz główny katalog karty SD.")
            return
        selected, _ = QFileDialog.getOpenFileName(
            self,
            f"Wybierz plik {filename} z karty SD",
            str(self.folder),
            "Pliki CSV (*.csv);;Wszystkie pliki (*)",
        )
        if not selected:
            return
        path = Path(selected)
        try:
            path.resolve().relative_to(self.folder.resolve())
        except ValueError:
            self._show_error("Wybierz plik znajdujący się w katalogu wybranej karty SD.")
            return
        try:
            rows = load_csv_file(path, fields)
            if path.stat().st_size == 0:
                raise ValueError(f"{path.name}: plik jest pusty i nie zawiera nagłówka CSV.")
        except (OSError, ValueError) as error:
            self._show_error(str(error))
            return
        self.csv_paths[filename] = path
        self._set_table_rows(table, fields, rows)
        self.status.setText(f"Wczytano {len(rows)} rekordów z {path}.")

    @staticmethod
    def _set_table_rows(
        table: QTableWidget, fields: tuple[str, ...], rows: list[dict[str, str]]
    ) -> None:
        table.setRowCount(len(rows))
        for row_index, row in enumerate(rows):
            for column, field in enumerate(fields):
                table.setItem(row_index, column, QTableWidgetItem(row[field]))

    @staticmethod
    def _remove_selected(table: QTableWidget) -> None:
        for row in sorted({index.row() for index in table.selectedIndexes()}, reverse=True):
            table.removeRow(row)

    def _table_rows(self, table: QTableWidget, fields: tuple[str, ...]) -> list[dict[str, str]]:
        rows = []
        for row_index in range(table.rowCount()):
            rows.append(
                {
                    field: (
                        table.item(row_index, column).text()
                        if table.item(row_index, column) is not None
                        else ""
                    )
                    for column, field in enumerate(fields)
                }
            )
        return rows

    def _save_table(
        self,
        table: QTableWidget,
        fields: tuple[str, ...],
        filename: str,
        path: Path | None = None,
    ) -> None:
        destination = path or (self.folder / filename if self.folder is not None else None)
        if destination is None:
            self._show_error("Najpierw wybierz katalog karty SD.")
            return
        try:
            save_csv_file(destination, fields, self._table_rows(table, fields))
        except (OSError, ValueError) as error:
            self._show_error(str(error))
            return
        self.status.setText(f"Zapisano plik {destination}.")

    def _save_remote_table(
        self, action: str, rows: list[dict[str, str]], filename: str
    ) -> None:
        answer = QMessageBox.warning(
            self,
            "Zastąp dane w Google Sheets",
            f"Zastąpić całą zawartość arkusza {filename} bieżącą tabelą? "
            "Ta operacja usunie wcześniejsze rekordy w tym arkuszu.",
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.Cancel,
            QMessageBox.StandardButton.Cancel,
        )
        if answer != QMessageBox.StandardButton.Yes:
            return
        read_action = {
            "replaceEmployees": "employees",
            "replaceAttendance": "attendanceRecords",
        }[action]
        revision = self.remote_revisions.get(read_action)
        if not revision:
            self._show_error("Najpierw odśwież dane z Google, aby uniknąć nadpisania nowych rekordów.")
            return
        endpoint = self.apps_script.text().strip()
        token = self.api_token.text()

        def apply_response(response: object) -> None:
            if not isinstance(response, dict):
                self._show_error("API zwróciło nieprawidłową odpowiedź.")
                return
            new_revision = response.get("revision")
            if isinstance(new_revision, str):
                self.remote_revisions[read_action] = new_revision
            self.status.setText(f"Wysłano tabelę do Google Sheets: {filename}.")

        self._start_api_worker(
            lambda: apps_script_request(
                endpoint,
                token,
                action,
                rows=rows,
                expected_revision=revision,
            ),
            apply_response,
            self._show_error,
        )

    def _save_employees(self) -> None:
        rows = self._table_rows(self.employee_table, EMPLOYEE_FIELDS)
        try:
            validate_employee_rows(rows)
        except ValueError as error:
            self._show_error(str(error))
            return
        if self.mode.currentData() == "online":
            self._save_remote_table("replaceEmployees", rows, EMPLOYEES_FILE)
            return
        self._save_table(self.employee_table, EMPLOYEE_FIELDS, EMPLOYEES_FILE)

    def _save_attendance(self) -> None:
        rows = self._table_rows(self.attendance_table, ATTENDANCE_FIELDS)
        try:
            validate_attendance_rows(rows)
        except ValueError as error:
            self._show_error(str(error))
            return
        if self.mode.currentData() == "online":
            self._save_remote_table("replaceAttendance", rows, ATTENDANCE_FILE)
            return
        path = self.csv_paths.get(ATTENDANCE_FILE)
        if path is None:
            self._show_error("Najpierw wybierz plik attendance.csv z karty SD.")
            return
        self._save_table(self.attendance_table, ATTENDANCE_FIELDS, ATTENDANCE_FILE, path)

    def _save_logs(self) -> None:
        self._show_error("Logi są tylko do odczytu.")

    def _start_api_worker(
        self,
        operation: Callable[[], object],
        on_success: Callable[[object], None],
        on_error: Callable[[str], None],
    ) -> None:
        if self._api_worker is not None:
            self.status.setText("Poczekaj na zakończenie trwającego żądania.")
            return
        worker = _AppsScriptWorker(operation, self)
        self._api_worker = worker
        worker.succeeded.connect(on_success)
        worker.failed.connect(on_error)
        worker.finished.connect(self._finish_api_worker)
        for button in self._api_buttons:
            button.setEnabled(False)
        self.status.setText("Łączenie z Google Apps Script…")
        worker.start()

    def _finish_api_worker(self) -> None:
        self._api_worker = None
        for button in self._api_buttons:
            button.setEnabled(True)

    def _show_help(self) -> None:
        bundle_root = Path(
            getattr(sys, "_MEIPASS", Path(__file__).resolve().parents[4])
        )
        script_path = bundle_root / "google_apps_script" / "Code.gs"
        try:
            script = script_path.read_text(encoding="utf-8")
        except OSError as error:
            self._show_error(f"Nie można wczytać przykładowego kodu Apps Script: {error}")
            return

        dialog = QDialog(self)
        dialog.setWindowTitle("Pomoc — konfiguracja Google Sheets i Apps Script")
        dialog.resize(1120, 820)
        layout = QVBoxLayout(dialog)
        help_tabs = QTabWidget()
        instructions = QTextBrowser()
        instructions.setOpenExternalLinks(True)
        instructions.setStyleSheet(
            "QTextBrowser { background: #ffffff; color: #172033; border: 0; "
            "font-family: 'Segoe UI'; font-size: 12pt; padding: 12px; }"
        )
        instructions.setHtml(
            """
            <html><head><style>
            body { font-family: 'Segoe UI', sans-serif; font-size: 12pt;
                   line-height: 1.5; color: #172033; }
            h2 { color: #1745b8; font-size: 17pt; margin-top: 18px; }
            h3 { color: #2457d6; font-size: 13pt; margin-bottom: 5px; }
            p, li { margin-top: 6px; margin-bottom: 8px; }
            code { font-family: Consolas, monospace; background: #eef2f8;
                   padding: 3px 5px; }
            .warning { background: #fff4df; border-left: 5px solid #e3a126;
                       padding: 10px; }
            </style></head><body>
            <h2>1. Przygotuj arkusze Google</h2>
            <p>Utwórz trzy osobne arkusze. W pierwszym wierszu pierwszej zakładki
            każdego arkusza wklej nagłówek z odpowiedniej sekcji, zachowując
            kolejność kolumn.</p>
            <h3>Pracownicy</h3>
            <p><code>employee_id, card_uid, first_name, last_name, active, relay_access</code></p>
            <h3>Ewidencja czasu</h3>
            <p><code>event_id, timestamp, employee_id, card_uid, event_type, photo_file, source, notes</code></p>
            <h3>Logi</h3>
            <p><code>event_id, timestamp, level, code, message</code></p>

            <h2>2. Utwórz projekt Apps Script</h2>
            <p>Otwórz <a href="https://script.google.com">script.google.com</a>,
            utwórz projekt i zastąp zawartość pliku <code>Code.gs</code> kodem
            z zakładki „Kod Apps Script”. Zapisz projekt.</p>

            <h2>3. Ustaw właściwości skryptu</h2>
            <p>W ustawieniach projektu → Właściwości skryptu dodaj:</p>
            <ul>
              <li><code>RCP_API_TOKEN</code> — ten sam tajny token co w aplikacji;</li>
              <li><code>EMPLOYEES_SHEET_ID</code>;</li>
              <li><code>ATTENDANCE_SHEET_ID</code>;</li>
              <li><code>LOGS_SHEET_ID</code>.</li>
            </ul>
            <p>ID arkusza to część adresu pomiędzy
            <code>/spreadsheets/d/</code> a następnym ukośnikiem.</p>

            <h2>4. Wdróż aplikację internetową</h2>
            <p>Wybierz Wdróż → Nowe wdrożenie → Aplikacja internetowa.
            Ustaw wykonywanie jako właściciel. Dostęp skonfiguruj zgodnie
            z polityką Google Workspace — urządzenie wymaga publicznego
            endpointu, a każda operacja jest chroniona tokenem. Skopiuj URL
            kończący się na <code>/exec</code>.</p>

            <h2>5. Skonfiguruj aplikację RCP2027</h2>
            <p>Wybierz tryb online, wklej URL <code>/exec</code>, token i linki
            do trzech arkuszy. Kliknij „Sprawdź linki” i „Testuj API”, a następnie
            „Przygotuj kartę SD”. Po zmianie kodu opublikuj nową wersję wdrożenia.</p>

            <p class="warning"><b>Bezpieczeństwo:</b> nie udostępniaj tokenu ani
            pliku <code>config.json</code> — zawiera token API i hasło Wi-Fi.</p>
            </body></html>
            """
        )
        help_tabs.addTab(instructions, "Instrukcja")
        code_box = QPlainTextEdit()
        code_box.setReadOnly(True)
        code_box.setPlainText(script)
        code_box.setFont(QFontDatabase.systemFont(QFontDatabase.SystemFont.FixedFont))
        code_box.setStyleSheet(
            "QPlainTextEdit { background: #ffffff; color: #172033; "
            "font-size: 10pt; padding: 10px; }"
        )
        code_box.setLineWrapMode(QPlainTextEdit.LineWrapMode.NoWrap)
        help_tabs.addTab(code_box, "Kod Apps Script")
        layout.addWidget(help_tabs, 1)
        controls = QHBoxLayout()
        copy_button = QPushButton("Kopiuj kod")
        copy_button.clicked.connect(
            lambda: QApplication.clipboard().setText(code_box.toPlainText())
        )
        close_button = QPushButton("Zamknij")
        close_button.clicked.connect(dialog.accept)
        controls.addWidget(copy_button)
        controls.addStretch()
        controls.addWidget(close_button)
        layout.addLayout(controls)
        dialog.exec()

    def _configuration(self) -> DeviceConfig:
        return DeviceConfig(
            mode=self.mode.currentData(),
            language=self.language.currentData(),
            wifi_ssid=self.ssid.text().strip(),
            wifi_password=self.wifi_password.text(),
            timezone_offset_minutes=int(self.timezone_offset.text()),
            apps_script_url=self.apps_script.text().strip(),
            api_token=self.api_token.text(),
            employees_sheet_url=self.employees_sheet.text().strip(),
            attendance_sheet_url=self.attendance_sheet.text().strip(),
            logs_sheet_url=self.logs_sheet.text().strip(),
            camera_enabled=self.camera_enabled.isChecked(),
        )

    def _put_config(self, config: DeviceConfig) -> None:
        for widget, value in ((self.mode, config.mode), (self.language, config.language)):
            index = widget.findData(value)
            if index >= 0:
                widget.setCurrentIndex(index)
        self.ssid.setText(config.wifi_ssid)
        self.wifi_password.setText(config.wifi_password)
        self.timezone_offset.setText(str(config.timezone_offset_minutes))
        self.apps_script.setText(config.apps_script_url)
        self.api_token.setText(config.api_token)
        self.employees_sheet.setText(config.employees_sheet_url)
        self.attendance_sheet.setText(config.attendance_sheet_url)
        self.logs_sheet.setText(config.logs_sheet_url)
        self.camera_enabled.setChecked(config.camera_enabled)

    def _verify_links(self) -> None:
        try:
            apps_script = self.apps_script.text().strip()
            if apps_script:
                validate_apps_script_url(apps_script)
            sheet_ids = []
            for field in (
                self.employees_sheet,
                self.attendance_sheet,
                self.logs_sheet,
            ):
                value = field.text().strip()
                if value:
                    sheet_ids.append(validate_sheet_url(value))
            if not apps_script and not sheet_ids:
                raise ValueError("Wklej co najmniej jeden link Google.")
        except ValueError as error:
            self.status.setText(f"Błąd linku: {error}")
            return
        self.status.setText(
            f"Format linków jest prawidłowy. Rozpoznano {len(sheet_ids)} arkuszy."
        )

    def _test_api(self) -> None:
        endpoint = self.apps_script.text().strip()
        token = self.api_token.text()
        sheet_urls = (
            self.employees_sheet.text().strip(),
            self.attendance_sheet.text().strip(),
            self.logs_sheet.text().strip(),
        )
        self._start_api_worker(
            lambda: check_apps_script(
                endpoint, token, sheet_urls
            ),
            lambda result: self.status.setText(str(result)),
            lambda message: self.status.setText(message),
        )

    def _save_configuration(self) -> None:
        if self.folder is None:
            self._show_error("Najpierw wybierz katalog karty SD.")
            return
        try:
            path = save_config(self.folder, self._configuration())
        except (OSError, ValueError) as error:
            self._show_error(str(error))
            return
        self.status.setText(f"Konfigurację zapisano do {path}.")

    def _prepare_sd_card(self) -> None:
        if self.folder is None:
            self._show_error("Najpierw wybierz główny katalog karty SD.")
            return
        config_path = self.folder / "config.json"
        if config_path.exists():
            answer = QMessageBox.warning(
                self,
                "Nadpisanie konfiguracji",
                "Na karcie istnieje już config.json. Zastąpić go ustawieniami "
                "wprowadzonymi w aplikacji? Istniejące pliki CSV i ich rekordy "
                "pozostaną bez zmian.",
                QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.Cancel,
                QMessageBox.StandardButton.Cancel,
            )
            if answer != QMessageBox.StandardButton.Yes:
                return
        try:
            paths = prepare_sd_card(self.folder, self._configuration())
        except (OSError, ValueError) as error:
            self._show_error(str(error))
            return
        self.csv_paths.clear()
        self._load_table(self.employee_table, EMPLOYEE_FIELDS, EMPLOYEES_FILE)
        self.attendance_table.setRowCount(0)
        self.log_table.setRowCount(0)
        created = [path.name for path in paths[1:]]
        created_text = ", ".join(created) if created else "żadne — pliki już istniały"
        self.status.setText(
            f"Karta przygotowana: {config_path}. Nowe pliki: {created_text}. "
            "Istniejące rekordy CSV zachowano. W trybie offline dodaj pracowników "
            "i zapisz employees.csv przed użyciem terminala. Aby przeglądać "
            "ewidencję lub logi, wskaż ich pliki CSV."
        )

    def _show_error(self, message: str) -> None:
        QMessageBox.critical(self, "RCP2027 — błąd", message)
