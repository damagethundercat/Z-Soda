param(
  [Parameter(Mandatory = $true)]
  [ValidateSet("windows", "macos")]
  [string]$Platform,

  [Parameter(Mandatory = $true)]
  [string]$BuildDir,

  [Parameter(Mandatory = $true)]
  [string]$OutputDir,

  [switch]$IncludeManifest,

  [string]$PythonRuntimeDir = "",

  [string]$ModelRepoDir = "",

  [string]$HfCacheDir = "",

  [switch]$RequireSelfContained,

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

function Resolve-OrtRuntimeDir {
  param(
    [string]$BuildRoot,
    [string]$ArtifactFullPath
  )

  $artifactDir = Split-Path -Path $ArtifactFullPath -Parent
  $candidates = @(
    (Join-Path $artifactDir "zsoda_ort"),
    (Join-Path $BuildRoot "plugin\Release\zsoda_ort"),
    (Join-Path $BuildRoot "plugin\zsoda_ort")
  )

  foreach ($candidate in $candidates | Select-Object -Unique) {
    if (Test-Path -LiteralPath $candidate -PathType Container) {
      return (Resolve-Path -LiteralPath $candidate).Path
    }
  }

  return $null
}

function Copy-ReplacedDirectory {
  param(
    [Parameter(Mandatory = $true)]
    [string]$SourceDir,

    [Parameter(Mandatory = $true)]
    [string]$DestinationDir
  )

  if (Test-Path -LiteralPath $DestinationDir) {
    Remove-Item -LiteralPath $DestinationDir -Recurse -Force
  }

  $parentDir = Split-Path -Path $DestinationDir -Parent
  if (-not [string]::IsNullOrWhiteSpace($parentDir)) {
    New-Item -ItemType Directory -Path $parentDir -Force | Out-Null
  }

  Copy-Item -LiteralPath $SourceDir -Destination $DestinationDir -Force -Recurse
}

function Copy-DirectoryContents {
  param(
    [Parameter(Mandatory = $true)]
    [string]$SourceDir,

    [Parameter(Mandatory = $true)]
    [string]$DestinationDir
  )

  New-Item -ItemType Directory -Path $DestinationDir -Force | Out-Null
  $entries = Get-ChildItem -LiteralPath $SourceDir -Force
  foreach ($entry in $entries) {
    Copy-Item -LiteralPath $entry.FullName -Destination $DestinationDir -Force -Recurse
  }
}

function Stage-ModelRepos {
  param(
    [Parameter(Mandatory = $true)]
    [string]$SourceRoot,

    [Parameter(Mandatory = $true)]
    [string]$DestinationRoot
  )

  New-Item -ItemType Directory -Path $DestinationRoot -Force | Out-Null
  $repoDirs = Get-ChildItem -LiteralPath $SourceRoot -Directory -Force
  if (-not $repoDirs) {
    throw "Model repo directory does not contain any model subdirectories: $SourceRoot"
  }

  foreach ($repoDir in $repoDirs) {
    $repoDestination = Join-Path $DestinationRoot $repoDir.Name
    if (Test-Path -LiteralPath $repoDestination) {
      Remove-Item -LiteralPath $repoDestination -Recurse -Force
    }
    Copy-DirectoryContents -SourceDir $repoDir.FullName -DestinationDir $repoDestination
  }
}

function Stage-ModelsMetadata {
  param(
    [Parameter(Mandatory = $true)]
    [string]$DestinationRoot
  )

  New-Item -ItemType Directory -Path $DestinationRoot -Force | Out-Null
  foreach ($metadataName in @("models.manifest", "README.md")) {
    $sourcePath = Join-Path "models" $metadataName
    if (Test-Path -LiteralPath $sourcePath -PathType Leaf) {
      Copy-Item -LiteralPath $sourcePath -Destination (Join-Path $DestinationRoot $metadataName) -Force
    }
  }
}

function Assert-SelfContainedPayload {
  param(
    [Parameter(Mandatory = $true)]
    [string]$StageRoot,

    [Parameter(Mandatory = $true)]
    [ValidateSet("windows", "macos")]
    [string]$PlatformName
  )

  $pythonStageDir = Join-Path $StageRoot "zsoda_py"
  $serviceScript = Join-Path $pythonStageDir "distill_any_depth_remote_service.py"
  if (-not (Test-Path -LiteralPath $serviceScript -PathType Leaf)) {
    throw "Self-contained packaging requires bundled service script: $serviceScript"
  }

  $pythonCandidates = if ($PlatformName -eq "windows") {
    @(
      (Join-Path $pythonStageDir "python.exe"),
      (Join-Path $pythonStageDir "python\python.exe"),
      (Join-Path $pythonStageDir "runtime\python.exe")
    )
  } else {
    @(
      (Join-Path $pythonStageDir "bin/python3"),
      (Join-Path $pythonStageDir "python/bin/python3"),
      (Join-Path $pythonStageDir "runtime/bin/python3")
    )
  }

  if (-not ($pythonCandidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1)) {
    throw "Self-contained packaging requires a bundled Python runtime under zsoda_py/."
  }

  $modelsHfDir = Join-Path $StageRoot "models\hf"
  if (-not (Test-Path -LiteralPath $modelsHfDir -PathType Container)) {
    throw "Self-contained packaging requires bundled local model repos under models/hf/."
  }

  $repoDirs = Get-ChildItem -LiteralPath $modelsHfDir -Directory -Force
  if (-not $repoDirs) {
    throw "Self-contained packaging requires at least one local model repo under models/hf/."
  }
}

function Resolve-PayloadPythonCommand {
  foreach ($candidate in @("py", "python", "python3")) {
    $command = Get-Command $candidate -ErrorAction SilentlyContinue
    if ($command) {
      return $command.Source
    }
  }
  return $null
}

$artifactPath = Resolve-ArtifactPath -PlatformName $Platform -BuildRoot $BuildDir
if (-not $artifactPath) {
  throw "Artifact not found for platform '$Platform' under build dir '$BuildDir'."
}

if ([string]::IsNullOrWhiteSpace($ModelRepoDir)) {
  $defaultModelRepoDir = "release-assets\models"
  if (Test-Path -LiteralPath $defaultModelRepoDir -PathType Container) {
    $ModelRepoDir = (Resolve-Path -LiteralPath $defaultModelRepoDir).Path
  }
}
if ([string]::IsNullOrWhiteSpace($HfCacheDir)) {
  $defaultHfCacheDir = "release-assets\hf-cache"
  if (Test-Path -LiteralPath $defaultHfCacheDir -PathType Container) {
    $HfCacheDir = (Resolve-Path -LiteralPath $defaultHfCacheDir).Path
  }
}
if ([string]::IsNullOrWhiteSpace($PythonRuntimeDir)) {
  $defaultPythonRuntimeDir = if ($Platform -eq "windows") {
    "release-assets\python-win"
  } else {
    "release-assets\python-macos"
  }
  if (Test-Path -LiteralPath $defaultPythonRuntimeDir -PathType Container) {
    $PythonRuntimeDir = (Resolve-Path -LiteralPath $defaultPythonRuntimeDir).Path
  }
}

New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
$artifactName = Split-Path -Path $artifactPath -Leaf
$destination = Join-Path $OutputDir $artifactName

if (Test-Path -LiteralPath $destination) {
  Remove-Item -LiteralPath $destination -Force -Recurse
}

Copy-Item -LiteralPath $artifactPath -Destination $destination -Force -Recurse

$packageResourceRoot = $OutputDir
if ($Platform -eq "macos") {
  $packageResourceRoot = Join-Path $destination "Contents/Resources"
  New-Item -ItemType Directory -Path $packageResourceRoot -Force | Out-Null
}

$payloadStageDir = Join-Path $OutputDir ".payload-stage"
if (Test-Path -LiteralPath $payloadStageDir) {
  Remove-Item -LiteralPath $payloadStageDir -Recurse -Force
}
New-Item -ItemType Directory -Path $payloadStageDir -Force | Out-Null

$modelsCopiedPath = $null
$embeddedPayloadRoots = New-Object System.Collections.Generic.List[string]
$embeddedPayloadStatus = $null
if ($IncludeManifest) {
  $modelsSourceDir = "models"
  if (Test-Path -LiteralPath $modelsSourceDir -PathType Container) {
    $modelsStageDir = Join-Path $payloadStageDir "models"
    if (Test-Path -LiteralPath $modelsStageDir) {
      Remove-Item -LiteralPath $modelsStageDir -Recurse -Force
    }
    Stage-ModelsMetadata -DestinationRoot $modelsStageDir
    $modelsCopiedPath = $modelsStageDir
  }
}

if (-not [string]::IsNullOrWhiteSpace($ModelRepoDir)) {
  if (-not (Test-Path -LiteralPath $ModelRepoDir -PathType Container)) {
    throw "Model repo directory was not found: $ModelRepoDir"
  }
  $modelsStageDir = Join-Path $payloadStageDir "models"
  Stage-ModelRepos -SourceRoot $ModelRepoDir -DestinationRoot (Join-Path $modelsStageDir "hf")
  $modelsCopiedPath = $modelsStageDir
}

if (-not [string]::IsNullOrWhiteSpace($HfCacheDir)) {
  if (-not (Test-Path -LiteralPath $HfCacheDir -PathType Container)) {
    throw "HF cache directory was not found: $HfCacheDir"
  }
  $modelsStageDir = Join-Path $payloadStageDir "models"
  Copy-ReplacedDirectory -SourceDir $HfCacheDir -DestinationDir (Join-Path $modelsStageDir "hf-cache")
  $modelsCopiedPath = $modelsStageDir
}

$ortRuntimeCopiedPath = $null
$ortRuntimeDllCopiedPath = $null
$ortProvidersCopiedPath = $null
$pythonRuntimeCopiedPath = $null
$archivePath = $null
$archiveShaPath = $null
$resolvedOrtRuntimeDir = Resolve-OrtRuntimeDir -BuildRoot $BuildDir -ArtifactFullPath $artifactPath
if ($resolvedOrtRuntimeDir) {
  $ortStageDir = Join-Path $payloadStageDir "zsoda_ort"
  Copy-ReplacedDirectory -SourceDir $resolvedOrtRuntimeDir -DestinationDir $ortStageDir
  $ortRuntimeCopiedPath = $ortStageDir
} elseif ($Platform -eq "windows") {
  $warn = "zsoda_ort runtime directory was not resolved for package output. This is expected for the default DAD-only remote build; local ORT runtime files are optional."
  if ($RequireOrtRuntimeDll) {
    throw $warn
  }
  Write-Host $warn
}

$resolvedPythonRuntimeDir = Resolve-PythonRuntimeDir -BuildRoot $BuildDir -ArtifactFullPath $artifactPath
if ($resolvedPythonRuntimeDir) {
  $pythonStageDir = Join-Path $payloadStageDir "zsoda_py"
  Copy-ReplacedDirectory -SourceDir $resolvedPythonRuntimeDir -DestinationDir $pythonStageDir
  $pythonRuntimeCopiedPath = $pythonStageDir
} else {
  $serviceScript = Join-Path "tools" "distill_any_depth_remote_service.py"
  if (Test-Path -LiteralPath $serviceScript -PathType Leaf) {
    $pythonStageDir = Join-Path $payloadStageDir "zsoda_py"
    New-Item -ItemType Directory -Path $pythonStageDir -Force | Out-Null
    Copy-Item -LiteralPath $serviceScript -Destination (Join-Path $pythonStageDir "distill_any_depth_remote_service.py") -Force
    $pythonRuntimeCopiedPath = $pythonStageDir
  } elseif ($Platform -eq "windows") {
    Write-Warning "zsoda_py runtime directory was not resolved for package output."
  }
}

if (-not [string]::IsNullOrWhiteSpace($PythonRuntimeDir)) {
  if (-not (Test-Path -LiteralPath $PythonRuntimeDir -PathType Container)) {
    throw "Python runtime directory was not found: $PythonRuntimeDir"
  }
  $pythonStageDir = Join-Path $payloadStageDir "zsoda_py"
  New-Item -ItemType Directory -Path $pythonStageDir -Force | Out-Null
  Copy-ReplacedDirectory -SourceDir $PythonRuntimeDir -DestinationDir (Join-Path $pythonStageDir "python")
  $pythonRuntimeCopiedPath = $pythonStageDir
}

if ($RequireSelfContained) {
  Assert-SelfContainedPayload -StageRoot $payloadStageDir -PlatformName $Platform
}

foreach ($stagedRoot in @("models", "zsoda_ort", "zsoda_py")) {
  $stagedPath = Join-Path $payloadStageDir $stagedRoot
  if (-not (Test-Path -LiteralPath $stagedPath -PathType Container)) {
    continue
  }

  if ($Platform -eq "windows") {
    [void]$embeddedPayloadRoots.Add($stagedPath)
    continue
  }

  $resourceDestination = Join-Path $packageResourceRoot $stagedRoot
  Copy-ReplacedDirectory -SourceDir $stagedPath -DestinationDir $resourceDestination
  switch ($stagedRoot) {
    "models" {
      $modelsCopiedPath = $resourceDestination
    }
    "zsoda_ort" {
      $ortRuntimeCopiedPath = $resourceDestination
      $runtimeDllCandidate = Join-Path $resourceDestination "onnxruntime.dll"
      if (Test-Path -LiteralPath $runtimeDllCandidate -PathType Leaf) {
        $ortRuntimeDllCopiedPath = $runtimeDllCandidate
      }
      $providersCandidate = Join-Path $resourceDestination "onnxruntime_providers_shared.dll"
      if (Test-Path -LiteralPath $providersCandidate -PathType Leaf) {
        $ortProvidersCopiedPath = $providersCandidate
      }
    }
    "zsoda_py" {
      $pythonRuntimeCopiedPath = $resourceDestination
    }
  }
}

if ($Platform -eq "windows" -and $embeddedPayloadRoots.Count -gt 0) {
  $payloadPython = Resolve-PayloadPythonCommand
  if (-not $payloadPython) {
    throw "Python is required to build embedded Windows payloads."
  }
  $payloadTool = Join-Path "tools" "build_embedded_payload.py"
  if (-not (Test-Path -LiteralPath $payloadTool -PathType Leaf)) {
    throw "Payload builder script not found: $payloadTool"
  }

  $payloadArgs = @($payloadTool, "--artifact", $destination)
  foreach ($payloadRoot in $embeddedPayloadRoots) {
    $payloadArgs += @("--root", $payloadRoot)
  }

  if ((Split-Path -Leaf $payloadPython).ToLowerInvariant() -eq "py.exe" -or
      (Split-Path -Leaf $payloadPython).ToLowerInvariant() -eq "py") {
    & $payloadPython -3 @payloadArgs
  } else {
    & $payloadPython @payloadArgs
  }
  $embeddedPayloadStatus = "embedded $($embeddedPayloadRoots.Count) root(s) into $destination"
}

if ($Platform -eq "macos") {
  $codesign = Get-Command codesign -ErrorAction SilentlyContinue
  if ($codesign) {
    & $codesign.Source --force --sign - --timestamp=none --deep $destination
  }
}

if ($Platform -eq "windows") {
  $archivePath = Join-Path $OutputDir "ZSoda-windows.zip"
  if (Test-Path -LiteralPath $archivePath) {
    Remove-Item -LiteralPath $archivePath -Force
  }

  $tar = Get-Command tar.exe -ErrorAction SilentlyContinue
  if ($null -eq $tar) {
    $tar = Get-Command tar -ErrorAction SilentlyContinue
  }

  if ($tar) {
    $archiveLeaf = Split-Path -Path $archivePath -Leaf
    Push-Location $OutputDir
    try {
      & $tar.Source -a -cf $archiveLeaf $artifactName
      if ($LASTEXITCODE -ne 0) {
        throw "tar failed to create archive: $archivePath"
      }
    } finally {
      Pop-Location
    }
  } elseif (Get-Command Compress-Archive -ErrorAction SilentlyContinue) {
    Compress-Archive -LiteralPath $destination -DestinationPath $archivePath -Force
  }
} elseif (Get-Command Compress-Archive -ErrorAction SilentlyContinue) {
  $archivePath = Join-Path $OutputDir "ZSoda-macos.zip"
  if (Test-Path -LiteralPath $archivePath) {
    Remove-Item -LiteralPath $archivePath -Force
  }
  Compress-Archive -LiteralPath $destination -DestinationPath $archivePath -Force
}

if (Get-Command Get-FileHash -ErrorAction SilentlyContinue) {
  if ($Platform -eq "windows") {
    $hash = Get-FileHash -Algorithm SHA256 -LiteralPath $destination
    "$($hash.Hash.ToLowerInvariant())  $artifactName" | Out-File -FilePath (Join-Path $OutputDir "$artifactName.sha256") -Encoding ascii
    if ($ortRuntimeDllCopiedPath) {
      $ortHash = Get-FileHash -Algorithm SHA256 -LiteralPath $ortRuntimeDllCopiedPath
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
  if ($archivePath -and (Test-Path -LiteralPath $archivePath -PathType Leaf)) {
    $archiveHash = Get-FileHash -Algorithm SHA256 -LiteralPath $archivePath
    $archiveShaPath = Join-Path $OutputDir "$(Split-Path -Path $archivePath -Leaf).sha256"
    "$($archiveHash.Hash.ToLowerInvariant())  $(Split-Path -Path $archivePath -Leaf)" |
      Out-File -FilePath $archiveShaPath -Encoding ascii
  }
}

Write-Host "Packaged artifact:"
Write-Host "  platform: $Platform"
Write-Host "  source:   $artifactPath"
Write-Host "  output:   $destination"
if ($modelsCopiedPath) {
  if ($Platform -eq "windows") {
    Write-Host "  models:   embedded"
  } else {
    Write-Host "  models:   $modelsCopiedPath"
  }
}
if ($ortRuntimeCopiedPath) {
  if ($Platform -eq "windows") {
    Write-Host "  ort dir:  embedded"
  } else {
    Write-Host "  ort dir:  $ortRuntimeCopiedPath"
  }
} elseif ($Platform -eq "windows") {
  Write-Host "  ort dir:  (not packaged)"
}
if ($ortProvidersCopiedPath) {
  Write-Host "  ort providers:  $ortProvidersCopiedPath"
} elseif ($Platform -eq "windows") {
  Write-Host "  ort providers:  (not packaged)"
}
if ($pythonRuntimeCopiedPath) {
  if ($Platform -eq "windows") {
    Write-Host "  python runtime: embedded"
  } else {
    Write-Host "  python runtime: $pythonRuntimeCopiedPath"
  }
} elseif ($Platform -eq "windows" -or $Platform -eq "macos") {
  Write-Host "  python runtime: (not packaged)"
}
if ($embeddedPayloadStatus) {
  Write-Host "  embedded: $embeddedPayloadStatus"
}
if ($archivePath -and (Test-Path -LiteralPath $archivePath -PathType Leaf)) {
  Write-Host "  archive:  $archivePath"
}
if ($archiveShaPath -and (Test-Path -LiteralPath $archiveShaPath -PathType Leaf)) {
  Write-Host "  archive sha256: $archiveShaPath"
}

if ($payloadStageDir -and (Test-Path -LiteralPath $payloadStageDir)) {
  Remove-Item -LiteralPath $payloadStageDir -Recurse -Force
}
