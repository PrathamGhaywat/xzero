# xzero installer — PowerShell
# Usage: irm https://raw.githubusercontent.com/REPO/main/install.ps1 | iex
#        irm .../install.ps1 | iex; Install-Xzero -Version v0.1.0
# Params: -Version (default latest), -InstallDir, -Repo
param(
  [string]$Version = "latest",
  [string]$InstallDir = "$env:USERPROFILE\.local\bin",
  [string]$Repo = $env:XZERO_REPO
)

if (-not $Repo -or $Repo -eq "") { $Repo = "xzero/xzero" }
if ($env:XZERO_INSTALL_DIR) { $InstallDir = $env:XZERO_INSTALL_DIR }

# Detect arch — modern only
$arch = $env:PROCESSOR_ARCHITECTURE
# Handle WOW64 via PROCESSOR_ARCHITEW6432
if ($env:PROCESSOR_ARCHITEW6432) { $arch = $env:PROCESSOR_ARCHITEW6432 }
switch -Regex ($arch) {
  "AMD64|x64" { $arch = "x64" }
  "ARM64" { $arch = "arm64" }
  default { Write-Error "Unsupported arch: $arch (only x64 and arm64)"; exit 1 }
}

$os = "windows"
Write-Host "Installing xzero $Version for $os-$arch -> $InstallDir" -ForegroundColor Cyan

# Resolve latest
if ($Version -eq "latest") {
  $headers = @{}
  if ($env:GITHUB_TOKEN) { $headers.Authorization = "Bearer $env:GITHUB_TOKEN" }
  try {
    $rel = Invoke-RestMethod -Uri "https://api.github.com/repos/$Repo/releases/latest" -Headers $headers -ErrorAction Stop
    $Version = $rel.tag_name
    Write-Host "Latest is $Version" -ForegroundColor Green
  } catch {
    Write-Error "Failed to resolve latest version. Check XZERO_REPO or pass -Version v0.1.0. $_"
    exit 1
  }
}
if (-not $Version.StartsWith("v")) { $Version = "v$Version" }

$artifact = "xzero-$Version-$os-$arch.zip"
$url = "https://github.com/$Repo/releases/download/$Version/$artifact"
$shaUrl = "https://github.com/$Repo/releases/download/$Version/SHA256SUMS"

Write-Host "Downloading $url"

$tmp = Join-Path $env:TEMP ("xzero-install-" + [Guid]::NewGuid().ToString())
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
$zipPath = Join-Path $tmp $artifact

try {
  # Download with progress
  Invoke-WebRequest -Uri $url -OutFile $zipPath -UseBasicParsing -ErrorAction Stop
} catch {
  Write-Error "Download failed: $url`n$_"
  exit 1
}

# Try verify
try {
  $sha = Invoke-WebRequest -Uri $shaUrl -UseBasicParsing -ErrorAction SilentlyContinue
  if ($sha -and $sha.Content -match $artifact) {
    $expected = ($sha.Content -split "`n" | Where-Object { $_ -match $artifact } | ForEach-Object { ($_ -split '\s+')[0] })[0]
    $actual = (Get-FileHash $zipPath -Algorithm SHA256).Hash.ToLower()
    if ($expected -and $actual -ne $expected.ToLower()) {
      Write-Warning "Checksum mismatch: expected $expected got $actual"
    } else {
      Write-Host "Checksum OK" -ForegroundColor Green
    }
  }
} catch { Write-Warning "Could not verify checksum: $_" }

# Extract
New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
try {
  Expand-Archive -Path $zipPath -DestinationPath $tmp -Force
} catch {
  # Fallback via .NET
  Add-Type -AssemblyName System.IO.Compression.FileSystem
  [System.IO.Compression.ZipFile]::ExtractToDirectory($zipPath, $tmp)
}

$exe = Get-ChildItem -Path $tmp -Recurse -Filter "xzero*.exe" | Select-Object -First 1
if (-not $exe) { $exe = Get-ChildItem -Path $tmp -Recurse -Filter "xzero*" | Where-Object { -not $_.PSIsContainer } | Select-Object -First 1 }
if (-not $exe) { Write-Error "No binary found in archive"; exit 1 }

$dest = Join-Path $InstallDir "xzero.exe"
Copy-Item -Path $exe.FullName -Destination $dest -Force
Unblock-File -Path $dest -ErrorAction SilentlyContinue

Write-Host "Installed to $dest" -ForegroundColor Green
try { & $dest --version } catch {}

# PATH
$path = [Environment]::GetEnvironmentVariable("Path", "User")
if ($path -notlike "*$InstallDir*") {
  Write-Host ""
  Write-Host "Add to PATH (restart terminal after):" -ForegroundColor Yellow
  Write-Host "  `$env:Path += `";$InstallDir`""
  Write-Host "  [Environment]::SetEnvironmentVariable('Path', `$env:Path + `";$InstallDir`", 'User')"
  Write-Host "  Or run: $dest --help"
} else {
  Write-Host "Already in PATH" -ForegroundColor Green
}

Write-Host "Done. Configure with: $dest  (will prompt for base URL / API key)" -ForegroundColor Cyan

# Cleanup
Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
