$ErrorActionPreference = "Stop"

$python = Join-Path $PSScriptRoot ".venv\Scripts\python.exe"
$entryPoint = Join-Path $PSScriptRoot "src\rcp2027_app\__main__.py"
$appsScript = Join-Path $PSScriptRoot "..\google_apps_script\Code.gs"

if (-not (Test-Path $python)) {
    throw "Desktop virtual environment not found. Create desktop\.venv and install the desktop package with its build extra."
}

if (-not (Test-Path $appsScript)) {
    throw "Google Apps Script source not found: $appsScript"
}

Push-Location $PSScriptRoot
try {
    & $python -m PyInstaller `
        --noconfirm `
        --clean `
        --onefile `
        --windowed `
        --name RCP2027 `
        --paths (Join-Path $PSScriptRoot "src") `
        --add-data "$appsScript;google_apps_script" `
        --distpath (Join-Path $PSScriptRoot "dist") `
        --workpath (Join-Path $PSScriptRoot "build") `
        $entryPoint
    if ($LASTEXITCODE -ne 0) {
        throw "PyInstaller failed with exit code $LASTEXITCODE."
    }
}
finally {
    Pop-Location
}
