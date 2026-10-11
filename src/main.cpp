#include <Arduino.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <HTTPClient.h>
#include <MFRC522.h>
#include <LovyanGFX.hpp>
#include <Preferences.h>
#include <RTClib.h>
#include <SD.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_camera.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <Wire.h>
#include <cstring>
#include <cmath>
#include <new>

#include <vector>

#include "pins.h"

namespace {

constexpr uint16_t kScreenWidth = 480;
constexpr uint16_t kScreenHeight = 320;
constexpr uint32_t kRelayPulseMs = 3000;
constexpr uint32_t kRfidDebounceMs = 1400;
constexpr uint32_t kClockRefreshMs = 1000;
constexpr uint32_t kAttendanceConfirmationMs = 3000;
constexpr uint32_t kHttpTimeoutMs = 8000;
constexpr uint32_t kNetworkQueueWaitMs = 100;
constexpr uint32_t kEmployeeRefreshMs = 300000;
constexpr uint8_t kMaximumEmployees = 180;
constexpr char kConfigPath[] = "/config.json";
constexpr char kEmployeesPath[] = "/employees.csv";
constexpr char kAttendancePath[] = "/attendance.csv";
constexpr char kPendingAttendancePath[] = "/attendance_pending.csv";
constexpr char kAttendanceHeader[] =
    "event_id,timestamp,employee_id,card_uid,event_type,photo_file,source,notes";
constexpr char kLogsPath[] = "/logs.csv";
constexpr char kWallpaperPath[] = "/WALLPAPER.RGB565";
constexpr size_t kWallpaperPixelCount =
    static_cast<size_t>(kScreenWidth) * kScreenHeight;
constexpr size_t kWallpaperByteCount = kWallpaperPixelCount * sizeof(uint16_t);
constexpr uint32_t kTouchCalibrationMagic = 0x52435055;
constexpr uint32_t kLegacyTouchCalibrationMagic = 0x52435054;
constexpr int kTouchCalibrationMargin = 30;

class LGFX_RCP : public lgfx::LGFX_Device {
    lgfx::Panel_ILI9488 panel_;
    lgfx::Bus_SPI bus_;
    lgfx::Touch_XPT2046 touch_;

public:
    LGFX_RCP() {
        auto busConfig = bus_.config();
        busConfig.spi_host = static_cast<spi_host_device_t>(SPI3_HOST);
        busConfig.spi_mode = 0;
        busConfig.freq_write = 20000000;
        busConfig.freq_read = 2500000;
        busConfig.pin_sclk = pins::tft_sck;
        busConfig.pin_mosi = pins::tft_mosi;
        busConfig.pin_miso = pins::tft_miso;
        busConfig.pin_dc = pins::tft_dc;
        bus_.config(busConfig);
        panel_.setBus(&bus_);

        auto panelConfig = panel_.config();
        panelConfig.pin_cs = pins::tft_cs;
        panelConfig.pin_rst = -1;
        panelConfig.panel_width = 320;
        panelConfig.panel_height = 480;
        panelConfig.memory_width = 320;
        panelConfig.memory_height = 480;
        panelConfig.readable = false;
        panel_.config(panelConfig);

        auto touchConfig = touch_.config();
        touchConfig.spi_host = static_cast<spi_host_device_t>(SPI3_HOST);
        touchConfig.bus_shared = true;
        touchConfig.pin_cs = pins::touch_cs;
        touchConfig.pin_int = -1;
        touchConfig.freq = 2500000;
        touchConfig.x_min = 300;
        touchConfig.x_max = 3800;
        touchConfig.y_min = 200;
        touchConfig.y_max = 3700;
        touchConfig.offset_rotation = 4;
        touch_.config(touchConfig);
        panel_.setTouch(&touch_);
        setPanel(&panel_);
    }
};

enum class Language : uint8_t { Polish, English, Spanish, German };
enum class Mode : uint8_t { Offline, Online };
enum class UiFont : uint8_t {
    Unicode,
    FreeSans,
    FreeSerif,
    FreeMono,
    FreeSansBold,
    FreeSerifBold,
    FreeMonoBold
};
constexpr uint8_t kUiFontCount = 7;
enum class Screen : uint8_t {
    Home,
    UnknownCard,
    AttendanceStateCheck,
    ActionChoice,
    PinEntry,
    EmployeeEnroll,
    Settings,
    PinChange,
    PinConfirm,
    ClockSet,
    EmployeeList,
    AddEmployeeCard,
    AttendanceConfirmation,
    WifiSetup,
    WifiPassword
};
enum class AttendanceAction : uint8_t { Entry, Exit, Door };

struct WifiNetwork {
    String ssid;
    int32_t rssi;
    bool open;
};

struct Employee {
    String id;
    String uid;
    String firstName;
    String lastName;
    bool active = true;
    bool relayAccess = false;
};

struct AttendanceState {
    String employeeId;
    String eventType;
};

struct NetworkJob {
    uint32_t requestId;
    String body;
    bool responseExpected;
};

struct NetworkResult {
    uint32_t requestId;
    bool success;
    bool responseReceived;
    String body;
};

struct Config {
    Mode mode = Mode::Offline;
    Language language = Language::Polish;
    String ssid;
    String wifiPassword;
    String appsScriptUrl;
    String apiToken;
    String employeesSheetUrl;
    String attendanceSheetUrl;
    String logsSheetUrl;
    int timezoneOffsetMinutes = 60;
    bool cameraEnabled = false;
};

struct TouchCalibration {
    uint32_t magic = 0;
    float x[3] = {};
    float y[3] = {};
};

LGFX_RCP display;
// Arduino's HSPI maps to the ESP32-S3 SPI3 peripheral used by LovyanGFX.
SPIClass sdBus(HSPI);
MFRC522 rfid(pins::rfid_cs, pins::rfid_rst);
RTC_DS3231 rtc;
Preferences preferences;
UiFont uiFont = UiFont::Unicode;
bool displayInverted = false;
uint16_t* wallpaperPixels = nullptr;
bool wallpaperEnabled = true;
constexpr size_t kVisibleSettings = 4;
constexpr size_t kTotalSettings = 11;
constexpr int kSettingsRowY = 45;
constexpr int kSettingsRowHeight = 51;
constexpr size_t kVisibleWifiNetworks = 4;
constexpr int kWifiNetworkRowY = 68;
constexpr int kWifiNetworkRowHeight = 49;

float uiFontScale() {
    switch (uiFont) {
        case UiFont::Unicode: return 0.8f;
        case UiFont::FreeSerif:
        case UiFont::FreeSerifBold: return 0.31f;
        case UiFont::FreeSansBold:
        case UiFont::FreeMonoBold: return 0.33f;
        case UiFont::FreeSans:
        case UiFont::FreeMono:
        default: return 0.35f;
    }
}

void setUiTextSize(float size) {
    const float fontScale = uiFontScale();
    display.setTextSize(size * fontScale);
}

float readableUiSize(float size) {
    return size * 0.8f / uiFontScale();
}

void applyUiFont() {
    switch (uiFont) {
        case UiFont::FreeSans:
            display.setFont(&lgfx::fonts::FreeSans12pt7b);
            break;
        case UiFont::FreeSerif:
            display.setFont(&lgfx::fonts::FreeSerif12pt7b);
            break;
        case UiFont::FreeMono:
            display.setFont(&lgfx::fonts::FreeMono12pt7b);
            break;
        case UiFont::FreeSansBold:
            display.setFont(&lgfx::fonts::FreeSansBold12pt7b);
            break;
        case UiFont::FreeSerifBold:
            display.setFont(&lgfx::fonts::FreeSerifBold12pt7b);
            break;
        case UiFont::FreeMonoBold:
            display.setFont(&lgfx::fonts::FreeMonoBold12pt7b);
            break;
        case UiFont::Unicode:
        default:
            display.setFont(&lgfx::fonts::efontJA_10);
            break;
    }
}

void drawFittedText(const String& value, int16_t x, int16_t y,
                    int16_t width, int16_t height, float baseSize,
                    lgfx::textdatum_t datum) {
    setUiTextSize(baseSize);
    const int32_t availableWidth = max<int32_t>(1, width - 8);
    const int32_t availableHeight = max<int32_t>(1, height - 6);
    const int32_t measuredWidth = display.textWidth(value);
    const int32_t measuredHeight = display.fontHeight();
    float fitScale = 1.0f;
    if (measuredWidth > availableWidth && measuredWidth > 0) {
        fitScale = min(fitScale,
                       static_cast<float>(availableWidth) / measuredWidth);
    }
    if (measuredHeight > availableHeight && measuredHeight > 0) {
        fitScale = min(fitScale,
                       static_cast<float>(availableHeight) / measuredHeight);
    }
    if (fitScale < 1.0f) setUiTextSize(baseSize * fitScale);

    int16_t anchorX = x;
    int16_t anchorY = y;
    if (datum == lgfx::textdatum_t::middle_center) {
        anchorX += width / 2;
        anchorY += height / 2;
    } else if (datum == lgfx::textdatum_t::middle_left) {
        anchorY += height / 2;
    } else if (datum == lgfx::textdatum_t::top_center) {
        anchorX += width / 2;
    }
    display.setTextDatum(datum);
    display.drawString(value, anchorX, anchorY);
    display.setTextDatum(lgfx::textdatum_t::top_left);
}

void drawVerticalScrollbar(int16_t x, int16_t y, int16_t height,
                           size_t totalItems, size_t visibleItems, size_t offset) {
    constexpr int16_t kScrollbarWidth = 24;
    constexpr int16_t kArrowHeight = 18;
    const int16_t radius = kScrollbarWidth / 2;
    display.fillRoundRect(x, y, kScrollbarWidth, height, radius,
                          display.color565(39, 54, 82));
    if (totalItems <= visibleItems) return;
    const uint16_t arrowColor = display.color565(232, 240, 255);
    const int16_t centerX = x + kScrollbarWidth / 2;
    display.fillTriangle(centerX, y + 4, x + 5, y + kArrowHeight - 2,
                         x + kScrollbarWidth - 5, y + kArrowHeight - 2,
                         arrowColor);
    display.fillTriangle(centerX, y + height - 4,
                         x + 5, y + height - kArrowHeight + 2,
                         x + kScrollbarWidth - 5, y + height - kArrowHeight + 2,
                         arrowColor);
    const int16_t trackY = y + kArrowHeight;
    const int16_t trackHeight = height - 2 * kArrowHeight;
    const int16_t thumbHeight = max<int16_t>(
        24, static_cast<int16_t>((static_cast<int32_t>(trackHeight) * visibleItems) /
                                  totalItems));
    const size_t maxOffset = totalItems - visibleItems;
    const int16_t thumbY = trackY + static_cast<int16_t>(
        (static_cast<int32_t>(trackHeight - thumbHeight) * offset) / maxOffset);
    display.fillRoundRect(x + 3, thumbY, kScrollbarWidth - 6, thumbHeight, 7,
                          display.color565(129, 172, 255));
}

size_t scrollbarOffsetForTouch(int16_t touchY, int16_t y, int16_t height,
                               size_t totalItems, size_t visibleItems,
                               size_t currentOffset) {
    if (totalItems <= visibleItems) return 0;
    const size_t maxOffset = totalItems - visibleItems;
    constexpr int16_t kArrowHeight = 18;
    if (touchY < y + kArrowHeight) {
        return currentOffset > 0 ? currentOffset - 1 : 0;
    }
    if (touchY >= y + height - kArrowHeight) {
        return currentOffset < maxOffset ? currentOffset + 1 : maxOffset;
    }
    const int16_t trackHeight = height - 2 * kArrowHeight;
    const int16_t trackPosition = touchY - y - kArrowHeight;
    return min(static_cast<size_t>(
                   (static_cast<uint32_t>(trackPosition) * maxOffset) /
                   trackHeight),
               maxOffset);
}

extern bool sdAvailable;

bool loadWallpaper() {
    if (!sdAvailable || !SD.exists(kWallpaperPath)) return false;
    File file = SD.open(kWallpaperPath, FILE_READ);
    if (!file) {
        Serial.println("Could not open WALLPAPER.RGB565");
        return false;
    }
    if (file.size() != kWallpaperByteCount) {
        Serial.printf("Invalid WALLPAPER.RGB565 size: %lu bytes; expected %u\n",
                      static_cast<unsigned long>(file.size()),
                      static_cast<unsigned>(kWallpaperByteCount));
        file.close();
        return false;
    }
    uint16_t* pixels = static_cast<uint16_t*>(
        heap_caps_malloc(kWallpaperByteCount, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (pixels == nullptr) {
        Serial.println("Could not allocate PSRAM buffer for wallpaper");
        file.close();
        return false;
    }
    const size_t bytesRead =
        file.read(reinterpret_cast<uint8_t*>(pixels), kWallpaperByteCount);
    file.close();
    if (bytesRead != kWallpaperByteCount) {
        Serial.printf("Incomplete WALLPAPER.RGB565 read: %lu bytes\n",
                      static_cast<unsigned long>(bytesRead));
        heap_caps_free(pixels);
        return false;
    }
    if (wallpaperPixels != nullptr) heap_caps_free(wallpaperPixels);
    wallpaperPixels = pixels;
    Serial.println("Loaded 480x320 RGB565 wallpaper from SD into PSRAM");
    return true;
}

void drawScreenBackground() {
    if (wallpaperEnabled && wallpaperPixels != nullptr) {
        display.pushImage(0, 0, kScreenWidth, kScreenHeight, wallpaperPixels);
    } else {
        display.fillScreen(display.color565(10, 17, 33));
    }
}

TouchCalibration touchCalibration;
Config config;
std::vector<Employee> employees;
std::vector<AttendanceState> offlineAttendanceStates;
Screen screen = Screen::Home;
Employee selectedEmployee;
String pinInput;
String storedPin = "0000";
String lastCardUid;
String lastMessage;
String lastHomeDate;
String lastHomeQuoteDate;
uint32_t lastCardAt = 0;
uint32_t lastClockDraw = 0;
uint32_t relayOffAt = 0;
uint32_t eventCounter = 0;
bool rtcAvailable = false;
bool sdAvailable = false;
bool rfidAvailable = false;
bool cameraAvailable = false;
esp_err_t cameraInitError = ESP_OK;
bool configurationValid = false;
bool offlineEmployeesLoaded = false;
bool onlineEmployeesLoaded = false;
bool sdFilesInitialized = false;
bool relayWasActive = false;
bool networkWorkerReady = false;
volatile bool relayAutoClosed = false;
String pendingPin;
String pendingEmployeeUid;
String newEmployeeId;
String newEmployeeFirstName;
String newEmployeeLastName;
String attendanceConfirmationTitle;
String attendanceConfirmationName;
String pendingOnlineEventId;
String pendingOnlineEmployeeId;
String pendingOnlineEmployeeName;
bool attendanceConfirmationSuccess = false;
bool employeeEnrollmentRequested = false;
bool newEmployeeRelayAccess = false;
bool enrollmentNumericKeyboard = false;
bool enrollmentLowercase = false;
bool wifiConnecting = false;
bool wifiScanInProgress = false;
bool wifiSetupRadioWasOn = false;
bool wifiSetupActive = false;
bool wifiPasswordLowercase = false;
uint8_t wifiKeyboardPage = 0;
uint8_t enrollmentField = 0;
uint8_t enrollmentAccentIndex = 0;
uint32_t screenSince = 0;
uint32_t attendanceConfirmationUntil = 0;
uint32_t wifiConnectionStartedAt = 0;
uint32_t nextNetworkAttemptAt = 0;
uint32_t nextEmployeeRefreshAt = 0;
uint16_t employeeListPage = 0;
uint8_t settingsScroll = 0;
size_t wifiNetworkScroll = 0;
bool employeeManagementReturnToList = false;
String selectedWifiSsid;
String wifiPasswordInput;
String wifiStatusMessage;
std::vector<WifiNetwork> wifiNetworks;
esp_timer_handle_t relayTimer = nullptr;
QueueHandle_t networkJobQueue = nullptr;
QueueHandle_t networkLogQueue = nullptr;
QueueHandle_t networkAsyncResultQueue = nullptr;
TaskHandle_t networkTaskHandle = nullptr;
uint32_t networkRequestCounter = 0;
uint32_t employeeRefreshRequestId = 0;
uint32_t attendanceStateRequestId = 0;
uint32_t pendingAttendanceRequestId = 0;
uint32_t pendingQueueUploadRequestId = 0;
uint32_t pendingQueueUploadStartedAt = 0;
uint32_t nextQueueUploadAttemptAt = 0;
uint32_t pendingStatusRequestId = 0;
uint32_t pendingEmployeeUpdateRequestId = 0;
uint32_t pendingEmployeeRegistrationRequestId = 0;
uint32_t pendingAttendanceRequestStartedAt = 0;
uint32_t pendingStatusRequestStartedAt = 0;
uint32_t employeeRefreshRequestStartedAt = 0;
uint32_t attendanceStateRequestStartedAt = 0;
uint32_t employeeUpdateRequestStartedAt = 0;
uint32_t employeeRegistrationRequestStartedAt = 0;
uint32_t nextPendingStatusAttemptAt = 0;
bool pendingStatusCheckInFlight = false;
bool selectedEmployeePresent = false;
bool outageActionChoice = false;
bool pendingQueueSyncFailed = false;
String pendingQueueUploadEventId;
bool pendingAttendanceAutomatic = false;
bool pendingAttendanceApiRejected = false;
String pendingAttendanceExpectedType;
String pendingAttendanceRequestBody;
Employee pendingEmployeeUpdate;
Employee pendingEmployeeRegistration;
constexpr char kGoogleRootCertificate[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIFVzCCAz+gAwIBAgINAgPlk28xsBNJiGuiFzANBgkqhkiG9w0BAQwFADBHMQsw\n"
    "CQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEU\n"
    "MBIGA1UEAxMLR1RTIFJvb3QgUjEwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAw\n"
    "MDAwWjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZp\n"
    "Y2VzIExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjEwggIiMA0GCSqGSIb3DQEBAQUA\n"
    "A4ICDwAwggIKAoICAQC2EQKLHuOhd5s73L+UPreVp0A8of2C+X0yBoJx9vaMf/vo\n"
    "27xqLpeXo4xL+Sv2sfnOhB2x+cWX3u+58qPpvBKJXqeqUqv4IyfLpLGcY9vXmX7w\n"
    "Cl7raKb0xlpHDU0QM+NOsROjyBhsS+z8CZDfnWQpJSMHobTSPS5g4M/SCYe7zUjw\n"
    "TcLCeoiKu7rPWRnWr4+wB7CeMfGCwcDfLqZtbBkOtdh+JhpFAz2weaSUKK0Pfybl\n"
    "qAj+lug8aJRT7oM6iCsVlgmy4HqMLnXWnOunVmSPlk9orj2XwoSPwLxAwAtcvfaH\n"
    "szVsrBhQf4TgTM2S0yDpM7xSma8ytSmzJSq0SPly4cpk9+aCEI3oncKKiPo4Zor8\n"
    "Y/kB+Xj9e1x3+naH+uzfsQ55lVe0vSbv1gHR6xYKu44LtcXFilWr06zqkUspzBmk\n"
    "MiVOKvFlRNACzqrOSbTqn3yDsEB750Orp2yjj32JgfpMpf/VjsPOS+C12LOORc92\n"
    "wO1AK/1TD7Cn1TsNsYqiA94xrcx36m97PtbfkSIS5r762DL8EGMUUXLeXdYWk70p\n"
    "aDPvOmbsB4om3xPXV2V4J95eSRQAogB/mqghtqmxlbCluQ0WEdrHbEg8QOB+DVrN\n"
    "VjzRlwW5y0vtOUucxD/SVRNuJLDWcfr0wbrM7Rv1/oFB2ACYPTrIrnqYNxgFlQID\n"
    "AQABo0IwQDAOBgNVHQ8BAf8EBAMCAYYwDwYDVR0TAQH/BAUwAwEB/zAdBgNVHQ4E\n"
    "FgQU5K8rJnEaK0gnhS9SZizv8IkTcT4wDQYJKoZIhvcNAQEMBQADggIBAJ+qQibb\n"
    "C5u+/x6Wki4+omVKapi6Ist9wTrYggoGxval3sBOh2Z5ofmmWJyq+bXmYOfg6LEe\n"
    "QkEzCzc9zolwFcq1JKjPa7XSQCGYzyI0zzvFIoTgxQ6KfF2I5DUkzps+GlQebtuy\n"
    "h6f88/qBVRRiClmpIgUxPoLW7ttXNLwzldMXG+gnoot7TiYaelpkttGsN/H9oPM4\n"
    "7HLwEXWdyzRSjeZ2axfG34arJ45JK3VmgRAhpuo+9K4l/3wV3s6MJT/KYnAK9y8J\n"
    "ZgfIPxz88NtFMN9iiMG1D53Dn0reWVlHxYciNuaCp+0KueIHoI17eko8cdLiA6Ef\n"
    "MgfdG+RCzgwARWGAtQsgWSl4vflVy2PFPEz0tv/bal8xa5meLMFrUKTX5hgUvYU/\n"
    "Z6tGn6D/Qqc6f1zLXbBwHSs09dR2CQzreExZBfMzQsNhFRAbd03OIozUhfJFfbdT\n"
    "6u9AWpQKXCBfTkBdYiJ23//OYb2MI3jSNwLgjt7RETeJ9r/tSQdirpLsQBqvFAnZ\n"
    "0E6yove+7u7Y/9waLd64NnHi/Hm3lCXRSHNboTXns5lndcEZOitHTtNCjv0xyBZm\n"
    "2tIMPNuzjsmhDYAPexZ3FL//2wmUspO8IFgV6dtxQ/PeEMMA3KgqlbbC1j+Qa3bb\n"
    "bP6MvPJwNQzcmRk13NfIRmPVNnGuV/u3gm3c\n"
    "-----END CERTIFICATE-----\n";

uint16_t color(uint8_t red, uint8_t green, uint8_t blue) {
    return display.color565(red, green, blue);
}

const char* text(const char* polish, const char* english, const char* spanish,
                 const char* german) {
    switch (config.language) {
        case Language::English:
            return english;
        case Language::Spanish:
            return spanish;
        case Language::German:
            return german;
        case Language::Polish:
        default:
            return polish;
    }
}

const char* uiFontLabel() {
    switch (uiFont) {
        case UiFont::FreeSans:
            return "FreeSans";
        case UiFont::FreeSerif:
            return "FreeSerif";
        case UiFont::FreeMono:
            return "FreeMono";
        case UiFont::FreeSansBold:
            return "FreeSans Bold";
        case UiFont::FreeSerifBold:
            return "FreeSerif Bold";
        case UiFont::FreeMonoBold:
            return "FreeMono Bold";
        case UiFont::Unicode:
        default:
            return "Unicode";
    }
}

String languageLabel() {
    switch (config.language) {
        case Language::English: return "English";
        case Language::Spanish: return "Español";
        case Language::German: return "Deutsch";
        case Language::Polish:
        default: return "Polski";
    }
}

const char* modeLabel() {
    return config.mode == Mode::Offline
               ? text("Offline", "Offline", "Sin conexión", "Offline")
               : text("Online", "Online", "En línea", "Online");
}

struct MotivationQuote {
    const char* polish;
    const char* english;
    const char* spanish;
    const char* german;
};

constexpr MotivationQuote kMotivationQuotes[] = {
    {"Zacznij tam, gdzie jesteś.", "Start where you are.", "Empieza donde estás.", "Fang an, wo du bist."},
    {"Mały krok też prowadzi naprzód.", "A small step still moves you forward.", "Un paso pequeño también te acerca.", "Auch ein kleiner Schritt bringt dich weiter."},
    {"Twoja praca ma znaczenie.", "Your work makes a difference.", "Tu trabajo marca la diferencia.", "Deine Arbeit macht einen Unterschied."},
    {"Rób dziś to, co możesz.", "Do what you can today.", "Haz hoy lo que puedas.", "Tu heute, was du kannst."},
    {"Wytrwałość buduje wielkie rzeczy.", "Persistence builds great things.", "La constancia construye grandes cosas.", "Ausdauer schafft Großes."},
    {"Każdy dzień to nowa szansa.", "Every day is a new chance.", "Cada día es una nueva oportunidad.", "Jeder Tag ist eine neue Chance."},
    {"Działaj spokojnie i pewnie.", "Work calmly and with confidence.", "Actúa con calma y confianza.", "Handle ruhig und selbstbewusst."},
    {"Razem osiągamy więcej.", "Together, we achieve more.", "Juntos logramos más.", "Gemeinsam erreichen wir mehr."},
    {"Twoje zaangażowanie się liczy.", "Your commitment counts.", "Tu compromiso cuenta.", "Dein Einsatz zählt."},
    {"Dobra praca zaczyna się od Ciebie.", "Good work starts with you.", "Un buen trabajo empieza contigo.", "Gute Arbeit beginnt bei dir."},
    {"Ucz się, próbuj, rozwijaj się.", "Learn, try, and grow.", "Aprende, prueba y crece.", "Lerne, probiere aus und wachse."},
    {"Konsekwencja daje efekty.", "Consistency brings results.", "La constancia da resultados.", "Beständigkeit bringt Ergebnisse."},
    {"Masz siłę, by zrobić kolejny krok.", "You have the strength for the next step.", "Tienes fuerza para dar el siguiente paso.", "Du hast die Kraft für den nächsten Schritt."},
    {"Wspólny cel jednoczy zespół.", "A shared goal brings a team together.", "Un objetivo común une al equipo.", "Ein gemeinsames Ziel verbindet das Team."},
    {"Jakość tkwi w codziennych wyborach.", "Quality lives in everyday choices.", "La calidad está en las decisiones diarias.", "Qualität zeigt sich in täglichen Entscheidungen."},
    {"Twoja uważność zwiększa bezpieczeństwo.", "Your focus helps keep everyone safe.", "Tu atención ayuda a mantenernos seguros.", "Deine Aufmerksamkeit sorgt für mehr Sicherheit."},
    {"Dziś możesz zrobić coś dobrze.", "You can do something well today.", "Hoy puedes hacer algo bien.", "Heute kannst du etwas gut machen."},
    {"Dobra współpraca zaczyna się od szacunku.", "Good teamwork begins with respect.", "El buen trabajo en equipo empieza con respeto.", "Gute Zusammenarbeit beginnt mit Respekt."},
    {"Nie poddawaj się po pierwszej próbie.", "Don't give up after the first try.", "No te rindas tras el primer intento.", "Gib nach dem ersten Versuch nicht auf."},
    {"Postęp tworzy się krok po kroku.", "Progress is made one step at a time.", "El progreso se construye paso a paso.", "Fortschritt entsteht Schritt für Schritt."},
    {"Twoje doświadczenie pomaga innym.", "Your experience helps others.", "Tu experiencia ayuda a los demás.", "Deine Erfahrung hilft anderen."},
    {"Dobre nastawienie pomaga działać.", "A positive attitude helps you act.", "Una actitud positiva ayuda a avanzar.", "Eine positive Haltung hilft beim Anpacken."},
    {"Zadbaj o szczegóły, a efekt się obroni.", "Care for the details; results will follow.", "Cuida los detalles y llegarán los resultados.", "Achte auf Details, dann stimmt das Ergebnis."},
    {"Każde zadanie to okazja do rozwoju.", "Every task is a chance to grow.", "Cada tarea es una oportunidad para crecer.", "Jede Aufgabe bietet eine Chance zu wachsen."},
    {"Twoja rzetelność buduje zaufanie.", "Your reliability builds trust.", "Tu fiabilidad genera confianza.", "Deine Zuverlässigkeit schafft Vertrauen."},
    {"Działając razem, pokonamy trudności.", "Together, we can overcome challenges.", "Juntos podemos superar las dificultades.", "Gemeinsam meistern wir Herausforderungen."},
    {"Zacznij od tego, co najważniejsze.", "Begin with what matters most.", "Empieza por lo más importante.", "Beginne mit dem Wichtigsten."},
    {"Cierpliwość pomaga osiągać cele.", "Patience helps you reach your goals.", "La paciencia ayuda a alcanzar tus metas.", "Geduld hilft dir, deine Ziele zu erreichen."},
    {"Twoja energia inspiruje zespół.", "Your energy inspires the team.", "Tu energía inspira al equipo.", "Deine Energie inspiriert das Team."},
    {"Dobra zmiana zaczyna się od działania.", "Positive change starts with action.", "El cambio positivo empieza actuando.", "Gute Veränderung beginnt mit Handeln."},
    {"Dbaj o jakość każdego kroku.", "Bring care to every step.", "Cuida la calidad en cada paso.", "Achte bei jedem Schritt auf Qualität."},
    {"Wyzwania pokazują, ile potrafisz.", "Challenges show what you can do.", "Los retos muestran de lo que eres capaz.", "Herausforderungen zeigen, was du kannst."},
    {"Dziel się wiedzą, wzmacniaj zespół.", "Share knowledge and strengthen the team.", "Comparte lo que sabes y fortalece al equipo.", "Teile dein Wissen und stärke das Team."},
    {"Dziś jest dobry dzień na postęp.", "Today is a good day to make progress.", "Hoy es un buen día para avanzar.", "Heute ist ein guter Tag für Fortschritt."},
    {"Twoja dokładność robi różnicę.", "Your attention to detail matters.", "Tu precisión marca la diferencia.", "Deine Genauigkeit macht den Unterschied."},
    {"Odwaga rośnie z każdym działaniem.", "Courage grows with every action.", "El valor crece con cada acción.", "Mit jedem Schritt wächst der Mut."},
    {"Wspieraj innych, a zajdziemy dalej.", "Support others and we'll go further.", "Apoya a los demás y llegaremos más lejos.", "Unterstütze andere, dann kommen wir weiter."},
    {"Dobre nawyki tworzą dobre wyniki.", "Good habits create good results.", "Los buenos hábitos crean buenos resultados.", "Gute Gewohnheiten führen zu guten Ergebnissen."},
    {"Każdy wkład jest ważny.", "Every contribution matters.", "Cada aportación es importante.", "Jeder Beitrag ist wichtig."},
    {"Wykorzystaj dobrze ten dzień.", "Make good use of today.", "Aprovecha bien el día de hoy.", "Nutze den heutigen Tag gut."},
    {"Szukaj rozwiązań, nie przeszkód.", "Look for solutions, not obstacles.", "Busca soluciones, no obstáculos.", "Suche Lösungen statt Hindernisse."},
    {"Twoja praca tworzy wspólny sukces.", "Your work builds shared success.", "Tu trabajo crea un éxito compartido.", "Deine Arbeit schafft gemeinsamen Erfolg."},
    {"Spokój pomaga podejmować dobre decyzje.", "Calm helps you make good decisions.", "La calma ayuda a tomar buenas decisiones.", "Ruhe hilft, gute Entscheidungen zu treffen."},
    {"Warto robić rzeczy porządnie.", "Doing things well is worth it.", "Vale la pena hacer bien las cosas.", "Es lohnt sich, die Dinge gut zu machen."},
    {"Z każdym dniem umiesz więcej.", "You know more with every passing day.", "Cada día sabes hacer un poco más.", "Mit jedem Tag kannst du mehr."},
    {"Wspólny wysiłek przynosi efekty.", "A shared effort brings results.", "El esfuerzo compartido da frutos.", "Gemeinsamer Einsatz zahlt sich aus."},
    {"Działaj z troską i odpowiedzialnością.", "Act with care and responsibility.", "Actúa con cuidado y responsabilidad.", "Handle sorgfältig und verantwortungsvoll."},
    {"Małe usprawnienia robią wielką różnicę.", "Small improvements make a big difference.", "Las pequeñas mejoras marcan una gran diferencia.", "Kleine Verbesserungen bewirken viel."},
    {"Masz wpływ na jakość swojej pracy.", "You can shape the quality of your work.", "Puedes influir en la calidad de tu trabajo.", "Du beeinflusst die Qualität deiner Arbeit."},
    {"Zakończ dzień z poczuciem dobrze wykonanej pracy.", "End the day proud of work well done.", "Termina el día con el orgullo del trabajo bien hecho.", "Beende den Tag mit dem guten Gefühl, etwas geschafft zu haben."}
};

constexpr size_t kMotivationQuoteCount =
    sizeof(kMotivationQuotes) / sizeof(kMotivationQuotes[0]);

uint8_t dailyMotivationIndex() {
    static uint8_t fallbackIndex = static_cast<uint8_t>(esp_random() % kMotivationQuoteCount);
    if (!rtcAvailable) return fallbackIndex;

    const DateTime now = rtc.now();
    const uint32_t dateKey = static_cast<uint32_t>(now.year()) * 10000U +
                             static_cast<uint32_t>(now.month()) * 100U + now.day();
    const uint32_t savedDate = preferences.getUInt("quote_day", 0);
    const uint8_t savedIndex = preferences.getUChar("quote_idx", 0);
    if (savedDate == dateKey && savedIndex < kMotivationQuoteCount) {
        return savedIndex;
    }

    uint8_t index = static_cast<uint8_t>(esp_random() % kMotivationQuoteCount);
    if (savedDate != 0 && savedIndex < kMotivationQuoteCount &&
        kMotivationQuoteCount > 1 && index == savedIndex) {
        index = static_cast<uint8_t>((index + 1) % kMotivationQuoteCount);
    }
    if (preferences.putUChar("quote_idx", index) == 0 ||
        preferences.putUInt("quote_day", dateKey) == 0) {
        Serial.println("Could not persist the daily motivation quote selection");
    }
    return index;
}

const char* dailyMotivationQuote() {
    const MotivationQuote& quote = kMotivationQuotes[dailyMotivationIndex()];
    switch (config.language) {
        case Language::English: return quote.english;
        case Language::Spanish: return quote.spanish;
        case Language::German: return quote.german;
        case Language::Polish:
        default: return quote.polish;
    }
}

std::vector<String> parseCsvRow(const String& line) {
    std::vector<String> fields;
    String field;
    bool quoted = false;
    for (size_t index = 0; index < line.length(); ++index) {
        const char value = line[index];
        if (value == '"') {
            if (quoted && index + 1 < line.length() && line[index + 1] == '"') {
                field += '"';
                ++index;
            } else {
                quoted = !quoted;
            }
        } else if (value == ',' && !quoted) {
            fields.push_back(field);
            field = "";
        } else {
            field += value;
        }
    }
    fields.push_back(field);
    return fields;
}

bool readCsvRecord(File& file, String& record) {
    record = "";
    bool inQuotes = false;
    bool readAny = false;
    while (file.available()) {
        const char value = static_cast<char>(file.read());
        readAny = true;
        if (value == '"') {
            if (inQuotes && file.peek() == '"') {
                record += "\"\"";
                file.read();
            } else {
                inQuotes = !inQuotes;
                record += value;
            }
        } else if (value == '\n' && !inQuotes) {
            return true;
        } else if (value != '\r') {
            record += value;
        }
    }
    return readAny;
}

String csvEscape(const String& value) {
    String output = "\"";
    for (size_t index = 0; index < value.length(); ++index) {
        if (value[index] == '"') output += '"';
        output += value[index];
    }
    output += '"';
    return output;
}

String currentTimestamp();
String capturePhoto(const Employee& employee, const String& eventId);
void drawHome();
void drawActionChoice();
void drawEmployeeList();
void drawEmployeeEnrollment();
void drawAttendanceConfirmation();
void finishEmployeeRegistration(const Employee& employee);
size_t activeEmployeeCount();
bool loadOfflineAttendanceIndex();

bool ensureCsvFile(const char* path, const char* header) {
    if (!sdAvailable) return false;
    if (SD.exists(path)) {
        File existing = SD.open(path, FILE_READ);
        if (!existing) {
            Serial.printf("Could not inspect %s\n", path);
            return false;
        }
        const bool hasContent = existing.size() > 0;
        existing.close();
        if (hasContent) return true;
    }
    File file = SD.open(path, FILE_WRITE);
    if (!file) {
        Serial.printf("Could not create %s\n", path);
        return false;
    }
    const size_t written = file.println(header);
    file.flush();
    file.close();
    if (written == 0) {
        Serial.printf("Could not write header to %s\n", path);
        return false;
    }
    Serial.printf("Initialized empty %s\n", path);
    return true;
}

bool performAppsScriptPost(const String& body, String& responseBody,
                           bool& responseReceived) {
    if (WiFi.status() != WL_CONNECTED || config.appsScriptUrl.isEmpty() ||
        config.apiToken.isEmpty()) return false;
    WiFiClientSecure secureClient;
    secureClient.setCACert(kGoogleRootCertificate);
    HTTPClient http;
    http.setTimeout(kHttpTimeoutMs);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    if (!http.begin(secureClient, config.appsScriptUrl)) return false;
    http.addHeader("Content-Type", "application/json");
    const int status = http.POST(body);
    responseReceived = status > 0;
    if (status < 200 || status >= 300) {
        Serial.printf("Apps Script HTTP error: %d\n", status);
        http.end();
        return false;
    }
    responseBody = http.getString();
    http.end();
    JsonDocument parsed;
    if (deserializeJson(parsed, responseBody) != DeserializationError::Ok ||
        !(parsed["ok"] | false)) {
        Serial.println("Apps Script returned an invalid or unsuccessful response");
        return false;
    }
    return true;
}

void networkTask(void*) {
    while (true) {
        NetworkJob* job = nullptr;
        if (xQueueReceive(networkJobQueue, &job, 0) != pdTRUE &&
            xQueueReceive(networkLogQueue, &job, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }
        if (job == nullptr) {
            continue;
        }
        String responseBody;
        bool responseReceived = false;
        const bool success =
            performAppsScriptPost(job->body, responseBody, responseReceived);
        if (job->responseExpected) {
            NetworkResult* result = new (std::nothrow) NetworkResult{
                job->requestId, success, responseReceived, responseBody
            };
            if (result == nullptr || networkAsyncResultQueue == nullptr ||
                xQueueSend(networkAsyncResultQueue, &result, portMAX_DELAY) != pdTRUE) {
                delete result;
                Serial.printf("Could not return network result for request %lu\n",
                              static_cast<unsigned long>(job->requestId));
            }
        } else if (!success) {
            Serial.println("Queued Apps Script log upload failed");
        }
        delete job;
    }
}

bool enqueueNetworkRequest(const String& body, bool responseExpected,
                           uint32_t requestId) {
    QueueHandle_t queue = responseExpected ? networkJobQueue : networkLogQueue;
    if (queue == nullptr) {
        Serial.println("Network request queue is unavailable");
        return false;
    }
    NetworkJob* job = new (std::nothrow) NetworkJob{requestId, body, responseExpected};
    if (job == nullptr) {
        Serial.println("Could not allocate an Apps Script request");
        return false;
    }
    if (xQueueSend(queue, &job, pdMS_TO_TICKS(kNetworkQueueWaitMs)) != pdTRUE) {
        delete job;
        Serial.println("Apps Script request queue is full");
        return false;
    }
    return true;
}

uint32_t nextNetworkRequestId() {
    ++networkRequestCounter;
    if (networkRequestCounter == 0) ++networkRequestCounter;
    return networkRequestCounter;
}

bool enqueueAppsScriptLog(JsonDocument& request) {
    if (config.mode != Mode::Online || WiFi.status() != WL_CONNECTED ||
        config.appsScriptUrl.isEmpty() || config.apiToken.isEmpty()) {
        return false;
    }
    request["token"] = config.apiToken;
    String body;
    serializeJson(request, body);
    return enqueueNetworkRequest(body, false, nextNetworkRequestId());
}

bool enqueueAsyncAppsScriptRequest(JsonDocument& request, uint32_t requestId,
                                   String& serializedRequest) {
    if (config.mode != Mode::Online || WiFi.status() != WL_CONNECTED ||
        config.appsScriptUrl.isEmpty() || config.apiToken.isEmpty()) {
        return false;
    }
    request["token"] = config.apiToken;
    serializeJson(request, serializedRequest);
    return enqueueNetworkRequest(serializedRequest, true, requestId);
}

bool initializeNetworkWorker() {
    networkJobQueue = xQueueCreate(8, sizeof(NetworkJob*));
    networkLogQueue = xQueueCreate(8, sizeof(NetworkJob*));
    networkAsyncResultQueue = xQueueCreate(8, sizeof(NetworkResult*));
    if (networkJobQueue == nullptr || networkLogQueue == nullptr ||
        networkAsyncResultQueue == nullptr) {
        Serial.println("Could not create Apps Script queues");
        if (networkJobQueue != nullptr) vQueueDelete(networkJobQueue);
        if (networkLogQueue != nullptr) vQueueDelete(networkLogQueue);
        if (networkAsyncResultQueue != nullptr) vQueueDelete(networkAsyncResultQueue);
        networkJobQueue = nullptr;
        networkLogQueue = nullptr;
        networkAsyncResultQueue = nullptr;
        return false;
    }
    const BaseType_t taskCreated = xTaskCreatePinnedToCore(
        networkTask, "rcp-network", 12288, nullptr, 1, &networkTaskHandle, 0);
    if (taskCreated != pdPASS) {
        Serial.println("Could not create the Apps Script network task");
        vQueueDelete(networkJobQueue);
        vQueueDelete(networkLogQueue);
        vQueueDelete(networkAsyncResultQueue);
        networkJobQueue = nullptr;
        networkLogQueue = nullptr;
        networkAsyncResultQueue = nullptr;
        return false;
    }
    return true;
}

void addLog(const char* level, const char* code, const String& message) {
    const String eventId = String(millis()) + "-" + String(++eventCounter);
    const String timestamp = currentTimestamp();
    bool localLogWritten = false;
    if (!sdAvailable) {
        Serial.printf("Log not saved locally (SD unavailable): %s %s\n",
                      code, message.c_str());
    } else {
        File file = SD.open(kLogsPath, FILE_APPEND);
        if (!file) {
            Serial.printf("Could not append log to SD: %s\n", code);
        } else {
            if (file.size() == 0) {
                file.println("event_id,timestamp,level,code,message");
            }
            const String csvRow =
                csvEscape(eventId) + "," + csvEscape(timestamp) + "," +
                csvEscape(level) + "," + csvEscape(code) + "," +
                csvEscape(message) + "\n";
            const size_t written = file.print(csvRow);
            file.flush();
            file.close();
            localLogWritten = written == csvRow.length();
            if (!localLogWritten) {
                Serial.printf("Incomplete SD log write: %s\n", code);
            }
        }
    }

    if (config.mode == Mode::Online) {
        JsonDocument request;
        request["action"] = "log";
        JsonObject row = request["row"].to<JsonObject>();
        row["event_id"] = eventId;
        row["timestamp"] = timestamp;
        row["level"] = level;
        row["code"] = code;
        row["message"] = message;
        if (!enqueueAppsScriptLog(request)) {
            Serial.printf("Online log upload failed%s: %s\n",
                          localLogWritten ? "; retained on SD" : "", code);
        }
    }
}

bool loadConfig() {
    if (!sdAvailable) return false;
    File file = SD.open(kConfigPath, FILE_READ);
    if (!file) return false;

    JsonDocument document;
    const DeserializationError error = deserializeJson(document, file);
    file.close();
    if (error) {
        Serial.printf("Invalid config.json: %s\n", error.c_str());
        return false;
    }

    if ((document["schema_version"] | 0) != 1) {
        Serial.println("Unsupported config.json schema_version");
        return false;
    }
    const char* mode = document["mode"] | "";
    const char* language = document["language"] | "";
    if (strcmp(mode, "offline") == 0) {
        config.mode = Mode::Offline;
    } else if (strcmp(mode, "online") == 0) {
        config.mode = Mode::Online;
    } else {
        Serial.println("config.json must explicitly select offline or online mode");
        return false;
    }
    config.ssid = document["wifi_ssid"] | "";
    config.wifiPassword = document["wifi_password"] | "";
    config.appsScriptUrl = document["apps_script_url"] | "";
    config.apiToken = document["api_token"] | "";
    config.employeesSheetUrl = document["employees_sheet_url"] | "";
    config.attendanceSheetUrl = document["attendance_sheet_url"] | "";
    config.logsSheetUrl = document["logs_sheet_url"] | "";
    config.timezoneOffsetMinutes = document["timezone_offset_minutes"] | 60;
    config.cameraEnabled = document["camera_enabled"] | false;

    if (strcmp(language, "pl") == 0) config.language = Language::Polish;
    else if (strcmp(language, "en") == 0) config.language = Language::English;
    else if (strcmp(language, "es") == 0) config.language = Language::Spanish;
    else if (strcmp(language, "de") == 0) config.language = Language::German;
    else {
        Serial.println("config.json has an unsupported language");
        return false;
    }
    if (config.mode == Mode::Online) {
        if (config.ssid.isEmpty() ||
            !config.appsScriptUrl.startsWith("https://script.google.com/macros/s/") ||
            !config.appsScriptUrl.endsWith("/exec") || config.apiToken.length() < 32 ||
            !config.employeesSheetUrl.startsWith("https://docs.google.com/spreadsheets/d/") ||
            !config.attendanceSheetUrl.startsWith("https://docs.google.com/spreadsheets/d/") ||
            !config.logsSheetUrl.startsWith("https://docs.google.com/spreadsheets/d/")) {
            Serial.println("Online config is incomplete; refusing offline fallback");
            return false;
        }
    }
    configurationValid = true;
    return true;
}

bool saveConfig() {
    if (!sdAvailable) return false;
    if (config.mode == Mode::Online &&
        (config.ssid.isEmpty() ||
         !config.appsScriptUrl.startsWith("https://script.google.com/macros/s/") ||
         !config.appsScriptUrl.endsWith("/exec") || config.apiToken.length() < 32 ||
         !config.employeesSheetUrl.startsWith("https://docs.google.com/spreadsheets/d/") ||
         !config.attendanceSheetUrl.startsWith("https://docs.google.com/spreadsheets/d/") ||
         !config.logsSheetUrl.startsWith("https://docs.google.com/spreadsheets/d/"))) {
        lastMessage = text("Uzupełnij Wi-Fi, API i trzy arkusze",
                           "Configure Wi-Fi, API and all three sheets",
                           "Configure Wi-Fi, API y las tres hojas",
                           "WLAN, API und alle drei Tabellen konfigurieren");
        return false;
    }
    JsonDocument document;
    document["schema_version"] = 1;
    document["mode"] = config.mode == Mode::Offline ? "offline" : "online";
    switch (config.language) {
        case Language::English: document["language"] = "en"; break;
        case Language::Spanish: document["language"] = "es"; break;
        case Language::German: document["language"] = "de"; break;
        case Language::Polish:
        default: document["language"] = "pl"; break;
    }
    document["wifi_ssid"] = config.ssid;
    document["wifi_password"] = config.wifiPassword;
    document["timezone_offset_minutes"] = config.timezoneOffsetMinutes;
    document["apps_script_url"] = config.appsScriptUrl;
    document["api_token"] = config.apiToken;
    document["employees_sheet_url"] = config.employeesSheetUrl;
    document["attendance_sheet_url"] = config.attendanceSheetUrl;
    document["logs_sheet_url"] = config.logsSheetUrl;
    document["camera_enabled"] = config.cameraEnabled;

    constexpr char temporaryPath[] = "/config.json.tmp";
    constexpr char backupPath[] = "/config.json.bak";
    File file = SD.open(temporaryPath, FILE_WRITE);
    if (!file) return false;
    const size_t written = serializeJson(document, file);
    file.flush();
    file.close();
    if (written == 0) {
        SD.remove(temporaryPath);
        return false;
    }
    SD.remove(backupPath);
    const bool hadOriginal = SD.exists(kConfigPath);
    if (hadOriginal && !SD.rename(kConfigPath, backupPath)) {
        SD.remove(temporaryPath);
        return false;
    }
    if (!SD.rename(temporaryPath, kConfigPath)) {
        if (hadOriginal) SD.rename(backupPath, kConfigPath);
        SD.remove(temporaryPath);
        return false;
    }
    SD.remove(backupPath);
    configurationValid = true;
    return true;
}

bool loadEmployeesFromCsv() {
    employees.clear();
    offlineEmployeesLoaded = false;
    if (!sdAvailable) return false;
    File file = SD.open(kEmployeesPath, FILE_READ);
    if (!file) return false;
    String line;
    if (!readCsvRecord(file, line)) {
        file.close();
        return false;
    }
    if (line.startsWith("\xEF\xBB\xBF")) line.remove(0, 3);
    if (line != "employee_id,card_uid,first_name,last_name,active,relay_access") {
        file.close();
        Serial.println("employees.csv has an unsupported header");
        return false;
    }
    while (employees.size() < kMaximumEmployees && readCsvRecord(file, line)) {
        if (line.isEmpty()) continue;
        const auto fields = parseCsvRow(line);
        if (fields.size() != 6) {
            file.close();
            employees.clear();
            Serial.println("employees.csv contains a row with the wrong number of columns");
            return false;
        }
        Employee employee;
        employee.id = fields[0];
        employee.uid = fields[1];
        employee.uid.toUpperCase();
        employee.firstName = fields[2];
        employee.lastName = fields[3];
        String active = fields[4];
        String relayAccess = fields[5];
        active.toLowerCase();
        relayAccess.toLowerCase();
        if ((active != "true" && active != "false" && active != "1" && active != "0") ||
            (relayAccess != "true" && relayAccess != "false" &&
             relayAccess != "1" && relayAccess != "0")) {
            file.close();
            employees.clear();
            Serial.println("employees.csv contains an invalid boolean value");
            return false;
        }
        employee.active = active == "true" || active == "1";
        employee.relayAccess = relayAccess == "true" || relayAccess == "1";
        bool duplicate = employee.id.isEmpty() || employee.uid.isEmpty();
        for (const auto& existing : employees) {
            if (existing.id == employee.id || existing.uid == employee.uid) duplicate = true;
        }
        if (duplicate) {
            file.close();
            employees.clear();
            Serial.println("employees.csv contains empty or duplicate employee/card IDs");
            return false;
        }
        employees.push_back(employee);
    }
    file.close();
    if (!loadOfflineAttendanceIndex()) {
        employees.clear();
        Serial.println("Could not build the offline attendance index");
        return false;
    }
    offlineEmployeesLoaded = true;
    return true;
}

bool loadOfflineAttendanceIndex() {
    offlineAttendanceStates.clear();
    if (!sdAvailable) return false;
    File file = SD.open(kAttendancePath, FILE_READ);
    if (!file) return false;
    String line;
    if (!readCsvRecord(file, line)) {
        file.close();
        return false;
    }
    if (line.startsWith("\xEF\xBB\xBF")) line.remove(0, 3);
    if (line != "event_id,timestamp,employee_id,card_uid,event_type,photo_file,source,notes") {
        file.close();
        return false;
    }
    while (readCsvRecord(file, line)) {
        if (line.isEmpty()) continue;
        const auto fields = parseCsvRow(line);
        if (fields.size() != 8 || fields[0].isEmpty() || fields[2].isEmpty() ||
            fields[3].isEmpty() ||
            (fields[4] != "ENTRY" && fields[4] != "EXIT")) {
            file.close();
            offlineAttendanceStates.clear();
            return false;
        }
        bool updated = false;
        for (auto& state : offlineAttendanceStates) {
            if (state.employeeId == fields[2]) {
                state.eventType = fields[4];
                updated = true;
                break;
            }
        }
        if (!updated) {
            offlineAttendanceStates.push_back({fields[2], fields[4]});
        }
    }
    file.close();
    return true;
}

String lastOfflineAttendance(const String& employeeId) {
    for (const auto& state : offlineAttendanceStates) {
        if (state.employeeId == employeeId) return state.eventType;
    }
    return "";
}

void updateOfflineAttendance(const String& employeeId, const String& eventType) {
    for (auto& state : offlineAttendanceStates) {
        if (state.employeeId == employeeId) {
            state.eventType = eventType;
            return;
        }
    }
    offlineAttendanceStates.push_back({employeeId, eventType});
}

bool persistEmployeesOffline(const std::vector<Employee>& rows) {
    constexpr char temporaryPath[] = "/employees.csv.tmp";
    constexpr char backupPath[] = "/employees.csv.bak";
    SD.remove(temporaryPath);
    SD.remove(backupPath);
    File file = SD.open(temporaryPath, FILE_WRITE);
    if (!file) return false;

    bool writeOk =
        file.println("employee_id,card_uid,first_name,last_name,active,relay_access") > 0;
    for (const auto& employee : rows) {
        if (!writeOk) break;
        const String csvRow = csvEscape(employee.id) + "," + csvEscape(employee.uid) + "," +
                              csvEscape(employee.firstName) + "," +
                              csvEscape(employee.lastName) + "," +
                              (employee.active ? "true" : "false") + "," +
                              (employee.relayAccess ? "true" : "false") + "\n";
        writeOk = file.print(csvRow) == csvRow.length();
    }
    file.flush();
    file.close();
    if (!writeOk) {
        SD.remove(temporaryPath);
        return false;
    }

    const bool hadOriginal = SD.exists(kEmployeesPath);
    if (hadOriginal && !SD.rename(kEmployeesPath, backupPath)) {
        SD.remove(temporaryPath);
        return false;
    }
    if (!SD.rename(temporaryPath, kEmployeesPath)) {
        if (hadOriginal && !SD.rename(backupPath, kEmployeesPath)) {
            Serial.println("CRITICAL: Could not restore employees.csv backup");
        }
        SD.remove(temporaryPath);
        return false;
    }
    SD.remove(backupPath);
    return true;
}

void applyEmployeeUpdate(const Employee& updated) {
    bool found = false;
    for (auto& employee : employees) {
        if (employee.id == updated.id) {
            employee.active = updated.active;
            employee.relayAccess = updated.relayAccess;
            found = true;
            break;
        }
    }
    if (!found) {
        Serial.println("Employee update completed for a record no longer in memory");
        return;
    }
    addLog("INFO", updated.active ? "EMPLOYEE_UPDATED" : "EMPLOYEE_REMOVED",
           updated.id);
    lastMessage = updated.active
                      ? (updated.relayAccess
                             ? text("Nadano dostęp do przekaźnika",
                                    "Relay access enabled",
                                    "Acceso al relé habilitado",
                                    "Relaiszugriff aktiviert")
                             : text("Zmieniono uprawnienia",
                                    "Permissions updated",
                                    "Permisos actualizados",
                                    "Berechtigungen aktualisiert"))
                      : text("Pracownik usunięty",
                             "Employee removed", "Empleado eliminado",
                             "Mitarbeiter entfernt");
    if (screen == Screen::EmployeeList) drawEmployeeList();
    else if (screen == Screen::Home) drawHome();
}

bool updateEmployeeRecord(size_t index, bool active, bool relayAccess) {
    if (index >= employees.size()) return false;
    if (config.mode == Mode::Online) {
        if (pendingEmployeeUpdateRequestId != 0) {
            lastMessage = text("Trwa aktualizacja pracownika",
                               "Employee update is still running",
                               "La actualización sigue en curso",
                               "Mitarbeiteraktualisierung läuft");
            return false;
        }
        Employee updated = employees[index];
        updated.active = active;
        updated.relayAccess = relayAccess;
        JsonDocument request;
        request["action"] = "updateEmployee";
        JsonObject row = request["row"].to<JsonObject>();
        row["employee_id"] = updated.id;
        row["card_uid"] = updated.uid;
        row["active"] = active;
        row["relay_access"] = relayAccess;
        pendingEmployeeUpdateRequestId = nextNetworkRequestId();
        employeeUpdateRequestStartedAt = millis();
        String serializedRequest;
        if (!enqueueAsyncAppsScriptRequest(request, pendingEmployeeUpdateRequestId,
                                           serializedRequest)) {
            pendingEmployeeUpdateRequestId = 0;
            employeeUpdateRequestStartedAt = 0;
            lastMessage = text("Nie można wysłać aktualizacji",
                               "Could not queue employee update",
                               "No se pudo enviar la actualización",
                               "Aktualisierung konnte nicht gesendet werden");
            return false;
        }
        pendingEmployeeUpdate = updated;
        lastMessage = text("Aktualizacja pracownika...",
                           "Updating employee...",
                           "Actualizando empleado...",
                           "Mitarbeiter wird aktualisiert...");
        return true;
    }
    std::vector<Employee> updated = employees;
    updated[index].active = active;
    updated[index].relayAccess = relayAccess;
    if (!sdAvailable || !offlineEmployeesLoaded || !persistEmployeesOffline(updated)) {
        lastMessage = text("Nie można zapisać employees.csv",
                           "Could not save employees.csv",
                           "No se pudo guardar employees.csv",
                           "employees.csv konnte nicht gespeichert werden");
        return false;
    }
    employees = std::move(updated);
    applyEmployeeUpdate(employees[index]);
    return true;
}

bool parseOnlineEmployees(JsonArrayConst rows, std::vector<Employee>& parsedEmployees) {
    parsedEmployees.clear();
    for (JsonObjectConst row : rows) {
        if (parsedEmployees.size() >= kMaximumEmployees) break;
        Employee employee;
        employee.id = row["employee_id"] | "";
        employee.uid = row["card_uid"] | "";
        employee.uid.toUpperCase();
        employee.firstName = row["first_name"] | "";
        employee.lastName = row["last_name"] | "";
        String active = row["active"] | "true";
        String relayAccess = row["relay_access"] | "false";
        active.toLowerCase();
        relayAccess.toLowerCase();
        if ((active != "true" && active != "false" && active != "1" && active != "0") ||
            (relayAccess != "true" && relayAccess != "false" &&
             relayAccess != "1" && relayAccess != "0")) {
            parsedEmployees.clear();
            Serial.println("Online employee sheet contains an invalid boolean value");
            return false;
        }
        employee.active = active == "true" || active == "1";
        employee.relayAccess = relayAccess == "true" || relayAccess == "1";
        bool duplicate = employee.id.isEmpty() || employee.uid.isEmpty();
        for (const auto& existing : parsedEmployees) {
            if (existing.id == employee.id || existing.uid == employee.uid) duplicate = true;
        }
        if (duplicate) {
            parsedEmployees.clear();
            Serial.println("Online employee sheet contains empty or duplicate identifiers");
            return false;
        }
        parsedEmployees.push_back(employee);
    }
    return true;
}

bool queueOnlineEmployeeRefresh() {
    if (employeeRefreshRequestId != 0 || config.mode != Mode::Online ||
        WiFi.status() != WL_CONNECTED) {
        return false;
    }
    JsonDocument request;
    request["action"] = "employees";
    employeeRefreshRequestId = nextNetworkRequestId();
    employeeRefreshRequestStartedAt = millis();
    String serializedRequest;
    if (!enqueueAsyncAppsScriptRequest(request, employeeRefreshRequestId,
                                       serializedRequest)) {
        employeeRefreshRequestId = 0;
        employeeRefreshRequestStartedAt = 0;
        return false;
    }
    return true;
}

bool queueOnlineAttendanceState(const Employee& employee) {
    if (attendanceStateRequestId != 0 || config.mode != Mode::Online ||
        WiFi.status() != WL_CONNECTED) {
        return false;
    }
    JsonDocument request;
    request["action"] = "lastAttendance";
    request["employee_id"] = employee.id;
    attendanceStateRequestId = nextNetworkRequestId();
    attendanceStateRequestStartedAt = millis();
    String serializedRequest;
    if (!enqueueAsyncAppsScriptRequest(request, attendanceStateRequestId,
                                       serializedRequest)) {
        attendanceStateRequestId = 0;
        attendanceStateRequestStartedAt = 0;
        return false;
    }
    screen = Screen::AttendanceStateCheck;
    display.fillScreen(color(10, 17, 33));
    display.setTextColor(color(246, 249, 255));
    drawFittedText(employee.firstName + " " + employee.lastName,
                   24, 70, 432, 48, 2.2f,
                   lgfx::textdatum_t::middle_center);
    display.setTextColor(color(174, 190, 215));
    drawFittedText(text("Sprawdzanie statusu obecności...",
                        "Checking attendance status...",
                        "Comprobando estado de asistencia...",
                        "Anwesenheitsstatus wird geprüft..."),
                   24, 132, 432, 64, 1.8f,
                   lgfx::textdatum_t::middle_center);
    return true;
}

String currentTimestamp() {
    if (!rtcAvailable) return "";
    const DateTime now = rtc.now();
    char buffer[26];
    const int offset = config.timezoneOffsetMinutes;
    const char sign = offset < 0 ? '-' : '+';
    const int absoluteOffset = abs(offset);
    snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d%c%02d:%02d",
             now.year(), now.month(), now.day(), now.hour(), now.minute(), now.second(),
             sign, absoluteOffset / 60, absoluteOffset % 60);
    return String(buffer);
}

bool appendAttendance(const Employee& employee, AttendanceAction action,
                      const String& eventId, const String& photoPath,
                      String& recordedType) {
    recordedType = action == AttendanceAction::Entry ? "ENTRY" : "EXIT";
    if (config.mode != Mode::Offline || !sdAvailable) return false;
    File file = SD.open(kAttendancePath, FILE_APPEND);
    if (!file) {
        addLog("ERROR", "ATTENDANCE_WRITE_FAILED", "Nie można dopisać attendance.csv");
        return false;
    }
    if (file.size() == 0) {
        constexpr char header[] =
            "event_id,timestamp,employee_id,card_uid,event_type,photo_file,source,notes";
        const size_t headerBytes = file.println(header);
        file.flush();
        if (headerBytes != strlen(header) + 2 || file.getWriteError() != 0) {
            file.close();
            addLog("ERROR", "ATTENDANCE_WRITE_FAILED",
                   "Niepełny zapis nagłówka attendance.csv");
            return false;
        }
    }
    const String csvRow =
        csvEscape(eventId) + "," + csvEscape(currentTimestamp()) + "," +
        csvEscape(employee.id) + "," + csvEscape(employee.uid) + "," +
        csvEscape(recordedType) + "," + csvEscape(photoPath) + "," +
        csvEscape("offline") + ",\"\"\n";
    const size_t written = file.print(csvRow);
    file.flush();
    const bool writeOk = written == csvRow.length() && file.getWriteError() == 0;
    file.close();
    if (!writeOk) {
        addLog("ERROR", "ATTENDANCE_WRITE_FAILED",
               "Niepełny zapis attendance.csv");
        return false;
    }
    updateOfflineAttendance(employee.id, recordedType);
    return true;
}

bool hasPendingOnlineAttendance() {
    if (!sdAvailable || !SD.exists(kPendingAttendancePath)) return false;
    File file = SD.open(kPendingAttendancePath, FILE_READ);
    if (!file) return true;
    String record;
    const bool headerOk = readCsvRecord(file, record) &&
                          record == kAttendanceHeader;
    const bool hasRows = headerOk && readCsvRecord(file, record);
    file.close();
    return !headerOk || hasRows;
}

bool appendPendingOnlineAttendance(const Employee& employee,
                                   AttendanceAction action,
                                   const String& eventId,
                                   const String& photoPath) {
    if (!sdAvailable || config.mode != Mode::Online || !rtcAvailable) return false;
    if (!ensureCsvFile(kPendingAttendancePath, kAttendanceHeader)) return false;
    File check = SD.open(kPendingAttendancePath, FILE_READ);
    String header;
    const bool headerValid =
        check && readCsvRecord(check, header) && header == kAttendanceHeader;
    if (check) check.close();
    if (!headerValid) {
        Serial.println("Temporary online attendance queue header is invalid");
        return false;
    }
    File file = SD.open(kPendingAttendancePath, FILE_APPEND);
    if (!file) {
        Serial.println("Could not open online attendance queue on SD");
        return false;
    }
    String row = csvEscape(eventId) + "," + csvEscape(currentTimestamp()) + "," +
                 csvEscape(employee.id) + "," + csvEscape(employee.uid) + "," +
                 csvEscape(action == AttendanceAction::Entry ? "ENTRY" : "EXIT") +
                 "," + csvEscape(photoPath) + "," + csvEscape("online-buffer") +
                 ",\"\"\n";
    const size_t written = file.print(row);
    file.flush();
    const bool writeOk = written == row.length() && file.getWriteError() == 0;
    file.close();
    if (!writeOk) {
        Serial.println("Could not fully write online attendance queue record");
        return false;
    }
    return true;
}

bool removePendingOnlineAttendance(const String& eventId) {
    constexpr char temporaryPath[] = "/attendance_pending.csv.tmp";
    constexpr char backupPath[] = "/attendance_pending.csv.bak";
    if (!sdAvailable || !SD.exists(kPendingAttendancePath)) return false;
    File source = SD.open(kPendingAttendancePath, FILE_READ);
    if (!source) return false;
    String record;
    if (!readCsvRecord(source, record) || record != kAttendanceHeader) {
        source.close();
        Serial.println("Online attendance queue has an invalid header");
        return false;
    }
    if (!readCsvRecord(source, record)) {
        source.close();
        Serial.println("Online attendance queue unexpectedly has no pending row");
        return false;
    }
    const auto firstRow = parseCsvRow(record);
    if (firstRow.size() != 8 || firstRow[0] != eventId) {
        source.close();
        Serial.println("Online attendance queue head does not match server acknowledgement");
        return false;
    }

    SD.remove(temporaryPath);
    SD.remove(backupPath);
    File destination = SD.open(temporaryPath, FILE_WRITE);
    if (!destination) {
        source.close();
        Serial.println("Could not create temporary online attendance queue");
        return false;
    }
    bool writeOk = destination.println(kAttendanceHeader) > 0;
    while (writeOk && readCsvRecord(source, record)) {
        if (record.isEmpty()) continue;
        writeOk = destination.println(record) > 0;
    }
    destination.flush();
    writeOk = writeOk && destination.getWriteError() == 0;
    destination.close();
    source.close();
    if (!writeOk) {
        SD.remove(temporaryPath);
        Serial.println("Could not rewrite online attendance queue");
        return false;
    }

    if (!SD.rename(kPendingAttendancePath, backupPath)) {
        SD.remove(temporaryPath);
        Serial.println("Could not back up online attendance queue");
        return false;
    }
    if (!SD.rename(temporaryPath, kPendingAttendancePath)) {
        SD.rename(backupPath, kPendingAttendancePath);
        SD.remove(temporaryPath);
        Serial.println("Could not commit online attendance queue update");
        return false;
    }
    SD.remove(backupPath);
    return true;
}

void clearPendingOnlineAttendance() {
    preferences.remove("pending_event");
    preferences.remove("pend_emp");
    preferences.remove("pending_name");
    pendingOnlineEventId = "";
    pendingOnlineEmployeeId = "";
    pendingOnlineEmployeeName = "";
    pendingAttendanceRequestId = 0;
    pendingStatusRequestId = 0;
    pendingAttendanceRequestStartedAt = 0;
    pendingStatusRequestStartedAt = 0;
    pendingStatusCheckInFlight = false;
    pendingAttendanceAutomatic = false;
    pendingAttendanceApiRejected = false;
    pendingAttendanceExpectedType = "";
    pendingAttendanceRequestBody = "";
    preferences.remove("pending_payload");
}

void resolvePendingAttendance(const String& eventType) {
    if (eventType != "ENTRY" && eventType != "EXIT") {
        Serial.println("Attendance status returned an invalid event type");
        return;
    }
    const String resolvedEmployeeId = pendingOnlineEmployeeId;
    const String resolvedEmployeeName = pendingOnlineEmployeeName;
    clearPendingOnlineAttendance();
    attendanceConfirmationSuccess = true;
    attendanceConfirmationTitle =
        eventType == "ENTRY"
            ? text("Dzień dobry", "Good morning", "Buenos días", "Guten Morgen")
            : text("Do widzenia", "Goodbye", "Hasta luego", "Auf Wiedersehen");
    attendanceConfirmationName = resolvedEmployeeName;
    for (const auto& employee : employees) {
        if (employee.id == resolvedEmployeeId) {
            String employeeName = employee.firstName + " " + employee.lastName;
            employeeName.trim();
            if (!employeeName.isEmpty()) attendanceConfirmationName = employeeName;
            break;
        }
    }
    lastMessage = "";
    screen = Screen::AttendanceConfirmation;
    screenSince = millis();
    attendanceConfirmationUntil = screenSince + kAttendanceConfirmationMs;
    drawAttendanceConfirmation();
}

void showAttendanceApiRejection() {
    attendanceConfirmationSuccess = false;
    attendanceConfirmationTitle =
        text("Zdarzenie odrzucone przez API",
             "Event rejected by API",
             "Evento rechazado por API",
             "Ereignis von API abgelehnt");
    attendanceConfirmationName = "";
    lastMessage = attendanceConfirmationTitle;
    clearPendingOnlineAttendance();
    screen = Screen::AttendanceConfirmation;
    screenSince = millis();
    attendanceConfirmationUntil = screenSince + kAttendanceConfirmationMs;
    drawAttendanceConfirmation();
}

void schedulePendingAttendanceStatusCheck() {
    if (pendingOnlineEventId.isEmpty() || pendingAttendanceRequestId != 0 ||
        pendingStatusCheckInFlight || config.mode != Mode::Online ||
        WiFi.status() != WL_CONNECTED ||
        static_cast<int32_t>(millis() - nextPendingStatusAttemptAt) < 0) {
        return;
    }
    JsonDocument request;
    request["action"] = "eventStatus";
    request["event_id"] = pendingOnlineEventId;
    pendingStatusRequestId = nextNetworkRequestId();
    pendingStatusRequestStartedAt = millis();
    String serializedRequest;
    if (enqueueAsyncAppsScriptRequest(request, pendingStatusRequestId,
                                      serializedRequest)) {
        pendingStatusCheckInFlight = true;
    } else {
        pendingStatusRequestId = 0;
        pendingStatusRequestStartedAt = 0;
        nextPendingStatusAttemptAt = millis() + 10000;
    }
}

bool startPendingOnlineAttendanceUpload() {
    if (pendingQueueUploadRequestId != 0 || config.mode != Mode::Online ||
        WiFi.status() != WL_CONNECTED || !sdAvailable ||
        static_cast<int32_t>(millis() - nextQueueUploadAttemptAt) < 0) {
        return false;
    }
    if (!SD.exists(kPendingAttendancePath)) return false;
    File file = SD.open(kPendingAttendancePath, FILE_READ);
    if (!file) {
        Serial.println("Could not open online attendance queue for upload");
        pendingQueueSyncFailed = true;
        lastMessage = text("Błąd kolejki odbić na karcie SD",
                           "SD attendance queue error",
                           "Error en la cola de asistencia SD",
                           "Fehler in der SD-Buchungswarteschlange");
        if (screen == Screen::Home) drawHome();
        nextQueueUploadAttemptAt = millis() + 10000;
        return false;
    }
    String record;
    if (!readCsvRecord(file, record) || record != kAttendanceHeader) {
        file.close();
        Serial.println("Online attendance queue has an invalid header");
        pendingQueueSyncFailed = true;
        lastMessage = text("Błędny plik attendance_pending.csv",
                           "Invalid attendance_pending.csv file",
                           "Archivo attendance_pending.csv no válido",
                           "Ungültige Datei attendance_pending.csv");
        if (screen == Screen::Home) drawHome();
        nextQueueUploadAttemptAt = millis() + 30000;
        return false;
    }
    if (!readCsvRecord(file, record)) {
        file.close();
        return false;
    }
    file.close();
    const auto fields = parseCsvRow(record);
    if (fields.size() != 8 || fields[0].isEmpty() || fields[2].isEmpty() ||
        fields[3].isEmpty() || (fields[4] != "ENTRY" && fields[4] != "EXIT")) {
        Serial.println("Online attendance queue contains an invalid row");
        pendingQueueSyncFailed = true;
        lastMessage = text("Błędny wpis w kolejce odbić na SD",
                           "Invalid row in SD attendance queue",
                           "Registro no válido en la cola SD",
                           "Ungültiger Eintrag in der SD-Warteschlange");
        if (screen == Screen::Home) drawHome();
        nextQueueUploadAttemptAt = millis() + 30000;
        return false;
    }

    JsonDocument request;
    request["action"] = "attendance";
    JsonObject row = request["row"].to<JsonObject>();
    row["event_id"] = fields[0];
    row["timestamp"] = fields[1];
    row["employee_id"] = fields[2];
    row["card_uid"] = fields[3];
    row["event_type"] = fields[4];
    row["photo_file"] = fields[5];
    row["source"] = fields[6];
    row["notes"] = fields[7];
    pendingQueueUploadEventId = fields[0];
    pendingQueueUploadRequestId = nextNetworkRequestId();
    pendingQueueUploadStartedAt = millis();
    String serializedRequest;
    if (!enqueueAsyncAppsScriptRequest(request, pendingQueueUploadRequestId,
                                       serializedRequest)) {
        pendingQueueUploadEventId = "";
        pendingQueueUploadRequestId = 0;
        pendingQueueUploadStartedAt = 0;
        nextQueueUploadAttemptAt = millis() + 10000;
        return false;
    }
    return true;
}

void handleAsyncNetworkResults() {
    if (networkAsyncResultQueue == nullptr) return;
    NetworkResult* result = nullptr;
    while (xQueueReceive(networkAsyncResultQueue, &result, 0) == pdTRUE &&
           result != nullptr) {
        if (pendingQueueUploadRequestId != 0 &&
            result->requestId == pendingQueueUploadRequestId) {
            pendingQueueUploadRequestId = 0;
            pendingQueueUploadStartedAt = 0;
            if (result->success) {
                JsonDocument response;
                const bool acknowledged =
                    deserializeJson(response, result->body) == DeserializationError::Ok &&
                    String(response["event_id"] | "") == pendingQueueUploadEventId;
                if (acknowledged &&
                    removePendingOnlineAttendance(pendingQueueUploadEventId)) {
                    pendingQueueUploadEventId = "";
                    pendingQueueSyncFailed = false;
                    nextQueueUploadAttemptAt = millis();
                    if (!hasPendingOnlineAttendance()) {
                        lastMessage = "";
                        addLog("INFO", "ONLINE_ATTENDANCE_QUEUE_SYNCED",
                               "Temporary attendance queue uploaded");
                        if (screen == Screen::Home) drawHome();
                    }
                } else {
                    Serial.println("Attendance queue upload acknowledgement was invalid");
                    addLog("ERROR", "ONLINE_ATTENDANCE_QUEUE_ACK_FAILED",
                           pendingQueueUploadEventId);
                    pendingQueueUploadEventId = "";
                    pendingQueueSyncFailed = true;
                    lastMessage =
                        text("Nie potwierdzono wysłania odbicia — kolejka zachowana",
                             "Punch was not confirmed — queue retained",
                             "No se confirmó el envío — cola conservada",
                             "Buchung nicht bestätigt — Warteschlange bleibt");
                    if (screen == Screen::Home) drawHome();
                    nextQueueUploadAttemptAt = millis() + 10000;
                }
            } else {
                Serial.println("Online attendance queue upload failed; retaining row");
                pendingQueueUploadEventId = "";
                pendingQueueSyncFailed = true;
                lastMessage =
                    text("Nie można wysłać odbić — sprawdź połączenie",
                         "Could not sync punches — check connection",
                         "No se pudieron enviar registros — revise conexión",
                         "Buchungen nicht synchronisiert — Verbindung prüfen");
                if (screen == Screen::Home) drawHome();
                nextQueueUploadAttemptAt = millis() + 10000;
            }
        } else if (employeeRefreshRequestId != 0 &&
            result->requestId == employeeRefreshRequestId) {
            employeeRefreshRequestId = 0;
            employeeRefreshRequestStartedAt = 0;
            if (result->success) {
                JsonDocument response;
                std::vector<Employee> loaded;
                if (deserializeJson(response, result->body) == DeserializationError::Ok &&
                    response["rows"].is<JsonArrayConst>() &&
                    parseOnlineEmployees(response["rows"].as<JsonArrayConst>(), loaded)) {
                    employees = std::move(loaded);
                    onlineEmployeesLoaded = true;
                    if (pendingOnlineEventId.isEmpty()) lastMessage = "";
                    if (screen == Screen::EmployeeList) drawEmployeeList();
                    else if (screen == Screen::Home) drawHome();
                } else {
                    Serial.println("Online employee response is invalid");
                    addLog("ERROR", "EMPLOYEE_SYNC_FAILED", "Invalid employee response");
                }
            } else {
                Serial.println("Online employee refresh failed; retaining current list");
                addLog("ERROR", "EMPLOYEE_SYNC_FAILED", "Request failed");
            }
        } else if (attendanceStateRequestId != 0 &&
                   result->requestId == attendanceStateRequestId) {
            attendanceStateRequestId = 0;
            attendanceStateRequestStartedAt = 0;
            String eventType;
            bool validResponse = false;
            if (result->success) {
                JsonDocument response;
                validResponse =
                    deserializeJson(response, result->body) == DeserializationError::Ok &&
                    (response["ok"] | false) &&
                    response["last_event_type"].is<const char*>();
                if (validResponse) {
                    eventType = response["last_event_type"] | "";
                    validResponse = eventType.isEmpty() ||
                                    eventType == "ENTRY" || eventType == "EXIT";
                }
            }
            if (validResponse) {
                selectedEmployeePresent = eventType == "ENTRY";
                screen = Screen::ActionChoice;
                drawActionChoice();
            } else {
                Serial.println("Attendance state check failed; requesting manual punch type");
                addLog("WARN", "ATTENDANCE_STATE_CHECK_FAILED",
                       "Falling back to employee-confirmed entry or exit");
                outageActionChoice = true;
                screen = Screen::ActionChoice;
                drawActionChoice();
            }
        } else if (pendingEmployeeUpdateRequestId != 0 &&
                   result->requestId == pendingEmployeeUpdateRequestId) {
            pendingEmployeeUpdateRequestId = 0;
            employeeUpdateRequestStartedAt = 0;
            if (result->success) {
                applyEmployeeUpdate(pendingEmployeeUpdate);
            } else {
                lastMessage =
                text("Wynik aktualizacji nieznany — odśwież listę",
                     "Update result unknown — refresh the list",
                     "Resultado desconocido — actualice la lista",
                     "Ergebnis unbekannt — Liste aktualisieren");
                if (screen == Screen::EmployeeList) drawEmployeeList();
            }
        } else if (pendingEmployeeRegistrationRequestId != 0 &&
                   result->requestId == pendingEmployeeRegistrationRequestId) {
            pendingEmployeeRegistrationRequestId = 0;
            employeeRegistrationRequestStartedAt = 0;
            if (result->success) {
                finishEmployeeRegistration(pendingEmployeeRegistration);
            } else {
                String apiError;
                if (result->responseReceived) {
                    JsonDocument response;
                    if (deserializeJson(response, result->body) == DeserializationError::Ok) {
                        apiError = response["error"] | "";
                    }
                }
                if (apiError == "card_uid_already_exists") {
                    lastMessage = text("Ta karta jest już przypisana w arkuszu Google",
                                       "This card is already assigned in Google Sheets",
                                       "La tarjeta ya está asignada en Google Sheets",
                                       "Diese Karte ist bereits in Google Sheets zugeordnet");
                } else if (apiError == "employee_id_already_exists") {
                    lastMessage = text("To ID jest już używane w arkuszu Google",
                                       "This ID is already used in Google Sheets",
                                       "Este ID ya se usa en Google Sheets",
                                       "Diese ID wird bereits in Google Sheets verwendet");
                } else {
                    lastMessage =
                        text("Nie można dodać pracownika — sprawdź arkusz i połączenie",
                             "Could not add employee — check the sheet and connection",
                             "No se pudo añadir — revise la hoja y la conexión",
                             "Mitarbeiter konnte nicht hinzugefügt werden — "
                             "Tabelle und Verbindung prüfen");
                    if (!apiError.isEmpty()) {
                        Serial.printf("Employee registration rejected by API: %s\n",
                                      apiError.c_str());
                    } else {
                        Serial.println("Employee registration result was not successful");
                    }
                }
                pendingEmployeeRegistration = Employee{};
                if (screen == Screen::EmployeeEnroll) drawEmployeeEnrollment();
                if (screen == Screen::EmployeeEnroll) drawEmployeeEnrollment();
            }
        } else if (pendingAttendanceRequestId != 0 &&
                   result->requestId == pendingAttendanceRequestId) {
            pendingAttendanceRequestId = 0;
            pendingAttendanceRequestStartedAt = 0;
            if (result->success) {
                JsonDocument response;
                const bool parsed =
                    deserializeJson(response, result->body) == DeserializationError::Ok;
                if (parsed &&
                    String(response["event_id"] | "") == pendingOnlineEventId) {
                    String eventType = pendingAttendanceExpectedType;
                    if (response["event_type"].is<const char*>()) {
                        eventType = response["event_type"] | "";
                    }
                    if (eventType == "ENTRY" || eventType == "EXIT") {
                        if (!pendingAttendanceAutomatic &&
                            eventType != pendingAttendanceExpectedType) {
                            nextPendingStatusAttemptAt = millis();
                        } else {
                            resolvePendingAttendance(eventType);
                        }
                    } else {
                        nextPendingStatusAttemptAt = millis();
                    }
                } else {
                    nextPendingStatusAttemptAt = millis();
                }
            } else {
                JsonDocument errorResponse;
                if (result->responseReceived &&
                    deserializeJson(errorResponse, result->body) == DeserializationError::Ok &&
                    errorResponse["ok"].is<bool>() &&
                    !(errorResponse["ok"] | true)) {
                    pendingAttendanceApiRejected = true;
                }
                nextPendingStatusAttemptAt = millis();
            }
        } else if (pendingStatusRequestId != 0 &&
                   result->requestId == pendingStatusRequestId) {
            pendingStatusRequestId = 0;
            pendingStatusRequestStartedAt = 0;
            pendingStatusCheckInFlight = false;
            if (result->success) {
                JsonDocument response;
                const bool parsed =
                    deserializeJson(response, result->body) == DeserializationError::Ok;
                if (parsed && response["found"].is<bool>()) {
                    if (response["found"] | false) {
                        resolvePendingAttendance(response["event_type"] | "");
                    } else if (pendingAttendanceApiRejected) {
                        showAttendanceApiRejection();
                    } else if (!pendingAttendanceRequestBody.isEmpty()) {
                        const uint32_t retryId = nextNetworkRequestId();
                        JsonDocument retryRequest;
                        String serializedRequest;
                        if (deserializeJson(retryRequest, pendingAttendanceRequestBody) ==
                                DeserializationError::Ok &&
                            enqueueAsyncAppsScriptRequest(retryRequest, retryId,
                                                          serializedRequest)) {
                            pendingAttendanceRequestId = retryId;
                        } else {
                            nextPendingStatusAttemptAt = millis() + 10000;
                        }
                    } else {
                        nextPendingStatusAttemptAt = millis() + 10000;
                    }
                } else {
                    nextPendingStatusAttemptAt = millis() + 10000;
                }
            } else {
                nextPendingStatusAttemptAt = millis() + 10000;
            }
        } else {
            Serial.printf("Discarding unhandled network result %lu\n",
                          static_cast<unsigned long>(result->requestId));
        }
        delete result;
        result = nullptr;
    }
    schedulePendingAttendanceStatusCheck();
    startPendingOnlineAttendanceUpload();
}

bool queueOnlineAttendance(const Employee& employee, AttendanceAction action,
                           const String& eventId, const String& photoPath,
                           bool automatic) {
    pendingAttendanceApiRejected = false;
    pendingAttendanceAutomatic = automatic;
    pendingAttendanceExpectedType =
        action == AttendanceAction::Entry ? "ENTRY" : "EXIT";
    JsonDocument request;
    request["action"] = automatic ? "punch" : "attendance";
    JsonObject row = request["row"].to<JsonObject>();
    row["event_id"] = eventId;
    row["timestamp"] = currentTimestamp();
    row["employee_id"] = employee.id;
    row["card_uid"] = employee.uid;
    if (!automatic) row["event_type"] = pendingAttendanceExpectedType;
    row["photo_file"] = photoPath;
    row["source"] = "online";
    row["notes"] = "";
    String employeeName = employee.firstName + " " + employee.lastName;
    employeeName.trim();
    if (employeeName.isEmpty()) employeeName = employee.id;
    serializeJson(request, pendingAttendanceRequestBody);
    if (preferences.putString("pend_emp", employee.id) == 0 ||
        preferences.putString("pending_payload", pendingAttendanceRequestBody) == 0 ||
        preferences.putString("pending_name", employeeName) == 0 ||
        preferences.putString("pending_event", eventId) == 0) {
        preferences.remove("pending_event");
        preferences.remove("pend_emp");
        preferences.remove("pending_payload");
        preferences.remove("pending_name");
        pendingAttendanceRequestBody = "";
        lastMessage = text("Nie można zabezpieczyć ID zdarzenia",
                           "Could not persist event ID",
                           "No se pudo guardar el ID del evento",
                           "Ereignis-ID konnte nicht gesichert werden");
        return false;
    }
    pendingOnlineEventId = eventId;
    pendingOnlineEmployeeId = employee.id;
    pendingOnlineEmployeeName = employeeName;
    pendingAttendanceRequestId = nextNetworkRequestId();
    pendingAttendanceRequestStartedAt = millis();
    String serializedRequest;
    if (!enqueueAsyncAppsScriptRequest(request, pendingAttendanceRequestId,
                                       serializedRequest)) {
        clearPendingOnlineAttendance();
        return false;
    }
    return true;
}

void processAttendance(AttendanceAction action, bool automatic = false) {
    lastMessage = "";
    if (config.mode == Mode::Online && automatic &&
        WiFi.status() != WL_CONNECTED) {
        outageActionChoice = true;
        screen = Screen::ActionChoice;
        drawActionChoice();
        return;
    }
    const bool queueLocally =
        config.mode == Mode::Online &&
        (outageActionChoice || WiFi.status() != WL_CONNECTED);
    outageActionChoice = false;
    const String eventId = String(esp_random(), HEX) + "-" +
                           String(esp_random(), HEX) + "-" +
                           String(++eventCounter);
    const String photo = capturePhoto(selectedEmployee, eventId);
    if (config.mode == Mode::Online) {
        if (queueLocally) {
            if (appendPendingOnlineAttendance(selectedEmployee, action, eventId, photo)) {
                attendanceConfirmationSuccess = true;
                attendanceConfirmationTitle =
                    text("Zapisano tymczasowo — brak połączenia",
                         "Saved temporarily — no connection",
                         "Guardado temporalmente — sin conexión",
                         "Vorübergehend gespeichert — keine Verbindung");
                attendanceConfirmationName =
                    selectedEmployee.firstName + " " + selectedEmployee.lastName;
                addLog("WARN", "ONLINE_ATTENDANCE_QUEUED",
                       eventId + ":" +
                           (action == AttendanceAction::Entry ? "ENTRY" : "EXIT"));
            } else {
                attendanceConfirmationSuccess = false;
                attendanceConfirmationTitle =
                    text("BŁĄD ZAPISU — odbicie nie zostało zapisane",
                         "SAVE ERROR — punch was not recorded",
                         "ERROR — registro no guardado",
                         "SPEICHERFEHLER — Buchung nicht gespeichert");
                attendanceConfirmationName = "";
            }
            screen = Screen::AttendanceConfirmation;
            screenSince = millis();
            attendanceConfirmationUntil = screenSince + kAttendanceConfirmationMs;
            drawAttendanceConfirmation();
            return;
        }
        if (queueOnlineAttendance(selectedEmployee, action, eventId, photo, automatic)) {
            attendanceConfirmationSuccess = false;
            attendanceConfirmationTitle =
                text("Zapisywanie odbicia...", "Saving punch...",
                     "Guardando registro...", "Buchung wird gespeichert...");
            attendanceConfirmationName =
                selectedEmployee.firstName + " " + selectedEmployee.lastName;
            screen = Screen::AttendanceConfirmation;
            screenSince = millis();
            attendanceConfirmationUntil = screenSince + kAttendanceConfirmationMs;
            drawAttendanceConfirmation();
            return;
        }
        if (WiFi.status() != WL_CONNECTED) {
            if (automatic) {
                outageActionChoice = true;
                screen = Screen::ActionChoice;
                drawActionChoice();
                return;
            }
            if (appendPendingOnlineAttendance(selectedEmployee, action,
                                              eventId, photo)) {
                attendanceConfirmationSuccess = true;
                attendanceConfirmationTitle =
                    text("Zapisano tymczasowo — brak połączenia",
                         "Saved temporarily — no connection",
                         "Guardado temporalmente — sin conexión",
                         "Vorübergehend gespeichert — keine Verbindung");
                attendanceConfirmationName =
                    selectedEmployee.firstName + " " + selectedEmployee.lastName;
                addLog("WARN", "ONLINE_ATTENDANCE_QUEUED",
                       eventId + ":" +
                           (action == AttendanceAction::Entry ? "ENTRY" : "EXIT"));
                screen = Screen::AttendanceConfirmation;
                screenSince = millis();
                attendanceConfirmationUntil = screenSince + kAttendanceConfirmationMs;
                drawAttendanceConfirmation();
                return;
            }
            attendanceConfirmationSuccess = false;
            attendanceConfirmationTitle =
                text("BŁĄD ZAPISU — odbicie nie zostało zapisane",
                     "SAVE ERROR — punch was not recorded",
                     "ERROR — registro no guardado",
                     "SPEICHERFEHLER — Buchung nicht gespeichert");
            attendanceConfirmationName = "";
            screen = Screen::AttendanceConfirmation;
            screenSince = millis();
            attendanceConfirmationUntil = screenSince + kAttendanceConfirmationMs;
            drawAttendanceConfirmation();
            return;
        }
        attendanceConfirmationSuccess = false;
        if (lastMessage.isEmpty()) {
            lastMessage = text("Nie można wysłać zdarzenia",
                               "Could not queue event",
                               "No se pudo enviar el evento",
                               "Ereignis konnte nicht gesendet werden");
        }
        attendanceConfirmationTitle = lastMessage;
        attendanceConfirmationName = "";
        screenSince = millis();
        attendanceConfirmationUntil = screenSince + kAttendanceConfirmationMs;
        screen = Screen::AttendanceConfirmation;
        drawAttendanceConfirmation();
        return;
    }
    String recordedType;
    if (!appendAttendance(selectedEmployee, action, eventId, photo, recordedType)) {
        attendanceConfirmationSuccess = false;
        if (lastMessage.isEmpty()) {
            lastMessage = text("Błąd zapisu", "Write failed", "Error al guardar",
                               "Schreibfehler");
        }
        attendanceConfirmationTitle = lastMessage;
        attendanceConfirmationName = "";
    } else {
        attendanceConfirmationSuccess = true;
        attendanceConfirmationTitle =
            recordedType == "ENTRY"
                ? text("Dzień dobry", "Good morning", "Buenos días", "Guten Morgen")
                : text("Do widzenia", "Goodbye", "Hasta luego", "Auf Wiedersehen");
        attendanceConfirmationName =
            selectedEmployee.firstName + " " + selectedEmployee.lastName;
        lastMessage = "";
    }
    screenSince = millis();
    attendanceConfirmationUntil = screenSince + kAttendanceConfirmationMs;
    screen = Screen::AttendanceConfirmation;
    drawAttendanceConfirmation();
}

String photoNameComponent(const String& value) {
    String result;
    result.reserve(min<size_t>(value.length(), 32));
    bool previousUnderscore = false;
    for (size_t index = 0; index < value.length() && result.length() < 32;) {
        const uint8_t character = static_cast<uint8_t>(value[index]);
        if (character >= 0x80) {
            size_t sequenceLength = (character & 0xE0) == 0xC0 ? 2
                                    : (character & 0xF0) == 0xE0 ? 3
                                    : (character & 0xF8) == 0xF0 ? 4
                                                                  : 0;
            if (sequenceLength == 0 || index + sequenceLength > value.length() ||
                result.length() + sequenceLength > 32) {
                break;
            }
            bool validSequence = true;
            for (size_t offset = 1; offset < sequenceLength; ++offset) {
                if ((static_cast<uint8_t>(value[index + offset]) & 0xC0) != 0x80) {
                    validSequence = false;
                    break;
                }
            }
            if (!validSequence) {
                ++index;
                continue;
            }
            for (size_t offset = 0; offset < sequenceLength; ++offset) {
                result += value[index + offset];
            }
            index += sequenceLength;
            previousUnderscore = false;
            continue;
        }
        const bool allowed =
            (character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') ||
            character == '-' || character == '_';
        if (allowed) {
            result += static_cast<char>(character);
            previousUnderscore = character == '_';
        } else if (!previousUnderscore && !result.isEmpty()) {
            result += '_';
            previousUnderscore = true;
        }
        ++index;
    }
    while (result.endsWith("_") || result.endsWith(".")) {
        result.remove(result.length() - 1);
    }
    return result.isEmpty() ? String("unknown") : result;
}

String capturePhoto(const Employee& employee, const String& eventId) {
    if (!config.cameraEnabled) return "";
    if (!cameraAvailable || !sdAvailable) {
        addLog("WARN", "CAMERA_UNAVAILABLE", "Aparat niedostępny");
        return "";
    }
    if (!rtcAvailable) {
        addLog("ERROR", "PHOTO_TIMESTAMP_UNAVAILABLE",
               "Nie można nadać zdjęciu daty i godziny bez RTC");
        return "";
    }
    camera_fb_t* frame = esp_camera_fb_get();
    if (frame == nullptr) {
        addLog("ERROR", "CAMERA_CAPTURE_FAILED", "Nie udało się wykonać zdjęcia");
        return "";
    }
    const DateTime now = rtc.now();
    char timestamp[14];
    snprintf(timestamp, sizeof(timestamp), "%04d%02d%02d_%02d%02d",
             now.year(), now.month(), now.day(), now.hour(), now.minute());
    const String filenameBase =
        photoNameComponent(employee.lastName) + "_" +
        photoNameComponent(employee.firstName) + "_" +
        photoNameComponent(employee.id) + "_" + timestamp;
    String path = String("/photos/") + filenameBase + ".jpg";
    if (!SD.exists("/photos") && !SD.mkdir("/photos")) {
        esp_camera_fb_return(frame);
        addLog("ERROR", "PHOTO_WRITE_FAILED", "Nie można utworzyć katalogu zdjęć");
        return "";
    }
    if (SD.exists(path)) {
        path = String("/photos/") + filenameBase + "_" +
               photoNameComponent(eventId) + ".jpg";
        if (SD.exists(path)) {
            esp_camera_fb_return(frame);
            addLog("ERROR", "PHOTO_NAME_COLLISION", eventId);
            return "";
        }
    }
    File file = SD.open(path, FILE_WRITE);
    if (!file) {
        esp_camera_fb_return(frame);
        addLog("ERROR", "PHOTO_WRITE_FAILED", "Nie można zapisać zdjęcia");
        return "";
    }
    const size_t expected = frame->len;
    const size_t written = file.write(frame->buf, expected);
    file.flush();
    const bool writeOk = written == expected && file.getWriteError() == 0;
    file.close();
    esp_camera_fb_return(frame);
    if (!writeOk) {
        addLog("ERROR", "PHOTO_WRITE_FAILED", "Niepełny zapis zdjęcia");
        SD.remove(path);
        return "";
    }
    return path;
}

bool initCamera() {
    camera_config_t cameraConfig = {};
    cameraConfig.ledc_channel = LEDC_CHANNEL_0;
    cameraConfig.ledc_timer = LEDC_TIMER_0;
    cameraConfig.pin_d0 = pins::camera::y2;
    cameraConfig.pin_d1 = pins::camera::y3;
    cameraConfig.pin_d2 = pins::camera::y4;
    cameraConfig.pin_d3 = pins::camera::y5;
    cameraConfig.pin_d4 = pins::camera::y6;
    cameraConfig.pin_d5 = pins::camera::y7;
    cameraConfig.pin_d6 = pins::camera::y8;
    cameraConfig.pin_d7 = pins::camera::y9;
    cameraConfig.pin_xclk = pins::camera::xclk;
    cameraConfig.pin_pclk = pins::camera::pclk;
    cameraConfig.pin_vsync = pins::camera::vsync;
    cameraConfig.pin_href = pins::camera::href;
    cameraConfig.pin_sccb_sda = pins::camera::siod;
    cameraConfig.pin_sccb_scl = pins::camera::sioc;
    cameraConfig.pin_pwdn = pins::camera::pwdn;
    cameraConfig.pin_reset = pins::camera::reset;
    cameraConfig.xclk_freq_hz = 20000000;
    cameraConfig.pixel_format = PIXFORMAT_JPEG;
    cameraConfig.frame_size = FRAMESIZE_VGA;
    cameraConfig.jpeg_quality = 12;
    cameraConfig.fb_count = 1;
    cameraConfig.fb_location = CAMERA_FB_IN_PSRAM;
    cameraConfig.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
    const esp_err_t result = esp_camera_init(&cameraConfig);
    if (result != ESP_OK) {
        cameraInitError = result;
        Serial.printf("Camera init failed: 0x%x\n", result);
        return false;
    }
    cameraInitError = ESP_OK;
    return true;
}

void relayTimerCallback(void*) {
    gpio_set_level(static_cast<gpio_num_t>(pins::relay), 1);
    relayAutoClosed = true;
}

void setRelay(bool enabled) {
    if (enabled) {
        digitalWrite(pins::relay, LOW);
        relayWasActive = true;
        relayOffAt = millis() + kRelayPulseMs;
        if (relayTimer != nullptr) {
            if (esp_timer_is_active(relayTimer)) esp_timer_stop(relayTimer);
            if (esp_timer_start_once(relayTimer, kRelayPulseMs * 1000) == ESP_OK) {
                relayOffAt = 0;
            }
        }
        return;
    }
    digitalWrite(pins::relay, HIGH);
    if (relayTimer != nullptr && esp_timer_is_active(relayTimer)) {
        esp_timer_stop(relayTimer);
    }
    relayOffAt = 0;
    if (relayWasActive) {
        relayWasActive = false;
        addLog("INFO", "RELAY_UNLOCK_PULSE_COMPLETED", "");
    }
}

void drawButton(int16_t x, int16_t y, int16_t width, int16_t height,
                const String& label, uint16_t fill) {
    display.fillRoundRect(x, y, width, height, 12, fill);
    display.drawRoundRect(x, y, width, height, 12, color(210, 222, 241));
    display.setTextColor(color(250, 252, 255), fill);
    drawFittedText(label, x, y, width, height, readableUiSize(3.1f),
                   lgfx::textdatum_t::middle_center);
}

void drawLeftAlignedButton(int16_t x, int16_t y, int16_t width, int16_t height,
                           const String& label, uint16_t fill) {
    display.fillRoundRect(x, y, width, height, 12, fill);
    display.drawRoundRect(x, y, width, height, 12, color(210, 222, 241));
    display.setTextColor(color(250, 252, 255), fill);
    drawFittedText(label, x + 18, y, width - 32, height, readableUiSize(3.1f),
                   lgfx::textdatum_t::middle_left);
}

void drawHomeQuoteArea() {
    const uint16_t panelColor = color(22, 32, 54);
    display.fillRoundRect(24, 188, 432, 84, 12, panelColor);
    if (lastMessage.isEmpty()) {
        const char* quote = dailyMotivationQuote();
        float quoteSize = 3.1f;
        String lines[3];
        uint8_t lineCount = 0;
        bool wrapped = false;
        for (; quoteSize >= 1.25f && !wrapped; quoteSize -= 0.1f) {
            lines[0] = "";
            lines[1] = "";
            lines[2] = "";
            uint8_t line = 0;
            wrapped = true;
            setUiTextSize(readableUiSize(quoteSize));
            String remaining(quote);
            while (!remaining.isEmpty()) {
                const int space = remaining.indexOf(' ');
                const String word = space < 0
                                        ? remaining
                                        : remaining.substring(0, space);
                const String candidate = lines[line].isEmpty()
                                             ? word
                                             : lines[line] + " " + word;
                if (!lines[line].isEmpty() &&
                    display.textWidth(candidate) > 408) {
                    if (line == 2) {
                        wrapped = false;
                        break;
                    }
                    ++line;
                    lines[line] = word;
                } else {
                    lines[line] = candidate;
                }
                remaining = space < 0 ? "" : remaining.substring(space + 1);
            }
            lineCount = line + 1;
        }
        if (!wrapped) {
            lines[0] = quote;
            lineCount = 1;
            quoteSize = 1.25f;
        } else {
            quoteSize += 0.1f;
        }
        const int16_t lineHeight = 60 / lineCount;
        for (uint8_t line = 0; line < lineCount; ++line) {
            drawFittedText(lines[line], 32, 188 + line * lineHeight, 416,
                           lineHeight, readableUiSize(quoteSize),
                           lgfx::textdatum_t::middle_center);
        }
    }

    const char* prompt = lastMessage.isEmpty()
                             ? text("Zbliż kartę pracownika", "Tap employee card",
                                    "Acerque la tarjeta", "Karte vorhalten")
                             : lastMessage.c_str();
    display.setTextColor(lastMessage.isEmpty() ? color(174, 190, 215)
                                                : color(255, 205, 112),
                         panelColor);
    drawFittedText(prompt, 34, 248, 412, 22,
                   readableUiSize(strlen(prompt) > 48 ? 1.35f : 1.9f),
                   lgfx::textdatum_t::middle_center);
    if (rtcAvailable) {
        const DateTime now = rtc.now();
        char dateKey[11];
        snprintf(dateKey, sizeof(dateKey), "%04d-%02d-%02d",
                 now.year(), now.month(), now.day());
        lastHomeQuoteDate = dateKey;
    }
}

void drawHome() {
    drawScreenBackground();
    const uint16_t panelColor = color(22, 32, 54);
    if (!wallpaperEnabled || wallpaperPixels == nullptr) {
        display.fillRoundRect(16, 16, 448, 288, 20, panelColor);
    } else {
        display.fillRoundRect(12, 12, 456, 42, 10, color(10, 17, 33));
        display.fillRoundRect(12, 270, 456, 40, 10, color(10, 17, 33));
    }
    display.fillRoundRect(32, 59, 416, 84, 14, panelColor);
    display.fillRoundRect(68, 145, 344, 40, 12, panelColor);
    display.setTextColor(color(129, 172, 255));
    drawFittedText("RCP2027 PROMIR-BUD PASSION LINE",
                   24, 14, 350, 34, readableUiSize(1.9f),
                   lgfx::textdatum_t::middle_left);
    display.setTextColor(color(174, 190, 215));
    setUiTextSize(1.5f);
    drawFittedText(config.mode == Mode::Offline ? "OFFLINE" : "ONLINE",
                   366, 16, 88, 30, 1.5f, lgfx::textdatum_t::middle_center);
    display.setTextColor(color(248, 250, 255));
    if (rtcAvailable) {
        const DateTime now = rtc.now();
        char clockText[10];
        snprintf(clockText, sizeof(clockText), "%02d:%02d:%02d",
                 now.hour(), now.minute(), now.second());
        display.setTextColor(color(248, 250, 255));
        drawFittedText(clockText, 40, 62, 396, 77, 9.0f,
                       lgfx::textdatum_t::middle_center);
        char dateText[20];
        snprintf(dateText, sizeof(dateText), "%04d-%02d-%02d",
                 now.year(), now.month(), now.day());
        lastHomeDate = dateText;
        display.setTextColor(color(174, 190, 215));
        drawFittedText(dateText, 70, 145, 336, 40, 3.8f,
                       lgfx::textdatum_t::middle_center);
    } else {
        display.setTextColor(color(255, 191, 87));
        drawFittedText(text("Brak zegara RTC", "RTC unavailable",
                            "RTC no disponible", "RTC nicht verfügbar"),
                       50, 75, 380, 66, 2.5f,
                       lgfx::textdatum_t::middle_center);
    }
    drawHomeQuoteArea();
    display.setTextColor(color(205, 216, 232));
    drawFittedText(configurationValid ? (sdAvailable ? "SD OK" : "SD ERROR")
                                      : "CONFIG ERROR",
                   20, 270, 92, 36, readableUiSize(1.7f),
                   lgfx::textdatum_t::middle_left);
    drawFittedText(rfidAvailable ? "RFID OK" : "RFID ERROR",
                   120, 270, 92, 36, readableUiSize(1.7f),
                   lgfx::textdatum_t::middle_left);
    drawFittedText(config.mode == Mode::Online
                       ? (WiFi.status() == WL_CONNECTED ? "WIFI OK" : "WIFI ERROR")
                       : "OFFLINE",
                   220, 270, 112, 36, readableUiSize(1.7f),
                   lgfx::textdatum_t::middle_left);
    drawButton(340, 270, 120, 36,
               text("USTAWIENIA", "SETTINGS", "AJUSTES", "EINSTELLUNGEN"),
               color(45, 101, 191));
}

void drawAttendanceConfirmation() {
    drawScreenBackground();
    display.fillRoundRect(28, 82, 424, 142, 20, color(22, 32, 54));
    const uint16_t accent = attendanceConfirmationSuccess
                                ? color(113, 230, 173)
                                : color(255, 191, 87);
    display.setTextColor(accent);
    drawFittedText(attendanceConfirmationTitle, 40, 94, 400, 55, 2.8f,
                   lgfx::textdatum_t::middle_center);

    if (attendanceConfirmationSuccess) {
        display.setTextColor(color(248, 250, 255));
        drawFittedText(attendanceConfirmationName, 40, 153, 400, 52,
                       2.8f, lgfx::textdatum_t::middle_center);
    }
}

void drawUnknownCard() {
    display.fillScreen(color(10, 17, 33));
    display.setTextColor(color(255, 191, 87));
    setUiTextSize(3);
    display.drawCenterString(text("Błędny kod", "Unknown card", "Código incorrecto",
                                  "Unbekannte Karte"),
                             240, 50);
    display.setTextColor(color(221, 230, 246));
    setUiTextSize(2);
    display.drawCenterString(pendingEmployeeUid, 240, 105);
    display.drawCenterString(text("Czy chcesz dodać nowy kod?",
                                  "Would you like to add this card?",
                                  "¿Desea añadir esta tarjeta?",
                                  "Möchten Sie diese Karte hinzufügen?"),
                             240, 150);
    drawButton(55, 215, 170, 62,
               text("Tak", "Yes", "Sí", "Ja"), color(25, 139, 103));
    drawButton(255, 215, 170, 62,
               text("Nie", "No", "No", "Nein"), color(103, 47, 58));
}

String* enrollmentFieldValue(uint8_t field) {
    if (field == 0) return &newEmployeeId;
    if (field == 1) return &newEmployeeFirstName;
    return &newEmployeeLastName;
}

const char* enrollmentAccentCharacter() {
    static const char* polishLower[] = {"ą", "ę", "ć", "ł", "ń", "ó", "ś", "ź", "ż"};
    static const char* polishUpper[] = {"Ą", "Ę", "Ć", "Ł", "Ń", "Ó", "Ś", "Ź", "Ż"};
    static const char* spanishLower[] = {"á", "é", "í", "ñ", "ó", "ú", "ü"};
    static const char* spanishUpper[] = {"Á", "É", "Í", "Ñ", "Ó", "Ú", "Ü"};
    static const char* germanLower[] = {"ä", "ö", "ü", "ß"};
    static const char* germanUpper[] = {"Ä", "Ö", "Ü", "ẞ"};
    if (config.language == Language::Polish) {
        return enrollmentLowercase ? polishLower[enrollmentAccentIndex % 9]
                                   : polishUpper[enrollmentAccentIndex % 9];
    }
    if (config.language == Language::Spanish) {
        return enrollmentLowercase ? spanishLower[enrollmentAccentIndex % 7]
                                   : spanishUpper[enrollmentAccentIndex % 7];
    }
    if (config.language == Language::German) {
        return enrollmentLowercase ? germanLower[enrollmentAccentIndex % 4]
                                   : germanUpper[enrollmentAccentIndex % 4];
    }
    return enrollmentAccentIndex % 2 == 0 ? "-" : "_";
}

void removeLastInputCharacter(String& value) {
    if (value.isEmpty()) return;
    size_t index = value.length() - 1;
    while (index > 0 &&
           (static_cast<uint8_t>(value[index]) & 0xC0) == 0x80) {
        --index;
    }
    value.remove(index);
}

void drawEmployeeEnrollment() {
    display.fillScreen(color(10, 17, 33));
    display.setTextColor(color(246, 249, 255));
    drawFittedText(text("Nowy pracownik", "New employee",
                        "Nuevo empleado", "Neuer Mitarbeiter"),
                   20, 3, 440, 25, readableUiSize(1.8f),
                   lgfx::textdatum_t::middle_center);
    display.fillRoundRect(16, 30, 448, 44, 8,
                          lastMessage.isEmpty() ? color(22, 32, 54)
                                                : color(104, 35, 45));
    if (lastMessage.isEmpty()) {
        display.setTextColor(color(174, 190, 215));
        drawFittedText(text("Wprowadź dane pracownika",
                            "Enter employee details",
                            "Introduzca los datos del empleado",
                            "Mitarbeiterdaten eingeben"),
                       24, 32, 432, 40, readableUiSize(1.9f),
                       lgfx::textdatum_t::middle_center);
    } else {
        display.setTextColor(color(255, 239, 240));
        String firstLine;
        String secondLine;
        String remaining = lastMessage;
        setUiTextSize(readableUiSize(2.0f));
        while (!remaining.isEmpty()) {
            const int space = remaining.indexOf(' ');
            const String word = space < 0 ? remaining : remaining.substring(0, space);
            const String candidate = firstLine.isEmpty() ? word : firstLine + " " + word;
            if (!firstLine.isEmpty() && display.textWidth(candidate) > 420) {
                secondLine = remaining;
                break;
            }
            firstLine = candidate;
            remaining = space < 0 ? "" : remaining.substring(space + 1);
        }
        drawFittedText(firstLine, 24, 31, 432, secondLine.isEmpty() ? 42 : 21,
                       readableUiSize(2.0f),
                       lgfx::textdatum_t::middle_center);
        if (!secondLine.isEmpty()) {
            drawFittedText(secondLine, 24, 51, 432, 21, readableUiSize(2.0f),
                           lgfx::textdatum_t::middle_center);
        }
    }

    const char* labels[] = {
        text("ID", "ID", "ID", "ID"),
        text("Imię", "First name", "Nombre", "Vorname"),
        text("Nazwisko", "Last name", "Apellido", "Nachname")
    };
    for (uint8_t field = 0; field < 3; ++field) {
        const int y = 78 + field * 27;
        const uint16_t border = field == enrollmentField
                                    ? color(129, 172, 255)
                                    : color(75, 92, 121);
        display.fillRoundRect(20, y, 440, 26, 6, color(22, 32, 54));
        display.drawRoundRect(20, y, 440, 26, 6, border);
        display.setTextColor(color(174, 190, 215));
        drawFittedText(String(labels[field]) + ":", 28, y + 3, 82, 20,
                       readableUiSize(1.5f),
                       lgfx::textdatum_t::middle_left);
        display.setTextColor(color(250, 252, 255));
        drawFittedText(*enrollmentFieldValue(field), 115, y + 3, 335, 20,
                       readableUiSize(1.5f),
                       lgfx::textdatum_t::middle_left);
    }

    const String relayLabel =
        String(text("Dostęp do przekaźnika: ", "Relay access: ",
                    "Acceso al relé: ", "Relaiszugriff: ")) +
        (newEmployeeRelayAccess ? text("TAK", "YES", "SÍ", "JA")
                                : text("NIE", "NO", "NO", "NEIN"));
    drawButton(20, 161, 440, 23, relayLabel,
               newEmployeeRelayAccess ? color(25, 116, 88) : color(39, 54, 82));

    const char* alphaRows[] = {
        "QWERTYUIOP",
        "ASDFGHJKL",
        "ZXCVBNM"
    };
    const int rowStarts[] = {20, 42, 86};
    const int rowLengths[] = {10, 9, 7};
    const char* numberRows[] = {
        "1234567890",
        "0987654321",
        ".,-_@"
    };
    const int numberLengths[] = {10, 10, 5};
    for (int row = 0; row < 3; ++row) {
        const char* keys = enrollmentNumericKeyboard ? numberRows[row] : alphaRows[row];
        const int count = enrollmentNumericKeyboard ? numberLengths[row] : rowLengths[row];
        const int start = enrollmentNumericKeyboard ? 20 : rowStarts[row];
        for (int index = 0; index < count; ++index) {
            String label = String(keys[index]);
            if (!enrollmentNumericKeyboard && enrollmentLowercase) {
                label.toLowerCase();
            }
            const int x = start + index * 44;
            drawButton(x, 184 + row * 30, 40, 27, label, color(39, 54, 82));
        }
    }

    drawButton(15, 273, 74, 20,
               enrollmentNumericKeyboard ? "ABC" : "123", color(52, 73, 109));
    drawButton(96, 273, 68, 20, "a/A", color(52, 73, 109));
    drawButton(171, 273, 54, 20, enrollmentAccentCharacter(), color(52, 73, 109));
    drawButton(232, 273, 102, 20,
               text("Spacja", "Space", "Espacio", "Leer"), color(52, 73, 109));
    drawButton(341, 273, 124, 20,
               text("Usuń znak", "Backspace", "Borrar", "Löschen"),
               color(103, 47, 58));
    drawButton(65, 298, 150, 20,
               text("Zapisz", "Save", "Guardar", "Speichern"), color(25, 139, 103));
    drawButton(265, 298, 150, 20,
               text("Anuluj", "Cancel", "Cancelar", "Abbruch"), color(103, 47, 58));
}

void refreshHomeClock() {
    if (!rtcAvailable) return;
    const DateTime now = rtc.now();
    char clockText[10];
    snprintf(clockText, sizeof(clockText), "%02d:%02d:%02d",
             now.hour(), now.minute(), now.second());
    display.fillRoundRect(32, 59, 416, 84, 14, color(22, 32, 54));
    display.setTextColor(color(248, 250, 255));
    drawFittedText(clockText, 40, 62, 396, 77, 9.0f,
                   lgfx::textdatum_t::middle_center);

    char dateText[20];
    snprintf(dateText, sizeof(dateText), "%04d-%02d-%02d",
             now.year(), now.month(), now.day());
    if (lastHomeDate != dateText) {
        lastHomeDate = dateText;
        display.fillRoundRect(68, 145, 344, 40, 12, color(22, 32, 54));
        display.setTextColor(color(174, 190, 215));
        drawFittedText(dateText, 70, 145, 336, 40, 3.8f,
                       lgfx::textdatum_t::middle_center);
    }
    if (lastHomeQuoteDate != dateText) drawHomeQuoteArea();
}

void drawActionChoice() {
    display.fillScreen(color(10, 17, 33));
    if (outageActionChoice) {
        display.setTextColor(color(255, 205, 112));
        drawFittedText(text("BRAK POŁĄCZENIA", "NO CONNECTION",
                            "SIN CONEXIÓN", "KEINE VERBINDUNG"),
                       24, 20, 432, 42, readableUiSize(2.4f),
                       lgfx::textdatum_t::middle_center);
        display.setTextColor(color(246, 249, 255));
        drawFittedText(selectedEmployee.firstName + " " + selectedEmployee.lastName,
                       24, 66, 432, 42, readableUiSize(2.0f),
                       lgfx::textdatum_t::middle_center);
        display.setTextColor(color(221, 230, 246));
        drawFittedText(text("Potwierdź rodzaj odbicia",
                            "Confirm attendance action",
                            "Confirme la acción",
                            "Buchungsart bestätigen"),
                       24, 110, 432, 34, readableUiSize(1.8f),
                       lgfx::textdatum_t::middle_center);
        drawButton(25, 154, 205, 72,
                   text("WEJŚCIE", "CLOCK IN", "ENTRADA", "KOMMEN"),
                   color(25, 139, 103));
        drawButton(250, 154, 205, 72,
                   text("WYJŚCIE", "CLOCK OUT", "SALIDA", "GEHEN"),
                   color(45, 101, 191));
        if (selectedEmployee.relayAccess) {
            drawButton(115, 236, 250, 54,
                       text("Otwórz drzwi", "Unlock door",
                            "Abrir puerta", "Tür öffnen"),
                       color(164, 94, 34));
        }
        return;
    }
    display.setTextColor(color(246, 249, 255));
    setUiTextSize(2);
    display.drawCenterString(selectedEmployee.firstName + " " + selectedEmployee.lastName,
                             240, 34);
    display.setTextColor(color(174, 190, 215));
    setUiTextSize(1);
    display.drawCenterString(
        selectedEmployeePresent
            ? text("Jesteś na zakładzie — wybierz", "On site — choose an action",
                   "Está en el trabajo — elija", "Anwesend — Aktion wählen")
            : text("Poza zakładem — zarejestruj wejście",
                   "Off site — clock in",
                   "Fuera — registre la entrada",
                   "Abwesend — einstempeln"),
        240, 72);
    if (selectedEmployeePresent) {
        drawButton(30, 132, 200, 92,
                   text("Wyjście", "Clock out", "Salida", "Gehen"),
                   color(45, 101, 191));
        drawButton(250, 132, 200, 92,
                   text("Otwórz drzwi", "Unlock door", "Abrir puerta",
                        "Tür öffnen"),
                   color(164, 94, 34));
    } else {
        drawButton(70, 132, 340, 92,
                   text("Wejście", "Clock in", "Entrada", "Kommen"),
                   color(25, 139, 103));
    }
}

void drawPinEntry() {
    display.fillScreen(color(10, 17, 33));
    display.setTextColor(color(246, 249, 255));
    setUiTextSize(2);
    const char* title = employeeEnrollmentRequested
                            ? text("PIN dodawania pracownika",
                                   "Employee registration PIN",
                                   "PIN de registro", "PIN zur Mitarbeiteraufnahme")
                            : screen == Screen::PinConfirm
                            ? text("Potwierdź nowy PIN", "Confirm new PIN",
                                   "Confirme el nuevo PIN", "Neue PIN bestätigen")
                            : screen == Screen::PinChange
                                  ? text("Wprowadź nowy PIN", "Enter new PIN",
                                         "Introduzca un nuevo PIN", "Neue PIN eingeben")
                                  : text("PIN ustawień", "Settings PIN", "PIN de ajustes",
                                         "Einstellungs-PIN");
    display.drawCenterString(title, 240, 18);
    setUiTextSize(2);
    String masked;
    for (size_t i = 0; i < pinInput.length(); ++i) masked += "* ";
    display.drawCenterString(masked, 240, 62);
    const char* keys[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "C", "0", "OK"};
    for (int index = 0; index < 12; ++index) {
        const int row = index / 3;
        const int column = index % 3;
        drawButton(84 + column * 106, 84 + row * 45, 92, 39, keys[index],
                   index == 11 ? color(25, 139, 103) : color(39, 54, 82));
    }
    drawButton(188, 270, 104, 38,
               text("Anuluj", "Cancel", "Cancelar", "Abbruch"),
               color(103, 47, 58));
}

void drawSettings() {
    static constexpr const char* settingLabels[][4] = {
        {"Język", "Language", "Idioma", "Sprache"},
        {"Tryb pracy", "Operating mode", "Modo de operación", "Betriebsmodus"},
        {"Zdjęcia", "Photos", "Fotos", "Fotos"},
        {"Czcionka", "Font", "Fuente", "Schriftart"},
        {"Tryb kolorów", "Color mode", "Modo de color", "Farbmodus"},
        {"Pracownicy", "Employees", "Empleados", "Mitarbeiter"},
        {"Data i godzina", "Date and time", "Fecha y hora", "Datum und Uhrzeit"},
        {"Zmień PIN", "Change PIN", "Cambiar PIN", "PIN ändern"},
        {"Kalibruj dotyk", "Calibrate touch", "Calibrar pantalla",
         "Touchscreen kalibrieren"},
        {"Wi-Fi", "Wi-Fi", "Wi-Fi", "WLAN"},
        {"Tapeta", "Wallpaper", "Fondo", "Hintergrund"}
    };
    const size_t maxScroll = kTotalSettings - kVisibleSettings;
    if (settingsScroll > maxScroll) settingsScroll = maxScroll;

    display.fillScreen(color(10, 17, 33));
    display.setTextColor(color(246, 249, 255));
    drawFittedText(text("Ustawienia", "Settings", "Ajustes", "Einstellungen"),
                   24, 3, 432, 32, readableUiSize(2.0f),
                   lgfx::textdatum_t::middle_center);
    for (size_t row = 0; row < kVisibleSettings; ++row) {
        const size_t index = settingsScroll + row;
        const int y = kSettingsRowY + static_cast<int>(row) * kSettingsRowHeight;
        String label = settingLabels[index][static_cast<uint8_t>(config.language)];
        if (index == 0) {
            label += ": ";
            label += languageLabel();
        } else if (index == 1) {
            label += ": ";
            label += modeLabel();
        } else if (index == 2) {
            label += ": ";
            label += config.cameraEnabled
                         ? text("WŁ", "ON", "SÍ", "EIN")
                         : text("WYŁ", "OFF", "NO", "AUS");
            if (config.cameraEnabled) {
                label += cameraAvailable ? "  OK" : "  !";
            }
        } else if (index == 3) {
            label += ": ";
            label += uiFontLabel();
        } else if (index == 4) {
            label += ": ";
            label += displayInverted
                         ? text("Jasny", "Light", "Claro", "Hell")
                         : text("Ciemny", "Dark", "Oscuro", "Dunkel");
        } else if (index == 5) {
            label += " (" + String(activeEmployeeCount()) + ")";
        } else if (index == 9) {
            label += ": ";
            label += config.ssid.isEmpty()
                         ? text("Nie skonfigurowano", "Not configured",
                                "Sin configurar", "Nicht eingerichtet")
                         : config.ssid.substring(0, 16) +
                               (config.ssid.length() > 16 ? "..." : "");
        } else if (index == 10) {
            label += ": ";
            label += wallpaperEnabled
                         ? text("WŁĄCZONA", "ON", "ACTIVADO", "EIN")
                         : text("WYŁĄCZONA", "OFF", "DESACTIVADO", "AUS");
        }
        drawLeftAlignedButton(20, y, 400, kSettingsRowHeight - 4, label,
                              index == 2 && config.cameraEnabled && !cameraAvailable
                                  ? color(103, 67, 36)
                                  : color(39, 54, 82));
    }
    drawVerticalScrollbar(450, kSettingsRowY,
                          kVisibleSettings * kSettingsRowHeight - 4,
                          kTotalSettings, kVisibleSettings, settingsScroll);
    if (!lastMessage.isEmpty()) {
        display.fillRoundRect(30, 253, 420, 24, 7, color(72, 48, 24));
        display.setTextColor(color(255, 205, 112));
        drawFittedText(lastMessage, 38, 254, 404, 22, readableUiSize(1.5f),
                       lgfx::textdatum_t::middle_center);
    }
    drawButton(115, 280, 250, 35,
               text("Powrót", "Back", "Volver", "Zurück"),
               color(45, 101, 191));
}

void drawWifiSetup() {
    display.fillScreen(color(10, 17, 33));
    display.setTextColor(color(246, 249, 255));
    drawFittedText(text("Połączenie z siecią Wi-Fi",
                        "Connect to Wi-Fi",
                        "Conexión Wi-Fi",
                        "WLAN-Verbindung"),
                        24, 2, 432, 26, readableUiSize(2.0f),
                   lgfx::textdatum_t::middle_center);
    display.setTextColor(wifiStatusMessage.isEmpty() ? color(174, 190, 215)
                                                     : color(255, 205, 112));
    const String status = wifiStatusMessage.isEmpty()
                              ? text("Wybierz sieć z listy",
                                     "Select a network",
                                     "Seleccione una red",
                                     "Netzwerk auswählen")
                              : wifiStatusMessage;
    drawFittedText(status, 22, 29, 436, 34, readableUiSize(1.6f),
                   lgfx::textdatum_t::middle_center);

    const size_t visibleCount =
        min(kVisibleWifiNetworks, wifiNetworks.size() > wifiNetworkScroll
                                     ? wifiNetworks.size() - wifiNetworkScroll
                                     : size_t{0});
    for (size_t row = 0; row < visibleCount; ++row) {
        const WifiNetwork& network = wifiNetworks[wifiNetworkScroll + row];
        const int y = kWifiNetworkRowY + static_cast<int>(row) *
                                             kWifiNetworkRowHeight;
        display.fillRoundRect(115, y, 250, kWifiNetworkRowHeight - 4, 8,
                              color(39, 54, 82));
        display.setTextColor(color(250, 252, 255));
        String name = network.ssid;
        if (name.length() > 30) name = name.substring(0, 27) + "...";
        drawFittedText(name, 121, y + 2, 238, 24, readableUiSize(2.0f),
                       lgfx::textdatum_t::middle_left);
        display.setTextColor(color(174, 190, 215));
        drawFittedText(
            String(network.rssi) + " dBm  " +
                (network.open ? text("Otwarta", "Open", "Abierta", "Offen")
                              : text("Zabezpieczona", "Secured",
                                     "Protegida", "Gesichert")),
            121, y + 25, 238, 17, readableUiSize(1.45f),
            lgfx::textdatum_t::middle_left);
    }
    if (wifiNetworks.empty() && !wifiScanInProgress) {
        display.setTextColor(color(174, 190, 215));
        drawFittedText(text("Nie znaleziono sieci",
                            "No networks found",
                            "No se encontraron redes",
                            "Keine Netzwerke gefunden"),
                       24, 122, 412, 42, readableUiSize(1.8f),
                       lgfx::textdatum_t::middle_center);
    }
    drawVerticalScrollbar(
        450, kWifiNetworkRowY, kVisibleWifiNetworks * kWifiNetworkRowHeight - 4,
        wifiNetworks.size(), kVisibleWifiNetworks, wifiNetworkScroll);
    drawButton(30, 277, 195, 36,
               text("Szukaj ponownie", "Scan again",
                    "Buscar de nuevo", "Erneut suchen"),
               color(45, 101, 191));
    drawButton(255, 277, 195, 36,
               text("Powrót", "Back", "Volver", "Zurück"),
               color(103, 47, 58));
}

void drawWifiPassword() {
    static constexpr const char* letterRows[] = {
        "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"
    };
    static constexpr const char* numberRows[] = {
        "1234567890", "!@#$%^&*()", "-_=+[]{};:"
    };
    static constexpr const char* symbolRows[] = {
        ".,/?<>\\|~`", "'\"();:{}[]", "+-_*&^%$#@"
    };
    static constexpr int letterStarts[] = {20, 42, 86};
    static constexpr int letterLengths[] = {10, 9, 7};

    display.fillScreen(color(10, 17, 33));
    display.setTextColor(color(246, 249, 255));
    drawFittedText(text("Hasło do sieci Wi-Fi",
                        "Wi-Fi password",
                        "Contraseña Wi-Fi",
                        "WLAN-Passwort"),
                   24, 2, 432, 24, readableUiSize(2.0f),
                   lgfx::textdatum_t::middle_center);
    display.setTextColor(wifiStatusMessage.isEmpty() ? color(174, 190, 215)
                                                     : color(255, 205, 112));
    drawFittedText(wifiStatusMessage.isEmpty() ? selectedWifiSsid
                                               : wifiStatusMessage,
                   22, 25, 436, 24, readableUiSize(1.6f),
                   lgfx::textdatum_t::middle_center);

    display.fillRoundRect(20, 51, 440, 35, 7, color(22, 32, 54));
    display.drawRoundRect(20, 51, 440, 35, 7, color(75, 92, 121));
    display.setTextColor(color(250, 252, 255));
    String masked;
    const size_t visiblePasswordLength =
        wifiPasswordInput.length() < 36 ? wifiPasswordInput.length() : 36;
    for (size_t index = 0; index < visiblePasswordLength; ++index) {
        masked += '*';
    }
    drawFittedText(masked, 28, 53, 420, 31, 1.8f,
                   lgfx::textdatum_t::middle_left);

    for (int row = 0; row < 3; ++row) {
        const char* keys = wifiKeyboardPage == 0
                               ? letterRows[row]
                               : wifiKeyboardPage == 1 ? numberRows[row]
                                                       : symbolRows[row];
        const int keyCount = wifiKeyboardPage == 0
                                 ? letterLengths[row]
                                 : static_cast<int>(strlen(keys));
        const int start = wifiKeyboardPage == 0 ? letterStarts[row] : 20;
        for (int index = 0; index < keyCount; ++index) {
            String key(keys[index]);
            if (wifiKeyboardPage == 0 && wifiPasswordLowercase) {
                key.toLowerCase();
            }
            drawButton(start + index * 44, 98 + row * 40, 40, 36, key,
                       color(39, 54, 82));
        }
    }
    drawButton(15, 222, 78, 30,
               wifiKeyboardPage == 0 ? "123"
                                     : wifiKeyboardPage == 1 ? "SYM" : "ABC",
               color(52, 73, 109));
    drawButton(100, 222, 68, 30, "a/A", color(52, 73, 109));
    drawButton(175, 222, 82, 30,
               text("Spacja", "Space", "Espacio", "Leer"),
               color(52, 73, 109));
    drawButton(264, 222, 105, 30,
               text("Usuń", "Delete", "Borrar", "Löschen"),
               color(103, 47, 58));
    drawButton(375, 222, 90, 30,
               text("Wyczyść", "Clear", "Vaciar", "Leeren"),
               color(103, 47, 58));
    drawButton(25, 270, 200, 39,
               wifiConnecting
                   ? text("Łączenie...", "Connecting...", "Conectando...",
                          "Verbinden...")
                   : text("Połącz", "Connect", "Conectar", "Verbinden"),
               wifiConnecting ? color(75, 92, 121) : color(25, 139, 103));
    drawButton(255, 270, 200, 39,
               text("Anuluj", "Cancel", "Cancelar", "Abbrechen"),
               color(103, 47, 58));
}

void finishWifiScan(int networkCount) {
    wifiScanInProgress = false;
    wifiNetworks.clear();
    if (networkCount < 0) {
        WiFi.scanDelete();
        wifiStatusMessage =
            text("Skanowanie sieci nie powiodło się",
                 "Network scan failed",
                 "Error al buscar redes",
                 "Netzwerksuche fehlgeschlagen");
        return;
    }
    for (int index = 0; index < networkCount; ++index) {
        const String ssid = WiFi.SSID(index);
        if (ssid.isEmpty()) continue;
        wifiNetworks.push_back(
            {ssid, WiFi.RSSI(index), WiFi.encryptionType(index) == WIFI_AUTH_OPEN});
    }
    WiFi.scanDelete();
    wifiStatusMessage = wifiNetworks.empty()
                            ? text("Nie znaleziono widocznych sieci",
                                   "No visible networks found",
                                   "No se encontraron redes visibles",
                                   "Keine sichtbaren Netzwerke gefunden")
                            : "";
}

void beginWifiScan() {
    WiFi.mode(WIFI_STA);
    WiFi.scanDelete();
    wifiNetworks.clear();
    wifiNetworkScroll = 0;
    wifiStatusMessage = text("Skanowanie sieci...",
                             "Scanning for networks...",
                             "Buscando redes...",
                             "Netzwerke werden gesucht...");
    const int result = WiFi.scanNetworks(true, true);
    if (result == WIFI_SCAN_RUNNING) {
        wifiScanInProgress = true;
    } else {
        finishWifiScan(result);
    }
}

void restoreWifiAfterSetup() {
    wifiConnecting = false;
    wifiConnectionStartedAt = 0;
    if (wifiScanInProgress) {
        WiFi.scanDelete();
        wifiScanInProgress = false;
    }
    if (wifiSetupRadioWasOn) {
        WiFi.mode(WIFI_STA);
        if (!config.ssid.isEmpty() &&
            (WiFi.status() != WL_CONNECTED || WiFi.SSID() != config.ssid)) {
            WiFi.disconnect(false);
            WiFi.begin(config.ssid.c_str(), config.wifiPassword.c_str());
        }
    } else {
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
    }
    wifiSetupActive = false;
}

void beginWifiConnection() {
    if (selectedWifiSsid.isEmpty() || selectedWifiSsid.length() > 32 ||
        wifiPasswordInput.length() > 63) {
        wifiStatusMessage =
            text("Nieprawidłowa nazwa sieci lub hasło",
                 "Invalid network name or password",
                 "Nombre de red o contraseña no válidos",
                 "Ungültiger Netzwerkname oder Passwort");
        drawWifiPassword();
        return;
    }
    const bool selectedOpenNetwork = [&]() {
        for (const WifiNetwork& network : wifiNetworks) {
            if (network.ssid == selectedWifiSsid) return network.open;
        }
        return false;
    }();
    if (!selectedOpenNetwork && wifiPasswordInput.length() < 8) {
        wifiStatusMessage =
            text("Hasło musi mieć co najmniej 8 znaków",
                 "Password must contain at least 8 characters",
                 "La contraseña debe tener al menos 8 caracteres",
                 "Das Passwort muss mindestens 8 Zeichen lang sein");
        drawWifiPassword();
        return;
    }

    WiFi.persistent(false);
    WiFi.disconnect(true);
    WiFi.mode(WIFI_STA);
    WiFi.begin(selectedWifiSsid.c_str(), wifiPasswordInput.c_str());
    wifiConnecting = true;
    wifiConnectionStartedAt = millis();
    wifiStatusMessage = text("Łączenie z siecią...",
                             "Connecting to network...",
                             "Conectando a la red...",
                             "Verbindung wird hergestellt...");
    drawWifiPassword();
}

void failWifiConnection(const char* messagePl, const char* messageEn,
                        const char* messageEs, const char* messageDe) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_STA);
    if (wifiSetupRadioWasOn) {
        if (!config.ssid.isEmpty()) {
            WiFi.begin(config.ssid.c_str(), config.wifiPassword.c_str());
        }
    }
    wifiConnecting = false;
    wifiConnectionStartedAt = 0;
    wifiStatusMessage = text(messagePl, messageEn, messageEs, messageDe);
    drawWifiPassword();
}

void updateWifiProgress() {
    if (screen == Screen::WifiSetup && wifiScanInProgress) {
        const int result = WiFi.scanComplete();
        if (result != WIFI_SCAN_RUNNING) {
            finishWifiScan(result);
            drawWifiSetup();
        }
    }
    if (screen != Screen::WifiPassword || !wifiConnecting) return;
    if (WiFi.status() == WL_CONNECTED && WiFi.SSID() == selectedWifiSsid) {
        const String previousSsid = config.ssid;
        const String previousPassword = config.wifiPassword;
        config.ssid = selectedWifiSsid;
        config.wifiPassword = wifiPasswordInput;
        if (!saveConfig()) {
            config.ssid = previousSsid;
            config.wifiPassword = previousPassword;
            failWifiConnection(
                "Połączono, ale nie zapisano konfiguracji na SD",
                "Connected, but could not save configuration to SD",
                "Conectado, pero no se pudo guardar en SD",
                "Verbunden, Konfiguration konnte aber nicht auf SD gespeichert werden");
            return;
        }
        configurationValid = true;
        wifiConnecting = false;
        wifiConnectionStartedAt = 0;
        wifiSetupActive = false;
        if (config.mode == Mode::Offline) {
            WiFi.disconnect(true);
            WiFi.mode(WIFI_OFF);
        } else {
            onlineEmployeesLoaded = false;
            nextNetworkAttemptAt = millis();
            nextEmployeeRefreshAt = 0;
        }
        lastMessage = text("Połączono i zapisano sieć",
                           "Connected; network saved",
                           "Conectado; red guardada",
                           "Verbunden; Netzwerk gespeichert");
        screen = Screen::Settings;
        drawSettings();
        return;
    }
    if (millis() - wifiConnectionStartedAt >= 20000) {
        failWifiConnection(
            "Nie udało się połączyć — sprawdź hasło",
            "Connection failed — check the password",
            "No se pudo conectar — compruebe la contraseña",
            "Verbindung fehlgeschlagen — Passwort prüfen");
    }
}

size_t activeEmployeeCount() {
    size_t count = 0;
    for (const auto& employee : employees) {
        if (employee.active) ++count;
    }
    return count;
}

int activeEmployeeIndex(size_t visibleIndex) {
    size_t current = 0;
    for (size_t index = 0; index < employees.size(); ++index) {
        if (!employees[index].active) continue;
        if (current == visibleIndex) return static_cast<int>(index);
        ++current;
    }
    return -1;
}

void drawEmployeeList() {
    constexpr size_t kRowsPerPage = 4;
    const size_t count = activeEmployeeCount();
    const size_t pageCount =
        (count + kRowsPerPage - 1) / kRowsPerPage > 0
            ? (count + kRowsPerPage - 1) / kRowsPerPage
            : 1;
    if (employeeListPage >= pageCount) employeeListPage = pageCount - 1;

    display.fillScreen(color(10, 17, 33));
    display.setTextColor(color(246, 249, 255));
    setUiTextSize(1.8f);
    display.drawCenterString(text("Pracownicy", "Employees",
                                  "Empleados", "Mitarbeiter"),
                             240, 5);
    display.setTextColor(color(174, 190, 215));
    setUiTextSize(1.1f);
    display.drawCenterString(String(employeeListPage + 1) + "/" +
                                 String(pageCount) + "  (" + String(count) + ")",
                             240, 29);

    if (count == 0) {
        display.setTextColor(color(174, 190, 215));
        setUiTextSize(1.6f);
        display.drawCenterString(text("Brak pracowników", "No employees",
                                      "No hay empleados", "Keine Mitarbeiter"),
                                 240, 128);
    }

    for (size_t row = 0; row < kRowsPerPage; ++row) {
        const int employeeIndex = activeEmployeeIndex(employeeListPage * kRowsPerPage + row);
        if (employeeIndex < 0) break;
        const Employee& employee = employees[employeeIndex];
        const int y = 47 + static_cast<int>(row) * 50;
        const uint16_t panelColor = color(22, 32, 54);
        display.fillRoundRect(16, y, 420, 46, 8, panelColor);
        display.setTextColor(color(246, 249, 255));
        drawFittedText(employee.firstName + " " + employee.lastName,
                       25, y + 2, 218, 22, 1.4f,
                       lgfx::textdatum_t::middle_left);
        display.setTextColor(color(174, 190, 215));
        drawFittedText("ID: " + employee.id, 25, y + 23, 218, 19, 1.2f,
                       lgfx::textdatum_t::middle_left);
        drawButton(254, y + 3, 116, 40,
                   text("Przekaźnik", "Relay", "Relé", "Relais"),
                   employee.relayAccess ? color(25, 139, 103) : color(150, 48, 58));
        drawButton(376, y + 3, 62, 40,
                   text("Usuń", "Remove", "Borrar", "Löschen"),
                   color(103, 47, 58));
    }
    drawVerticalScrollbar(450, 48, 196, count, kRowsPerPage,
                          employeeListPage * kRowsPerPage);
    if (!lastMessage.isEmpty()) {
        display.setTextColor(color(255, 205, 112));
        drawFittedText(lastMessage, 20, 248, 420, 17,
                       readableUiSize(1.5f),
                       lgfx::textdatum_t::middle_center);
    }

    drawButton(16, 267, 214, 43,
               text("Dodaj nowego", "Add employee",
                    "Añadir empleado", "Mitarbeiter hinzufügen"),
               color(25, 116, 88));
    drawButton(240, 267, 214, 43,
               text("-> Ustawienia", "-> Settings",
                    "-> Ajustes", "-> Einstellungen"),
               color(45, 101, 191));
}

void drawAddEmployeeCard() {
    display.fillScreen(color(10, 17, 33));
    display.setTextColor(color(246, 249, 255));
    drawFittedText(text("Dodawanie pracownika",
                        "Add employee", "Añadir empleado",
                        "Mitarbeiter hinzufügen"),
                   24, 48, 432, 44, readableUiSize(2.0f),
                   lgfx::textdatum_t::middle_center);
    if (lastMessage.isEmpty()) {
        display.setTextColor(color(174, 190, 215));
        drawFittedText(text("Przyłóż nową kartę",
                            "Scan the new card",
                            "Acerque la nueva tarjeta",
                            "Neue Karte vorhalten"),
                            24, 112, 432, 54, readableUiSize(1.8f),
                       lgfx::textdatum_t::middle_center);
    } else {
        display.fillRoundRect(20, 166, 440, 64, 10, color(104, 35, 45));
        display.setTextColor(color(255, 239, 240));
        drawFittedText(lastMessage, 32, 170, 416, 56,
                       readableUiSize(2.0f),
                       lgfx::textdatum_t::middle_center);
    }
    drawButton(170, 250, 140, 48,
               text("Anuluj", "Cancel", "Cancelar", "Abbrechen"),
               color(103, 47, 58));
}

bool solveTouchLeastSquares(const float rawX[4], const float rawY[4],
                            const int targetX[4], const int targetY[4],
                            TouchCalibration& result) {
    double normal[3][3] = {};
    double targetSums[2][3] = {};
    for (int i = 0; i < 4; ++i) {
        const double input[] = {rawX[i], rawY[i], 1.0};
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column) {
                normal[row][column] += input[row] * input[column];
            }
            targetSums[0][row] += input[row] * targetX[i];
            targetSums[1][row] += input[row] * targetY[i];
        }
    }

    for (int axis = 0; axis < 2; ++axis) {
        double matrix[3][4] = {};
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column) {
                matrix[row][column] = normal[row][column];
            }
            matrix[row][3] = targetSums[axis][row];
        }

