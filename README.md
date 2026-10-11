# RCP2027 PROMIR-BUD PASSION LINE

RCP2027 is a time and attendance system built around an ESP32-S3 N16R8
terminal with a 3.5-inch ILI9488 touchscreen (320 × 480), together with a
desktop application for preparing SD cards, managing employees, and reviewing
or correcting attendance records.

The project includes the terminal firmware, a desktop application, and Google
Sheets integration. The terminal interface supports Polish, English, Spanish,
and German.

## System components

- `src/`, `include/`, `boards/`, `platformio.ini` — the PlatformIO firmware,
  including LovyanGFX, the display and touch interface, MFRC522 RFID reader,
  DS3231 RTC, SD card, optional OV2640 camera, relay, PIN, and operating modes.
- `desktop/` — tools for configuring SD cards and managing employees,
  attendance records, and logs.
- `google_apps_script/Code.gs` — authenticated API for reading and writing
  Google Sheets.
- `data/` — example configuration and CSV headers.
- `docs/ARCHITECTURE.md` — data formats, pinout, API configuration, and
  implementation boundaries.
- [`MANUAL.PDF`](./MANUAL.PDF) — Polish user manual.
- [`MANUAL_EN.PDF`](./MANUAL_EN.PDF) — English user manual.

## Terminal features

- RFID attendance with automatic entry/exit detection; the display confirms
  the employee's name after a recorded punch.
- Offline mode restores each employee's last attendance state from the log at
  startup and confirms a punch only after the complete CSV row has been
  written. In online mode, a timeout is treated as an unknown result: the
  terminal checks the same event ID and pauses subsequent punches to avoid
  reversing attendance state by retrying a scan.
- If the network is unavailable in online mode, the terminal asks the employee
  to choose entry or exit. The event is saved to `/attendance_pending.csv` on
  the SD card and uploaded to Google Sheets after connectivity is restored.
  Events are sent in order and are not removed until acknowledged by the API.
  An available SD card is required.
- Cards with relay access can record entry when the employee is outside the
  facility; after entry, they can choose to record an exit or briefly unlock
  the door.
- Unknown cards can be enrolled at the terminal after confirmation and PIN
  entry.
- PIN-protected settings include language, operating mode, photos, seven font
  styles (Unicode, regular or bold Sans/Serif/Mono), light or dark color mode,
  employee list, date and time, PIN changes, and touch calibration. Wi-Fi
  settings allow the user to select a network, enter its password, and save
  the configuration to `config.json` on the SD card after a successful
  connection.
- Employee management can change relay permissions and deactivate employees
  without deleting their history. New employees can be added by scanning a
  new card.
- The idle screen shows the time, date, and a daily motivational quote selected
  from a set of 50 quotes per language.
- An optional `/WALLPAPER.RGB565` image is loaded into PSRAM and shown on the
  home screen and attendance confirmations. It can be enabled or disabled in
  settings.
- Touch calibration runs on first startup and can be repeated from settings;
  the calibration is stored in persistent memory.
- Logs are saved to `/logs.csv` on the SD card in both operating modes. In
  online mode, they are also sent to the configured spreadsheet.
- When enabled and the camera is available, photos are saved under `/photos`
  on the SD card as `surname_firstname_id_YYYYMMDD_HHMM.jpg`. An event ID is
  added if a filename already exists so that photos are not overwritten.

## Desktop application

Python 3.10 or later:

```powershell
cd RCP2027\desktop
py -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install -e .
python -m rcp2027_app
```

To build a standalone Windows executable, install the build extra and run the
packaging script:

```powershell
python -m pip install -e ".[build]"
.\package_windows.ps1
```

The executable is created at `desktop\dist\RCP2027.exe`; it includes the
Google Apps Script example used by the in-app help.

First, select the SD card's root directory (for example, `E:\`) and choose
**Prepare SD card**. The application writes `config.json`, creates missing CSV
files with headers, and creates the `photos` directory. Existing CSV files and
records are preserved. Missing CSV files are also created automatically when
an SD card directory is selected.

