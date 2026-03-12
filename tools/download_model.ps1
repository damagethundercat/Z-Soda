param(
  [string]$ModelId = "distill-any-depth-base",
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

$supported = @(
  "distill-any-depth",
  "distill-any-depth-base",
  "distill-any-depth-large"
)

if ($supported -notcontains $ModelId) {
  throw "unsupported model id: $ModelId"
}

if (-not (Test-Path -LiteralPath $ModelRoot)) {
  New-Item -ItemType Directory -Path $ModelRoot | Out-Null
}

Write-Host "ZSoda production path is DistillAnyDepth via remote service."
Write-Host "Model: $ModelId"
Write-Host "Model root: $ModelRoot"
Write-Host "Manifest: $ManifestPath"
Write-Host ""
Write-Host "No local ONNX asset is downloaded by this helper."
Write-Host "The runtime service resolves the Hugging Face model on demand:"
Write-Host "  tools\\distill_any_depth_remote_service.py"
