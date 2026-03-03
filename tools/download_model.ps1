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

function Add-ModelAsset {
  param(
    [Parameter(Mandatory = $true)]
    [System.Collections.ArrayList]$Assets,
    [Parameter(Mandatory = $true)]
    [string]$RelativePath,
    [Parameter(Mandatory = $true)]
    [string]$Url
  )

  $cleanPath = $RelativePath.Trim()
  $cleanUrl = $Url.Trim()
  if ([string]::IsNullOrWhiteSpace($cleanPath) -or [string]::IsNullOrWhiteSpace($cleanUrl)) {
    throw "모델 자산 항목 누락(relative_path/url)"
  }
  $null = $Assets.Add(@{
    RelativePath = $cleanPath
    Url = $cleanUrl
  })
}

function Parse-AuxiliaryAssets {
  param(
    [Parameter(Mandatory = $true)]
    [string]$RawValue,
    [Parameter(Mandatory = $true)]
    [System.Collections.ArrayList]$Assets
  )

  $value = $RawValue.Trim()
  if ([string]::IsNullOrWhiteSpace($value)) {
    return
  }

  foreach ($entryRaw in $value.Split(";", [StringSplitOptions]::RemoveEmptyEntries)) {
    $entry = $entryRaw.Trim()
    if ([string]::IsNullOrWhiteSpace($entry)) {
      continue
    }
    $separator = $entry.IndexOf("::", [StringComparison]::Ordinal)
    if ($separator -lt 0) {
      throw "매니페스트 auxiliary_assets 형식 오류: $entry"
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
    $commentIndex = $line.IndexOf("#", [StringComparison]::Ordinal)
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
    $relativePath = $parts[2].Trim()
    $url = $parts[3].Trim()
    $auxiliaryAssets = ""
    if ($parts.Length -ge 6) {
      $auxiliaryAssets = $parts[5].Trim()
    }
    if ($id -ne $TargetId) {
      continue
    }
    if ([string]::IsNullOrWhiteSpace($relativePath) -or [string]::IsNullOrWhiteSpace($url)) {
      throw "매니페스트 항목 누락: $id (relative_path/download_url 필요)"
    }
    $assets = New-Object System.Collections.ArrayList
    Add-ModelAsset -Assets $assets -RelativePath $relativePath -Url $url
    Parse-AuxiliaryAssets -RawValue $auxiliaryAssets -Assets $assets
    return @{
      Assets = $assets
    }
  }

  return $null
}

$resolved = Resolve-ModelFromManifest -Path $ManifestPath -TargetId $ModelId
if ($null -eq $resolved) {
  $assets = New-Object System.Collections.ArrayList
  switch ($ModelId) {
    "depth-anything-v3-small" {
      Add-ModelAsset -Assets $assets `
        -RelativePath "depth-anything-v3/depth_anything_v3_small.onnx" `
        -Url "https://huggingface.co/onnx-community/depth-anything-v3-small/resolve/main/onnx/model.onnx"
      Add-ModelAsset -Assets $assets `
        -RelativePath "depth-anything-v3/depth_anything_v3_small.onnx_data" `
        -Url "https://huggingface.co/onnx-community/depth-anything-v3-small/resolve/main/onnx/model.onnx_data"
    }
    "depth-anything-v3-base" {
      Add-ModelAsset -Assets $assets `
        -RelativePath "depth-anything-v3/depth_anything_v3_base.onnx" `
        -Url "https://huggingface.co/onnx-community/depth-anything-v3-base/resolve/main/onnx/model.onnx"
      Add-ModelAsset -Assets $assets `
        -RelativePath "depth-anything-v3/depth_anything_v3_base.onnx_data" `
        -Url "https://huggingface.co/onnx-community/depth-anything-v3-base/resolve/main/onnx/model.onnx_data"
    }
    "depth-anything-v3-large" {
      Add-ModelAsset -Assets $assets `
        -RelativePath "depth-anything-v3/depth_anything_v3_large.onnx" `
        -Url "https://huggingface.co/onnx-community/depth-anything-v3-large/resolve/main/onnx/model.onnx"
      Add-ModelAsset -Assets $assets `
        -RelativePath "depth-anything-v3/depth_anything_v3_large.onnx_data" `
        -Url "https://huggingface.co/onnx-community/depth-anything-v3-large/resolve/main/onnx/model.onnx_data"
    }
    "midas-dpt-large" {
      Add-ModelAsset -Assets $assets `
        -RelativePath "midas/dpt_large_384.onnx" `
        -Url "https://github.com/isl-org/MiDaS/releases/download/v3_1/dpt_large_384.onnx"
    }
    default {
      throw "지원하지 않는 모델 ID: $ModelId"
    }
  }
  $resolved = @{
    Assets = $assets
  }
}

if ($resolved.Assets.Count -eq 0) {
  throw "다운로드 자산 목록이 비어 있습니다: $ModelId"
}

$hfToken = $env:HF_TOKEN
if ([string]::IsNullOrWhiteSpace($hfToken)) {
  $hfToken = $env:HUGGINGFACE_TOKEN
}
if ([string]::IsNullOrWhiteSpace($hfToken)) {
  $hfToken = $env:HUGGING_FACE_HUB_TOKEN
}

Write-Host "다운로드 시작: $ModelId"
foreach ($asset in $resolved.Assets) {
  $destination = Join-Path $ModelRoot $asset.RelativePath
  $destinationDir = Split-Path -Parent $destination
  if (-not [string]::IsNullOrWhiteSpace($destinationDir)) {
    New-Item -ItemType Directory -Force -Path $destinationDir | Out-Null
  }

  Write-Host "URL: $($asset.Url)"
  Write-Host "저장 경로: $destination"

  $headers = @{}
  if (-not [string]::IsNullOrWhiteSpace($hfToken) -and
      $asset.Url.StartsWith("https://huggingface.co/", [StringComparison]::OrdinalIgnoreCase)) {
    $headers["Authorization"] = "Bearer $hfToken"
  }

  if ($headers.Count -gt 0) {
    Invoke-WebRequest -Uri $asset.Url -OutFile $destination -Headers $headers
  } else {
    Invoke-WebRequest -Uri $asset.Url -OutFile $destination
  }
  Write-Host "완료: $destination"
}