Offline mode can start with an empty employee list; add the first card at the
terminal or in the desktop application. To view attendance records or logs,
select the corresponding CSV file on the SD card. Attendance records can be
manually edited and saved back to the selected file.

In online mode, refresh buttons retrieve data through Apps Script. Uploading
employees or attendance to the cloud requires confirmation because it
replaces the contents of the corresponding spreadsheet. The **Help** button
explains how to create the sheets, configure required headers, deploy the
service, and use the Apps Script code, with a copy button. Network requests
run in a `QThread` so the window remains responsive while waiting for the API.

The card configuration also includes the language, operating mode, Wi-Fi
settings, spreadsheet and Apps Script links, and the photo setting. Links can
be validated in the application before saving the configuration.

The **Wallpaper** tab lets you fit an image to the 480 × 320 display, preview
interface fields on the home screen and attendance confirmation, and save
`WALLPAPER.RGB565` to the SD card. The export also includes
`wallpaper_rgb565.h` and a `wallpaper_layout.json` area template. These areas
are only a preview of the firmware's fixed layout; they do not change the
position of display elements.

## First terminal startup

1. Build and flash the firmware as described below.
2. Insert an SD card prepared with the desktop application. In offline mode,
   the terminal can start with an empty employee list.
3. On first startup, touch each of the four calibration points in sequence.
   Calibration can be repeated later from settings.
4. Touch **SETTINGS** on the home screen and enter the default PIN, `0000`.
   Change it before normal operation.
5. Add employees in the desktop application or at the terminal. To enroll
   someone at the device, scan an unknown card, choose **Yes**, enter the PIN,
   and fill in the ID, first name, and surname. Relay access can also be
   granted.
6. For online mode, install the Apps Script described below, configure the
   spreadsheets, and use **Test API** in the desktop application.
7. Scan an employee card. Cards without relay permission automatically record
   entry or exit; cards with relay permission display an action selection.

Settings and employee enrollment are protected by a PIN. Employee management
can change relay access or deactivate an entry while preserving its
attendance history.

## Google Apps Script setup

See the [architecture guide](./docs/ARCHITECTURE.md) for the full procedure
and required project properties. In brief, add `google_apps_script/Code.gs` to
an Apps Script project, create the required spreadsheet headers, set the token
and spreadsheet IDs in Script Properties, deploy the web app as its owner, and
paste the `/exec` URL into the application. Then choose **Test API**.

Public Google Sheets links identify the spreadsheets. A CSV or URL alone
cannot securely append records, so the terminal writes through the
Apps-Script API protected by a token. The Wi-Fi password and token are stored
in plain text on the SD card; keep the card physically secure.

## Building the firmware

Run these commands from the repository root:

```powershell
py -m platformio run -e esp32-s3-devkitc-1-n16r8
py -m platformio run -e esp32-s3-devkitc-1-n16r8 -t upload --upload-port COMx
```

Close the serial monitor before flashing to release the COM port. The pinout is
defined in [`include/pins.h`](./include/pins.h); data formats and schemas are
described in [`docs/ARCHITECTURE.md`](./docs/ARCHITECTURE.md).

## Verification

```powershell
py -m unittest discover -s desktop\tests -v
py -m ruff check desktop\src desktop\tests
py -m platformio run -e esp32-s3-devkitc-1-n16r8
```

Compilation does not replace testing on the device. Initial hardware testing
should cover touch, GPIO3 reset, SPI buses for the SD card, RFID reader and
display, RTC, camera, and safe operation of the physical relay.

## Security and local data

Do not store a real `config.json`, Apps Script token, Wi-Fi password, private
employee data, device logs, or photos in the repository.
[`data/config.example.json`](./data/config.example.json) contains only empty
example values. Store the real `config.json` on the SD card and keep the card
physically secure. Change the default PIN, `0000`, on first startup.
