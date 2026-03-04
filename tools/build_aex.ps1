param(
  [Parameter(Mandatory = $true)]
  [string]$AeSdkIncludeDir,

  [Parameter(Mandatory = $true)]
  [string]$OrtIncludeDir,

  [Parameter(Mandatory = $true)]
  [string]$OrtLibrary,

  [ValidateSet("AUTO", "ON", "OFF")]
  [string]$OrtDirectLinkMode = "OFF",

  [switch]$EnableOrtApi,

  [switch]$DisableOrtApi,

  [string]$OrtRuntimeDllPath,

  [switch]$RequireOrtRuntimeDll,

  [string]$BuildDir = "build-win-aex",

  [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
  [string]$Config = "Release",

  [string]$Generator = "Visual Studio 17 2022",

  [ValidateSet("Win32", "x64", "ARM64")]
  [string]$Platform = "x64",

  [string]$MsvcRuntime = 'MultiThreaded$<$<CONFIG:Debug>:Debug>',

  [switch]$Clean,

  [switch]$CopyToMediaCore,

  [switch]$BuildLoaderProbe,

  [switch]$LoaderOnlyMain,

  [string]$MediaCoreDir,

  [switch]$SkipDuplicateInstallCheck
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Assert-Path {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path,

    [Parameter(Mandatory = $true)]
    [ValidateSet("Leaf", "Container")]
    [string]$PathType,

    [Parameter(Mandatory = $true)]
    [string]$Message
  )

  if (-not (Test-Path -LiteralPath $Path -PathType $PathType)) {
    throw $Message
  }
}

function Resolve-AbsolutePath {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path,

    [switch]$AllowMissing
  )

  if (Test-Path -LiteralPath $Path) {
    return (Resolve-Path -LiteralPath $Path).Path
  }

  if ($AllowMissing) {
    return [System.IO.Path]::GetFullPath($Path)
  }

  throw "Path does not exist: $Path"
}

function Get-ZsodaGlobalOutFlagsToken {
  param(
    [Parameter(Mandatory = $true)]
    [string]$HeaderPath
  )

  Assert-Path -Path $HeaderPath -PathType Leaf -Message "ZSodaAeFlags header not found: $HeaderPath"
  $content = Get-Content -LiteralPath $HeaderPath -Raw
  $pattern = '(?m)^\s*#define\s+ZSODA_AE_GLOBAL_OUTFLAGS\s+(0x[0-9A-Fa-f]+)\b'
  $match = [regex]::Match($content, $pattern)
  if (-not $match.Success) {
    throw "Failed to parse ZSODA_AE_GLOBAL_OUTFLAGS from: $HeaderPath"
  }
  return $match.Groups[1].Value
}

function Invoke-CMake {
  param(
    [Parameter(Mandatory = $true)]
    [string[]]$Arguments,

    [Parameter(Mandatory = $true)]
    [string]$FailureMessage
  )

  & cmake @Arguments
  if ($LASTEXITCODE -ne 0) {
    throw "$FailureMessage (exit code: $LASTEXITCODE)"
  }
}

function Find-Artifact {
  param(
    [Parameter(Mandatory = $true)]
    [string[]]$Candidates,

    [Parameter(Mandatory = $true)]
    [string]$Name
  )

  foreach ($candidate in $Candidates) {
    if (Test-Path -LiteralPath $candidate -PathType Leaf) {
      return (Resolve-Path -LiteralPath $candidate).Path
    }
  }

  $candidatesText = $Candidates -join [Environment]::NewLine
  throw "Failed to locate artifact '$Name'. Checked candidates:`n$candidatesText"
}

function Find-OptionalArtifact {
  param(
    [Parameter(Mandatory = $true)]
    [string[]]$Candidates
  )

  foreach ($candidate in $Candidates) {
    if (Test-Path -LiteralPath $candidate -PathType Leaf) {
      return (Resolve-Path -LiteralPath $candidate).Path
    }
  }
  return $null
}

function Print-ArtifactInfo {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Label,

    [Parameter(Mandatory = $true)]
    [string]$Path
  )

  $hash = Get-FileHash -Algorithm SHA256 -LiteralPath $Path
  Write-Host "$($Label):"
  Write-Host "  path:   $Path"
  Write-Host "  sha256: $($hash.Hash.ToLowerInvariant())"
}

function Contains-AsciiTokenInBinary {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path,

    [Parameter(Mandatory = $true)]
    [string]$Token
  )

  Assert-Path -Path $Path -PathType Leaf -Message "Binary not found: $Path"
  $bytes = [System.IO.File]::ReadAllBytes($Path)
  $tokenBytes = [System.Text.Encoding]::ASCII.GetBytes($Token)
  if ($tokenBytes.Length -eq 0 -or $bytes.Length -lt $tokenBytes.Length) {
    return $false
  }

  for ($i = 0; $i -le $bytes.Length - $tokenBytes.Length; $i++) {
    $matched = $true
    for ($j = 0; $j -lt $tokenBytes.Length; $j++) {
      if ($bytes[$i + $j] -ne $tokenBytes[$j]) {
        $matched = $false
        break
      }
    }
    if ($matched) {
      return $true
    }
  }
  return $false
}

