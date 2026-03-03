param(
  [string]$ModelId = "depth-anything-v3-small",
  [string]$ModelRoot = $env:ZSODA_MODEL_ROOT,
  [string]$ManifestPath = $env:ZSODA_MODEL_MANIFEST
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

if ([string]::IsNullOrWhiteSpace($ModelRoot)) {
  $ModelRoot = "models"
}
if ([string]::IsNullOrWhiteSpace($ManifestPath)) {
  $ManifestPath = Join-Path $ModelRoot "models.manifest"
}

function New-AssetList {
  return ,(New-Object System.Collections.ArrayList)
}

function Add-ModelAsset {
  param(
    [Parameter(Mandatory = $true)]
    [ref]$Assets,
    [Parameter(Mandatory = $true)]
    [string]$RelativePath,
    [Parameter(Mandatory = $true)]
    [string]$Url
  )

  if ($null -eq $Assets.Value) {
    throw "model asset list is null"
  }

  $cleanPath = $RelativePath.Trim()
  $cleanUrl = $Url.Trim()
  if ([string]::IsNullOrWhiteSpace($cleanPath) -or [string]::IsNullOrWhiteSpace($cleanUrl)) {
    throw "model asset entry is missing required fields (relative_path/url)"
  }

  $null = $Assets.Value.Add(@{
    RelativePath = $cleanPath
    Url          = $cleanUrl
  })
}

function Parse-AuxiliaryAssets {
  param(
    [Parameter(Mandatory = $true)]
    [string]$RawValue,
    [Parameter(Mandatory = $true)]
    [ref]$Assets
  )

  if ($null -eq $Assets.Value) {
    throw "model asset list is null"
  }

  $value = $RawValue.Trim()
  if ([string]::IsNullOrWhiteSpace($value)) {
    return
  }

  foreach ($entryRaw in $value.Split(";", [System.StringSplitOptions]::RemoveEmptyEntries)) {
    $entry = $entryRaw.Trim()
    if ([string]::IsNullOrWhiteSpace($entry)) {
      continue
    }

    $separator = $entry.IndexOf("::", [System.StringComparison]::Ordinal)
    if ($separator -lt 0) {
      throw "manifest auxiliary_assets format error: $entry"
    }

    $relativePath = $entry.Substring(0, $separator).Trim()
    $url = $entry.Substring($separator + 2).Trim()
    Add-ModelAsset -Assets $Assets -RelativePath $relativePath -Url $url
  }
}

function Resolve-ModelFromManifest {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path,
    [Parameter(Mandatory = $true)]
    [string]$TargetId
  )

  if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
    return $null
  }

  foreach ($raw in Get-Content -LiteralPath $Path) {
    $line = $raw
    $commentIndex = $line.IndexOf("#", [System.StringComparison]::Ordinal)
    if ($commentIndex -ge 0) {
      $line = $line.Substring(0, $commentIndex)
    }
    $line = $line.Trim()
    if ([string]::IsNullOrWhiteSpace($line)) {
      continue
    }

    $parts = $line.Split("|")
    if ($parts.Length -lt 4) {
      continue
    }

    $id = $parts[0].Trim()
    if ($id -ne $TargetId) {
      continue
    }

    $relativePath = $parts[2].Trim()
    $url = $parts[3].Trim()
    $auxiliaryAssets = ""
    if ($parts.Length -ge 6) {
      $auxiliaryAssets = $parts[5].Trim()
    }

    if ([string]::IsNullOrWhiteSpace($relativePath) -or [string]::IsNullOrWhiteSpace($url)) {
      throw "manifest entry is missing required fields: $id"
    }

    $assets = New-AssetList
    Add-ModelAsset -Assets ([ref]$assets) -RelativePath $relativePath -Url $url
    Parse-AuxiliaryAssets -RawValue $auxiliaryAssets -Assets ([ref]$assets)

    return @{ Assets = $assets }
  }

  return $null
}

function Resolve-ModelFromBuiltInCatalog {
  param(
    [Parameter(Mandatory = $true)]
    [string]$TargetId
  )

  $assets = New-AssetList
  switch ($TargetId) {
    "depth-anything-v3-small" {
      Add-ModelAsset -Assets ([ref]$assets) -RelativePath "depth-anything-v3/depth_anything_v3_small.onnx" -Url "https://huggingface.co/onnx-community/depth-anything-v3-small/resolve/main/onnx/model.onnx"
      Add-ModelAsset -Assets ([ref]$assets) -RelativePath "depth-anything-v3/depth_anything_v3_small.onnx_data" -Url "https://huggingface.co/onnx-community/depth-anything-v3-small/resolve/main/onnx/model.onnx_data"
    }
    "depth-anything-v3-base" {
      Add-ModelAsset -Assets ([ref]$assets) -RelativePath "depth-anything-v3/depth_anything_v3_base.onnx" -Url "https://huggingface.co/onnx-community/depth-anything-v3-base/resolve/main/onnx/model.onnx"
      Add-ModelAsset -Assets ([ref]$assets) -RelativePath "depth-anything-v3/depth_anything_v3_base.onnx_data" -Url "https://huggingface.co/onnx-community/depth-anything-v3-base/resolve/main/onnx/model.onnx_data"
    }
    "depth-anything-v3-large" {
      Add-ModelAsset -Assets ([ref]$assets) -RelativePath "depth-anything-v3/depth_anything_v3_large.onnx" -Url "https://huggingface.co/onnx-community/depth-anything-v3-large/resolve/main/onnx/model.onnx"
      Add-ModelAsset -Assets ([ref]$assets) -RelativePath "depth-anything-v3/depth_anything_v3_large.onnx_data" -Url "https://huggingface.co/onnx-community/depth-anything-v3-large/resolve/main/onnx/model.onnx_data"
    }
    "midas-dpt-large" {
      Add-ModelAsset -Assets ([ref]$assets) -RelativePath "midas/dpt_large_384.onnx" -Url "https://github.com/isl-org/MiDaS/releases/download/v3_1/dpt_large_384.onnx"
    }
    default {
      throw "unsupported model id: $TargetId"
    }
  }

  return @{ Assets = $assets }
}

