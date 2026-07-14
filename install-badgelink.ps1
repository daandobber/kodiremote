param(
    [switch]$NoBuild,
    [switch]$NoStart,
    [string]$Slug = "kodiremote",
    [string]$Title = "Kodi Remote",
    [string]$Device = "tanmatsu",
    [string]$EspIdfPath = "C:\espressif\esp-idf-v5.5.1",
    [string]$IdfToolsPath = "C:\espressif\esp-idf-tools",
    [string]$BadgeLinkDir = "C:\espressif\badgelink_v020"
)

$ErrorActionPreference = "Stop"

function Invoke-Step {
    param([string]$Title, [scriptblock]$Body)
    Write-Host ""
    Write-Host "==> $Title"
    & $Body
}

$repoRoot = $PSScriptRoot
Set-Location $repoRoot

if (-not $NoBuild) {
    Invoke-Step "Build firmware ($Device)" {
        $env:PYTHONIOENCODING = "utf-8"
        $env:PYTHONUTF8       = "1"
        $env:IDF_TOOLS_PATH   = $IdfToolsPath
        [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new()

        $exportScript = Join-Path $EspIdfPath "export.ps1"
        if (-not (Test-Path $exportScript)) {
            throw "ESP-IDF not found at $EspIdfPath. Pass -EspIdfPath if it lives somewhere else."
        }

        . $exportScript

        idf.py --no-ccache -B "build/$Device" build `
            -DDEVICE=$Device `
            -DSDKCONFIG_DEFAULTS="sdkconfigs/general;sdkconfigs/$Device" `
            -DSDKCONFIG="sdkconfig_$Device" `
            -DIDF_TARGET=esp32p4 `
            -DFAT=0

        if ($LASTEXITCODE -ne 0) {
            throw "Firmware build failed with exit code $LASTEXITCODE"
        }
    }
}

$firmwarePath = Join-Path $repoRoot "build\$Device\application.bin"
if (-not (Test-Path $firmwarePath)) {
    throw "Firmware binary not found at $firmwarePath. Build the app first (omit -NoBuild)."
}

$badgeLinkTools = Join-Path $BadgeLinkDir "tools"
if (-not (Test-Path (Join-Path $badgeLinkTools "badgelink.py"))) {
    throw "BadgeLink tooling not found at $badgeLinkTools. Pass -BadgeLinkDir if it lives somewhere else, or clone it: git clone https://github.com/badgeteam/esp32-component-badgelink.git $BadgeLinkDir"
}

$venvDir    = Join-Path $badgeLinkTools ".venv"
$venvPython = Join-Path $venvDir "Scripts\python.exe"

if (-not (Test-Path $venvPython)) {
    Invoke-Step "Create BadgeLink Python virtual environment" {
        Push-Location $badgeLinkTools
        try {
            python -m venv .venv
            & $venvPython -m pip install --upgrade pip | Out-Null
            & $venvPython -m pip install -r requirements.txt
        } finally {
            Pop-Location
        }
    }
}

# device.py expects libraries\libusb-1.0.dll next to itself on Windows, but
# neither the git repo nor the released tools.zip ship that binary.
$libusbDll = Join-Path $badgeLinkTools "libraries\libusb-1.0.dll"
if (-not (Test-Path $libusbDll)) {
    Invoke-Step "Fetch libusb-1.0.dll for BadgeLink's PyUSB backend" {
        & $venvPython -m pip install libusb | Out-Null
        $bundled = Join-Path $venvDir "Lib\site-packages\libusb\_platform\windows\x86_64\libusb-1.0.dll"
        if (-not (Test-Path $bundled)) {
            throw "libusb PyPI package did not provide $bundled"
        }
        Copy-Item $bundled $libusbDll
    }
}

$versionFile = Join-Path $repoRoot ".appfs_version"
$version = 0
if (Test-Path $versionFile) {
    $version = [int](Get-Content $versionFile -Raw).Trim()
}
$version = ($version + 1) % 65536
Set-Content -Path $versionFile -Value $version -NoNewline

Invoke-Step "Upload $Title ($Slug) v$version via BadgeLink" {
    Push-Location $badgeLinkTools
    try {
        & $venvPython badgelink.py appfs upload $Slug $Title $version $firmwarePath
        if ($LASTEXITCODE -ne 0) {
            throw "BadgeLink upload failed with exit code $LASTEXITCODE. Is the Tanmatsu connected over USB in device mode?"
        }
    } finally {
        Pop-Location
    }
}

if (-not $NoStart) {
    Invoke-Step "Start $Slug on the device" {
        Push-Location $badgeLinkTools
        try {
            & $venvPython badgelink.py start $Slug
        } finally {
            Pop-Location
        }
    }
}

Write-Host ""
Write-Host "Done. Installed '$Slug' v$version via BadgeLink."