function Assert-RrSignature {
  param(
    [Parameter(Mandatory = $true)]
    [string]$PiPlRrPath,

    [string[]]$RequiredTokens = @(
      "CodeWin64X86",
      "EffectMain",
      "AE_Effect_Global_OutFlags"
    )
  )

  Assert-Path -Path $PiPlRrPath -PathType Leaf -Message "PiPL RR not found: $PiPlRrPath"
  $content = Get-Content -LiteralPath $PiPlRrPath -Raw

  foreach ($token in $RequiredTokens) {
    if ($content.IndexOf($token, [StringComparison]::OrdinalIgnoreCase) -lt 0) {
      throw "PiPL RR signature validation failed. Missing token '$token' in $PiPlRrPath"
    }
  }
}

function Assert-AexLoaderSignature {
  param(
    [Parameter(Mandatory = $true)]
    [string]$AexPath,

    [Parameter(Mandatory = $true)]
    [ValidateSet("Win32", "x64", "ARM64")]
    [string]$Platform
  )

  Assert-Path -Path $AexPath -PathType Leaf -Message "AEX not found: $AexPath"

  $dumpbin = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
  if ($null -eq $dumpbin) {
    $dumpbin = Get-Command dumpbin -ErrorAction SilentlyContinue
  }

  if ($null -ne $dumpbin) {
    $exportsText = (& $dumpbin.Source /exports $AexPath 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0) {
      throw "dumpbin /exports failed for $AexPath"
    }
    if ($exportsText -notmatch '(?i)\bEffectMain\b') {
      throw "Loader signature check failed: EffectMain export not found in $AexPath"
    }

    $headersText = (& $dumpbin.Source /headers $AexPath 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0) {
      throw "dumpbin /headers failed for $AexPath"
    }
    if ($headersText -notmatch '(?i)\.rsrc') {
      throw "Loader signature check failed: .rsrc section not found in $AexPath"
    }

    switch ($Platform.ToLowerInvariant()) {
      "x64" {
        if ($headersText -notmatch '(?i)8664 machine|machine \(x64\)') {
          throw "Loader signature check failed: expected x64 machine in PE headers"
        }
      }
      "arm64" {
        if ($headersText -notmatch '(?i)aa64 machine|machine \(arm64\)') {
          throw "Loader signature check failed: expected ARM64 machine in PE headers"
        }
      }
      "win32" {
        if ($headersText -notmatch '(?i)14C machine|machine \(x86\)') {
          throw "Loader signature check failed: expected x86 machine in PE headers"
        }
      }
    }

    $resourceText = (& $dumpbin.Source /rawdata:.rsrc $AexPath 2>&1 | Out-String)
    if ($LASTEXITCODE -eq 0) {
      if ($resourceText -notmatch '(?i)PiPL|ZSoda Depth|Z-Soda|EffectMain') {
        Write-Warning "PiPL hint tokens were not found in dumpbin .rsrc output. Verify PiPL resource manually."
      }
    } else {
      Write-Warning "dumpbin /rawdata:.rsrc failed; skipped PiPL hint scan."
    }
    return
  }

  Write-Warning "dumpbin is not available; using binary token fallback checks."
  if (-not (Contains-AsciiTokenInBinary -Path $AexPath -Token "EffectMain")) {
    throw "Loader signature check failed: EffectMain token not found in binary fallback scan"
  }
  if (-not (Contains-AsciiTokenInBinary -Path $AexPath -Token "PiPL") -and
      -not (Contains-AsciiTokenInBinary -Path $AexPath -Token "ZSoda Depth")) {
    Write-Warning "PiPL/MatchName tokens were not found in binary fallback scan."
  }
}

function Find-GeneratedPiPlArtifact {
  param(
    [Parameter(Mandatory = $true)]
    [string]$BuildDir,

    [Parameter(Mandatory = $true)]
    [string]$FileName
  )

  return Find-Artifact -Name $FileName -Candidates @(
    (Join-Path $BuildDir ("plugin\pipl\{0}" -f $FileName)),
    (Join-Path $BuildDir ("pipl\{0}" -f $FileName))
  )
}

function Find-OptionalGeneratedPiPlArtifact {
  param(
    [Parameter(Mandatory = $true)]
    [string]$BuildDir,

    [Parameter(Mandatory = $true)]
    [string]$FileName
  )

  return Find-OptionalArtifact -Candidates @(
    (Join-Path $BuildDir ("plugin\pipl\{0}" -f $FileName)),
    (Join-Path $BuildDir ("pipl\{0}" -f $FileName))
  )
}

function Write-LoaderCheckSummary {
  param(
    [Parameter(Mandatory = $true)]
    [string]$SummaryPath,

    [Parameter(Mandatory = $true)]
    [string[]]$Lines
  )

  try {
    Set-Content -LiteralPath $SummaryPath -Value ($Lines -join [Environment]::NewLine) -Encoding UTF8
    Write-Host "loader_check_summary: $SummaryPath"
  } catch {
    Write-Warning "Failed to write loader check summary: $SummaryPath"
  }
}