        for (int column = 0; column < 3; ++column) {
            int pivot = column;
            for (int row = column + 1; row < 3; ++row) {
                if (fabs(matrix[row][column]) > fabs(matrix[pivot][column])) {
                    pivot = row;
                }
            }
            if (fabs(matrix[pivot][column]) < 1e-6) return false;
            if (pivot != column) {
                for (int j = column; j < 4; ++j) {
                    const double value = matrix[column][j];
                    matrix[column][j] = matrix[pivot][j];
                    matrix[pivot][j] = value;
                }
            }
            const double divisor = matrix[column][column];
            for (int j = column; j < 4; ++j) matrix[column][j] /= divisor;
            for (int row = 0; row < 3; ++row) {
                if (row == column) continue;
                const double factor = matrix[row][column];
                for (int j = column; j < 4; ++j) {
                    matrix[row][j] -= factor * matrix[column][j];
                }
            }
        }

        float* coefficients = axis == 0 ? result.x : result.y;
        for (int i = 0; i < 3; ++i) {
            coefficients[i] = static_cast<float>(matrix[i][3]);
            if (!isfinite(coefficients[i])) return false;
        }
    }

    if (fabs(result.x[0]) > 2.0f || fabs(result.x[1]) > 2.0f ||
        fabs(result.y[0]) > 2.0f || fabs(result.y[1]) > 2.0f ||
        fabs(result.x[2]) > 8000.0f || fabs(result.y[2]) > 8000.0f) {
        return false;
    }
    result.magic = kTouchCalibrationMagic;
    return true;
}

