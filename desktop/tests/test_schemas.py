import json
import tempfile
import unittest
from http.client import RemoteDisconnected
from io import BytesIO
from pathlib import Path
from unittest.mock import Mock, patch

from rcp2027_app.data.schemas import (
    ATTENDANCE_FIELDS,
    ATTENDANCE_FILE,
    EMPLOYEE_FIELDS,
    EMPLOYEES_FILE,
    LOG_FIELDS,
    LOGS_FILE,
    DeviceConfig,
    apps_script_request,
    generate_api_token,
    load_config,
    load_csv,
    load_csv_file,
    initialize_sd_csv_files,
    prepare_sd_card,
    save_config,
    save_csv,
    save_csv_file,
    validate_attendance_rows,
    validate_employee_rows,
    validate_apps_script_url,
    validate_sheet_url,
)


class SchemaTests(unittest.TestCase):
    def test_config_round_trip_and_reject_invalid_online_config(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            config = DeviceConfig(language="de", camera_enabled=True)
            save_config(folder, config)
            self.assertEqual(load_config(folder), config)
            with self.assertRaises(ValueError):
                save_config(folder, DeviceConfig(mode="online"))

    def test_csv_round_trip_escapes_commas_and_newlines(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            rows = [
                {
                    field: "linia 1,\nlinia 2" if field == "notes" else f"value-{field}"
                    for field in ATTENDANCE_FIELDS
                }
            ]
            save_csv(folder, ATTENDANCE_FILE, ATTENDANCE_FIELDS, rows)
            self.assertEqual(load_csv(folder, ATTENDANCE_FILE, ATTENDANCE_FIELDS), rows)

    def test_csv_rejects_incompatible_header(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / ATTENDANCE_FILE
            path.write_text("old,header\n", encoding="utf-8")
            with self.assertRaises(ValueError):
                load_csv(Path(temporary), ATTENDANCE_FILE, ATTENDANCE_FIELDS)

    def test_selected_csv_file_can_be_loaded_and_saved(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            card_folder = Path(temporary) / "SD-card"
            card_folder.mkdir()
            selected_file = card_folder / ATTENDANCE_FILE
            rows = [
                {
                    field: "linia 1,\nlinia 2" if field == "notes" else f"value-{field}"
                    for field in ATTENDANCE_FIELDS
                }
            ]

            save_csv_file(selected_file, ATTENDANCE_FIELDS, rows)

            self.assertEqual(load_csv_file(selected_file, ATTENDANCE_FIELDS), rows)

    def test_google_link_validation(self) -> None:
        self.assertEqual(
            validate_sheet_url("https://docs.google.com/spreadsheets/d/abc_123/edit"),
            "abc_123",
        )
        validate_apps_script_url("https://script.google.com/macros/s/deployment/exec")
        for link in (
            "http://docs.google.com/spreadsheets/d/abc/edit",
            "https://example.com/spreadsheets/d/abc/edit",
            "https://docs.google.com/document/d/abc/edit",
        ):
            with self.subTest(link=link), self.assertRaises(ValueError):
                validate_sheet_url(link)

    def test_employee_and_attendance_validation(self) -> None:
        employee = {
            "employee_id": "E-1",
            "card_uid": "A1B2",
            "first_name": "Ada",
            "last_name": "Lovelace",
            "active": "true",
            "relay_access": "false",
        }
        validate_employee_rows([employee])
        with self.assertRaisesRegex(ValueError, "powtórzone ID"):
            validate_employee_rows([employee, {**employee, "card_uid": "C3D4"}])
        attendance = {
            "event_id": "ev-1",
            "timestamp": "2026-10-06T08:00:00+02:00",
            "employee_id": "E-1",
            "card_uid": "A1B2",
            "event_type": "ENTRY",
            "photo_file": "",
            "source": "manual",
            "notes": "",
        }
        validate_attendance_rows([attendance])
        with self.assertRaisesRegex(ValueError, "ISO 8601"):
            validate_attendance_rows([{**attendance, "timestamp": "not-a-time"}])

    def test_generated_api_token_is_high_entropy_sized(self) -> None:
        token = generate_api_token()
        self.assertGreaterEqual(len(token), 32)

    def test_apps_script_rejects_non_object_response(self) -> None:
        response = Mock()
        response.__enter__ = Mock(return_value=BytesIO(b"[]"))
        response.__exit__ = Mock(return_value=False)
        with patch(
            "rcp2027_app.data.schemas.urlopen", return_value=response
        ), self.assertRaisesRegex(ValueError, "nieprawidłową odpowiedź"):
            apps_script_request(
                "https://script.google.com/macros/s/deployment/exec",
                "x" * 32,
                "ping",
            )

    def test_saved_config_is_json(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = save_config(Path(temporary), DeviceConfig())
            self.assertEqual(json.loads(path.read_text(encoding="utf-8"))["mode"], "offline")

    def test_prepare_sd_card_creates_firmware_files(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            paths = prepare_sd_card(folder, DeviceConfig())

            self.assertEqual(
                {path.name for path in paths},
                {"config.json", EMPLOYEES_FILE, ATTENDANCE_FILE, LOGS_FILE},
            )
            self.assertEqual(load_config(folder), DeviceConfig())
            self.assertTrue((folder / "photos").is_dir())
            self.assertEqual(load_csv(folder, EMPLOYEES_FILE, EMPLOYEE_FIELDS), [])
            self.assertEqual(load_csv(folder, ATTENDANCE_FILE, ATTENDANCE_FIELDS), [])
            self.assertEqual(load_csv(folder, LOGS_FILE, LOG_FIELDS), [])

    def test_prepare_sd_card_preserves_existing_csv_records(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            existing_rows = [
                {
                    "employee_id": "E-1",
                    "card_uid": "A1B2",
                    "first_name": "Ada",
                    "last_name": "Lovelace",
                    "active": "true",
                    "relay_access": "false",
                }
            ]
            save_csv(folder, EMPLOYEES_FILE, EMPLOYEE_FIELDS, existing_rows)

            created_paths = prepare_sd_card(folder, DeviceConfig(language="en"))

            self.assertNotIn(EMPLOYEES_FILE, {path.name for path in created_paths})
            self.assertEqual(load_csv(folder, EMPLOYEES_FILE, EMPLOYEE_FIELDS), existing_rows)
            self.assertEqual(load_config(folder).language, "en")

    def test_prepare_sd_card_rejects_incompatible_existing_csv(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            (folder / EMPLOYEES_FILE).write_text("wrong,header\n", encoding="utf-8")

            with self.assertRaisesRegex(ValueError, "niezgodny nagłówek"):
                prepare_sd_card(folder, DeviceConfig())

            self.assertFalse((folder / "config.json").exists())

    def test_initialize_sd_csv_files_creates_empty_files_and_keeps_existing_rows(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            employee = {
                "employee_id": "E-1",
                "card_uid": "A1B2",
                "first_name": "Ada",
                "last_name": "Lovelace",
                "active": "true",
                "relay_access": "false",
            }
            save_csv(folder, EMPLOYEES_FILE, EMPLOYEE_FIELDS, [employee])

            created_paths = initialize_sd_csv_files(folder)

            self.assertEqual(
                {path.name for path in created_paths},
                {ATTENDANCE_FILE, LOGS_FILE},
            )
            self.assertEqual(load_csv(folder, EMPLOYEES_FILE, EMPLOYEE_FIELDS), [employee])
            self.assertEqual(load_csv(folder, ATTENDANCE_FILE, ATTENDANCE_FIELDS), [])
            self.assertEqual(load_csv(folder, LOGS_FILE, LOG_FIELDS), [])
            self.assertEqual(initialize_sd_csv_files(folder), ())

    def test_initialize_sd_csv_files_writes_headers_to_zero_byte_files(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            (folder / EMPLOYEES_FILE).write_bytes(b"")
            (folder / ATTENDANCE_FILE).write_bytes(b"")

            created_paths = initialize_sd_csv_files(folder)

            self.assertEqual(
                {path.name for path in created_paths},
                {EMPLOYEES_FILE, ATTENDANCE_FILE, LOGS_FILE},
            )
            self.assertEqual(load_csv(folder, EMPLOYEES_FILE, EMPLOYEE_FIELDS), [])
            self.assertEqual(load_csv(folder, ATTENDANCE_FILE, ATTENDANCE_FIELDS), [])

    def test_load_csv_rejects_truncated_row(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / EMPLOYEES_FILE
            path.write_text(
                "employee_id,card_uid,first_name,last_name,active,relay_access\n"
                "E-1,A1B2,Ada\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ValueError, "inną liczbę kolumn"):
                load_csv_file(path, EMPLOYEE_FIELDS)

    def test_load_config_rejects_invalid_json_field_types(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            for payload in (
                {"schema_version": 1, "mode": "offline", "language": "pl", "camera_enabled": "false"},
                {"schema_version": 1, "mode": "offline", "language": "pl", "timezone_offset_minutes": "60"},
                {"schema_version": True, "mode": "offline", "language": "pl"},
                {"schema_version": 1, "mode": "offline", "language": "pl", "wifi_ssid": 123},
            ):
                with self.subTest(payload=payload):
                    (folder / "config.json").write_text(json.dumps(payload), encoding="utf-8")
                    with self.assertRaises(ValueError):
                        load_config(folder)

    def test_wifi_ssid_validates_utf8_byte_length(self) -> None:
        # 17 Polish characters = 34 UTF-8 bytes (> 32 byte 802.11 limit)
        with self.assertRaisesRegex(ValueError, "UTF-8"):
            DeviceConfig(wifi_ssid="ł" * 17).validate()
        DeviceConfig(wifi_ssid="ł" * 16).validate()

    def test_attendance_timestamp_requires_explicit_utc_offset(self) -> None:
        base = {
            "event_id": "ev-1",
            "timestamp": "2026-10-06T08:00:00Z",
            "employee_id": "E-1",
            "card_uid": "A1B2",
            "event_type": "ENTRY",
            "photo_file": "",
            "source": "manual",
            "notes": "",
        }
        validate_attendance_rows([base])
        for bad_timestamp in ("2026-10-06", "2026-10-06T08:00:00", "2026-10-06 08:00:00+02:00"):
            with self.subTest(timestamp=bad_timestamp), self.assertRaisesRegex(
                ValueError, "ISO 8601"
            ):
                validate_attendance_rows([{**base, "timestamp": bad_timestamp}])

    def test_apps_script_wraps_http_exception_as_value_error(self) -> None:
        with patch(
            "rcp2027_app.data.schemas.urlopen",
            side_effect=RemoteDisconnected("connection closed"),
        ), self.assertRaisesRegex(ValueError, "Apps Script"):
            apps_script_request(
                "https://script.google.com/macros/s/deployment/exec",
                "x" * 32,
                "ping",
            )


if __name__ == "__main__":
    unittest.main()