function Build-LoaderCheckSummary {
  param(
    [Parameter(Mandatory = $true)]
    [string]$AexPath,

    [Parameter(Mandatory = $true)]
    [string]$PiPlRrPath,

    [string]$OutFlagsToken,

    [string]$PiPlRcPath,

    [string]$PiPlRrcPath
  )

  $lines = @()
  $lines += "timestamp: $(Get-Date -Format o)"
  $lines += "aex: $AexPath"
  $lines += "pipl_rr: $PiPlRrPath"
  if (-not [string]::IsNullOrWhiteSpace($PiPlRcPath)) {
    $lines += "pipl_rc: $PiPlRcPath"
  }
  if (-not [string]::IsNullOrWhiteSpace($PiPlRrcPath)) {
    $lines += "pipl_rrc: $PiPlRrcPath"
  }
  $lines += "checks:"
  if ([string]::IsNullOrWhiteSpace($OutFlagsToken)) {
    $lines += "  - pipl_rr literal tokens: CodeWin64X86/EffectMain/AE_Effect_Global_OutFlags/<unspecified>"
  } else {
    $lines += "  - pipl_rr literal tokens: CodeWin64X86/EffectMain/AE_Effect_Global_OutFlags/$OutFlagsToken"
  }
  $lines += "  - aex export: EffectMain"
  $lines += "  - aex section: .rsrc"
  return $lines
}

function Print-LoaderEvidence {
  param(
    [Parameter(Mandatory = $true)]
    [string]$AexPath
  )

  $dumpbin = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
  if ($null -eq $dumpbin) {
    $dumpbin = Get-Command dumpbin -ErrorAction SilentlyContinue
  }
  if ($null -eq $dumpbin) {
    Write-Warning "dumpbin is unavailable; skipped detailed loader evidence output."
    return
  }

  $exportMatch = (& $dumpbin.Source /exports $AexPath 2>&1 | Select-String -Pattern "EffectMain")
  if ($LASTEXITCODE -eq 0 -and $null -ne $exportMatch) {
    $first = $exportMatch | Select-Object -First 1
    Write-Host "loader_export: $($first.Line.Trim())"
  }

  $headerMatch = (& $dumpbin.Source /headers $AexPath 2>&1 | Select-String -Pattern "machine|\.rsrc")
  if ($LASTEXITCODE -eq 0 -and $null -ne $headerMatch) {
    foreach ($line in $headerMatch) {
      Write-Host "loader_header: $($line.Line.Trim())"
    }
  }
}

function Get-Sha256Lower {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path
  )

  return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant()
}

function Find-InstalledEffectsAexCandidates {
  param(
    [Parameter(Mandatory = $true)]
    [string]$FileName
  )

  $results = @()
  $adobeRoot = Join-Path $env:ProgramFiles "Adobe"
  if (-not (Test-Path -LiteralPath $adobeRoot -PathType Container)) {
    return $results
  }

  $aeDirs = Get-ChildItem -LiteralPath $adobeRoot -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -like "Adobe After Effects*" }
  foreach ($dir in $aeDirs) {
    $candidate = Join-Path $dir.FullName ("Support Files\Plug-ins\Effects\{0}" -f $FileName)
    if (Test-Path -LiteralPath $candidate -PathType Leaf) {
      $results += (Resolve-Path -LiteralPath $candidate).Path
    }
  }
  return $results
}

function Assert-NoMismatchedInstalledCopies {
  param(
    [Parameter(Mandatory = $true)]
    [string]$PrimaryAexPath,

    [Parameter(Mandatory = $true)]
    [string]$FileName
  )

  if (-not (Test-Path -LiteralPath $PrimaryAexPath -PathType Leaf)) {
    throw "Primary plugin path not found for duplicate check: $PrimaryAexPath"
  }
  $primaryAbs = (Resolve-Path -LiteralPath $PrimaryAexPath).Path
  $primaryHash = Get-Sha256Lower -Path $primaryAbs

  $mismatched = @()
  $duplicates = @()
  foreach ($candidate in (Find-InstalledEffectsAexCandidates -FileName $FileName)) {
    if ($candidate -ieq $primaryAbs) {
      continue
    }
    $candidateHash = Get-Sha256Lower -Path $candidate
    $duplicates += [PSCustomObject]@{ Path = $candidate; Hash = $candidateHash }
    if ($candidateHash -ieq $primaryHash) {
      continue
    }
    $mismatched += [PSCustomObject]@{ Path = $candidate; Hash = $candidateHash }
  }

  if ($duplicates.Count -eq 0) {
    return
  }

  $details = @()
  $details += ("Primary: {0} ({1})" -f $primaryAbs, $primaryHash)
  foreach ($entry in $duplicates) {
    if ($entry.Hash -ieq $primaryHash) {
      $details += ("Duplicate: {0} ({1}) [same-hash]" -f $entry.Path, $entry.Hash)
    } else {
      $details += ("Duplicate: {0} ({1}) [mismatch]" -f $entry.Path, $entry.Hash)
    }
  }
  $details += "Action: keep only one installed copy (recommended: MediaCore only), remove all Effects duplicates, clear PluginCache keys, then retest."
  $joined = $details -join [Environment]::NewLine
  if ($mismatched.Count -gt 0) {
    throw "Detected duplicated installs with hash mismatch for $FileName.`n$joined"
  }
  throw "Detected duplicated installs for $FileName (same hash). Single-install policy is required for reliable AE loading.`n$joined"
}

