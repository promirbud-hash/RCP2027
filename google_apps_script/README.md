# Instalacja API Google Apps Script

1. Utwórz trzy arkusze Google. W pierwszym wierszu pierwszej zakładki każdego
   arkusza wstaw dokładny nagłówek z `../docs/ARCHITECTURE.md`.
2. W `script.google.com` utwórz projekt Apps Script i wklej zawartość `Code.gs`.
3. W ustawieniach projektu dodaj Script Properties:
   - `RCP_API_TOKEN` — wygeneruj go przyciskiem **Generuj token** w aplikacji.
   - `EMPLOYEES_SHEET_ID` — ID arkusza pracowników.
   - `ATTENDANCE_SHEET_ID` — ID arkusza ewidencji wejść/wyjść.
   - `LOGS_SHEET_ID` — ID arkusza logów.

   ID to część linku pomiędzy `/spreadsheets/d/` i kolejnym `/`.
4. Wybierz **Deploy → New deployment → Web app**, ustaw wykonywanie jako
   właściciel i dostęp dla wszystkich. Dostęp publiczny jest potrzebny, bo
   ESP32 nie wykonuje interaktywnego logowania Google; każda operacja API
   weryfikuje jednak prywatny token.
5. Skopiuj URL wdrożenia `/exec` do aplikacji. Wprowadź te same trzy linki
   arkuszy i token, zapisz konfigurację SD, a następnie użyj **Testuj API**.
   Test sprawdza token, nagłówki, dostęp projektu Apps Script do arkuszy oraz
   zgodność ID linków ze Script Properties.
6. Po zmianie kodu Apps Script utwórz nową wersję wdrożenia i ponownie sprawdź
   API z aplikacji.

Terminal online może również dopisywać nowych pracowników przez chronioną
tokenem operację `registerEmployee`. Skrypt sprawdza unikalność ID pracownika
i UID karty pod blokadą przed zapisem do arkusza.

Operacje `attendance` i `punch` są idempotentne względem `event_id`. Ponowienie
żądania z tym samym ID zwraca istniejące zdarzenie, zamiast dopisać drugi wiersz
lub przełączyć wejście na wyjście. Operacja `eventStatus` pozwala sprawdzić, czy
zdarzenie o podanym ID znajduje się już w arkuszu. Po timeoutcie terminal
pokazuje wynik jako nieznany, zapisuje ID w NVS i automatycznie pyta API o
wynik; do jego rozstrzygnięcia nie przyjmuje kolejnego odbicia.

Edytor arkuszy wykrywa zmiany wykonane przez API lub bezpośrednio w arkuszu.
Jeśli od pobrania danych ktoś dopisał albo zmienił rekord, zapis pełnej tabeli
zostanie odrzucony i trzeba odświeżyć dane przed ponowną edycją. Nadal zalecane
jest ograniczenie ręcznej edycji arkuszy do uprawnionych osób.

Operacja zastąpienia tabeli przyjmuje maksymalnie 20 000 rekordów. Kierunek
następnego odbicia jest buforowany w Script Properties i aktualizowany przez
API. Bezpośrednia ręczna zmiana arkusza ewidencji może więc pozostawić ten
bufor nieaktualny; po takiej zmianie odśwież ewidencję w aplikacji i zapisz ją
z powrotem przez API, aby bufor został odbudowany.

Token i hasło Wi-Fi są zapisane jawnie na karcie SD, więc karta i kopie
konfiguracji zawierają sekrety. Ogranicz dostęp do nich, nie wysyłaj ich w
zgłoszeniach i unieważnij token po utracie karty.

Żądania firmware'u wykonuje osobne zadanie sieciowe; odbicia i logi trafiają do
kolejki, a zadanie sieciowe nie korzysta ze współdzielonej magistrali SPI
ekranu i karty SD. Operacje API w aplikacji desktopowej wykonują się w wątku
`QThread`, a wynik jest przekazywany do GUI sygnałami Qt.
