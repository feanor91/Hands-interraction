<#
.SYNOPSIS
  Desinstalle la couche OpenXR XR_APILAYER_HANDS_bare (a lancer en administrateur).

.DESCRIPTION
  Retire du registre toute entree de couche implicite pointant vers
  XR_APILAYER_HANDS_bare.json. Avec -RemoveFiles, supprime aussi le dossier
  d'installation (DLL, config, journaux).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File .\scripts\uninstall.ps1 -RemoveFiles
#>
[CmdletBinding()]
param(
    [string]$InstallDir = 'C:\ProgramData\HandsLayer',
    [switch]$RemoveFiles
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$ManifestName = 'XR_APILAYER_HANDS_bare.json'
$RegKey       = 'HKLM:\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit'

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Ce script doit etre lance en administrateur."
}
if (-not [Environment]::Is64BitProcess) {
    throw "Lancez PowerShell 64 bits."
}

$removed = 0
if (Test-Path $RegKey) {
    $props = Get-Item -Path $RegKey
    foreach ($name in $props.GetValueNames()) {
        if ($name -like "*$ManifestName") {
            Remove-ItemProperty -Path $RegKey -Name $name -Force
            Write-Host "Registre : entree supprimee -> $name"
            $removed++
        }
    }
}
if ($removed -eq 0) { Write-Host "Registre : aucune entree a supprimer." }

if ($RemoveFiles) {
    if (Test-Path $InstallDir) {
        try {
            Remove-Item -Recurse -Force $InstallDir
            Write-Host "Fichiers : $InstallDir supprime."
        } catch {
            throw "Suppression impossible (DLL chargee ? fermez MSFS et tout programme VR). Detail : $($_.Exception.Message)"
        }
    } else {
        Write-Host "Fichiers : $InstallDir n'existe pas."
    }
} else {
    Write-Host "Fichiers conserves dans $InstallDir (utilisez -RemoveFiles pour les supprimer)."
}
Write-Host "Desinstallation terminee."