function Resolve-OrtRuntimeDll {
  param(
    [Parameter(Mandatory = $true)]
    [string]$OrtLibraryPath,

    [string]$ExplicitDllPath
  )

  if (-not [string]::IsNullOrWhiteSpace($ExplicitDllPath)) {
    $explicitAbs = Resolve-AbsolutePath -Path $ExplicitDllPath
    Assert-Path -Path $explicitAbs -PathType Leaf -Message "ORT runtime DLL not found: $explicitAbs"
    return $explicitAbs
  }

  $libraryDir = Split-Path -Path $OrtLibraryPath -Parent
  if ([string]::IsNullOrWhiteSpace($libraryDir)) {
    return $null
  }

  $candidates = @(
    (Join-Path $libraryDir "onnxruntime.dll"),
    (Join-Path $libraryDir "..\bin\onnxruntime.dll")
  )
  foreach ($candidate in $candidates) {
    if (Test-Path -LiteralPath $candidate -PathType Leaf) {
      return (Resolve-Path -LiteralPath $candidate).Path
    }
  }

  return $null
}

function Resolve-OrtProvidersSharedDll {
  param(
    [Parameter(Mandatory = $true)]
    [string]$OrtLibraryPath,

    [string]$OrtRuntimeDllPath
  )

  $libraryDir = Split-Path -Path $OrtLibraryPath -Parent
  if ([string]::IsNullOrWhiteSpace($libraryDir)) {
    return $null
  }

  $candidates = @(
    (Join-Path $libraryDir "onnxruntime_providers_shared.dll"),
    (Join-Path $libraryDir "..\\bin\\onnxruntime_providers_shared.dll")
  )
  if (-not [string]::IsNullOrWhiteSpace($OrtRuntimeDllPath)) {
    $runtimeDir = Split-Path -Path $OrtRuntimeDllPath -Parent
    if (-not [string]::IsNullOrWhiteSpace($runtimeDir)) {
      $candidates += (Join-Path $runtimeDir "onnxruntime_providers_shared.dll")
    }
  }

  foreach ($candidate in $candidates | Select-Object -Unique) {
    if (Test-Path -LiteralPath $candidate -PathType Leaf) {
      return (Resolve-Path -LiteralPath $candidate).Path
    }
  }

  return $null
}

function Sync-ModelAssets {
  param(
    [Parameter(Mandatory = $true)]
    [string]$SourceRoot,

    [Parameter(Mandatory = $true)]
    [string]$DestinationRoot
  )

  if (-not (Test-Path -LiteralPath $SourceRoot -PathType Container)) {
    Write-Host "models_sync: source models directory not found ($SourceRoot)"
    return
  }

  New-Item -ItemType Directory -Path $DestinationRoot -Force | Out-Null

  $manifestSource = Join-Path $SourceRoot "models.manifest"
  if (Test-Path -LiteralPath $manifestSource -PathType Leaf) {
    $manifestDestination = Join-Path $DestinationRoot "models.manifest"
    Copy-Item -LiteralPath $manifestSource -Destination $manifestDestination -Force
    Print-ArtifactInfo -Label "models_manifest" -Path $manifestDestination
  } else {
    Write-Warning "models.manifest not found under $SourceRoot"
  }

  $modelFiles = @(Get-ChildItem -LiteralPath $SourceRoot -Recurse -File -ErrorAction SilentlyContinue |
      Where-Object {
        $ext = $_.Extension.ToLowerInvariant()
        $ext -eq ".onnx" -or $ext -eq ".onnx_data"
      })
  if ($modelFiles.Count -eq 0) {
    Write-Host "models_sync: no model files (.onnx/.onnx_data) found under $SourceRoot"
    return
  }

  foreach ($file in $modelFiles) {
    $relativePath = $file.FullName.Substring($SourceRoot.Length).TrimStart('\', '/')
    $targetPath = Join-Path $DestinationRoot $relativePath
    $targetDir = Split-Path -Path $targetPath -Parent
    if (-not [string]::IsNullOrWhiteSpace($targetDir)) {
      New-Item -ItemType Directory -Path $targetDir -Force | Out-Null
    }
    Copy-Item -LiteralPath $file.FullName -Destination $targetPath -Force
    Print-ArtifactInfo -Label "models_copy" -Path $targetPath
  }
}

$runningOnWindows = $false
if (Get-Variable -Name IsWindows -ErrorAction SilentlyContinue) {
  $runningOnWindows = [bool]$IsWindows
} else {
  $runningOnWindows = ($env:OS -eq "Windows_NT")
}

if (-not $runningOnWindows) {
  throw "This script is for Windows .aex builds only."
}

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
  throw "cmake command not found in PATH."
}