bool loadTouchCalibration() {
    TouchCalibration candidate;
    if (!preferences.isKey("touch_v1") ||
        preferences.getBytes("touch_v1", &candidate,
                             sizeof(candidate)) != sizeof(candidate) ||
        (candidate.magic != kTouchCalibrationMagic &&
         candidate.magic != kLegacyTouchCalibrationMagic)) {
        return false;
    }
    for (int i = 0; i < 3; ++i) {
        if (!isfinite(candidate.x[i]) || !isfinite(candidate.y[i])) {
            return false;
        }
    }
    if (candidate.magic == kLegacyTouchCalibrationMagic) {
        candidate.x[0] = -candidate.x[0];
        candidate.x[1] = -candidate.x[1];
        candidate.x[2] = static_cast<float>(kScreenWidth - 1) - candidate.x[2];
        candidate.y[0] = -candidate.y[0];
        candidate.y[1] = -candidate.y[1];
        candidate.y[2] = static_cast<float>(kScreenHeight - 1) - candidate.y[2];
        candidate.magic = kTouchCalibrationMagic;
        if (preferences.putBytes("touch_v1", &candidate, sizeof(candidate)) !=
            sizeof(candidate)) {
            Serial.println("Could not migrate touch calibration for 180-degree display rotation");
            return false;
        }
    }
    touchCalibration = candidate;
    return true;
}

