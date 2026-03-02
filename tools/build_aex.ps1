param(
  [Parameter(Mandatory = $true)]
  [string]$AeSdkIncludeDir,

  [Parameter(Mandatory = $true)]
  [string]$OrtIncludeDir,

  [Parameter(Mandatory = $true)]
  [string]$OrtLibrary,

  [string]$BuildDir = "build-win-aex",

  [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
  [string]$Config = "Release",

  [string]$Generator = "Visual Studio 17 2022",

  [ValidateSet("Win32", "x64", "ARM64")]
  [string]$Platform = "x64",

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

function Print-ArtifactInfo {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Label,

    [Parameter(Mandatory = $true)]
    [string]$Path
  )

  $hash = Get-FileHash -Algorithm SHA256 -LiteralPath $Path
  Write-Host "$Label:"
  Write-Host "  path:   $Path"
  Write-Host "  sha256: $($hash.Hash.ToLowerInvariant())"
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

if ($Clean -and (Test-Path -LiteralPath $buildDirAbs)) {
  Remove-Item -LiteralPath $buildDirAbs -Recurse -Force
}
New-Item -ItemType Directory -Path $buildDirAbs -Force | Out-Null

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
  "-DZSODA_WITH_ONNX_RUNTIME_API=ON",
  "-DAE_SDK_INCLUDE_DIR=$aeSdkIncludeDirAbs",
  "-DONNXRUNTIME_INCLUDE_DIR=$ortIncludeDirAbs",
  "-DONNXRUNTIME_LIBRARY=$ortLibraryAbs"
)
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

Write-Host "==> Build Succeeded"
Print-ArtifactInfo -Label "zsoda_plugin" -Path $pluginLib
Print-ArtifactInfo -Label "zsoda_aex" -Path $aex

if ($CopyToMediaCore) {
  $mediaCoreOutput = Join-Path $MediaCoreDir "ZSoda.aex"
  Copy-Item -LiteralPath $aex -Destination $mediaCoreOutput -Force
  $mediaCoreOutputAbs = Resolve-AbsolutePath -Path $mediaCoreOutput
  Print-ArtifactInfo -Label "mediacore_copy" -Path $mediaCoreOutputAbs
}