$repoRoot = Resolve-AbsolutePath -Path (Join-Path $PSScriptRoot "..")
$zsodaAeFlagsHeader = Join-Path $repoRoot "plugin\ae\ZSodaAeFlags.h"
$expectedOutFlagsToken = Get-ZsodaGlobalOutFlagsToken -HeaderPath $zsodaAeFlagsHeader
$buildDirAbs = Resolve-AbsolutePath -Path $BuildDir -AllowMissing
$aeSdkIncludeDirAbs = Resolve-AbsolutePath -Path $AeSdkIncludeDir
$ortIncludeDirAbs = Resolve-AbsolutePath -Path $OrtIncludeDir
$ortLibraryAbs = Resolve-AbsolutePath -Path $OrtLibrary

Assert-Path -Path $aeSdkIncludeDirAbs -PathType Container -Message "AE SDK include dir not found: $aeSdkIncludeDirAbs"
Assert-Path -Path (Join-Path $aeSdkIncludeDirAbs "AE_Effect.h") -PathType Leaf -Message "AE_Effect.h not found under AE SDK include dir: $aeSdkIncludeDirAbs"
Assert-Path -Path $ortIncludeDirAbs -PathType Container -Message "ONNX Runtime include dir not found: $ortIncludeDirAbs"
Assert-Path -Path $ortLibraryAbs -PathType Leaf -Message "ONNX Runtime library not found: $ortLibraryAbs"

$ortHeaderDirect = Join-Path $ortIncludeDirAbs "onnxruntime_cxx_api.h"
$ortHeaderNested = Join-Path $ortIncludeDirAbs "onnxruntime\core\session\onnxruntime_cxx_api.h"
if (-not (Test-Path -LiteralPath $ortHeaderDirect -PathType Leaf) -and
    -not (Test-Path -LiteralPath $ortHeaderNested -PathType Leaf)) {
  throw "onnxruntime_cxx_api.h not found under ORT include dir: $ortIncludeDirAbs"
}

$ortRuntimeDllAbs = Resolve-OrtRuntimeDll -OrtLibraryPath $ortLibraryAbs -ExplicitDllPath $OrtRuntimeDllPath
if ($null -eq $ortRuntimeDllAbs) {
  $warn = "onnxruntime.dll was not resolved (checked explicit path and OrtLibrary neighbors). Runtime package may miss ORT DLL."
  if ($RequireOrtRuntimeDll) {
    throw $warn
  }
  Write-Warning $warn
}
$ortProvidersSharedAbs = Resolve-OrtProvidersSharedDll -OrtLibraryPath $ortLibraryAbs -OrtRuntimeDllPath $ortRuntimeDllAbs
if ($null -eq $ortProvidersSharedAbs) {
  Write-Warning "onnxruntime_providers_shared.dll was not resolved. ORT initialization may fail at runtime."
}

if ($Clean -and (Test-Path -LiteralPath $buildDirAbs)) {
  Remove-Item -LiteralPath $buildDirAbs -Recurse -Force
}
New-Item -ItemType Directory -Path $buildDirAbs -Force | Out-Null

if ($EnableOrtApi -and $DisableOrtApi) {
  throw "EnableOrtApi and DisableOrtApi cannot be used together."
}

$enableOrtApiEffective = $true
if ($DisableOrtApi) {
  $enableOrtApiEffective = $false
}
if ($EnableOrtApi) {
  $enableOrtApiEffective = $true
}
$loaderOnlyMainInt = if ($LoaderOnlyMain.IsPresent) { 1 } else { 0 }
$enableOrtApiInt = if ($enableOrtApiEffective) { 1 } else { 0 }

if (-not $enableOrtApiEffective) {
  Write-Host "ORT mode: structural-safe (ZSODA_WITH_ONNX_RUNTIME_API=OFF)"
}
if ($enableOrtApiEffective -and $OrtDirectLinkMode -eq "OFF") {
  Write-Host "ORT mode: API enabled with structural dynamic-loader path (direct link OFF)."
}
if (-not $enableOrtApiEffective -and $OrtDirectLinkMode -eq "ON") {
  Write-Warning "OrtDirectLinkMode=ON is ignored because ORT API is disabled."
}
if ($LoaderOnlyMain) {
  Write-Host "AE mode: loader-only main effect (ZSODA_AE_LOADER_ONLY_MODE=ON)."
}

if ($CopyToMediaCore) {
  if ([string]::IsNullOrWhiteSpace($MediaCoreDir)) {
    $MediaCoreDir = Join-Path $env:ProgramFiles "Adobe\Common\Plug-ins\7.0\MediaCore"
  }
  $MediaCoreDir = Resolve-AbsolutePath -Path $MediaCoreDir
  Assert-Path -Path $MediaCoreDir -PathType Container -Message "MediaCore directory not found: $MediaCoreDir"
}