function Initialize-NetworkDefaults {
  try {
    $tls12 = [System.Net.SecurityProtocolType]::Tls12
    $tls13 = 0
    try {
      $tls13 = [System.Net.SecurityProtocolType]::Tls13
    } catch {
      $tls13 = 0
    }

    if ($tls13 -ne 0) {
      [System.Net.ServicePointManager]::SecurityProtocol = $tls12 -bor $tls13
    } else {
      [System.Net.ServicePointManager]::SecurityProtocol = $tls12
    }
  } catch {
    # Ignore platform-specific TLS defaults configuration errors.
  }
}

function Invoke-DownloadFile {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Url,
    [Parameter(Mandatory = $true)]
    [string]$Destination,
    [hashtable]$Headers
  )

  if ($null -eq $Headers) {
    $Headers = @{}
  }

  try {
    $request = @{
      Uri                = $Url
      OutFile            = $Destination
      MaximumRedirection = 10
    }
    if ($PSVersionTable.PSVersion.Major -le 5) {
      $request["UseBasicParsing"] = $true
    }
    if ($Headers.Count -gt 0) {
      $request["Headers"] = $Headers
    }

    Invoke-WebRequest @request
    return
  } catch {
    $invokeError = $_
    $curl = Get-Command curl.exe -ErrorAction SilentlyContinue
    if ($null -eq $curl) {
      throw $invokeError
    }

    $args = @(
      "-L",
      "--fail",
      "--retry", "3",
      "--retry-delay", "2",
      "--connect-timeout", "30",
      "--output", $Destination
    )
    if ($Headers.ContainsKey("Authorization")) {
      $args += @("-H", "Authorization: $($Headers["Authorization"])" )
    }
    $args += $Url

    & $curl.Source @args
    if ($LASTEXITCODE -ne 0) {
      throw $invokeError
    }
  }
}

function Ensure-OrtExternalDataAlias {
  param(
    [Parameter(Mandatory = $true)]
    [string]$DownloadedPath
  )

  $leaf = [System.IO.Path]::GetFileName($DownloadedPath)
  if ([string]::IsNullOrWhiteSpace($leaf)) {
    return
  }
  if (-not $leaf.EndsWith(".onnx_data", [System.StringComparison]::OrdinalIgnoreCase)) {
    return
  }
  if ($leaf.Equals("model.onnx_data", [System.StringComparison]::OrdinalIgnoreCase)) {
    return
  }

  $directory = [System.IO.Path]::GetDirectoryName($DownloadedPath)
  if ([string]::IsNullOrWhiteSpace($directory)) {
    return
  }

  $aliasPath = Join-Path $directory "model.onnx_data"
  if ($aliasPath.Equals($DownloadedPath, [System.StringComparison]::OrdinalIgnoreCase)) {
    return
  }

  Copy-Item -LiteralPath $DownloadedPath -Destination $aliasPath -Force
  Write-Host "Alias: $aliasPath <= $DownloadedPath"
}

$resolved = Resolve-ModelFromManifest -Path $ManifestPath -TargetId $ModelId
if ($null -eq $resolved) {
  $resolved = Resolve-ModelFromBuiltInCatalog -TargetId $ModelId
}

if ($null -eq $resolved -or $null -eq $resolved.Assets -or $resolved.Assets.Count -eq 0) {
  throw "download asset list is empty: $ModelId"
}

$hfToken = $env:HF_TOKEN
if ([string]::IsNullOrWhiteSpace($hfToken)) {
  $hfToken = $env:HUGGINGFACE_TOKEN
}
if ([string]::IsNullOrWhiteSpace($hfToken)) {
  $hfToken = $env:HUGGING_FACE_HUB_TOKEN
}

Initialize-NetworkDefaults

Write-Host "Download start: $ModelId"
foreach ($asset in $resolved.Assets) {
  $destination = Join-Path $ModelRoot $asset.RelativePath
  $destinationDir = Split-Path -Parent $destination
  if (-not [string]::IsNullOrWhiteSpace($destinationDir)) {
    New-Item -ItemType Directory -Force -Path $destinationDir | Out-Null
  }

  Write-Host "URL: $($asset.Url)"
  Write-Host "Target: $destination"

  $headers = @{}
  if (-not [string]::IsNullOrWhiteSpace($hfToken) -and
      $asset.Url.StartsWith("https://huggingface.co/", [System.StringComparison]::OrdinalIgnoreCase)) {
    $headers["Authorization"] = "Bearer $hfToken"
  }

  Invoke-DownloadFile -Url $asset.Url -Destination $destination -Headers $headers

  if (-not (Test-Path -LiteralPath $destination -PathType Leaf)) {
    throw "download failed: file not created ($destination)"
  }
  $length = (Get-Item -LiteralPath $destination).Length
  if ($length -le 0) {
    throw "download failed: file is empty ($destination)"
  }

  Ensure-OrtExternalDataAlias -DownloadedPath $destination
  Write-Host "Done: $destination"
}
