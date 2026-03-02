param(
  [Parameter(Mandatory = $true)]
  [ValidateSet("windows", "macos")]
  [string]$Platform,

  [Parameter(Mandatory = $true)]
  [string]$BuildDir,

  [Parameter(Mandatory = $true)]
  [string]$OutputDir,

  [switch]$IncludeManifest,

  [string]$OrtRuntimeDllPath,

  [switch]$RequireOrtRuntimeDll
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Resolve-ArtifactPath {
  param(
    [string]$PlatformName,
    [string]$BuildRoot
  )

  if ($PlatformName -eq "windows") {
    $candidates = @(
      (Join-Path $BuildRoot "plugin\Release\ZSoda.aex"),
      (Join-Path $BuildRoot "plugin\ZSoda.aex")
    )
    foreach ($candidate in $candidates) {
      if (Test-Path -LiteralPath $candidate -PathType Leaf) {
        return $candidate
      }
    }
    return $null
  }

  $bundleCandidates = @(
    (Join-Path $BuildRoot "plugin\Release\ZSoda.plugin"),
    (Join-Path $BuildRoot "plugin\ZSoda.plugin")
  )
  foreach ($candidate in $bundleCandidates) {
    if (Test-Path -LiteralPath $candidate -PathType Container) {
      return $candidate
    }
  }
  return $null
}

function Resolve-OrtRuntimeDllPath {
  param(
    [string]$ExplicitPath,
    [string]$BuildRoot,
    [string]$ArtifactFullPath
  )

  if (-not [string]::IsNullOrWhiteSpace($ExplicitPath)) {
    if (Test-Path -LiteralPath $ExplicitPath -PathType Leaf) {
      return (Resolve-Path -LiteralPath $ExplicitPath).Path
    }
    Write-Warning "OrtRuntimeDllPath does not point to a file: '$ExplicitPath'."
  }

  $artifactDir = Split-Path -Path $ArtifactFullPath -Parent
  $candidates = @(
    (Join-Path $artifactDir "onnxruntime.dll"),
    (Join-Path $BuildRoot "plugin\Release\onnxruntime.dll"),
    (Join-Path $BuildRoot "plugin\onnxruntime.dll")
  )

  foreach ($candidate in $candidates) {
    if (Test-Path -LiteralPath $candidate -PathType Leaf) {
      return (Resolve-Path -LiteralPath $candidate).Path
    }
  }

  return $null
}

$artifactPath = Resolve-ArtifactPath -PlatformName $Platform -BuildRoot $BuildDir
if (-not $artifactPath) {
  throw "Artifact not found for platform '$Platform' under build dir '$BuildDir'."
}

New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
$artifactName = Split-Path -Path $artifactPath -Leaf
$destination = Join-Path $OutputDir $artifactName

if (Test-Path -LiteralPath $destination) {
  Remove-Item -LiteralPath $destination -Force -Recurse
}

Copy-Item -LiteralPath $artifactPath -Destination $destination -Force -Recurse

if ($IncludeManifest) {
  $manifestPath = "models\models.manifest"
  if (Test-Path -LiteralPath $manifestPath -PathType Leaf) {
    Copy-Item -LiteralPath $manifestPath -Destination (Join-Path $OutputDir "models.manifest") -Force
  }
}

$ortRuntimeCopiedPath = $null
if ($Platform -eq "windows") {
  $resolvedOrtRuntimeDll = Resolve-OrtRuntimeDllPath -ExplicitPath $OrtRuntimeDllPath -BuildRoot $BuildDir -ArtifactFullPath $artifactPath
  if ($resolvedOrtRuntimeDll) {
    $ortDestination = Join-Path $OutputDir "onnxruntime.dll"
    Copy-Item -LiteralPath $resolvedOrtRuntimeDll -Destination $ortDestination -Force
    $ortRuntimeCopiedPath = $ortDestination
  } else {
    $warn = "onnxruntime.dll was not resolved for Windows package output. The plugin may fail to load ORT at runtime."
    if ($RequireOrtRuntimeDll) {
      throw $warn
    }
    Write-Warning $warn
  }
}

if (Get-Command Get-FileHash -ErrorAction SilentlyContinue) {
  if ($Platform -eq "windows") {
    $hash = Get-FileHash -Algorithm SHA256 -LiteralPath $destination
    "$($hash.Hash.ToLowerInvariant())  $artifactName" | Out-File -FilePath (Join-Path $OutputDir "$artifactName.sha256") -Encoding ascii
    if ($ortRuntimeCopiedPath) {
      $ortHash = Get-FileHash -Algorithm SHA256 -LiteralPath $ortRuntimeCopiedPath
      "$($ortHash.Hash.ToLowerInvariant())  onnxruntime.dll" | Out-File -FilePath (Join-Path $OutputDir "onnxruntime.dll.sha256") -Encoding ascii
    }
  } else {
    $tempTar = Join-Path $env:TEMP "zsoda_plugin_bundle.tar"
    if (Test-Path -LiteralPath $tempTar) {
      Remove-Item -LiteralPath $tempTar -Force
    }
    tar -cf $tempTar -C $OutputDir $artifactName
    $hash = Get-FileHash -Algorithm SHA256 -LiteralPath $tempTar
    "$($hash.Hash.ToLowerInvariant())  $artifactName" | Out-File -FilePath (Join-Path $OutputDir "$artifactName.sha256") -Encoding ascii
    Remove-Item -LiteralPath $tempTar -Force
  }
}

Write-Host "Packaged artifact:"
Write-Host "  platform: $Platform"
Write-Host "  source:   $artifactPath"
Write-Host "  output:   $destination"
if ($IncludeManifest) {
  Write-Host "  manifest: $(Join-Path $OutputDir 'models.manifest')"
}
if ($ortRuntimeCopiedPath) {
  Write-Host "  ort dll:  $ortRuntimeCopiedPath"
} elseif ($Platform -eq "windows") {
  Write-Host "  ort dll:  (not packaged)"
}