Write-Host "==> Configuring CMake"
$configureArgs = @(
  "-S", $repoRoot,
  "-B", $buildDirAbs,
  "-G", $Generator,
  "-DZSODA_BUILD_TESTS=OFF",
  "-DZSODA_WITH_AE_SDK=ON",
  "-DZSODA_AE_LOADER_ONLY_MODE=$loaderOnlyMainInt",
  "-DZSODA_WITH_ONNX_RUNTIME=ON",
  "-DZSODA_WITH_ONNX_RUNTIME_API=$enableOrtApiInt",
  "-DZSODA_MSVC_RUNTIME_LIBRARY=$MsvcRuntime",
  "-DZSODA_ONNXRUNTIME_DIRECT_LINK_MODE=$OrtDirectLinkMode",
  "-DAE_SDK_INCLUDE_DIR=$aeSdkIncludeDirAbs",
  "-DONNXRUNTIME_INCLUDE_DIR=$ortIncludeDirAbs",
  "-DONNXRUNTIME_LIBRARY=$ortLibraryAbs"
)
if ($ortRuntimeDllAbs) {
  $configureArgs += "-DZSODA_ONNXRUNTIME_DLL_PATH_HINT=$ortRuntimeDllAbs"
}
if ($Generator -like "Visual Studio*") {
  $configureArgs += @("-A", $Platform)
}
Invoke-CMake -Arguments $configureArgs -FailureMessage "CMake configure failed"

Write-Host "==> Building target: zsoda_plugin"
Invoke-CMake -Arguments @("--build", $buildDirAbs, "--config", $Config, "--target", "zsoda_plugin") -FailureMessage "Build failed for target zsoda_plugin"

Write-Host "==> Building target: zsoda_aex"
Invoke-CMake -Arguments @("--build", $buildDirAbs, "--config", $Config, "--target", "zsoda_aex") -FailureMessage "Build failed for target zsoda_aex"

if ($BuildLoaderProbe) {
  Write-Host "==> Building target: zsoda_loader_probe_aex"
  Invoke-CMake -Arguments @("--build", $buildDirAbs, "--config", $Config, "--target", "zsoda_loader_probe_aex") -FailureMessage "Build failed for target zsoda_loader_probe_aex"
}

$pluginLib = Find-Artifact -Name "zsoda_plugin.lib" -Candidates @(
  (Join-Path $buildDirAbs ("plugin\{0}\zsoda_plugin.lib" -f $Config)),
  (Join-Path $buildDirAbs ("plugin\{0}\libzsoda_plugin.lib" -f $Config)),
  (Join-Path $buildDirAbs "plugin\zsoda_plugin.lib"),
  (Join-Path $buildDirAbs "plugin\libzsoda_plugin.lib")
)
$aex = Find-Artifact -Name "ZSoda.aex" -Candidates @(
  (Join-Path $buildDirAbs ("plugin\{0}\ZSoda.aex" -f $Config)),
  (Join-Path $buildDirAbs "plugin\ZSoda.aex")
)
$pdb = Find-OptionalArtifact -Candidates @(
  (Join-Path $buildDirAbs ("plugin\{0}\ZSoda.pdb" -f $Config)),
  (Join-Path $buildDirAbs "plugin\ZSoda.pdb")
)
$map = Find-OptionalArtifact -Candidates @(
  (Join-Path $buildDirAbs ("plugin\{0}\ZSoda.map" -f $Config)),
  (Join-Path $buildDirAbs "plugin\ZSoda.map")
)
$piplRr = Find-GeneratedPiPlArtifact -BuildDir $buildDirAbs -FileName "ZSodaPiPL.rr"
$piplRc = Find-OptionalGeneratedPiPlArtifact -BuildDir $buildDirAbs -FileName "ZSodaPiPL.rc"
$piplRrc = Find-OptionalGeneratedPiPlArtifact -BuildDir $buildDirAbs -FileName "ZSodaPiPL.rrc"

$loaderProbeAex = $null
$loaderProbePdb = $null
$loaderProbeMap = $null
$loaderProbePiPlRr = $null
$loaderProbePiPlRc = $null
$loaderProbePiPlRrc = $null
$loaderProbeSummaryPath = $null
if ($BuildLoaderProbe) {
  $loaderProbeAex = Find-Artifact -Name "ZSodaLoaderProbe.aex" -Candidates @(
    (Join-Path $buildDirAbs ("plugin\{0}\ZSodaLoaderProbe.aex" -f $Config)),
    (Join-Path $buildDirAbs "plugin\ZSodaLoaderProbe.aex")
  )
  $loaderProbePdb = Find-OptionalArtifact -Candidates @(
    (Join-Path $buildDirAbs ("plugin\{0}\ZSodaLoaderProbe.pdb" -f $Config)),
    (Join-Path $buildDirAbs "plugin\ZSodaLoaderProbe.pdb")
  )
  $loaderProbeMap = Find-OptionalArtifact -Candidates @(
    (Join-Path $buildDirAbs ("plugin\{0}\ZSodaLoaderProbe.map" -f $Config)),
    (Join-Path $buildDirAbs "plugin\ZSodaLoaderProbe.map")
  )
  $loaderProbePiPlRr = Find-GeneratedPiPlArtifact -BuildDir $buildDirAbs -FileName "ZSodaLoaderProbePiPL.rr"
  $loaderProbePiPlRc = Find-OptionalGeneratedPiPlArtifact -BuildDir $buildDirAbs -FileName "ZSodaLoaderProbePiPL.rc"
  $loaderProbePiPlRrc = Find-OptionalGeneratedPiPlArtifact -BuildDir $buildDirAbs -FileName "ZSodaLoaderProbePiPL.rrc"
}