bool captureCalibrationPoint(float& rawX, float& rawY) {
    constexpr uint32_t kPointTimeoutMs = 30000;
    const uint32_t started = millis();
    lgfx::touch_point_t raw;
    while (millis() - started < kPointTimeoutMs) {
        if (display.getTouchRaw(&raw)) break;
        delay(20);
    }
    if (millis() - started >= kPointTimeoutMs) return false;

    uint32_t sumX = 0;
    uint32_t sumY = 0;
    uint8_t samples = 0;
    while (samples < 12) {
        if (display.getTouchRaw(&raw)) {
            sumX += static_cast<uint16_t>(raw.x);
            sumY += static_cast<uint16_t>(raw.y);
            ++samples;
        } else if (samples > 0) {
            break;
        }
        delay(12);
    }
    while (display.getTouchRaw(&raw)) delay(20);
    if (samples < 4) return false;
    rawX = static_cast<float>(sumX) / samples;
    rawY = static_cast<float>(sumY) / samples;
    return true;
}

bool runTouchCalibration() {
    static constexpr int targetX[] = {
        kTouchCalibrationMargin, kScreenWidth - kTouchCalibrationMargin - 1,
        kScreenWidth - kTouchCalibrationMargin - 1, kTouchCalibrationMargin
    };
    static constexpr int targetY[] = {
        kTouchCalibrationMargin, kTouchCalibrationMargin,
        kScreenHeight - kTouchCalibrationMargin - 1,
        kScreenHeight - kTouchCalibrationMargin - 1
    };
    float rawX[4] = {};
    float rawY[4] = {};

    for (int i = 0; i < 4; ++i) {
        display.fillScreen(color(10, 17, 33));
        display.setTextColor(color(246, 249, 255));
        setUiTextSize(2);
        display.drawCenterString(
            text("Kalibracja dotyku", "Touch calibration",
                 "Calibración táctil", "Touch-Kalibrierung"),
            240, 62);
        setUiTextSize(1);
        display.setTextColor(color(174, 190, 215));
        display.drawCenterString(
            text("Dotknij i przytrzymaj każdy punkt",
                 "Touch and hold each target",
                 "Toque y mantenga cada punto",
                 "Jeden Punkt berühren und halten"),
            240, 104);
        display.drawCenterString(
            String(i + 1) + "/4",
            240, 126);
        display.drawCircle(targetX[i], targetY[i], 12, color(79, 146, 255));
        display.fillCircle(targetX[i], targetY[i], 3, color(255, 191, 87));

        if (!captureCalibrationPoint(rawX[i], rawY[i])) {
            Serial.println("Touch calibration timed out");
            return false;
        }
    }

    TouchCalibration candidate;
    if (!solveTouchLeastSquares(rawX, rawY, targetX, targetY, candidate)) {
        display.fillScreen(color(10, 17, 33));
        display.setTextColor(color(255, 150, 150));
        setUiTextSize(2);
        display.drawCenterString(
            text("Kalibracja nieudana", "Calibration failed",
                 "Calibración fallida", "Kalibrierung fehlgeschlagen"),
            240, 130);
        delay(1200);
        return false;
    }

    touchCalibration = candidate;
    if (preferences.putBytes("touch_v1", &touchCalibration,
                            sizeof(touchCalibration)) != sizeof(touchCalibration)) {
        Serial.println("Could not save touch calibration");
    }
    display.fillScreen(color(10, 17, 33));
    display.setTextColor(color(103, 220, 163));
    setUiTextSize(2);
    display.drawCenterString(
        text("Kalibracja zapisana", "Calibration saved",
             "Calibración guardada", "Kalibrierung gespeichert"),
        240, 145);
    delay(700);
    return true;
}

