<#
.SYNOPSIS
  Pipeline de release : compile tout, lance les tests, construit l'installateur (Setup) et, sur demande, publie la release GitHub.

.DESCRIPTION
  1. Lit la version dans le fichier VERSION (ou -Version, qui reecrit ce fichier).
  2. Configure et compile en Release dans build-release\ (couche OpenXR, tray, pont SimConnect, tests).
  3. Lance les tests unitaires.
  4. Verifie que tous les fichiers de l'installateur sont la.
  5. Compile installer\HandsSetup.iss avec Inno Setup 6 -> dist\HandsInteraction-Setup-<version>.exe (+ SHA256SUMS.txt).
  6. Avec -Publish : cree le tag v<version> (s'il n'existe pas), le pousse et cree la release GitHub avec le Setup (gh CLI).

  Prerequis : Visual Studio 2022 (C++), CMake >= 3.20, SDK SimConnect de MSFS (pour HandsBridge.exe), Inno Setup 6, et gh pour -Publish.
  Le SDK SimConnect n'est pas redistribuable : c'est pourquoi la release se construit sur votre machine et non sur un serveur d'integration.

.PARAMETER Version
  Numero de version X.Y.Z. Reecrit VERSION s'il differe.

.PARAMETER SimConnectSdk
  Dossier "SimConnect SDK" de MSFS (contient include\ et lib\static\). Defaut : C:\MSFS 2024 SDK\SimConnect SDK.

.PARAMETER SkipTests
  N'execute pas les tests unitaires.

.PARAMETER Publish
  Publie la release GitHub (tag + Setup en piece jointe).

.PARAMETER Prerelease
  Avec -Publish : marque la release comme pre-version.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File .\scripts\build-release.ps1
  powershell -ExecutionPolicy Bypass -File .\scripts\build-release.ps1 -Version 0.1.0 -Publish
#>
[CmdletBinding()]
param(
    [string]$Version,
    [string]$SimConnectSdk = 'C:\MSFS 2024 SDK\SimConnect SDK',
    [switch]$SkipTests,
    [switch]$Publish,
    [switch]$Prerelease
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

function Step($msg) { Write-Host ""; Write-Host "== $msg" -ForegroundColor Cyan }
function Run($exe, [string[]]$arguments) {
    & $exe @arguments
    if ($LASTEXITCODE -ne 0) { throw "$exe a echoue (code $LASTEXITCODE)." }
}

# --- Version -------------------------------------------------------------------------
$versionFile = Join-Path $root 'VERSION'
if ($Version) {
    if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw "Version invalide '$Version' (attendu : X.Y.Z)." }
    Set-Content -Path $versionFile -Value $Version -Encoding ascii
}
$Version = (Get-Content $versionFile -TotalCount 1).Trim()
if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw "VERSION invalide : '$Version'." }
Write-Host "Version : $Version"

# --- Outils --------------------------------------------------------------------------
Step 'Outils'
$cmake = (Get-Command cmake -ErrorAction SilentlyContinue)
if (-not $cmake) { throw 'CMake introuvable dans le PATH.' }
$iscc = $null
foreach ($c in @("${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe", "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe")) {
    if (Test-Path $c) { $iscc = $c; break }
}
if (-not $iscc) { $found = Get-Command ISCC.exe -ErrorAction SilentlyContinue; if ($found) { $iscc = $found.Source } }
if (-not $iscc) { throw 'Inno Setup 6 introuvable (https://jrsoftware.org/isdl.php).' }
if (-not (Test-Path (Join-Path $SimConnectSdk 'include\SimConnect.h'))) {
    throw "SDK SimConnect introuvable dans '$SimConnectSdk' (option -SimConnectSdk). Sans lui, HandsBridge.exe ne peut pas etre compile."
}
Write-Host "CMake : $($cmake.Source)"
Write-Host "Inno  : $iscc"
Write-Host "SDK   : $SimConnectSdk"

