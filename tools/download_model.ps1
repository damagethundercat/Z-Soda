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
    if ($id -ne $TargetId) {
      continue
    }
    if ([string]::IsNullOrWhiteSpace($relativePath) -or [string]::IsNullOrWhiteSpace($url)) {
      throw "매니페스트 항목 누락: $id (relative_path/download_url 필요)"
    }
    return @{
      Url = $url
      RelativePath = $relativePath
    }
  }

  return $null
}

$resolved = Resolve-ModelFromManifest -Path $ManifestPath -TargetId $ModelId
if ($null -eq $resolved) {
  switch ($ModelId) {
    "depth-anything-v3-small" {
      $resolved = @{
        Url = "https://huggingface.co/depth-anything/Depth-Anything-V3/resolve/main/depth_anything_v3_small.onnx"
        RelativePath = "depth-anything-v3/depth_anything_v3_small.onnx"
      }
    }
    "depth-anything-v3-base" {
      $resolved = @{
        Url = "https://huggingface.co/depth-anything/Depth-Anything-V3/resolve/main/depth_anything_v3_base.onnx"
        RelativePath = "depth-anything-v3/depth_anything_v3_base.onnx"
      }
    }
    "depth-anything-v3-large" {
      $resolved = @{
        Url = "https://huggingface.co/depth-anything/Depth-Anything-V3/resolve/main/depth_anything_v3_large.onnx"
        RelativePath = "depth-anything-v3/depth_anything_v3_large.onnx"
      }
    }
    "midas-dpt-large" {
      $resolved = @{
        Url = "https://github.com/isl-org/MiDaS/releases/download/v3_1/dpt_large_384.onnx"
        RelativePath = "midas/dpt_large_384.onnx"
      }
    }
    default {
      throw "지원하지 않는 모델 ID: $ModelId"
    }
  }
}

$destination = Join-Path $ModelRoot $resolved.RelativePath
$destinationDir = Split-Path -Parent $destination
if (-not [string]::IsNullOrWhiteSpace($destinationDir)) {
  New-Item -ItemType Directory -Force -Path $destinationDir | Out-Null
}

Write-Host "다운로드 시작: $ModelId"
Write-Host "URL: $($resolved.Url)"
Write-Host "저장 경로: $destination"
Invoke-WebRequest -Uri $resolved.Url -OutFile $destination
Write-Host "완료: $destination"
