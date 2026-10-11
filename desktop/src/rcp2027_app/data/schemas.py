from __future__ import annotations

import csv
import json
import os
import re
import secrets
import tempfile
from dataclasses import asdict, dataclass
from datetime import datetime
from pathlib import Path
from typing import Iterable
from urllib.error import HTTPError, URLError
from urllib.parse import urlparse
from urllib.request import Request, urlopen

EMPLOYEE_FIELDS = (
    "employee_id",
    "card_uid",
    "first_name",
    "last_name",
    "active",
    "relay_access",
)
ATTENDANCE_FIELDS = (
    "event_id",
    "timestamp",
    "employee_id",
    "card_uid",
    "event_type",
    "photo_file",
    "source",
    "notes",
)
LOG_FIELDS = ("event_id", "timestamp", "level", "code", "message")
CONFIG_FILE = "config.json"
EMPLOYEES_FILE = "employees.csv"
ATTENDANCE_FILE = "attendance.csv"
LOGS_FILE = "logs.csv"


@dataclass(slots=True)
class DeviceConfig:
    schema_version: int = 1
    mode: str = "offline"
    language: str = "pl"
    wifi_ssid: str = ""
    wifi_password: str = ""
    timezone_offset_minutes: int = 60
    apps_script_url: str = ""
    api_token: str = ""
    employees_sheet_url: str = ""
    attendance_sheet_url: str = ""
    logs_sheet_url: str = ""
    camera_enabled: bool = False

    def validate(self) -> None:
        if self.schema_version != 1:
            raise ValueError("Nieobsługiwana wersja pliku konfiguracji.")
        if self.mode not in {"offline", "online"}:
            raise ValueError("Tryb musi mieć wartość offline albo online.")
        if self.language not in {"pl", "en", "es", "de"}:
            raise ValueError("Nieobsługiwany język.")
        if len(self.wifi_ssid) > 32 or len(self.wifi_password) > 63:
            raise ValueError("SSID lub hasło Wi-Fi przekracza dopuszczalną długość.")
        if not -720 <= self.timezone_offset_minutes <= 840:
            raise ValueError("Przesunięcie strefy czasowej musi być w zakresie UTC-12 do UTC+14.")
        if self.mode == "online":
            if not self.wifi_ssid.strip():
                raise ValueError("Tryb online wymaga nazwy sieci Wi-Fi.")
            if not self.apps_script_url:
                raise ValueError("Tryb online wymaga adresu Google Apps Script.")
            validate_apps_script_url(self.apps_script_url)
            if len(self.api_token.strip()) < 32:
                raise ValueError("Token API musi mieć co najmniej 32 znaki.")
            sheet_urls = (
                self.employees_sheet_url,
                self.attendance_sheet_url,
                self.logs_sheet_url,
            )
            if not all(value.strip() for value in sheet_urls):
                raise ValueError("Tryb online wymaga linków do trzech arkuszy Google.")
            for value in sheet_urls:
                validate_sheet_url(value)


def validate_sheet_url(value: str) -> str:
    parsed = urlparse(value.strip())
    if parsed.scheme != "https" or parsed.hostname not in {
        "docs.google.com",
        "drive.google.com",
        "docs.googleusercontent.com",
    }:
        raise ValueError("Oczekiwany jest bezpieczny link HTTPS do Arkuszy Google.")
    match = re.search(r"/spreadsheets/d/([A-Za-z0-9_-]+)", parsed.path)
    if not match:
        raise ValueError("Link nie zawiera identyfikatora arkusza Google.")
    return match.group(1)


def generate_api_token() -> str:
    return secrets.token_urlsafe(32)


def validate_apps_script_url(value: str) -> str:
    parsed = urlparse(value.strip())
    if (
        parsed.scheme != "https"
        or parsed.hostname != "script.google.com"
        or not re.fullmatch(r"/macros/s/[^/]+/exec", parsed.path)
    ):
        raise ValueError("Podaj wdrożony link HTTPS Google Apps Script zakończony /exec.")
    return value.strip()


