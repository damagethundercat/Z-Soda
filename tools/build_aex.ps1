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

  [string]$MediaCoreDir
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
    [string]$PiPlRrPath
  )

  Assert-Path -Path $PiPlRrPath -PathType Leaf -Message "PiPL RR not found: $PiPlRrPath"
  $content = Get-Content -LiteralPath $PiPlRrPath -Raw
  $requiredTokens = @(
    "CodeWin64X86",
    "EffectMain",
    "AE_Effect_Global_OutFlags",
    "0x04008120"
  )

  foreach ($token in $requiredTokens) {
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
  $lines += "  - pipl_rr literal tokens: CodeWin64X86/EffectMain/AE_Effect_Global_OutFlags/0x04008120"
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

if ($Clean -and (Test-Path -LiteralPath $buildDirAbs)) {
  Remove-Item -LiteralPath $buildDirAbs -Recurse -Force
}
New-Item -ItemType Directory -Path $buildDirAbs -Force | Out-Null

if (-not $EnableOrtApi) {
  Write-Host "ORT mode: structural-safe (ZSODA_WITH_ONNX_RUNTIME_API=OFF)"
}
if ($EnableOrtApi -and $OrtDirectLinkMode -eq "OFF") {
  Write-Host "ORT mode: API enabled with structural dynamic-loader path (direct link OFF)."
}
if (-not $EnableOrtApi -and $OrtDirectLinkMode -eq "ON") {
  Write-Warning "OrtDirectLinkMode=ON is ignored because EnableOrtApi is not set."
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
  "-DZSODA_WITH_ONNX_RUNTIME=ON",
  "-DZSODA_WITH_ONNX_RUNTIME_API=$([int]$EnableOrtApi.IsPresent)",
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

Assert-RrSignature -PiPlRrPath $piplRr
Assert-AexLoaderSignature -AexPath $aex -Platform $Platform

$aexDir = Split-Path -Path $aex -Parent
$loaderSummaryPath = Join-Path $aexDir "ZSoda.loader_check.txt"
$summaryLines = Build-LoaderCheckSummary -AexPath $aex -PiPlRrPath $piplRr -PiPlRcPath $piplRc -PiPlRrcPath $piplRrc
Write-LoaderCheckSummary -SummaryPath $loaderSummaryPath -Lines $summaryLines

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

$stagedOrtRuntimePath = $null
if ($ortRuntimeDllAbs) {
  $stagedOrtRuntimePath = Join-Path $aexDir "onnxruntime.dll"
  Copy-Item -LiteralPath $ortRuntimeDllAbs -Destination $stagedOrtRuntimePath -Force
  Print-ArtifactInfo -Label "staged_ort_dll" -Path $stagedOrtRuntimePath
} else {
  Write-Warning "onnxruntime.dll was not staged next to ZSoda.aex."
}

Print-LoaderEvidence -AexPath $aex

if ($CopyToMediaCore) {
  $mediaCoreOutput = Join-Path $MediaCoreDir "ZSoda.aex"
  Copy-Item -LiteralPath $aex -Destination $mediaCoreOutput -Force
  $mediaCoreOutputAbs = Resolve-AbsolutePath -Path $mediaCoreOutput
  Print-ArtifactInfo -Label "mediacore_copy" -Path $mediaCoreOutputAbs

  if ($ortRuntimeDllAbs) {
    $mediaCoreDllOutput = Join-Path $MediaCoreDir "onnxruntime.dll"
    Copy-Item -LiteralPath $ortRuntimeDllAbs -Destination $mediaCoreDllOutput -Force
    $mediaCoreDllOutputAbs = Resolve-AbsolutePath -Path $mediaCoreDllOutput
    Print-ArtifactInfo -Label "mediacore_ort_dll" -Path $mediaCoreDllOutputAbs
  } else {
    Write-Warning "MediaCore copy requested but onnxruntime.dll is unavailable."
  }
}
