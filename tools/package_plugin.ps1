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
    (Join-Path $artifactDir "zsoda_ort\onnxruntime.dll"),
    (Join-Path $artifactDir "onnxruntime.dll"),
    (Join-Path $BuildRoot "plugin\Release\zsoda_ort\onnxruntime.dll"),
    (Join-Path $BuildRoot "plugin\Release\onnxruntime.dll"),
    (Join-Path $BuildRoot "plugin\zsoda_ort\onnxruntime.dll"),
    (Join-Path $BuildRoot "plugin\onnxruntime.dll")
  )

  foreach ($candidate in $candidates) {
    if (Test-Path -LiteralPath $candidate -PathType Leaf) {
      return (Resolve-Path -LiteralPath $candidate).Path
    }
  }

  return $null
}

function Resolve-OrtProvidersSharedDllPath {
  param(
    [string]$BuildRoot,
    [string]$ArtifactFullPath,
    [string]$ResolvedOrtRuntimeDll
  )

  $artifactDir = Split-Path -Path $ArtifactFullPath -Parent
  $runtimeDir = $null
  if (-not [string]::IsNullOrWhiteSpace($ResolvedOrtRuntimeDll)) {
    $runtimeDir = Split-Path -Path $ResolvedOrtRuntimeDll -Parent
  }

  $candidates = @(
    (Join-Path $artifactDir "zsoda_ort\\onnxruntime_providers_shared.dll"),
    (Join-Path $artifactDir "onnxruntime_providers_shared.dll"),
    (Join-Path $BuildRoot "plugin\\Release\\zsoda_ort\\onnxruntime_providers_shared.dll"),
    (Join-Path $BuildRoot "plugin\\Release\\onnxruntime_providers_shared.dll"),
    (Join-Path $BuildRoot "plugin\\zsoda_ort\\onnxruntime_providers_shared.dll"),
    (Join-Path $BuildRoot "plugin\\onnxruntime_providers_shared.dll")
  )
  if (-not [string]::IsNullOrWhiteSpace($runtimeDir)) {
    $candidates += (Join-Path $runtimeDir "onnxruntime_providers_shared.dll")
  }

  foreach ($candidate in $candidates | Select-Object -Unique) {
    if (Test-Path -LiteralPath $candidate -PathType Leaf) {
      return (Resolve-Path -LiteralPath $candidate).Path
    }
  }

  return $null
}

function Resolve-PythonRuntimeDir {
  param(
    [string]$BuildRoot,
    [string]$ArtifactFullPath
  )

  $artifactDir = Split-Path -Path $ArtifactFullPath -Parent
  $candidates = @(
    (Join-Path $artifactDir "zsoda_py"),
    (Join-Path $BuildRoot "plugin\\Release\\zsoda_py"),
    (Join-Path $BuildRoot "plugin\\zsoda_py")
  )

  foreach ($candidate in $candidates | Select-Object -Unique) {
    if (Test-Path -LiteralPath $candidate -PathType Container) {
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
$ortProvidersCopiedPath = $null
$pythonRuntimeCopiedPath = $null
if ($Platform -eq "windows") {
  $resolvedOrtRuntimeDll = Resolve-OrtRuntimeDllPath -ExplicitPath $OrtRuntimeDllPath -BuildRoot $BuildDir -ArtifactFullPath $artifactPath
  if ($resolvedOrtRuntimeDll) {
    $ortOutputDir = Join-Path $OutputDir "zsoda_ort"
    New-Item -ItemType Directory -Path $ortOutputDir -Force | Out-Null
    $ortDestination = Join-Path $ortOutputDir "onnxruntime.dll"
    Copy-Item -LiteralPath $resolvedOrtRuntimeDll -Destination $ortDestination -Force
    $ortRuntimeCopiedPath = $ortDestination

    $resolvedProvidersDll = Resolve-OrtProvidersSharedDllPath -BuildRoot $BuildDir -ArtifactFullPath $artifactPath -ResolvedOrtRuntimeDll $resolvedOrtRuntimeDll
    if ($resolvedProvidersDll) {
      $providersDestination = Join-Path $ortOutputDir "onnxruntime_providers_shared.dll"
      Copy-Item -LiteralPath $resolvedProvidersDll -Destination $providersDestination -Force
      $ortProvidersCopiedPath = $providersDestination
    } else {
      Write-Warning "onnxruntime_providers_shared.dll was not resolved for Windows package output."
    }
  } else {
    $warn = "onnxruntime.dll was not resolved for Windows package output. This is expected for the default DAD-only remote build; local ORT runtime files are optional."
    if ($RequireOrtRuntimeDll) {
      throw $warn
    }
    Write-Host $warn
  }

  $resolvedPythonRuntimeDir = Resolve-PythonRuntimeDir -BuildRoot $BuildDir -ArtifactFullPath $artifactPath
  if ($resolvedPythonRuntimeDir) {
    $pythonDestination = Join-Path $OutputDir "zsoda_py"
    if (Test-Path -LiteralPath $pythonDestination) {
      Remove-Item -LiteralPath $pythonDestination -Recurse -Force
    }
    Copy-Item -LiteralPath $resolvedPythonRuntimeDir -Destination $pythonDestination -Force -Recurse
    $pythonRuntimeCopiedPath = $pythonDestination
  } else {
    Write-Warning "zsoda_py runtime directory was not resolved for Windows package output."
  }
}

if (Get-Command Get-FileHash -ErrorAction SilentlyContinue) {
  if ($Platform -eq "windows") {
    $hash = Get-FileHash -Algorithm SHA256 -LiteralPath $destination
    "$($hash.Hash.ToLowerInvariant())  $artifactName" | Out-File -FilePath (Join-Path $OutputDir "$artifactName.sha256") -Encoding ascii
    if ($ortRuntimeCopiedPath) {
      $ortHash = Get-FileHash -Algorithm SHA256 -LiteralPath $ortRuntimeCopiedPath
      "$($ortHash.Hash.ToLowerInvariant())  zsoda_ort/onnxruntime.dll" | Out-File -FilePath (Join-Path $OutputDir "onnxruntime.dll.sha256") -Encoding ascii
    }
    if ($ortProvidersCopiedPath) {
      $providersHash = Get-FileHash -Algorithm SHA256 -LiteralPath $ortProvidersCopiedPath
      "$($providersHash.Hash.ToLowerInvariant())  zsoda_ort/onnxruntime_providers_shared.dll" | Out-File -FilePath (Join-Path $OutputDir "onnxruntime_providers_shared.dll.sha256") -Encoding ascii
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
if ($ortProvidersCopiedPath) {
  Write-Host "  ort providers:  $ortProvidersCopiedPath"
} elseif ($Platform -eq "windows") {
  Write-Host "  ort providers:  (not packaged)"
}
if ($pythonRuntimeCopiedPath) {
  Write-Host "  python runtime: $pythonRuntimeCopiedPath"
} elseif ($Platform -eq "windows") {
  Write-Host "  python runtime: (not packaged)"
}