Assert-RrSignature -PiPlRrPath $piplRr -RequiredTokens @(
  "CodeWin64X86",
  "EffectMain",
  "AE_Effect_Global_OutFlags",
  $expectedOutFlagsToken
)
Assert-AexLoaderSignature -AexPath $aex -Platform $Platform
if ($BuildLoaderProbe) {
  Assert-RrSignature -PiPlRrPath $loaderProbePiPlRr -RequiredTokens @(
    "CodeWin64X86",
    "EffectMain",
    "AE_Effect_Match_Name",
    "ZSoda Loader Probe",
    "AE_Effect_Global_OutFlags",
    $expectedOutFlagsToken
  )
  Assert-AexLoaderSignature -AexPath $loaderProbeAex -Platform $Platform
}

$aexDir = Split-Path -Path $aex -Parent
$loaderSummaryPath = Join-Path $aexDir "ZSoda.loader_check.txt"
$summaryLines = Build-LoaderCheckSummary -AexPath $aex -PiPlRrPath $piplRr -OutFlagsToken $expectedOutFlagsToken -PiPlRcPath $piplRc -PiPlRrcPath $piplRrc
Write-LoaderCheckSummary -SummaryPath $loaderSummaryPath -Lines $summaryLines
if ($BuildLoaderProbe) {
  $loaderProbeAexDir = Split-Path -Path $loaderProbeAex -Parent
  $loaderProbeSummaryPath = Join-Path $loaderProbeAexDir "ZSodaLoaderProbe.loader_check.txt"
  $loaderProbeSummaryLines = Build-LoaderCheckSummary -AexPath $loaderProbeAex -PiPlRrPath $loaderProbePiPlRr -OutFlagsToken $expectedOutFlagsToken -PiPlRcPath $loaderProbePiPlRc -PiPlRrcPath $loaderProbePiPlRrc
  Write-LoaderCheckSummary -SummaryPath $loaderProbeSummaryPath -Lines $loaderProbeSummaryLines
}

Write-Host "==> Build Succeeded"
Print-ArtifactInfo -Label "zsoda_plugin" -Path $pluginLib
Print-ArtifactInfo -Label "zsoda_aex" -Path $aex
Print-ArtifactInfo -Label "zsoda_pipl_rr" -Path $piplRr
if ($piplRc) {
  Print-ArtifactInfo -Label "zsoda_pipl_rc" -Path $piplRc
} else {
  Write-Warning "ZSodaPiPL.rc not found; RR-based validation was used."
}
if ($piplRrc) {
  Print-ArtifactInfo -Label "zsoda_pipl_rrc" -Path $piplRrc
}
Print-ArtifactInfo -Label "zsoda_loader_check" -Path $loaderSummaryPath
if ($pdb) {
  Print-ArtifactInfo -Label "zsoda_pdb" -Path $pdb
} else {
  Write-Warning "ZSoda.pdb not found; dump symbolization may be limited."
}
if ($map) {
  Print-ArtifactInfo -Label "zsoda_map" -Path $map
} else {
  Write-Warning "ZSoda.map not found; RVA->function mapping may be limited."
}
if ($BuildLoaderProbe) {
  Print-ArtifactInfo -Label "loader_probe_aex" -Path $loaderProbeAex
  Print-ArtifactInfo -Label "loader_probe_pipl_rr" -Path $loaderProbePiPlRr
  if ($loaderProbePiPlRc) {
    Print-ArtifactInfo -Label "loader_probe_pipl_rc" -Path $loaderProbePiPlRc
  }
  if ($loaderProbePiPlRrc) {
    Print-ArtifactInfo -Label "loader_probe_pipl_rrc" -Path $loaderProbePiPlRrc
  }
  if ($loaderProbeSummaryPath) {
    Print-ArtifactInfo -Label "loader_probe_loader_check" -Path $loaderProbeSummaryPath
  }
  if ($loaderProbePdb) {
    Print-ArtifactInfo -Label "loader_probe_pdb" -Path $loaderProbePdb
  } else {
    Write-Warning "ZSodaLoaderProbe.pdb not found."
  }
  if ($loaderProbeMap) {
    Print-ArtifactInfo -Label "loader_probe_map" -Path $loaderProbeMap
  } else {
    Write-Warning "ZSodaLoaderProbe.map not found."
  }
}

$stagedOrtRuntimePath = $null
$stagedOrtProvidersPath = $null
if ($ortRuntimeDllAbs) {
  # Deploy into isolated subdirectory to avoid LoadLibraryW conflict with
  # Adobe AE's preloaded onnxruntime.dll. The loader checks zsoda_ort/ first.
  $stagedOrtDir = Join-Path $aexDir "zsoda_ort"
  New-Item -ItemType Directory -Path $stagedOrtDir -Force | Out-Null
  $stagedOrtRuntimePath = Join-Path $stagedOrtDir "onnxruntime.dll"
  Copy-Item -LiteralPath $ortRuntimeDllAbs -Destination $stagedOrtRuntimePath -Force
  Print-ArtifactInfo -Label "staged_ort_dll" -Path $stagedOrtRuntimePath
} else {
  Write-Warning "onnxruntime.dll was not staged next to ZSoda.aex."
}
if ($ortProvidersSharedAbs) {
  $stagedOrtDir = Join-Path $aexDir "zsoda_ort"
  New-Item -ItemType Directory -Path $stagedOrtDir -Force | Out-Null
  $stagedOrtProvidersPath = Join-Path $stagedOrtDir "onnxruntime_providers_shared.dll"
  Copy-Item -LiteralPath $ortProvidersSharedAbs -Destination $stagedOrtProvidersPath -Force
  Print-ArtifactInfo -Label "staged_ort_providers_shared_dll" -Path $stagedOrtProvidersPath
}