bool getCalibratedTouch(int32_t& x, int32_t& y) {
    if (touchCalibration.magic != kTouchCalibrationMagic) {
        return display.getTouch(&x, &y);
    }
    lgfx::touch_point_t raw;
    if (!display.getTouchRaw(&raw)) return false;
    x = lroundf(touchCalibration.x[0] * raw.x +
                touchCalibration.x[1] * raw.y + touchCalibration.x[2]);
    y = lroundf(touchCalibration.y[0] * raw.x +
                touchCalibration.y[1] * raw.y + touchCalibration.y[2]);
    x = constrain(x, 0, static_cast<int32_t>(kScreenWidth - 1));
    y = constrain(y, 0, static_cast<int32_t>(kScreenHeight - 1));
    return true;
}

int daysInMonth(int year, int month) {
    static constexpr int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))) {
        return 29;
    }
    return days[month - 1];
}

void drawClockSettings() {
    display.fillScreen(color(10, 17, 33));
    display.setTextColor(color(246, 249, 255));
    setUiTextSize(2);
    display.drawCenterString(text("Ustaw datę i godzinę", "Set date and time",
                                  "Ajustar fecha y hora", "Datum und Uhrzeit einstellen"),
                             240, 20);
    const DateTime now = rtc.now();
    const String values[] = {
        String(text("Godzina: ", "Hour: ", "Hora: ", "Stunde: ")) + String(now.hour()),
        String(text("Minuta: ", "Minute: ", "Minuto: ", "Minute: ")) + String(now.minute()),
        String(text("Dzień: ", "Day: ", "Día: ", "Tag: ")) + String(now.day()),
        String(text("Miesiąc: ", "Month: ", "Mes: ", "Monat: ")) + String(now.month()),
        String(text("Rok: ", "Year: ", "Año: ", "Jahr: ")) + String(now.year()),
    };
    for (int row = 0; row < 5; ++row) {
        const int y = 58 + row * 42;
        setUiTextSize(2);
        display.setTextColor(color(221, 230, 246));
        display.drawString(values[row], 64, y + 8);
        drawButton(300, y, 60, 38, "-", color(39, 54, 82));
        drawButton(372, y, 60, 38, "+", color(39, 54, 82));
    }
    drawButton(188, 274, 104, 38,
               text("Gotowe", "Done", "Listo", "Fertig"), color(45, 101, 191));
}

