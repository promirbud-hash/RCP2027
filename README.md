# RCP2027 PROMIR-BUD PASSION LINE

System składa się z terminala na ESP32-S3 N16R8 z ekranem dotykowym ILI9488
3,5" (320 × 480) oraz desktopowej aplikacji PySide6 do przygotowania karty SD,
zarządzania pracownikami i przeglądania/korygowania zdarzeń.

Projekt zawiera firmware terminala, aplikację desktopową i integrację z Google
Sheets. Terminal obsługuje cztery języki: polski, angielski, hiszpański i
niemiecki.

## Elementy systemu

- `src/`, `include/`, `boards/`, `platformio.ini` — główny projekt PlatformIO
  firmware: LovyanGFX, ekran i dotyk, MFRC522, RTC
  DS3231, karta SD, opcjonalna kamera OV2640, przekaźnik, PIN i tryby pracy.
- `desktop/` — edytor konfiguracji SD, pracowników, ewidencji oraz logów.
- `google_apps_script/Code.gs` — pośredniczące, autoryzowane API do odczytu i
  zapisu Arkuszy Google.
- `data/` — wzorcowy plik konfiguracji i nagłówki CSV.
- `docs/ARCHITECTURE.md` — formaty danych, pinout, konfiguracja API i granice
  obecnej implementacji.

## Funkcje terminala

- Rejestracja wejść i wyjść kartą RFID z automatycznym rozpoznaniem kolejnego
  zdarzenia; ekran potwierdza zapis imieniem i nazwiskiem.
- Tryb offline odtwarza ostatni stan pracownika z historii raz przy starcie i
  potwierdza odbicie dopiero po pełnym zapisie wiersza CSV. W trybie online
  timeout jest pokazywany jako wynik nieznany; terminal sprawdza ten sam ID
  zdarzenia i wstrzymuje następne odbicia, aby nie odwrócić stanu przez
  powtórzenie skanu.
- Gdy w trybie online zabraknie połączenia, terminal informuje o tym i prosi
  pracownika o wybór wejścia lub wyjścia. Zdarzenie trafia do
  `/attendance_pending.csv` na SD i jest wysyłane do Arkusza Google po
  odzyskaniu sieci; terminal zachowuje kolejność i nie usuwa wpisu przed
  potwierdzeniem przez API. Wymaga to dostępnej karty SD.
- Karty z uprawnieniem przekaźnika pozwalają zarejestrować wejście, gdy
  pracownik jest poza zakładem; po wejściu umożliwiają wybór wyjścia albo
  krótkiego odblokowania drzwi.
- Nieznaną kartę można dodać na terminalu po potwierdzeniu i podaniu PIN-u.
- Ustawienia chronione PIN-em: język, tryb pracy, zdjęcia, wybór jednego z
  siedmiu krojów pisma (Unicode, Sans/Serif/Mono regular lub pogrubione), jasny
  lub ciemny tryb kolorów, lista pracowników, data i godzina, zmiana PIN-u oraz
  kalibracja dotyku. Dostępna jest też konfiguracja Wi-Fi: wybór sieci,
  wpisanie hasła i zapis po udanym połączeniu do `config.json` na karcie SD.
- Lista pracowników pozwala zmienić uprawnienia przekaźnika i dezaktywować
  pracownika bez usuwania jego historii. Nowego pracownika można dodać po
  zeskanowaniu nowej karty.
- Ekran oczekiwania pokazuje godzinę, datę i losowe hasło motywujące: po jednym
  z zestawu 50 haseł na każdy język. Hasło zmienia się raz na dzień.
- Opcjonalna tapeta z pliku `/WALLPAPER.RGB565` jest ładowana do PSRAM i
  wyświetlana na ekranie głównym oraz potwierdzeniach wejścia/wyjścia; można
  ją włączać i wyłączać w ustawieniach.
- Kalibracja dotyku uruchamia się przy pierwszym starcie i może być ponowiona
  z ustawień; wynik jest zapisywany w pamięci trwałej.
- Logi zapisywane są w `/logs.csv` na SD także w trybie online. Online są
  dodatkowo wysyłane do skonfigurowanego arkusza.
- Zdjęcia, jeśli funkcja jest włączona i kamera dostępna, zapisywane są w
  katalogu `/photos` na karcie SD jako
  `nazwisko_imie_id_RRRRMMDD_GGMM.jpg`; przy kolizji nazwy dodawany jest
  identyfikator zdarzenia, aby zdjęcia nie zostały nadpisane.

## Aplikacja desktopowa

Python 3.10+:

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