def validate_employee_rows(rows: list[dict[str, str]]) -> None:
    ids: set[str] = set()
    card_uids: set[str] = set()
    for index, row in enumerate(rows, start=2):
        employee_id = row.get("employee_id", "").strip().casefold()
        card_uid = row.get("card_uid", "").strip().upper()
        if not employee_id or not card_uid:
            raise ValueError(f"employees.csv, wiersz {index}: ID i UID karty są wymagane.")
        if employee_id in ids:
            raise ValueError(f"employees.csv, wiersz {index}: powtórzone ID pracownika.")
        if card_uid in card_uids:
            raise ValueError(f"employees.csv, wiersz {index}: karta jest przypisana wielokrotnie.")
        if row.get("active", "").strip().lower() not in {"true", "false", "1", "0"}:
            raise ValueError(f"employees.csv, wiersz {index}: active musi być true/false.")
        if row.get("relay_access", "").strip().lower() not in {"true", "false", "1", "0"}:
            raise ValueError(
                f"employees.csv, wiersz {index}: relay_access musi być true/false."
            )
        ids.add(employee_id)
        card_uids.add(card_uid)


def validate_attendance_rows(rows: list[dict[str, str]]) -> None:
    event_ids: set[str] = set()
    for index, row in enumerate(rows, start=2):
        event_id = row.get("event_id", "").strip()
        if not event_id or event_id in event_ids:
            raise ValueError(f"attendance.csv, wiersz {index}: brak lub powtórzone event_id.")
        if not row.get("employee_id", "").strip() or not row.get("card_uid", "").strip():
            raise ValueError(f"attendance.csv, wiersz {index}: brak pracownika lub UID karty.")
        if row.get("event_type", "") not in {"ENTRY", "EXIT"}:
            raise ValueError(f"attendance.csv, wiersz {index}: event_type musi być ENTRY/EXIT.")
        try:
            datetime.fromisoformat(row.get("timestamp", ""))
        except ValueError as error:
            raise ValueError(
                f"attendance.csv, wiersz {index}: timestamp musi być ISO 8601."
            ) from error
        event_ids.add(event_id)


def check_apps_script(
    url: str, token: str, sheet_urls: tuple[str, str, str], timeout: float = 8.0
) -> str:
    sheet_ids = [validate_sheet_url(value) for value in sheet_urls]
    result = apps_script_request(
        url, token, "ping", timeout=timeout, sheet_ids=sheet_ids
    )
    if result.get("ok") is not True:
        raise ValueError("Apps Script odrzucił sprawdzenie. Zweryfikuj token i wdrożenie.")
    return "Połączenie z API i skonfigurowanymi arkuszami Google działa."


def apps_script_request(
    url: str,
    token: str,
    action: str,
    timeout: float = 8.0,
    rows: list[dict[str, str]] | None = None,
    expected_revision: str | None = None,
    sheet_ids: list[str] | None = None,
) -> dict:
    endpoint = validate_apps_script_url(url)
    request_data = {"action": action, "token": token}
    if rows is not None:
        request_data["rows"] = rows
    if expected_revision is not None:
        request_data["expected_revision"] = expected_revision
    if sheet_ids is not None:
        request_data["sheet_ids"] = sheet_ids
    payload = json.dumps(request_data).encode("utf-8")
    request = Request(
        endpoint,
        data=payload,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urlopen(request, timeout=timeout) as response:
            body = json.load(response)
    except (HTTPError, URLError, TimeoutError, json.JSONDecodeError) as error:
        raise ValueError(f"Nie udało się sprawdzić Apps Script: {error}") from error
    if not isinstance(body, dict):
        raise ValueError("Apps Script zwrócił nieprawidłową odpowiedź.")
    if body.get("ok") is not True:
        raise ValueError(
            str(body.get("error", "Apps Script zwrócił nieprawidłową odpowiedź."))
        )
    return body


def _atomic_write(path: Path, writer) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            "w", encoding="utf-8", newline="", dir=path.parent, delete=False
        ) as stream:
            temporary = Path(stream.name)
            writer(stream)
            stream.flush()
            os.fsync(stream.fileno())
        temporary.replace(path)
    finally:
        if temporary is not None and temporary.exists():
            temporary.unlink()