Employee* findEmployee(const String& uid) {
    for (auto& employee : employees) {
        if (employee.active && employee.uid == uid) return &employee;
    }
    return nullptr;
}

void finishEmployeeRegistration(const Employee& employee) {
    for (size_t index = employees.size(); index > 0; --index) {
        const Employee& existing = employees[index - 1];
        if (existing.id.equalsIgnoreCase(employee.id) ||
            existing.uid.equalsIgnoreCase(employee.uid)) {
            employees.erase(employees.begin() + index - 1);
        }
    }
    employees.push_back(employee);
    if (config.mode == Mode::Online) onlineEmployeesLoaded = true;
    addLog("INFO", "EMPLOYEE_REGISTERED", employee.id + ":" + employee.uid);
    lastMessage = text("Dodano: ", "Added: ", "Añadido: ", "Hinzugefügt: ") +
                  employee.firstName + " " + employee.lastName;
    const bool returnToList = employeeManagementReturnToList;
    employeeManagementReturnToList = false;
    pendingEmployeeUid = "";
    screen = returnToList ? Screen::EmployeeList : Screen::Home;
    if (returnToList) drawEmployeeList();
    else drawHome();
}

bool saveNewEmployee() {
    if (pendingEmployeeRegistrationRequestId != 0) return false;
    newEmployeeId.trim();
    newEmployeeFirstName.trim();
    newEmployeeLastName.trim();
    if (newEmployeeId.isEmpty() || newEmployeeFirstName.isEmpty() ||
        newEmployeeLastName.isEmpty() || pendingEmployeeUid.isEmpty()) {
        lastMessage = text("Uzupełnij ID, imię i nazwisko",
                           "Enter ID, first and last name",
                           "Introduzca ID, nombre y apellido",
                           "ID, Vor- und Nachname eingeben");
        drawEmployeeEnrollment();
        return false;
    }
    if (newEmployeeId.length() > 32 || newEmployeeFirstName.length() > 32 ||
        newEmployeeLastName.length() > 32) {
        lastMessage = text("Każde pole może mieć maks. 32 znaki",
                           "Each field is limited to 32 characters",
                           "Cada campo admite 32 caracteres",
                           "Jedes Feld darf höchstens 32 Zeichen haben");
        drawEmployeeEnrollment();
        return false;
    }

    if (config.mode == Mode::Offline) {
        const auto previousEmployees = employees;
        const auto previousAttendanceStates = offlineAttendanceStates;
        const bool previousOfflineLoaded = offlineEmployeesLoaded;
        if (!loadEmployeesFromCsv()) {
            employees = previousEmployees;
            offlineAttendanceStates = previousAttendanceStates;
            offlineEmployeesLoaded = previousOfflineLoaded;
            lastMessage = text("Nie można odświeżyć employees.csv z karty SD",
                               "Could not refresh employees.csv from SD card",
                               "No se pudo actualizar employees.csv desde SD",
                               "employees.csv konnte nicht von SD geladen werden");
            drawEmployeeEnrollment();
            return false;
        }
        for (const auto& existing : employees) {
            if (existing.id.equalsIgnoreCase(newEmployeeId)) {
                lastMessage = text("To ID jest już przypisane do pracownika",
                                   "This ID is already assigned to an employee",
                                   "Este ID ya está asignado a un empleado",
                                   "Diese ID ist bereits einem Mitarbeiter zugeordnet");
                drawEmployeeEnrollment();
                return false;
            }
            if (existing.uid.equalsIgnoreCase(pendingEmployeeUid)) {
                lastMessage = text("Ta karta jest już przypisana do pracownika",
                                   "This card is already assigned to an employee",
                                   "Esta tarjeta ya está asignada a un empleado",
                                   "Diese Karte ist bereits einem Mitarbeiter zugeordnet");
                drawEmployeeEnrollment();
                return false;
            }
        }
    }
    if (config.mode == Mode::Offline && employees.size() >= kMaximumEmployees) {
        lastMessage = text("Osiągnięto limit pracowników",
                           "Employee limit reached",
                           "Límite de empleados alcanzado",
                           "Mitarbeiterlimit erreicht");
        drawEmployeeEnrollment();
        return false;
    }

    Employee employee;
    employee.id = newEmployeeId;
    employee.uid = pendingEmployeeUid;
    employee.firstName = newEmployeeFirstName;
    employee.lastName = newEmployeeLastName;
    employee.active = true;
    employee.relayAccess = newEmployeeRelayAccess;

    if (config.mode == Mode::Online) {
        JsonDocument request;
        request["action"] = "registerEmployee";
        JsonObject row = request["row"].to<JsonObject>();
        row["employee_id"] = employee.id;
        row["card_uid"] = employee.uid;
        row["first_name"] = employee.firstName;
        row["last_name"] = employee.lastName;
        row["active"] = true;
        row["relay_access"] = employee.relayAccess;
        pendingEmployeeRegistration = employee;
        pendingEmployeeRegistrationRequestId = nextNetworkRequestId();
        employeeRegistrationRequestStartedAt = millis();
        String serializedRequest;
        if (!enqueueAsyncAppsScriptRequest(request,
                                           pendingEmployeeRegistrationRequestId,
                                           serializedRequest)) {
            pendingEmployeeRegistrationRequestId = 0;
            employeeRegistrationRequestStartedAt = 0;
            pendingEmployeeRegistration = Employee{};
            lastMessage = text("Nie można wysłać pracownika do API",
                               "Could not queue employee registration",
                               "No se pudo enviar el registro",
                               "Mitarbeiterregistrierung konnte nicht gesendet werden");
            drawEmployeeEnrollment();
            return false;
        }
        lastMessage = text("Zapisywanie pracownika...",
                           "Saving employee...",
                           "Guardando empleado...",
                           "Mitarbeiter wird gespeichert...");
        drawEmployeeEnrollment();
        return true;
    } else {
        if (!sdAvailable || !offlineEmployeesLoaded) {
            lastMessage = text("Brak gotowego pliku employees.csv",
                               "employees.csv is unavailable",
                               "employees.csv no está disponible",
                               "employees.csv ist nicht verfügbar");
            drawEmployeeEnrollment();
            return false;
        }
        constexpr char temporaryPath[] = "/employees.csv.tmp";
        constexpr char backupPath[] = "/employees.csv.bak";
        SD.remove(temporaryPath);
        SD.remove(backupPath);
        File file = SD.open(temporaryPath, FILE_WRITE);
        if (!file) {
            lastMessage = text("Nie można zapisać employees.csv",
                               "Could not write employees.csv",
                               "No se pudo escribir employees.csv",
                               "employees.csv konnte nicht geschrieben werden");
            drawEmployeeEnrollment();
            return false;
        }
        bool writeOk =
            file.println("employee_id,card_uid,first_name,last_name,active,relay_access") > 0;
        auto writeEmployee = [&file, &writeOk](const Employee& row) {
            if (!writeOk) return;
            String csvRow = csvEscape(row.id) + "," + csvEscape(row.uid) + "," +
                            csvEscape(row.firstName) + "," + csvEscape(row.lastName) +
                            "," + (row.active ? "true" : "false") + "," +
                            (row.relayAccess ? "true" : "false") + "\n";
            writeOk = file.print(csvRow) == csvRow.length();
        };
        for (const auto& existing : employees) {
            writeEmployee(existing);
        }
        writeEmployee(employee);
        file.flush();
        file.close();
        if (!writeOk) {
            SD.remove(temporaryPath);
            lastMessage = text("Błąd zapisu employees.csv",
                               "Failed to write employees.csv",
                               "Error al escribir employees.csv",
                               "Fehler beim Schreiben von employees.csv");
            drawEmployeeEnrollment();
            return false;
        }
        const bool hadOriginal = SD.exists(kEmployeesPath);
        if (hadOriginal && !SD.rename(kEmployeesPath, backupPath)) {
            SD.remove(temporaryPath);
            lastMessage = text("Nie można zabezpieczyć employees.csv",
                               "Could not back up employees.csv",
                               "No se pudo respaldar employees.csv",
                               "employees.csv konnte nicht gesichert werden");
            drawEmployeeEnrollment();
            return false;
        }
        if (!SD.rename(temporaryPath, kEmployeesPath)) {
            if (hadOriginal) SD.rename(backupPath, kEmployeesPath);
            SD.remove(temporaryPath);
            lastMessage = text("Nie można zatwierdzić employees.csv",
                               "Could not replace employees.csv",
                               "No se pudo reemplazar employees.csv",
                               "employees.csv konnte nicht ersetzt werden");
            drawEmployeeEnrollment();
            return false;
        }
        SD.remove(backupPath);
    }

    finishEmployeeRegistration(employee);
    return true;
}

