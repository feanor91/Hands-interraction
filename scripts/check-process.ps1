<#
.SYNOPSIS
  Aide au diagnostic : trouve le nom exact de l'executable de MSFS 2024, verifie
  l'etat de la couche et affiche la fin du journal.

.DESCRIPTION
  A lancer PENDANT que MSFS 2024 tourne (idealement deja en VR). Copiez-collez
  toute la sortie dans votre rapport. Lecture seule : ne modifie rien.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File .\scripts\check-process.ps1
#>
[CmdletBinding()]
param(
    [string[]]$Pattern = @('*flight*', '*msfs*', '*limitless*'),
    [string]$InstallDir = 'C:\ProgramData\HandsLayer',
    [int]$Tail = 60
)

$ErrorActionPreference = 'Continue'

Write-Host "=== 1. Processus correspondant a $($Pattern -join ', ') ==="
$found = @()
foreach ($p in $Pattern) { $found += @(Get-Process -Name $p -ErrorAction SilentlyContinue) }
$found = $found | Sort-Object Id -Unique
if (-not $found) {
    Write-Host "Aucun processus trouve. MSFS est-il lance ? Voici les processus ayant une fenetre :"
    Get-Process | Where-Object { $_.MainWindowTitle } |
        Select-Object Name, Id, MainWindowTitle | Format-Table -AutoSize | Out-String | Write-Host
} else {
    foreach ($proc in $found) {
        $cim = Get-CimInstance Win32_Process -Filter "ProcessId = $($proc.Id)" -ErrorAction SilentlyContinue
        $path = $null
        if ($cim) { $path = $cim.ExecutablePath }
        Write-Host ("Nom: {0}.exe | PID: {1} | Chemin: {2} | Fenetre: {3}" -f $proc.Name, $proc.Id, $path, $proc.MainWindowTitle)
    }
}

Write-Host ""
Write-Host "=== 2. Paquets Store lies a Flight Simulator ==="
try {
    Get-AppxPackage -Name '*Flight*', '*Limitless*' -ErrorAction SilentlyContinue |
        Select-Object Name, Version, InstallLocation | Format-List | Out-String | Write-Host
} catch { Write-Host "Get-AppxPackage indisponible : $($_.Exception.Message)" }

Write-Host "=== 3. Enregistrement de la couche (registre) ==="
$RegKey = 'HKLM:\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit'
if (Test-Path $RegKey) {
    $item = Get-Item -Path $RegKey
    foreach ($n in $item.GetValueNames()) { Write-Host ("  {0} = {1}" -f $n, $item.GetValue($n)) }
} else {
    Write-Host "  Cle absente : aucune couche implicite enregistree."
}
$Rt = 'HKLM:\SOFTWARE\Khronos\OpenXR\1'
if (Test-Path $Rt) { Write-Host ("  Runtime actif : {0}" -f (Get-ItemProperty $Rt).ActiveRuntime) }

Write-Host ""
Write-Host "=== 4. Journaux de la couche ($InstallDir\logs) ==="
$logs = @(Get-ChildItem -Path (Join-Path $InstallDir 'logs') -Filter 'layer_*.log' -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending)
if (-not $logs) {
    Write-Host "  Aucun journal. La couche ne s'est pas chargee, ou n'a pas pu ecrire ici."
    Write-Host "  Cherchez aussi : $env:TEMP\HandsLayer\ (repli) et %LOCALAPPDATA%\Packages\*\LocalCache\ (virtualisation Store)."
} else {
    $logs | Select-Object Name, Length, LastWriteTime | Format-Table -AutoSize | Out-String | Write-Host
    Write-Host "--- fin de $($logs[0].Name) ($Tail dernieres lignes) ---"
    Get-Content -Path $logs[0].FullName -Tail $Tail
}