Najpierw wybierz główny katalog karty SD (np. `E:\`) i użyj **Przygotuj kartę
SD**. Aplikacja zapisze `config.json`, utworzy brakujące pliki CSV z nagłówkami
oraz katalog `photos`; istniejące pliki CSV i rekordy pozostaną bez zmian.
Brakujące CSV są również tworzone automatycznie po wybraniu katalogu karty SD.
Tryb offline może rozpocząć pracę z pustą listą pracowników; dodaj pierwszą
kartę przy terminalu albo przez aplikację desktopową. Do odczytu ewidencji i
logów wskaż odpowiedni plik CSV na karcie SD; ewidencję możesz później ręcznie
skorygować i zapisać do wybranego pliku. W
trybie online przyciski odświeżania pobierają dane przez Apps Script; zapis
pracowników lub ewidencji do chmury wymaga
potwierdzenia, ponieważ zastępuje zawartość odpowiedniego arkusza.
Przycisk **Pomoc** pokazuje instrukcję tworzenia arkuszy, wymagane nagłówki,
procedurę wdrożenia i kod Apps Script z przyciskiem kopiowania. Żądania sieciowe
uruchamiane są w wątku `QThread`, więc okno pozostaje responsywne podczas
oczekiwania na API.

Konfiguracja karty zawiera również język, tryb pracy, ustawienia Wi-Fi, linki
do arkuszy i Apps Script oraz przełącznik zdjęć. Linki można sprawdzić w
aplikacji przed zapisaniem konfiguracji.

Zakładka **Tapeta** pozwala dopasować obraz do ekranu 480 × 320, obejrzeć
podgląd pól interfejsu dla ekranu głównego i potwierdzenia oraz zapisać na
karcie `WALLPAPER.RGB565`. Eksport zawiera także nagłówek `wallpaper_rgb565.h`
i szablon obszarów `wallpaper_layout.json`. Oznaczenia pól są podglądem
stałego układu firmware; nie zmieniają położenia elementów na ekranie.

## Pierwsze uruchomienie terminala

1. Zbuduj i wgraj firmware zgodnie z sekcją poniżej.
2. Włóż kartę SD przygotowaną aplikacją desktopową. W trybie offline można
   rozpocząć od pustej listy pracowników.
3. Przy pierwszym uruchomieniu dotknij kolejno czterech punktów kalibracji.
   Kalibrację można później powtórzyć w menu ustawień.
4. Dotknij **USTAWIENIA** na ekranie startowym i podaj domyślny PIN `0000`.
   Zmień go przed rozpoczęciem normalnej pracy.
5. Dodaj pracowników w aplikacji desktopowej albo przy terminalu. Aby dodać
   osobę przy urządzeniu, zeskanuj nieznaną kartę, wybierz **Tak**, podaj PIN
   i wypełnij ID, imię oraz nazwisko. Możesz też nadać dostęp do przekaźnika.
6. W trybie online zainstaluj Apps Script opisany poniżej, skonfiguruj arkusze
   i użyj w aplikacji desktopowej **Testuj API**.
7. Przyłóż kartę pracownika. Karta bez uprawnienia przekaźnika rejestruje
   automatycznie wejście albo wyjście; karta z tym uprawnieniem wyświetla
   wybór czynności.

Menu ustawień i dodawanie pracowników są chronione PIN-em. Lista pracowników
umożliwia zmianę dostępu do przekaźnika albo dezaktywację wpisu, która zachowuje
historię obecności.

## Przygotowanie Google Apps Script

Szczegółowa procedura i wymagane właściwości projektu są opisane w
[architekturze](./docs/ARCHITECTURE.md). W skrócie: wgraj
`google_apps_script/Code.gs` do projektu Apps Script, utwórz wymagane nagłówki
arkuszy, ustaw token i identyfikatory dokumentów w Script Properties, wdroż
web app jako właściciel i wklej URL `/exec` do konfiguratora. Następnie użyj
**Testuj API**.

Publiczne linki Arkuszy Google służą do identyfikacji arkuszy. Sam CSV/URL nie
umożliwia bezpiecznego dopisywania rekordów, dlatego terminal zapisuje przez
chronione tokenem API Apps Script. Hasło Wi-Fi i token są przechowywane jawnie
na karcie SD — zabezpiecz ją fizycznie.

## Budowanie firmware

Polecenia uruchamiaj z katalogu głównego repozytorium:

```powershell
py -m platformio run -e esp32-s3-devkitc-1-n16r8
py -m platformio run -e esp32-s3-devkitc-1-n16r8 -t upload --upload-port COMx
```

Zamknij monitor szeregowy przed wgrywaniem, aby zwolnić port COM. Pinout jest
zdefiniowany w [`include/pins.h`](./include/pins.h); szczegóły sprzętowe i
schematy danych opisuje [`docs/ARCHITECTURE.md`](./docs/ARCHITECTURE.md).

## Sprawdzenie zmian

```powershell
py -m unittest discover -s desktop\tests -v
py -m ruff check desktop\src desktop\tests
py -m platformio run -e esp32-s3-devkitc-1-n16r8
```

Kompilacja nie zastępuje testu na urządzeniu. Pierwsze uruchomienie wymaga
sprawdzenia dotyku, resetu GPIO3, magistral SPI dla SD/RFID/ekranu, RTC, kamery
i bezpiecznego sterowania rzeczywistym przekaźnikiem.

## Bezpieczeństwo i dane lokalne

Nie przechowuj w repozytorium prawdziwego `config.json`, tokenu Apps Script,
hasła Wi-Fi, prywatnych danych pracowników, logów urządzenia ani zdjęć. Plik
[`data/config.example.json`](./data/config.example.json) zawiera wyłącznie
puste wartości przykładowe. Rzeczywisty `config.json` zapisuj na karcie SD,
którą należy fizycznie zabezpieczyć. Zmień domyślny PIN `0000` przy pierwszym
uruchomieniu.