void handleCard() {
    if (config.mode == Mode::Online &&
        (!onlineEmployeesLoaded || !pendingOnlineEventId.isEmpty())) return;
    if (config.mode == Mode::Online && WiFi.status() == WL_CONNECTED &&
        hasPendingOnlineAttendance()) {
        lastMessage = text("Wysyłanie tymczasowych odbić — poczekaj",
                           "Syncing temporary punches — please wait",
                           "Enviando registros — espere",
                           "Temporäre Buchungen werden gesendet — bitte warten");
        if (pendingQueueSyncFailed) {
            lastMessage = text("Odbicia oczekują na wysłanie — sprawdź połączenie",
                               "Punches are pending — check the connection",
                               "Registros pendientes — revise la conexión",
                               "Buchungen warten — Verbindung prüfen");
        }
        if (screen == Screen::Home) drawHome();
        return;
    }
    if (!rfidAvailable || !rfid.PICC_IsNewCardPresent() ||
        !rfid.PICC_ReadCardSerial()) return;
    String uid;
    for (byte index = 0; index < rfid.uid.size; ++index) {
        if (rfid.uid.uidByte[index] < 0x10) uid += '0';
        uid += String(rfid.uid.uidByte[index], HEX);
    }
    uid.toUpperCase();
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();

    const uint32_t now = millis();
    if (uid == lastCardUid && now - lastCardAt < kRfidDebounceMs) return;
    lastCardUid = uid;
    lastCardAt = now;

    if (!configurationValid ||
        (config.mode == Mode::Offline && !offlineEmployeesLoaded) ||
        (config.mode == Mode::Online && !onlineEmployeesLoaded)) {
        lastMessage = text("Brak konfiguracji lub listy pracowników",
                           "Configuration or employee list unavailable",
                           "Configuración o empleados no disponibles",
                           "Konfiguration oder Mitarbeiterliste fehlt");
        drawHome();
        return;
    }

    if (screen == Screen::AddEmployeeCard) {
        if (config.mode == Mode::Offline) {
            const auto previousEmployees = employees;
            const auto previousAttendanceStates = offlineAttendanceStates;
            const bool previousOfflineLoaded = offlineEmployeesLoaded;
            if (!loadEmployeesFromCsv()) {
                employees = previousEmployees;
                offlineAttendanceStates = previousAttendanceStates;
                offlineEmployeesLoaded = previousOfflineLoaded;
                lastMessage = text("Nie można odświeżyć employees.csv z karty SD",
                                   "Could not refresh employees.csv from SD card",
                                   "No se pudo actualizar employees.csv desde SD",
                                   "employees.csv konnte nicht von SD geladen werden");
                drawAddEmployeeCard();
                return;
            }
        }
        Employee* employee = config.mode == Mode::Online ? nullptr : findEmployee(uid);
        if (employee != nullptr) {
            lastMessage = text("Ta karta jest już przypisana do pracownika",
                               "This card is already assigned to an employee",
                               "Esta tarjeta ya está asignada a un empleado",
                               "Diese Karte ist bereits einem Mitarbeiter zugeordnet");
            drawAddEmployeeCard();
            return;
        }
        lastMessage = "";
        pendingEmployeeUid = uid;
        employeeEnrollmentRequested = true;
        pinInput = "";
        screenSince = millis();
        screen = Screen::PinEntry;
        drawPinEntry();
        return;
    }
    Employee* employee = findEmployee(uid);
    if (employee == nullptr) {
        pendingEmployeeUid = uid;
        employeeEnrollmentRequested = false;
        screenSince = millis();
        screen = Screen::UnknownCard;
        drawUnknownCard();
        Serial.printf("Unknown RFID card: %s\n", uid.c_str());
        return;
    }
    if (!rtcAvailable) {
        lastMessage = text("Brak zegara RTC — odbicie anulowane",
                           "RTC unavailable — punch rejected",
                           "RTC no disponible — registro rechazado",
                           "RTC nicht verfügbar — Buchung abgelehnt");
        addLog("ERROR", "RTC_UNAVAILABLE", uid);
        drawHome();
        return;
    }
    selectedEmployee = *employee;
    if (config.mode == Mode::Online && WiFi.status() != WL_CONNECTED) {
        outageActionChoice = true;
        screen = Screen::ActionChoice;
        drawActionChoice();
        return;
    }
    if (employee->relayAccess) {
        if (config.mode == Mode::Online) {
            if (!queueOnlineAttendanceState(selectedEmployee)) {
                lastMessage = text("Nie można sprawdzić statusu obecności",
                                   "Could not check attendance status",
                                   "No se pudo comprobar la asistencia",
                                   "Anwesenheitsstatus konnte nicht geprüft werden");
                screen = Screen::Home;
                drawHome();
            }
            return;
        }
        if (!sdAvailable || !offlineEmployeesLoaded) {
            lastMessage = text("Brak danych obecności na karcie SD",
                               "Attendance data unavailable on SD card",
                               "Datos de asistencia no disponibles en SD",
                               "Anwesenheitsdaten auf SD nicht verfügbar");
            drawHome();
            return;
        }
        selectedEmployeePresent =
            lastOfflineAttendance(employee->id) == "ENTRY";
        screen = Screen::ActionChoice;
        drawActionChoice();
        return;
    }

    if (config.mode == Mode::Online) {
        processAttendance(AttendanceAction::Entry, true);
        return;
    }
    if (!sdAvailable || !offlineEmployeesLoaded) {
        lastMessage = text("Brak karty SD — odbicie anulowane",
                           "SD unavailable — punch rejected",
                           "SD no disponible — registro rechazado",
                           "SD-Karte fehlt — Buchung abgelehnt");
        drawHome();
        return;
    }
    const String previous = lastOfflineAttendance(employee->id);
    processAttendance(previous == "ENTRY" ? AttendanceAction::Exit
                                          : AttendanceAction::Entry);
}

void handlePinTouch(int32_t x, int32_t y) {
    if (y >= 270 && x >= 188 && x <= 292) {
        screen = employeeManagementReturnToList ? Screen::EmployeeList : Screen::Home;
        pinInput = "";
        employeeEnrollmentRequested = false;
        pendingEmployeeUid = "";
        if (employeeManagementReturnToList) drawEmployeeList();
        else drawHome();
        return;
    }
    if (y >= 84 && y < 264) {
        const int column = (x - 84) / 106;
        const int row = (y - 84) / 45;
        if (column < 0 || column > 2 || row < 0 || row > 3) return;
        const int key = row * 3 + column;
        if (key == 9) {
            pinInput = "";
        } else if (key == 11) {
            if (pinInput.length() != 4) return;
            if (screen == Screen::PinChange) {
                pendingPin = pinInput;
                pinInput = "";
                screen = Screen::PinConfirm;
            } else if (screen == Screen::PinConfirm) {
                if (pinInput == pendingPin) {
                    storedPin = pendingPin;
                    preferences.putString("pin", storedPin);
                    addLog("INFO", "PIN_CHANGED", "");
                    screen = Screen::Settings;
                } else {
                    pinInput = "";
                    pendingPin = "";
                    screen = Screen::PinChange;
                    lastMessage = text("PIN nie pasuje — spróbuj ponownie",
                                       "PINs did not match — retry",
                                       "Los PIN no coinciden — inténtelo de nuevo",
                                       "PINs stimmen nicht überein");
                }
            } else if (pinInput == storedPin) {
                if (employeeEnrollmentRequested) {
                    employeeEnrollmentRequested = false;
                    newEmployeeId = "";
                    newEmployeeFirstName = "";
                    newEmployeeLastName = "";
                    newEmployeeRelayAccess = false;
                    enrollmentNumericKeyboard = false;
                    enrollmentField = 0;
                    enrollmentAccentIndex = 0;
                    enrollmentLowercase = false;
                    lastMessage = "";
                    screen = Screen::EmployeeEnroll;
                    screenSince = millis();
                    drawEmployeeEnrollment();
                } else {
                    screen = Screen::Settings;
                    drawSettings();
                }
                pinInput = "";
            } else {
                pinInput = "";
                lastMessage = text("Nieprawidłowy PIN", "Incorrect PIN",
                                   "PIN incorrecto", "Falsche PIN");
                addLog("WARN", "PIN_REJECTED", "");
            }
        } else if (pinInput.length() < 4) {
            const char* digits[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9",
                                    "", "0", ""};
            pinInput += digits[key];
        }
        if (screen == Screen::PinEntry || screen == Screen::PinChange ||
            screen == Screen::PinConfirm) {
            drawPinEntry();
        }
    }
}

