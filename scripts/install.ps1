<#
.SYNOPSIS
  Installe la couche OpenXR XR_APILAYER_HANDS_bare (a lancer en administrateur).

.DESCRIPTION
  1. Copie la DLL, le manifeste JSON et hands.ini dans C:\ProgramData\HandsLayer
  2. Regle les droits : lecture/execution pour les applications Microsoft Store
     ("TOUS LES PACKAGES D'APPLICATION"), ecriture seulement sur logs\ et hands.ini
  3. Enregistre le manifeste dans le registre (couche implicite OpenXR) :
     HKLM\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit  (nom = chemin du manifeste, DWORD 0 = active)

  La DLL n'est PAS modifiable par un utilisateur standard (securite : elle est
  chargee dans le processus du jeu).

.PARAMETER SourceDir
  Dossier contenant XR_APILAYER_HANDS_bare.dll, le .json et hands.ini.
  Par defaut : build\layer\Release ou build\layer\RelWithDebInfo du depot, ou le dossier du script.

.PARAMETER InstallDir
  Dossier d'installation (defaut : C:\ProgramData\HandsLayer).

.PARAMETER ResetConfig
  Ecrase hands.ini meme s'il existe deja (sinon vos reglages sont conserves).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File .\scripts\install.ps1
#>
[CmdletBinding()]
param(
    [string]$SourceDir,
    [string]$InstallDir = 'C:\ProgramData\HandsLayer',
    [switch]$ResetConfig
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$LayerName    = 'XR_APILAYER_HANDS_bare'
$DllName      = "$LayerName.dll"
$ManifestName = "$LayerName.json"
$RegKey       = 'HKLM:\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit'

# --- Verifications prealables --------------------------------------------------
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Ce script doit etre lance en administrateur (clic droit sur PowerShell > Executer en tant qu'administrateur)."
}
if (-not [Environment]::Is64BitProcess) {
    throw "Lancez PowerShell 64 bits : en 32 bits, le registre serait ecrit dans la mauvaise vue (WOW6432Node)."
}

if (-not $SourceDir) {
    $root = Split-Path -Parent $PSScriptRoot
    $candidates = @(
        (Join-Path $root 'build\layer\Release'),
        (Join-Path $root 'build\layer\RelWithDebInfo'),
        $PSScriptRoot,
        $root
    )
    foreach ($c in $candidates) {
        if (Test-Path (Join-Path $c $DllName)) { $SourceDir = $c; break }
    }
}
if (-not $SourceDir -or -not (Test-Path (Join-Path $SourceDir $DllName))) {
    throw "DLL introuvable. Compilez d'abord le projet (voir README) ou indiquez -SourceDir <dossier contenant $DllName>."
}
foreach ($f in @($DllName, $ManifestName, 'hands.ini')) {
    if (-not (Test-Path (Join-Path $SourceDir $f))) { throw "Fichier manquant dans ${SourceDir} : $f" }
}
Write-Host "Source        : $SourceDir"
Write-Host "Installation  : $InstallDir"

# --- Copie des fichiers ----------------------------------------------------------
New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $InstallDir 'logs') | Out-Null
try {
    Copy-Item -Force (Join-Path $SourceDir $DllName)      (Join-Path $InstallDir $DllName)
    Copy-Item -Force (Join-Path $SourceDir $ManifestName) (Join-Path $InstallDir $ManifestName)
} catch {
    throw "Copie impossible (la DLL est probablement chargee : fermez MSFS et tout programme VR). Detail : $($_.Exception.Message)"
}
$iniDest = Join-Path $InstallDir 'hands.ini'
if ((-not (Test-Path $iniDest)) -or $ResetConfig) {
    Copy-Item -Force (Join-Path $SourceDir 'hands.ini') $iniDest
    Write-Host "hands.ini     : cree"
} else {
    Write-Host "hands.ini     : existant conserve (utilisez -ResetConfig pour le remplacer)"
}

# --- Droits d'acces ---------------------------------------------------------------
# SID : S-1-5-18 = SYSTEM ; S-1-5-32-544 = Administrateurs ; S-1-5-32-545 = Utilisateurs ;
#       S-1-15-2-1 = TOUS LES PACKAGES D'APPLICATION (applications Store) ;
#       S-1-15-2-2 = TOUS LES PACKAGES D'APPLICATION RESTREINTS.
# (OI)(CI) = herite aux fichiers et sous-dossiers ; F = controle total ; RX = lecture/execution ; M = modification.
function Invoke-Icacls {
    param([string[]]$IcaclsArgs)
    & icacls.exe @IcaclsArgs | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "icacls a echoue (code $LASTEXITCODE) : icacls $($IcaclsArgs -join ' ')" }
}
Invoke-Icacls @($InstallDir, '/inheritance:r', '/grant:r',
    '*S-1-5-18:(OI)(CI)F', '*S-1-5-32-544:(OI)(CI)F', '*S-1-5-32-545:(OI)(CI)RX',
    '*S-1-15-2-1:(OI)(CI)RX', '*S-1-15-2-2:(OI)(CI)RX')
Invoke-Icacls @((Join-Path $InstallDir 'logs'), '/grant', '*S-1-5-32-545:(OI)(CI)M', '*S-1-15-2-1:(OI)(CI)M')
Invoke-Icacls @($iniDest, '/grant', '*S-1-5-32-545:M')
Write-Host "Droits        : appliques"

# --- Registre ----------------------------------------------------------------------
$manifestPath = Join-Path $InstallDir $ManifestName
if (-not (Test-Path $RegKey)) { New-Item -Path $RegKey -Force | Out-Null }
New-ItemProperty -Path $RegKey -Name $manifestPath -PropertyType DWord -Value 0 -Force | Out-Null
$check = (Get-ItemProperty -Path $RegKey).$manifestPath
if ($check -ne 0) { throw "Verification du registre echouee (valeur lue : $check)." }
Write-Host "Registre      : $RegKey  ->  $manifestPath = 0 (active)"

Write-Host ""
Write-Host "Installation terminee."
Write-Host "Journaux      : $InstallDir\logs\layer_<programme>.log"
Write-Host "Configuration : $iniDest (rechargee a chaud)"
Write-Host "Desactiver temporairement : mettre la valeur DWORD du registre a 1, ou definir DISABLE_XR_APILAYER_HANDS_BARE=1."
Write-Host "Desinstaller  : .\scripts\uninstall.ps1"