Print-LoaderEvidence -AexPath $aex
if ($BuildLoaderProbe) {
  Print-LoaderEvidence -AexPath $loaderProbeAex
}

if ($CopyToMediaCore) {
  $mediaCoreOutput = Join-Path $MediaCoreDir "ZSoda.aex"
  Copy-Item -LiteralPath $aex -Destination $mediaCoreOutput -Force
  $mediaCoreOutputAbs = Resolve-AbsolutePath -Path $mediaCoreOutput
  Print-ArtifactInfo -Label "mediacore_copy" -Path $mediaCoreOutputAbs
  if ($BuildLoaderProbe) {
    $mediaCoreProbeOutput = Join-Path $MediaCoreDir "ZSodaLoaderProbe.aex"
    Copy-Item -LiteralPath $loaderProbeAex -Destination $mediaCoreProbeOutput -Force
    $mediaCoreProbeOutputAbs = Resolve-AbsolutePath -Path $mediaCoreProbeOutput
    Print-ArtifactInfo -Label "mediacore_loader_probe_copy" -Path $mediaCoreProbeOutputAbs
  }

  if ($ortRuntimeDllAbs) {
    # Deploy into isolated subdirectory to avoid same-name collision with
    # Adobe AE's preloaded onnxruntime.dll in the host process.
    $mediaCoreOrtDir = Join-Path $MediaCoreDir "zsoda_ort"
    New-Item -ItemType Directory -Path $mediaCoreOrtDir -Force | Out-Null
    $mediaCoreDllOutput = Join-Path $mediaCoreOrtDir "onnxruntime.dll"
    Copy-Item -LiteralPath $ortRuntimeDllAbs -Destination $mediaCoreDllOutput -Force
    $mediaCoreDllOutputAbs = Resolve-AbsolutePath -Path $mediaCoreDllOutput
    Print-ArtifactInfo -Label "mediacore_ort_dll" -Path $mediaCoreDllOutputAbs
    # Remove old copies from MediaCore root to avoid confusion.
    foreach ($oldName in @("onnxruntime.dll", "onnxruntime_zsoda.dll")) {
      $oldPath = Join-Path $MediaCoreDir $oldName
      if (Test-Path -LiteralPath $oldPath -PathType Leaf) {
        Remove-Item -LiteralPath $oldPath -Force -ErrorAction SilentlyContinue
        Write-Host "Removed old $oldName from MediaCore directory."
      }
    }
  } else {
    Write-Warning "MediaCore copy requested but onnxruntime.dll is unavailable."
  }
  if ($ortProvidersSharedAbs) {
    $mediaCoreOrtDir = Join-Path $MediaCoreDir "zsoda_ort"
    New-Item -ItemType Directory -Path $mediaCoreOrtDir -Force | Out-Null
    $mediaCoreProvidersOutput = Join-Path $mediaCoreOrtDir "onnxruntime_providers_shared.dll"
    Copy-Item -LiteralPath $ortProvidersSharedAbs -Destination $mediaCoreProvidersOutput -Force
    $mediaCoreProvidersOutputAbs = Resolve-AbsolutePath -Path $mediaCoreProvidersOutput
    Print-ArtifactInfo -Label "mediacore_ort_providers_shared_dll" -Path $mediaCoreProvidersOutputAbs
    # Remove old copy from MediaCore root.
    $oldProvidersPath = Join-Path $MediaCoreDir "onnxruntime_providers_shared.dll"
    if (Test-Path -LiteralPath $oldProvidersPath -PathType Leaf) {
      Remove-Item -LiteralPath $oldProvidersPath -Force -ErrorAction SilentlyContinue
      Write-Host "Removed old onnxruntime_providers_shared.dll from MediaCore directory."
    }
  } else {
    Write-Warning "MediaCore copy requested but onnxruntime_providers_shared.dll is unavailable."
  }

  $repoModelsRoot = Join-Path $repoRoot "models"
  $mediaCoreModelsRoot = Join-Path $MediaCoreDir "models"
  Sync-ModelAssets -SourceRoot $repoModelsRoot -DestinationRoot $mediaCoreModelsRoot

  if (-not $SkipDuplicateInstallCheck) {
    Assert-NoMismatchedInstalledCopies -PrimaryAexPath $mediaCoreOutputAbs -FileName "ZSoda.aex"
    if ($BuildLoaderProbe) {
      Assert-NoMismatchedInstalledCopies -PrimaryAexPath $mediaCoreProbeOutputAbs -FileName "ZSodaLoaderProbe.aex"
    }
  }
}