void handleEmployeeEnrollmentTouch(int32_t x, int32_t y) {
    if (pendingEmployeeRegistrationRequestId != 0) return;
    if (y >= 298) {
        if (x >= 65 && x < 215) {
            saveNewEmployee();
        } else if (x >= 265 && x < 415) {
            pendingEmployeeUid = "";
            employeeEnrollmentRequested = false;
            screen = employeeManagementReturnToList ? Screen::EmployeeList : Screen::Home;
            lastMessage = "";
            if (employeeManagementReturnToList) drawEmployeeList();
            else drawHome();
        }
        return;
    }
    if (y >= 78 && y < 159) {
        lastMessage = "";
        enrollmentField = static_cast<uint8_t>((y - 78) / 27);
        enrollmentLowercase = enrollmentField != 0;
        screenSince = millis();
        drawEmployeeEnrollment();
        return;
    }
    if (y >= 161 && y < 184) {
        lastMessage = "";
        newEmployeeRelayAccess = !newEmployeeRelayAccess;
        screenSince = millis();
        drawEmployeeEnrollment();
        return;
    }
    if (y >= 184 && y < 271) {
        lastMessage = "";
        const int row = (y - 184) / 30;
        if (row < 0 || row > 2) return;
        const int rowStarts[] = {20, 42, 86};
        const int rowLengths[] = {10, 9, 7};
        const char* alphaRows[] = {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
        const char* numberRows[] = {"1234567890", "0987654321", ".,-_@"};
        const int numberLengths[] = {10, 10, 5};
        const int start = enrollmentNumericKeyboard ? 20 : rowStarts[row];
        const int keyIndex = (x - start) / 44;
        const int keyCount = enrollmentNumericKeyboard ? numberLengths[row]
                                                       : rowLengths[row];
        if (x < start || keyIndex < 0 || keyIndex >= keyCount) return;
        String key = enrollmentNumericKeyboard
                         ? String(numberRows[row][keyIndex])
                         : String(alphaRows[row][keyIndex]);
        if (!enrollmentNumericKeyboard && enrollmentLowercase) key.toLowerCase();
        String* value = enrollmentFieldValue(enrollmentField);
        if (value->length() < 32) *value += key;
        screenSince = millis();
        drawEmployeeEnrollment();
        return;
    }
    if (y >= 273 && y < 296) {
        lastMessage = "";
        String* value = enrollmentFieldValue(enrollmentField);
        if (x < 90) {
            enrollmentNumericKeyboard = !enrollmentNumericKeyboard;
        } else if (x < 165) {
            enrollmentLowercase = !enrollmentLowercase;
        } else if (x < 230) {
            const char* accent = enrollmentAccentCharacter();
            if (value->length() + strlen(accent) <= 32) *value += accent;
            enrollmentAccentIndex++;
        } else if (x < 340) {
            if (value->length() < 32) *value += ' ';
        } else {
            removeLastInputCharacter(*value);
        }
        screenSince = millis();
        drawEmployeeEnrollment();
    }
}

void switchMode() {
    const Mode previous = config.mode;
    config.mode = config.mode == Mode::Offline ? Mode::Online : Mode::Offline;
    if (!saveConfig()) {
        config.mode = previous;
        return;
    }
    employees.clear();
    onlineEmployeesLoaded = false;
    offlineEmployeesLoaded = false;
    onlineEmployeesLoaded = false;
    if (config.mode == Mode::Offline) {
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        loadEmployeesFromCsv();
        lastMessage = text("Tryb offline", "Offline mode", "Modo sin conexión",
                           "Offline-Modus");
    } else if (config.ssid.isEmpty()) {
        lastMessage = text("Brak konfiguracji Wi-Fi", "Wi-Fi is not configured",
                           "Wi-Fi no está configurado", "WLAN nicht konfiguriert");
    } else {
        WiFi.persistent(false);
        WiFi.mode(WIFI_STA);
        WiFi.begin(config.ssid.c_str(), config.wifiPassword.c_str());
        nextNetworkAttemptAt = millis();
        lastMessage = text("Łączenie z trybem online...",
                           "Connecting to online service...",
                           "Conectando al servicio en línea...",
                           "Verbindung zum Online-Dienst...");
    }
}

void setCameraEnabled(bool enabled) {
    if (enabled && !cameraAvailable) cameraAvailable = initCamera();
    if (!enabled && cameraAvailable) {
        const esp_err_t result = esp_camera_deinit();
        if (result != ESP_OK) {
            addLog("ERROR", "CAMERA_DEINIT_FAILED", String(result));
            lastMessage = text("Nie można wyłączyć aparatu",
                               "Could not stop camera",
                               "No se pudo detener la cámara",
                               "Kamera konnte nicht beendet werden");
            return;
        }
        cameraAvailable = false;
    }
    config.cameraEnabled = enabled;
    if (preferences.putBool("camera", enabled) == 0) {
        Serial.println("Could not persist camera setting in NVS");
        lastMessage = text("Nie zapisano ustawienia aparatu",
                           "Could not save camera setting",
                           "No se pudo guardar la cámara",
                           "Kameraeinstellung konnte nicht gespeichert werden");
        return;
    }
    if (sdAvailable && !saveConfig()) {
        Serial.println("Could not update camera_enabled in config.json");
        lastMessage = text("Nie zapisano config.json",
                           "Could not update config.json",
                           "No se pudo actualizar config.json",
                           "config.json konnte nicht aktualisiert werden");
    } else if (enabled && !cameraAvailable) {
        Serial.printf("Photo mode enabled, but camera init failed: 0x%x\n",
                      cameraInitError);
        addLog("ERROR", "CAMERA_INIT_FAILED",
               "esp_camera_init failed: 0x" +
                   String(static_cast<uint32_t>(cameraInitError), HEX));
        lastMessage = text("Tryb zdjęć ON, aparat niedostępny",
                           "Photos ON, camera unavailable",
                           "Fotos activadas, cámara no disponible",
                           "Fotos EIN, Kamera nicht verfügbar");
    } else {
        lastMessage = "";
    }
}

void adjustClock(int field, int delta) {
    if (!rtcAvailable) return;
    const DateTime current = rtc.now();
    int year = current.year();
    int month = current.month();
    int day = current.day();
    int hour = current.hour();
    int minute = current.minute();
    switch (field) {
        case 0:
            hour = (hour + delta + 24) % 24;
            break;
        case 1:
            minute = (minute + delta + 60) % 60;
            break;
        case 2:
            day = constrain(day + delta, 1, daysInMonth(year, month));
            break;
        case 3:
            month = constrain(month + delta, 1, 12);
            day = min(day, daysInMonth(year, month));
            break;
        case 4:
            year = constrain(year + delta, 2000, 2099);
            day = min(day, daysInMonth(year, month));
            break;
        default:
            return;
    }
    rtc.adjust(DateTime(year, month, day, hour, minute, current.second()));
}

void handleTouch() {
    int32_t x = 0;
    int32_t y = 0;
    static bool touchWasDown = false;
    if (!getCalibratedTouch(x, y)) {
        touchWasDown = false;
        return;
    }
    if (touchWasDown) return;
    touchWasDown = true;
    if (screen == Screen::Home) {
        if (x >= 340 && x < 460 && y >= 270 && y < 306) {
            screen = Screen::PinEntry;
            pinInput = "";
            screenSince = millis();
            drawPinEntry();
        }
    } else if (screen == Screen::UnknownCard) {
        if (y >= 215 && y < 277 && x < 240) {
            employeeEnrollmentRequested = true;
            pinInput = "";
            screenSince = millis();
            screen = Screen::PinEntry;
            drawPinEntry();
        } else if (y >= 215 && y < 277 && x >= 240) {
            pendingEmployeeUid = "";
            employeeEnrollmentRequested = false;
            lastMessage = "";
            screen = Screen::Home;
            drawHome();
        }
    } else if (screen == Screen::ActionChoice) {
        if (outageActionChoice) {
            if (y >= 154 && y < 226 && x >= 25 && x < 230) {
                processAttendance(AttendanceAction::Entry);
            } else if (y >= 154 && y < 226 && x >= 250 && x < 455) {
                processAttendance(AttendanceAction::Exit);
            } else if (selectedEmployee.relayAccess &&
                       y >= 236 && y < 290 && x >= 115 && x < 365) {
                outageActionChoice = false;
                setRelay(true);
                lastMessage = text("Drzwi odblokowane — brak połączenia",
                                   "Door unlocked — no connection",
                                   "Puerta abierta — sin conexión",
                                   "Tür geöffnet — keine Verbindung");
                screen = Screen::Home;
                drawHome();
            }
        } else if (selectedEmployeePresent) {
            if (y >= 132 && y < 224 && x >= 30 && x < 230) {
                processAttendance(AttendanceAction::Exit);
            } else if (y >= 132 && y < 224 && x >= 250 && x < 450) {
                setRelay(true);
                lastMessage = text("Drzwi odblokowane", "Door unlocked",
                                   "Puerta desbloqueada", "Tür entriegelt");
                screen = Screen::Home;
                drawHome();
            }
        } else if (y >= 132 && y < 224 && x >= 70 && x < 410) {
            processAttendance(AttendanceAction::Entry);
        }
    } else if (screen == Screen::PinEntry || screen == Screen::PinChange ||
               screen == Screen::PinConfirm) {
        handlePinTouch(x, y);
    } else if (screen == Screen::EmployeeEnroll) {
        handleEmployeeEnrollmentTouch(x, y);
    } else if (screen == Screen::WifiSetup) {
        constexpr int kWifiScrollHeight =
            kVisibleWifiNetworks * kWifiNetworkRowHeight - 4;
        if (y >= 277 && x >= 255 && x < 450) {
            restoreWifiAfterSetup();
            wifiStatusMessage = "";
            screen = Screen::Settings;
            lastMessage = "";
            drawSettings();
            return;
        }
        if (x >= 448 && x < 476 && y >= kWifiNetworkRowY &&
            y < kWifiNetworkRowY + kWifiScrollHeight) {
            wifiNetworkScroll = scrollbarOffsetForTouch(
                y, kWifiNetworkRowY, kWifiScrollHeight, wifiNetworks.size(),
                kVisibleWifiNetworks, wifiNetworkScroll);
            drawWifiSetup();
            return;
        }
        if (y >= 277 && x >= 30 && x < 225) {
            beginWifiScan();
            drawWifiSetup();
            return;
        }
        if (x >= 115 && x < 365 && y >= kWifiNetworkRowY &&
            y < kWifiNetworkRowY +
                    kVisibleWifiNetworks * kWifiNetworkRowHeight) {
            const size_t row =
                static_cast<size_t>((y - kWifiNetworkRowY) /
                                    kWifiNetworkRowHeight);
            const size_t networkIndex = wifiNetworkScroll + row;
            if (networkIndex < wifiNetworks.size()) {
                selectedWifiSsid = wifiNetworks[networkIndex].ssid;
                wifiPasswordInput =
                    !wifiNetworks[networkIndex].open &&
                            selectedWifiSsid == config.ssid
                        ? config.wifiPassword
                        : "";
                wifiPasswordLowercase = false;
                wifiKeyboardPage = 0;
                wifiStatusMessage = "";
                screen = Screen::WifiPassword;
                drawWifiPassword();
            }
        }
    } else if (screen == Screen::WifiPassword) {
        static constexpr const char* letterRows[] = {
            "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"
        };
        static constexpr const char* numberRows[] = {
            "1234567890", "!@#$%^&*()", "-_=+[]{};:"
        };
        static constexpr const char* symbolRows[] = {
            ".,/?<>\\|~`", "'\"();:{}[]", "+-_*&^%$#@"
        };
        static constexpr int letterStarts[] = {20, 42, 86};
        static constexpr int letterLengths[] = {10, 9, 7};
        if (y >= 270 && x >= 255 && x < 455) {
            if (wifiConnecting) {
                WiFi.disconnect(true);
                WiFi.mode(WIFI_STA);
                if (wifiSetupRadioWasOn && !config.ssid.isEmpty()) {
                    WiFi.begin(config.ssid.c_str(), config.wifiPassword.c_str());
                }
                wifiConnecting = false;
                wifiConnectionStartedAt = 0;
            }
            screen = Screen::WifiSetup;
            wifiStatusMessage = "";
            drawWifiSetup();
            return;
        }
        if (y >= 270 && x >= 25 && x < 225) {
            if (!wifiConnecting) beginWifiConnection();
            return;
        }
        if (y >= 98 && y < 218 && !wifiConnecting) {
            const int row = (y - 98) / 40;
            if (row < 0 || row >= 3) return;
            const char* keys = wifiKeyboardPage == 0
                                   ? letterRows[row]
                                   : wifiKeyboardPage == 1 ? numberRows[row]
                                                           : symbolRows[row];
            const int keyCount = wifiKeyboardPage == 0
                                     ? letterLengths[row]
                                     : static_cast<int>(strlen(keys));
            const int start = wifiKeyboardPage == 0 ? letterStarts[row] : 20;
            const int keyIndex = (x - start) / 44;
            if (x < start || keyIndex < 0 || keyIndex >= keyCount ||
                (x - start) % 44 >= 40) {
                return;
            }
            if (wifiPasswordInput.length() < 63) {
                String key(keys[keyIndex]);
                if (wifiKeyboardPage == 0 && wifiPasswordLowercase) {
                    key.toLowerCase();
                }
                wifiPasswordInput += key;
            }
            drawWifiPassword();
            return;
        }
        if (y >= 222 && y < 252 && !wifiConnecting) {
            if (x >= 15 && x < 93) {
                wifiKeyboardPage = (wifiKeyboardPage + 1) % 3;
            } else if (x >= 100 && x < 168 && wifiKeyboardPage == 0) {
                wifiPasswordLowercase = !wifiPasswordLowercase;
            } else if (x >= 175 && x < 257 &&
                       wifiPasswordInput.length() < 63) {
                wifiPasswordInput += ' ';
            } else if (x >= 264 && x < 370) {
                removeLastInputCharacter(wifiPasswordInput);
            } else if (x >= 375 && x < 465) {
                wifiPasswordInput = "";
            }
            drawWifiPassword();
        }
    } else if (screen == Screen::Settings) {
        if (y >= 280 && x >= 115 && x < 365) {
            employeeManagementReturnToList = false;
            screen = Screen::Home;
            drawHome();
            return;
        }
        constexpr int kSettingsScrollHeight =
            kVisibleSettings * kSettingsRowHeight - 4;
        if (x >= 448 && x < 476 && y >= kSettingsRowY &&
            y < kSettingsRowY + kSettingsScrollHeight) {
            settingsScroll = static_cast<uint8_t>(scrollbarOffsetForTouch(
                y, kSettingsRowY, kSettingsScrollHeight, kTotalSettings,
                kVisibleSettings, settingsScroll));
            drawSettings();
            return;
        }
        if (x >= 20 && x < 420 && y >= kSettingsRowY &&
            y < kSettingsRowY + kVisibleSettings * kSettingsRowHeight) {
            const size_t settingIndex =
                settingsScroll +
                static_cast<size_t>((y - kSettingsRowY) / kSettingsRowHeight);
            switch (settingIndex) {
                case 0:
                    config.language = static_cast<Language>(
                        (static_cast<uint8_t>(config.language) + 1) % 4);
                    preferences.putUChar("language",
                                         static_cast<uint8_t>(config.language));
                    break;
                case 1:
                    switchMode();
                    break;
                case 2:
                    setCameraEnabled(!config.cameraEnabled);
                    break;
                case 3:
                    uiFont = static_cast<UiFont>(
                        (static_cast<uint8_t>(uiFont) + 1) % kUiFontCount);
                    applyUiFont();
                    if (preferences.putUChar("font",
                                             static_cast<uint8_t>(uiFont)) == 0) {
                        lastMessage = text("Nie zapisano czcionki",
                                           "Could not save font",
                                           "No se pudo guardar la fuente",
                                           "Schriftart konnte nicht gespeichert werden");
                    } else {
                        lastMessage =
                            uiFont == UiFont::Unicode
                                ? ""
                                : text("Ta czcionka może nie wyświetlać polskich znaków",
                                       "This font may not display accented characters",
                                       "Esta fuente puede omitir caracteres acentuados",
                                       "Diese Schrift zeigt möglicherweise keine Umlaute");
                    }
                    break;
                case 4:
                    if (preferences.putBool("invert", !displayInverted) == 0) {
                        lastMessage =
                            text("Nie zapisano trybu kolorów",
                                 "Could not save color mode",
                                 "No se pudo guardar el modo de color",
                                 "Farbmodus konnte nicht gespeichert werden");
                        break;
                    }
                    displayInverted = !displayInverted;
                    display.invertDisplay(displayInverted);
                    lastMessage = "";
                    break;
                case 5:
                    employeeListPage = 0;
                    lastMessage = "";
                    screen = Screen::EmployeeList;
                    drawEmployeeList();
                    return;
                case 6:
                    screen = Screen::ClockSet;
                    drawClockSettings();
                    return;
                case 7:
                    screen = Screen::PinChange;
                    pinInput = "";
                    screenSince = millis();
                    drawPinEntry();
                    return;
                case 8:
                    runTouchCalibration();
                    screen = Screen::Settings;
                    drawSettings();
                    return;
                case 9:
                    wifiSetupRadioWasOn = WiFi.getMode() != WIFI_OFF;
                    wifiStatusMessage = "";
                    wifiNetworks.clear();
                    wifiNetworkScroll = 0;
                    wifiSetupActive = true;
                    beginWifiScan();
                    screen = Screen::WifiSetup;
                    drawWifiSetup();
                    return;
                case 10: {
                    if (!wallpaperEnabled && wallpaperPixels == nullptr &&
                        !loadWallpaper()) {
                        lastMessage = text("Brak poprawnego pliku WALLPAPER.RGB565",
                                           "WALLPAPER.RGB565 is missing or invalid",
                                           "WALLPAPER.RGB565 falta o no es válido",
                                           "WALLPAPER.RGB565 fehlt oder ist ungültig");
                        break;
                    }
                    const bool enabled = !wallpaperEnabled;
                    if (preferences.putBool("wallpaper", enabled) == 0) {
                        lastMessage = text("Nie zapisano ustawienia tapety",
                                           "Could not save wallpaper setting",
                                           "No se pudo guardar el fondo",
                                           "Hintergrund-Einstellung konnte nicht "
                                           "gespeichert werden");
                        break;
                    }
                    wallpaperEnabled = enabled;
                    lastMessage = "";
                    break;
                }
                default:
                    return;
            }
            drawSettings();
        }
    } else if (screen == Screen::EmployeeList) {
        constexpr size_t kRowsPerPage = 4;
        const size_t count = activeEmployeeCount();
        const size_t pageCount =
            (count + kRowsPerPage - 1) / kRowsPerPage > 0
                ? (count + kRowsPerPage - 1) / kRowsPerPage
                : 1;
        if (x >= 448 && x < 476 && y >= 48 && y < 244) {
            employeeListPage = static_cast<uint16_t>(scrollbarOffsetForTouch(
                y, 48, 196, count, kRowsPerPage, employeeListPage));
        } else if (y >= 47 && y < 247) {
            const size_t row = static_cast<size_t>((y - 47) / 50);
            const int employeeIndex =
                activeEmployeeIndex(employeeListPage * kRowsPerPage + row);
            if (employeeIndex >= 0) {
                const Employee employee = employees[employeeIndex];
                if (x >= 254 && x < 374) {
                    updateEmployeeRecord(static_cast<size_t>(employeeIndex),
                                         employee.active, !employee.relayAccess);
                } else if (x >= 376 && x < 440) {
                    updateEmployeeRecord(static_cast<size_t>(employeeIndex),
                                         false, employee.relayAccess);
                }
            }
        } else if (y >= 267 && y < 312 && x >= 16 && x < 230) {
            employeeManagementReturnToList = true;
            employeeEnrollmentRequested = false;
            lastMessage = "";
            screenSince = millis();
            screen = Screen::AddEmployeeCard;
            drawAddEmployeeCard();
            return;
        } else if (y >= 267 && y < 312 && x >= 240 && x < 460) {
            employeeManagementReturnToList = false;
            lastMessage = "";
            screen = Screen::Settings;
            drawSettings();
            return;
        }
        drawEmployeeList();
    } else if (screen == Screen::AddEmployeeCard) {
        if (y >= 250 && y < 300 && x >= 170 && x < 310) {
            employeeManagementReturnToList = false;
            lastMessage = "";
            screen = Screen::EmployeeList;
            drawEmployeeList();
        }
    } else if (screen == Screen::ClockSet) {
        if (y >= 274) {
            screen = Screen::Settings;
            drawSettings();
        } else if (x >= 300 && x < 432 && y >= 58 && y < 268) {
            const int row = (y - 58) / 42;
            adjustClock(row, x >= 372 ? 1 : -1);
            drawClockSettings();
        }
    }
    delay(180);
}

bool initDisplay() {
    pinMode(pins::rfid_rst, OUTPUT);
    digitalWrite(pins::rfid_rst, LOW);
    delay(20);
    digitalWrite(pins::rfid_rst, HIGH);
    delay(20);
    if (!display.init()) return false;
    display.setColorDepth(16);
    applyUiFont();
    display.setRotation(3);
    display.fillScreen(color(10, 17, 33));
    display.setTextColor(color(245, 248, 255));
    setUiTextSize(3);
    display.drawCenterString("RCP2027", 240, 136);
    setUiTextSize(1);
    display.setTextColor(color(145, 160, 185));
    display.drawCenterString("Inicjalizacja terminala", 240, 180);
    return true;
}

void setupRcp() {
    Serial.begin(115200);
    const bool displayReady = initDisplay();
    if (!displayReady) {
        Serial.println("ILI9488 initialization failed");
    }

    preferences.begin("rcp", false);
    networkWorkerReady = initializeNetworkWorker();
    storedPin = preferences.getString("pin", "0000");
    pendingOnlineEventId = preferences.getString("pending_event", "");
    pendingOnlineEmployeeId = preferences.getString("pend_emp", "");
    pendingOnlineEmployeeName = preferences.getString("pending_name", "");
    pendingAttendanceRequestBody = preferences.getString("pending_payload", "");
    if (pendingOnlineEventId.isEmpty()) {
        preferences.remove("pend_emp");
        preferences.remove("pending_name");
        preferences.remove("pending_payload");
    }
    const bool savedLanguage = preferences.isKey("language");
    const bool savedCamera = preferences.isKey("camera");
    displayInverted = preferences.getBool("invert", false);
    wallpaperEnabled = preferences.getBool("wallpaper", true);
    if (displayReady) display.invertDisplay(displayInverted);
    const uint8_t savedFont = preferences.getUChar("font", 0);
    if (savedFont < kUiFontCount) {
        uiFont = static_cast<UiFont>(savedFont);
    } else {
        preferences.remove("font");
    }
    applyUiFont();
    if (loadTouchCalibration()) {
        Serial.println("Loaded touchscreen calibration from NVS");
    }

    Wire.begin(pins::i2c_sda, pins::i2c_scl);
    rtcAvailable = rtc.begin(&Wire);
    if (rtcAvailable && rtc.lostPower()) {
        rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
        Serial.println("RTC lost power; set to firmware build time");
    }

    pinMode(pins::relay, OUTPUT_OPEN_DRAIN);
    digitalWrite(pins::relay, HIGH);
    esp_timer_create_args_t relayTimerConfig = {};
    relayTimerConfig.callback = relayTimerCallback;
    relayTimerConfig.name = "rcp-relay-close";
    if (esp_timer_create(&relayTimerConfig, &relayTimer) != ESP_OK) {
        relayTimer = nullptr;
        Serial.println("Relay safety timer creation failed; using loop timeout");
    }
    pinMode(pins::buzzer, OUTPUT);
    digitalWrite(pins::buzzer, LOW);

    sdBus.begin(pins::tft_sck, pins::tft_miso, pins::tft_mosi, pins::sd_cs);
    sdAvailable = SD.begin(pins::sd_cs, sdBus, 20000000);
    if (sdAvailable) {
        loadWallpaper();
        const bool employeesFileReady = ensureCsvFile(
            kEmployeesPath,
            "employee_id,card_uid,first_name,last_name,active,relay_access");
        const bool attendanceFileReady = ensureCsvFile(
            kAttendancePath,
            "event_id,timestamp,employee_id,card_uid,event_type,photo_file,source,notes");
        const bool logsFileReady = ensureCsvFile(
            kLogsPath, "event_id,timestamp,level,code,message");
        const bool pendingAttendanceFileReady =
            ensureCsvFile(kPendingAttendancePath, kAttendanceHeader);
        if (!pendingAttendanceFileReady) {
            Serial.println("Could not initialize temporary online attendance queue");
        }
        sdFilesInitialized = employeesFileReady && attendanceFileReady && logsFileReady;

        if (!SD.exists(kConfigPath)) {
            config.mode = Mode::Offline;
            config.language = Language::Polish;
            configurationValid = saveConfig();
            if (configurationValid) {
                Serial.println("Created default offline config.json");
            } else {
                Serial.println("Could not create default offline config.json");
            }
        } else if (!loadConfig()) {
            addLog("ERROR", "CONFIG_INVALID", "Invalid config.json; refusing defaults");
        }
    } else {
        Serial.println("SD initialization failed");
    }
    if (savedLanguage) {
        const uint8_t saved = preferences.getUChar("language", 0);
        if (saved <= static_cast<uint8_t>(Language::German)) {
            config.language = static_cast<Language>(saved);
        } else {
            preferences.remove("language");
        }
    }
    if (savedCamera) config.cameraEnabled = preferences.getBool("camera", false);
    if (displayReady && touchCalibration.magic != kTouchCalibrationMagic) {
        runTouchCalibration();
    }

    SPI.begin(pins::rfid_sck, pins::rfid_miso, pins::rfid_mosi, pins::rfid_cs);
    rfid.PCD_Init();
    const byte rfidVersion = rfid.PCD_ReadRegister(MFRC522::VersionReg);
    rfidAvailable = rfidVersion != 0x00 && rfidVersion != 0xFF;
    Serial.printf("MFRC522 VersionReg=0x%02X\n", rfidVersion);
    if (rfidAvailable) {
        rfid.PCD_SetAntennaGain(rfid.RxGain_max);
    } else {
        Serial.println("MFRC522 did not respond over SPI; check power, reset and SPI pins");
    }

    if (config.cameraEnabled) {
        cameraAvailable = initCamera();
        if (!cameraAvailable) {
            addLog("ERROR", "CAMERA_INIT_FAILED",
                   "esp_camera_init failed: 0x" +
                       String(static_cast<uint32_t>(cameraInitError), HEX));
        }
    }
    if (configurationValid && config.mode == Mode::Online && !config.ssid.isEmpty()) {
        WiFi.persistent(false);
        WiFi.mode(WIFI_STA);
        WiFi.begin(config.ssid.c_str(), config.wifiPassword.c_str());
        nextNetworkAttemptAt = millis();
    } else if (configurationValid && config.mode == Mode::Offline) {
        loadEmployeesFromCsv();
    }

    tone(pins::buzzer, 2500, 300);
    delay(300);
    noTone(pins::buzzer);
    if (!configurationValid) {
        lastMessage = text("Brak poprawnej konfiguracji SD",
                           "Invalid or missing SD configuration",
                           "Configuración SD ausente o inválida",
                           "SD-Konfiguration fehlt oder ungültig");
    } else if (!rtcAvailable) {
        lastMessage = text("Brak zegara RTC", "RTC unavailable",
                           "RTC no disponible", "RTC nicht verfügbar");
    } else if (!rfidAvailable) {
        lastMessage = text("Błąd czytnika RFID — sprawdź połączenia",
                           "RFID reader error — check wiring",
                           "Error RFID — revise las conexiones",
                           "RFID-Fehler — Verkabelung prüfen");
    } else if (sdAvailable && !sdFilesInitialized) {
        lastMessage = text("Błąd plików SD — sprawdź kartę",
                           "SD files unavailable — check card",
                           "Error en archivos SD — revise tarjeta",
                           "SD-Dateien fehlen — Karte prüfen");
    } else if (configurationValid && config.mode == Mode::Offline &&
               !offlineEmployeesLoaded) {
        lastMessage = text("Brak poprawnego employees.csv",
                           "Missing or invalid employees.csv",
                           "employees.csv ausente o inválido",
                           "employees.csv fehlt oder ungültig");
    } else if (config.mode == Mode::Online && !pendingOnlineEventId.isEmpty()) {
        lastMessage = text("Wynik odbicia nieznany — trwa weryfikacja",
                           "Punch result unknown — checking",
                           "Resultado desconocido — verificando",
                           "Buchung unbekannt — Prüfung läuft");
    } else if (config.mode == Mode::Online && !networkWorkerReady) {
        lastMessage = text("Błąd zadania sieciowego — uruchom ponownie",
                           "Network task unavailable — restart device",
                           "Tarea de red no disponible — reinicie",
                           "Netzwerkaufgabe nicht verfügbar — neu starten");
    } else if (config.mode == Mode::Online && WiFi.status() != WL_CONNECTED) {
        lastMessage = text("BRAK POŁĄCZENIA Wi-Fi",
                           "NO WI-Fi CONNECTION",
                           "SIN CONEXIÓN Wi-Fi",
                           "KEINE Wi-Fi-VERBINDUNG");
    } else if (config.mode == Mode::Online && !onlineEmployeesLoaded) {
        lastMessage = text("Pobieranie listy pracowników...",
                           "Loading employee list...",
                           "Cargando lista de empleados...",
                           "Mitarbeiterliste wird geladen...");
    } else {
        lastMessage = "";
    }
    screen = Screen::Home;
    drawHome();
    Serial.printf("RCP2027 ready: display=%s sd=%s rtc=%s rfid=%s employees=%u\n",
                  displayReady ? "ok" : "error", sdAvailable ? "ok" : "error",
                  rtcAvailable ? "ok" : "error", rfidAvailable ? "ok" : "error",
                  static_cast<unsigned>(employees.size()));
}

void expireNetworkRequests() {
    constexpr uint32_t kRequestDeadlineMs = kHttpTimeoutMs + 5000;
    const uint32_t now = millis();
    if (employeeRefreshRequestId != 0 &&
        now - employeeRefreshRequestStartedAt > kRequestDeadlineMs) {
        Serial.println("Employee refresh result timed out");
        employeeRefreshRequestId = 0;
        employeeRefreshRequestStartedAt = 0;
        nextNetworkAttemptAt = now;
    }
    if (attendanceStateRequestId != 0 &&
        now - attendanceStateRequestStartedAt > kRequestDeadlineMs) {
        Serial.println("Attendance state check timed out");
        attendanceStateRequestId = 0;
        attendanceStateRequestStartedAt = 0;
        lastMessage = text("Nie można sprawdzić statusu obecności",
                           "Could not check attendance status",
                           "No se pudo comprobar la asistencia",
                           "Anwesenheitsstatus konnte nicht geprüft werden");
        screen = Screen::Home;
        drawHome();
    }
    if (pendingEmployeeUpdateRequestId != 0 &&
        now - employeeUpdateRequestStartedAt > kRequestDeadlineMs) {
        Serial.println("Employee update result timed out");
        pendingEmployeeUpdateRequestId = 0;
        employeeUpdateRequestStartedAt = 0;
        lastMessage =
            text("Wynik aktualizacji nieznany — odśwież listę",
                 "Update result unknown — refresh the list",
                 "Resultado desconocido — actualice la lista",
                 "Ergebnis unbekannt — Liste aktualisieren");
        if (screen == Screen::EmployeeList) drawEmployeeList();
    }
    if (pendingEmployeeRegistrationRequestId != 0 &&
        now - employeeRegistrationRequestStartedAt > kRequestDeadlineMs) {
        Serial.println("Employee registration result timed out");
        pendingEmployeeRegistrationRequestId = 0;
        employeeRegistrationRequestStartedAt = 0;
        lastMessage =
            text("Wynik nieznany — ponów zapis, jeśli pracownik nie jest na liście",
                 "Result unknown — retry if employee is not listed",
                 "Resultado desconocido — reintente si no aparece",
                 "Ergebnis unbekannt — erneut speichern, falls nicht gelistet");
        if (screen == Screen::EmployeeEnroll) drawEmployeeEnrollment();
    }
    if (pendingQueueUploadRequestId != 0 &&
        now - pendingQueueUploadStartedAt > kRequestDeadlineMs) {
        Serial.println("Temporary attendance upload timed out; keeping queue row");
        pendingQueueUploadRequestId = 0;
        pendingQueueUploadStartedAt = 0;
        pendingQueueUploadEventId = "";
        pendingQueueSyncFailed = true;
        lastMessage = text("Wysyłanie odbić nie powiodło się — kolejka zachowana",
                           "Punch sync failed — queue retained",
                           "Error al sincronizar — cola conservada",
                           "Synchronisierung fehlgeschlagen — Warteschlange bleibt");
        if (screen == Screen::Home) drawHome();
        nextQueueUploadAttemptAt = now + 10000;
    }
    if (pendingAttendanceRequestId != 0 &&
        now - pendingAttendanceRequestStartedAt > kRequestDeadlineMs) {
        Serial.println("Attendance write result timed out; checking event ID");
        pendingAttendanceRequestId = 0;
        pendingAttendanceRequestStartedAt = 0;
        nextPendingStatusAttemptAt = now;
    }
    if (pendingStatusRequestId != 0 &&
        now - pendingStatusRequestStartedAt > kRequestDeadlineMs) {
        Serial.println("Attendance status result timed out");
        pendingStatusRequestId = 0;
        pendingStatusRequestStartedAt = 0;
        pendingStatusCheckInFlight = false;
        nextPendingStatusAttemptAt = now + 1000;
    }
}

void loopRcp() {
    expireNetworkRequests();
    handleAsyncNetworkResults();
    updateWifiProgress();
    handleTouch();
    if (screen == Screen::Home || screen == Screen::AddEmployeeCard) handleCard();

    if (screen == Screen::AttendanceConfirmation &&
        static_cast<int32_t>(millis() - attendanceConfirmationUntil) >= 0) {
        screen = Screen::Home;
        attendanceConfirmationTitle = "";
        attendanceConfirmationName = "";
        if (pendingOnlineEventId.isEmpty()) {
            lastMessage = "";
        } else {
            lastMessage = text("Wynik odbicia nieznany — trwa weryfikacja",
                               "Punch result unknown — checking",
                               "Resultado desconocido — verificando",
                               "Buchung unbekannt — Prüfung läuft");
        }
        lastClockDraw = 0;
        drawHome();
    }

    if (relayAutoClosed) {
        relayAutoClosed = false;
        relayOffAt = 0;
        if (relayWasActive) {
            relayWasActive = false;
            addLog("INFO", "RELAY_UNLOCK_PULSE_COMPLETED", "");
        }
    } else if (relayOffAt != 0 &&
               static_cast<int32_t>(millis() - relayOffAt) >= 0) {
        setRelay(false);
    }

    if (!wifiSetupActive && configurationValid && config.mode == Mode::Online &&
        static_cast<int32_t>(millis() - nextNetworkAttemptAt) >= 0) {
        nextNetworkAttemptAt = millis() + 10000;
        if (WiFi.status() != WL_CONNECTED) {
            const String disconnectedMessage =
                text("BRAK POŁĄCZENIA Wi-Fi", "NO WI-Fi CONNECTION",
                     "SIN CONEXIÓN Wi-Fi", "KEINE Wi-Fi-VERBINDUNG");
            if (lastMessage.isEmpty()) {
                lastMessage = disconnectedMessage;
                if (screen == Screen::Home) drawHome();
            }
            WiFi.reconnect();
        } else if (!onlineEmployeesLoaded || millis() >= nextEmployeeRefreshAt) {
            const String disconnectedMessage =
                text("BRAK POŁĄCZENIA Wi-Fi", "NO WI-Fi CONNECTION",
                     "SIN CONEXIÓN Wi-Fi", "KEINE Wi-Fi-VERBINDUNG");
            if (lastMessage == disconnectedMessage && !hasPendingOnlineAttendance()) {
                lastMessage = "";
                if (screen == Screen::Home) drawHome();
            }
            nextEmployeeRefreshAt = millis() + kEmployeeRefreshMs;
            queueOnlineEmployeeRefresh();
        }
    }
    if (screen == Screen::Home && config.mode == Mode::Online &&
        WiFi.status() == WL_CONNECTED && hasPendingOnlineAttendance() &&
        pendingQueueUploadRequestId == 0) {
        startPendingOnlineAttendanceUpload();
    }

    if (screen == Screen::Home && millis() - lastClockDraw >= kClockRefreshMs) {
        lastClockDraw = millis();
        refreshHomeClock();
    }
    if ((screen == Screen::PinEntry || screen == Screen::PinChange ||
         screen == Screen::PinConfirm) &&
        millis() - screenSince > 30000) {
        screen = employeeManagementReturnToList ? Screen::EmployeeList : Screen::Home;
        pinInput = "";
        pendingPin = "";
        employeeEnrollmentRequested = false;
        pendingEmployeeUid = "";
        if (employeeManagementReturnToList) drawEmployeeList();
        else drawHome();
    }
    if (screen == Screen::EmployeeEnroll && millis() - screenSince > 90000) {
        screen = employeeManagementReturnToList ? Screen::EmployeeList : Screen::Home;
        pendingEmployeeUid = "";
        lastMessage = text("Rejestrację anulowano", "Registration timed out",
                           "Registro cancelado", "Registrierung abgelaufen");
        if (employeeManagementReturnToList) drawEmployeeList();
        else drawHome();
    }
    delay(12);
}

}  // namespace

void setup() {
    setupRcp();
}

void loop() {
    loopRcp();
}
