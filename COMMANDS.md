# Commands

Run these from the repository root in PowerShell.

## One-time SDK setup

Only needed once per machine/checkout.

```powershell
git clone --recursive --branch v5.5.1 --depth=1 --shallow-submodules https://github.com/espressif/esp-idf.git esp-idf
$env:IDF_TOOLS_PATH = "$(Get-Location)\esp-idf-tools"
powershell -ExecutionPolicy Bypass -File .\esp-idf\install.ps1 all
```

## BadgeLink tooling setup

Only needed once, to be able to install the built firmware onto a Tanmatsu
over USB.

```powershell
git clone https://github.com/badgeteam/esp32-component-badgelink.git badgelink_v020
```

`install-badgelink.ps1` creates the required Python virtual environment
inside `badgelink_v020\tools` automatically on first run.

## Build Tanmatsu Firmware

```powershell
$env:PYTHONIOENCODING='utf-8'
$env:PYTHONUTF8='1'
$env:IDF_TOOLS_PATH="$(Get-Location)\esp-idf-tools"
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new()
. .\esp-idf\export.ps1
idf.py --no-ccache -B build/tanmatsu build -DDEVICE=tanmatsu -DSDKCONFIG_DEFAULTS='sdkconfigs/general;sdkconfigs/tanmatsu' -DSDKCONFIG=sdkconfig_tanmatsu -DIDF_TARGET=esp32p4 -DFAT=0
```

## Install With BadgeLink

Make sure the Tanmatsu is connected over USB and set to USB device mode
(press the purple diamond key, top right of the keyboard, until the screen
icon switches from bug to USB).

Build, install and start in one step:

```powershell
.\install-badgelink.ps1
```

Use this after a build already succeeded:

```powershell
.\install-badgelink.ps1 -NoBuild
```

Install without auto-start:

```powershell
.\install-badgelink.ps1 -NoBuild -NoStart
```

## Git

This checkout has no remote configured yet. Once you push it somewhere:

```powershell
git status -sb
git diff
git add path\to\file1 path\to\file2
git commit -m "feat: short description"
git push <remote> <branch>
```

Do not blindly stage all files; inspect `git status -sb` and stage explicit
paths only.
