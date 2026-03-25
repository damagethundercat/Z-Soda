param(
  [Parameter(Mandatory = $true)]
  [ValidateSet("windows", "macos")]
  [string]$Platform,

  [Parameter(Mandatory = $true)]
  [string]$BuildDir,

  [Parameter(Mandatory = $true)]
  [string]$OutputDir,

  [switch]$IncludeManifest,

  [ValidateSet("embedded-windows", "sidecar-ort")]
  [string]$PackageMode = "embedded-windows",

  [string]$PythonRuntimeDir = "",

  [string]$ModelRepoDir = "",

  [string]$ModelRootDir = "",

  [string]$HfCacheDir = "",

  [switch]$RequireSelfContained,

  [string]$GoldenMacFixture = "",

  [string]$OrtRuntimeDllPath,

  [switch]$RequireOrtRuntimeDll
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

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
function Resolve-PayloadPythonCommand {
  foreach ($candidate in @("py", "python", "python3")) {
    $command = Get-Command $candidate -ErrorAction SilentlyContinue
    if ($command) {
      return $command.Source
    }
  }
  return $null
}

function Invoke-PythonScript {
  param(
    [Parameter(Mandatory = $true)]
    [string]$PythonCommand,

    [Parameter(Mandatory = $true)]
    [string[]]$Arguments,

    [Parameter(Mandatory = $true)]
    [string]$FailureMessage
  )

  $pythonLeaf = (Split-Path -Leaf $PythonCommand).ToLowerInvariant()
  if ($pythonLeaf -eq "py.exe" -or $pythonLeaf -eq "py") {
    & $PythonCommand -3 @Arguments
  } else {
    & $PythonCommand @Arguments
  }
  if ($LASTEXITCODE -ne 0) {
    throw "$FailureMessage (exit code: $LASTEXITCODE)"
  }
}

$payloadPython = Resolve-PayloadPythonCommand
if (-not $payloadPython) {
  throw "Python is required to prepare package staging layout."
}

$stagePrepTool = Join-Path "tools" "prepare_package_stage.py"
if (-not (Test-Path -LiteralPath $stagePrepTool -PathType Leaf)) {
  throw "Package stage preparation script not found: $stagePrepTool"
}

New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
$stagePlanPath = Join-Path $OutputDir ".package-stage.json"
$stagePrepArgs = @(
  $stagePrepTool,
  "--platform",
  $Platform,
  "--package-mode",
  $PackageMode,
  "--build-dir",
  $BuildDir,
  "--output-dir",
  $OutputDir,
  "--plan-out",
  $stagePlanPath,
  "--quiet"
)
if ($IncludeManifest) {
  $stagePrepArgs += "--include-manifest"
}
if (-not [string]::IsNullOrWhiteSpace($PythonRuntimeDir)) {
  $stagePrepArgs += @("--python-runtime-dir", $PythonRuntimeDir)
}
if (-not [string]::IsNullOrWhiteSpace($ModelRepoDir)) {
  $stagePrepArgs += @("--model-repo-dir", $ModelRepoDir)
}
if (-not [string]::IsNullOrWhiteSpace($ModelRootDir)) {
  $stagePrepArgs += @("--model-root-dir", $ModelRootDir)
}
if (-not [string]::IsNullOrWhiteSpace($HfCacheDir)) {
  $stagePrepArgs += @("--hf-cache-dir", $HfCacheDir)
}
if (-not [string]::IsNullOrWhiteSpace($OrtRuntimeDllPath)) {
  if (-not (Test-Path -LiteralPath $OrtRuntimeDllPath -PathType Leaf)) {
    throw "OrtRuntimeDllPath does not point to a file: '$OrtRuntimeDllPath'."
  }
  $stagePrepArgs += @("--ort-runtime-dir", (Split-Path -Path (Resolve-Path -LiteralPath $OrtRuntimeDllPath).Path -Parent))
}
if ($RequireSelfContained) {
  $stagePrepArgs += "--require-self-contained"
}

Invoke-PythonScript -PythonCommand $payloadPython -Arguments $stagePrepArgs -FailureMessage "Package stage preparation failed"

$stagePlan = Get-Content -LiteralPath $stagePlanPath -Raw | ConvertFrom-Json
$artifactPath = $stagePlan.artifact_source
$artifactName = $stagePlan.artifact_name
$payloadStageDir = $stagePlan.payload_stage_dir
$packageRootDirName = [string]$stagePlan.package_root_dir_name
$packageOutputRoot = if ([string]::IsNullOrWhiteSpace($packageRootDirName)) {
  $OutputDir
} else {
  Join-Path $OutputDir $packageRootDirName
}
New-Item -ItemType Directory -Path $packageOutputRoot -Force | Out-Null

if ($RequireSelfContained -and $PackageMode -ne "sidecar-ort" -and
    $stagePlan.staged_root_paths.PSObject.Properties["zsoda_py"] -and
    $stagePlan.staged_root_paths.PSObject.Properties["models"]) {
  $runtimeValidationTool = Join-Path "tools" "validate_self_contained_runtime.py"
  if (-not (Test-Path -LiteralPath $runtimeValidationTool -PathType Leaf)) {
    throw "Self-contained runtime validation script not found: $runtimeValidationTool"
  }
  $runtimeValidationArgs = @(
    $runtimeValidationTool,
    "--stage-root",
    $payloadStageDir,
    "--platform",
    $Platform,
    "--model-id",
    "distill-any-depth-base",
    "--validate-device",
    "cpu"
  )
  Invoke-PythonScript -PythonCommand $payloadPython -Arguments $runtimeValidationArgs -FailureMessage "Bundled runtime validation failed"
}

foreach ($warning in @($stagePlan.warnings)) {
  if (-not [string]::IsNullOrWhiteSpace([string]$warning)) {
    Write-Host "warning: $warning"
  }
}

if ($RequireOrtRuntimeDll -and (-not $stagePlan.staged_root_paths.PSObject.Properties["zsoda_ort"])) {
  throw "zsoda_ort runtime directory was not resolved for package output."
}

$destination = Join-Path $packageOutputRoot $artifactName
if (Test-Path -LiteralPath $destination) {
  Remove-Item -LiteralPath $destination -Force -Recurse
}
Copy-Item -LiteralPath $artifactPath -Destination $destination -Force -Recurse

$packageResourceRoot = if ([string]::IsNullOrWhiteSpace($stagePlan.package_resource_subdir)) {
  $packageOutputRoot
} else {
  Join-Path $destination $stagePlan.package_resource_subdir
}
if ($Platform -eq "macos") {
  New-Item -ItemType Directory -Path $packageResourceRoot -Force | Out-Null
}

$modelsCopiedPath = $null
$embeddedPayloadRoots = New-Object System.Collections.Generic.List[string]
$embeddedPayloadStatus = $null

$ortRuntimeCopiedPath = $null
$ortRuntimeDllCopiedPath = $null
$ortProvidersCopiedPath = $null
$pythonRuntimeCopiedPath = $null
$archivePath = $null
$archiveShaPath = $null
$archiveEntries = New-Object System.Collections.Generic.List[string]
$artifactHashLabel = if ([string]::IsNullOrWhiteSpace($packageRootDirName)) {
  $artifactName
} else {
  ($packageRootDirName + "\" + $artifactName).Replace('\', '/')
}
if ([string]::IsNullOrWhiteSpace($packageRootDirName)) {
  [void]$archiveEntries.Add($artifactName)
} else {
  [void]$archiveEntries.Add($packageRootDirName)
}
foreach ($stagedRoot in @($stagePlan.staged_roots)) {
  $stagedPath = [string]$stagePlan.staged_root_paths.PSObject.Properties[$stagedRoot].Value
  if ([string]::IsNullOrWhiteSpace($stagedPath) -or -not (Test-Path -LiteralPath $stagedPath -PathType Container)) {
    continue
  }

  if ($Platform -eq "windows" -and $PackageMode -eq "embedded-windows") {
    [void]$embeddedPayloadRoots.Add($stagedPath)
    switch ($stagedRoot) {
      "models" { $modelsCopiedPath = $stagedPath }
      "zsoda_ort" { $ortRuntimeCopiedPath = $stagedPath }
      "zsoda_py" { $pythonRuntimeCopiedPath = $stagedPath }
    }
    continue
  }

  $resourceDestination = if ($Platform -eq "windows") {
    Join-Path $packageOutputRoot $stagedRoot
  } else {
    Join-Path $packageResourceRoot $stagedRoot
  }
  Copy-ReplacedDirectory -SourceDir $stagedPath -DestinationDir $resourceDestination
  if ($Platform -eq "windows" -and [string]::IsNullOrWhiteSpace($packageRootDirName)) {
    [void]$archiveEntries.Add($stagedRoot)
  }
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

if ($Platform -eq "windows" -and $PackageMode -eq "embedded-windows" -and $embeddedPayloadRoots.Count -gt 0) {
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

  Invoke-PythonScript -PythonCommand $payloadPython -Arguments $payloadArgs -FailureMessage "Embedded payload build failed"
  $embeddedPayloadStatus = "embedded $($embeddedPayloadRoots.Count) root(s) into $destination"

  $payloadValidationTool = Join-Path "tools" "check_release_readiness.py"
  if (-not (Test-Path -LiteralPath $payloadValidationTool -PathType Leaf)) {
    throw "Payload validation script not found: $payloadValidationTool"
  }

  $payloadValidationArgs = @(
    $payloadValidationTool,
    "--inspect-windows-artifact",
    $destination,
    "--require-self-contained"
  )
  if (-not [string]::IsNullOrWhiteSpace($GoldenMacFixture)) {
    if (-not (Test-Path -LiteralPath $GoldenMacFixture)) {
      throw "Golden macOS fixture was not found: $GoldenMacFixture"
    }
    $payloadValidationArgs += @(
      "--compare-windows-artifact-to-macos-fixture",
      $GoldenMacFixture
    )
  }
  Invoke-PythonScript -PythonCommand $payloadPython -Arguments $payloadValidationArgs -FailureMessage "Embedded payload validation failed"
  $embeddedPayloadStatus = "$embeddedPayloadStatus; validation passed"
}

if ($Platform -eq "macos") {
  $codesign = Get-Command codesign -ErrorAction SilentlyContinue
  if ($codesign) {
    & $codesign.Source --force --sign - --timestamp=none --deep $destination
  }
}

if ($Platform -eq "windows") {
  $archivePath = Join-Path $OutputDir $stagePlan.archive_name
  if (Test-Path -LiteralPath $archivePath) {
    Remove-Item -LiteralPath $archivePath -Force
  }

  $tar = Get-Command tar.exe -ErrorAction SilentlyContinue
  if ($null -eq $tar) {
    $tar = Get-Command tar -ErrorAction SilentlyContinue
  }

  if ($tar) {
    $archiveLeaf = Split-Path -Path $archivePath -Leaf
    $archiveInputPaths = @($archiveEntries)
    Push-Location $OutputDir
    try {
      & $tar.Source -a -cf $archiveLeaf @archiveInputPaths
      if ($LASTEXITCODE -ne 0) {
        throw "tar failed to create archive: $archivePath"
      }
    } finally {
      Pop-Location
    }
  } elseif (Get-Command Compress-Archive -ErrorAction SilentlyContinue) {
    $literalPaths = @($archiveEntries | ForEach-Object { Join-Path $OutputDir $_ })
    Compress-Archive -LiteralPath $literalPaths -DestinationPath $archivePath -Force
  }
} elseif (Get-Command Compress-Archive -ErrorAction SilentlyContinue) {
  $archivePath = Join-Path $OutputDir $stagePlan.archive_name
  if (Test-Path -LiteralPath $archivePath) {
    Remove-Item -LiteralPath $archivePath -Force
  }
  Compress-Archive -LiteralPath $destination -DestinationPath $archivePath -Force
}

if (Get-Command Get-FileHash -ErrorAction SilentlyContinue) {
  if ($Platform -eq "windows") {
    $hash = Get-FileHash -Algorithm SHA256 -LiteralPath $destination
    "$($hash.Hash.ToLowerInvariant())  $artifactHashLabel" | Out-File -FilePath (Join-Path $OutputDir "$artifactName.sha256") -Encoding ascii
    if ($ortRuntimeDllCopiedPath) {
      $ortHash = Get-FileHash -Algorithm SHA256 -LiteralPath $ortRuntimeDllCopiedPath
      $ortHashLabel = if ([string]::IsNullOrWhiteSpace($packageRootDirName)) {
        "zsoda_ort/onnxruntime.dll"
      } else {
        ($packageRootDirName + "\zsoda_ort\onnxruntime.dll").Replace('\', '/')
      }
      "$($ortHash.Hash.ToLowerInvariant())  $ortHashLabel" | Out-File -FilePath (Join-Path $OutputDir "onnxruntime.dll.sha256") -Encoding ascii
    }
    if ($ortProvidersCopiedPath) {
      $providersHash = Get-FileHash -Algorithm SHA256 -LiteralPath $ortProvidersCopiedPath
      $providersHashLabel = if ([string]::IsNullOrWhiteSpace($packageRootDirName)) {
        "zsoda_ort/onnxruntime_providers_shared.dll"
      } else {
        ($packageRootDirName + "\zsoda_ort\onnxruntime_providers_shared.dll").Replace('\', '/')
      }
      "$($providersHash.Hash.ToLowerInvariant())  $providersHashLabel" | Out-File -FilePath (Join-Path $OutputDir "onnxruntime_providers_shared.dll.sha256") -Encoding ascii
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
  if ($Platform -eq "windows" -and $PackageMode -eq "embedded-windows") {
    Write-Host "  models:   embedded"
  } else {
    Write-Host "  models:   $modelsCopiedPath"
  }
}
if ($ortRuntimeCopiedPath) {
  if ($Platform -eq "windows" -and $PackageMode -eq "embedded-windows") {
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
  if ($Platform -eq "windows" -and $PackageMode -eq "embedded-windows") {
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
if (Test-Path -LiteralPath $stagePlanPath -PathType Leaf) {
  Remove-Item -LiteralPath $stagePlanPath -Force
}