def save_config(folder: Path, config: DeviceConfig) -> Path:
    config.validate()
    destination = folder / CONFIG_FILE

    def write(stream) -> None:
        json.dump(asdict(config), stream, ensure_ascii=False, indent=2)
        stream.write("\n")

    _atomic_write(destination, write)
    return destination


def prepare_sd_card(folder: Path, config: DeviceConfig) -> tuple[Path, ...]:
    if not folder.is_dir():
        raise ValueError("Wybierz istniejący katalog główny karty SD.")
    config.validate()

    created_paths = list(initialize_sd_csv_files(folder))
    photos_folder = folder / "photos"
    photos_folder.mkdir(exist_ok=True)
    config_path = save_config(folder, config)
    return (config_path, *created_paths)


def initialize_sd_csv_files(folder: Path) -> tuple[Path, ...]:
    if not folder.is_dir():
        raise ValueError("Wybierz istniejący katalog główny karty SD.")

    csv_files = (
        (EMPLOYEES_FILE, EMPLOYEE_FIELDS),
        (ATTENDANCE_FILE, ATTENDANCE_FIELDS),
        (LOGS_FILE, LOG_FIELDS),
    )
    created_paths = []
    for filename, fields in csv_files:
        path = folder / filename
        if path.exists():
            rows = load_csv(folder, filename, fields)
            if filename == EMPLOYEES_FILE:
                validate_employee_rows(rows)
            elif filename == ATTENDANCE_FILE:
                validate_attendance_rows(rows)
            if path.stat().st_size == 0:
                created_paths.append(save_csv(folder, filename, fields, ()))
        else:
            created_paths.append(save_csv(folder, filename, fields, ()))
    return tuple(created_paths)


def load_config(folder: Path) -> DeviceConfig:
    path = folder / CONFIG_FILE
    with path.open(encoding="utf-8") as stream:
        raw = json.load(stream)
    if not isinstance(raw, dict):
        raise ValueError("Plik config.json musi zawierać obiekt JSON.")
    allowed = DeviceConfig.__dataclass_fields__
    config = DeviceConfig(**{key: value for key, value in raw.items() if key in allowed})
    config.validate()
    return config


def load_csv(folder: Path, filename: str, fields: tuple[str, ...]) -> list[dict[str, str]]:
    return load_csv_file(folder / filename, fields)


def load_csv_file(path: Path, fields: tuple[str, ...]) -> list[dict[str, str]]:
    if not path.exists():
        return []
    with path.open(encoding="utf-8-sig", newline="") as stream:
        reader = csv.DictReader(stream)
        if tuple(reader.fieldnames or ()) != fields:
            raise ValueError(f"{path.name}: niezgodny nagłówek CSV.")
        rows = []
        for row in reader:
            if None in row:
                raise ValueError(
                    f"{path.name}: rekord ma inną liczbę kolumn niż nagłówek."
                )
            rows.append({field: row.get(field, "") or "" for field in fields})
        return rows


def save_csv(
    folder: Path, filename: str, fields: tuple[str, ...], rows: Iterable[dict[str, str]]
) -> Path:
    return save_csv_file(folder / filename, fields, rows)


def save_csv_file(
    destination: Path, fields: tuple[str, ...], rows: Iterable[dict[str, str]]
) -> Path:

    def write(stream) -> None:
        writer = csv.DictWriter(stream, fieldnames=fields, extrasaction="raise")
        writer.writeheader()
        for row in rows:
            writer.writerow({field: row.get(field, "") for field in fields})

    _atomic_write(destination, write)
    return destination