# --- Compilation ---------------------------------------------------------------------
$build = Join-Path $root 'build-release'
Step 'Configuration'
Run cmake @('-S', $root, '-B', $build, '-G', 'Visual Studio 17 2022', '-A', 'x64', "-DHANDS_SIMCONNECT_SDK=$SimConnectSdk")
Step 'Compilation (Release)'
Run cmake @('--build', $build, '--config', 'Release', '--parallel')

if (-not $SkipTests) {
    Step 'Tests unitaires'
    Run (Join-Path $build 'tests\Release\core_tests.exe') @()
}

# --- Verification des fichiers ---------------------------------------------------------
Step 'Verification des fichiers'
$required = @(
    'layer\Release\XR_APILAYER_HANDS_bare.dll', 'layer\Release\XR_APILAYER_HANDS_bare.json', 'layer\Release\hands.ini',
    'tray\Release\HandsTray.exe', 'tools\Release\HandsBridge.exe')
foreach ($f in $required) {
    $p = Join-Path $build $f
    if (-not (Test-Path $p)) { throw "Fichier manquant apres compilation : $f" }
    Write-Host ("  {0,-55} {1,10:N0} octets" -f $f, (Get-Item $p).Length)
}

# --- Installateur --------------------------------------------------------------------
Step 'Installateur (Inno Setup)'
$dist = Join-Path $root 'dist'
New-Item -ItemType Directory -Force -Path $dist | Out-Null
Run $iscc @("/DAppVersion=$Version", "/DBuildDir=$build", '/Qp', (Join-Path $root 'installer\HandsSetup.iss'))
$setup = Join-Path $dist "HandsInteraction-Setup-$Version.exe"
if (-not (Test-Path $setup)) { throw "Installateur non produit : $setup" }
$hash = (Get-FileHash $setup -Algorithm SHA256).Hash.ToLower()
Set-Content -Path (Join-Path $dist 'SHA256SUMS.txt') -Value "$hash  $(Split-Path -Leaf $setup)" -Encoding ascii
Write-Host ""
Write-Host "Setup  : $setup ($([math]::Round((Get-Item $setup).Length / 1MB, 1)) Mo)"
Write-Host "SHA256 : $hash"

# --- Publication ---------------------------------------------------------------------
if ($Publish) {
    Step 'Publication GitHub'
    $gh = Get-Command gh -ErrorAction SilentlyContinue
    if (-not $gh) { throw 'gh (GitHub CLI) introuvable : necessaire pour -Publish.' }
    if (git status --porcelain) { throw 'Des modifications ne sont pas commitees : commitez avant de publier.' }
    $tag = "v$Version"
    if (git tag --list $tag) { throw "Le tag $tag existe deja : incrementez la version." }
    $notes = Join-Path $root 'dist\release-notes.md'
    $changelog = Get-Content (Join-Path $root 'CHANGELOG.md') -Raw
    $m = [regex]::Match($changelog, "(?ms)^## \[$([regex]::Escape($Version))\][^\r\n]*\r?\n(.*?)(?=^## \[|\z)")
    $body = if ($m.Success) { $m.Groups[1].Value.Trim() } else { "Version $Version." }
    Set-Content -Path $notes -Value ($body + "`n`nSHA256 : $hash") -Encoding utf8
    Run git @('tag', '-a', $tag, '-m', "Hands Interaction $Version")
    Run git @('push', 'origin', $tag)
    $ghArgs = @('release', 'create', $tag, $setup, (Join-Path $dist 'SHA256SUMS.txt'), '--title', "Hands Interaction $Version", '--notes-file', $notes)
    if ($Prerelease) { $ghArgs += '--prerelease' }
    Run $gh.Source $ghArgs
    Write-Host "Release $tag publiee."
} else {
    Write-Host ""
    Write-Host "Pour publier : relancez avec -Publish (tag v$Version + release GitHub)."
}
