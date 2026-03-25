param(
  [string]$AfterEffectsVersion = "25.0",
  [string]$OutputRoot = ".\\artifacts\\diagnostics",
  [int]$ContextLines = 8,
  [string[]]$AexPaths = @(
    "C:\\Program Files\\Adobe\\Common\\Plug-ins\\7.0\\MediaCore\\Z-Soda\\ZSoda.aex"
  )
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

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

function New-Directory {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path
  )

  New-Item -ItemType Directory -Path $Path -Force | Out-Null
}

function Find-PluginLoadingLog {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Version
  )

  $candidates = @(
    (Join-Path $env:APPDATA ("Adobe\\After Effects\\{0}\\logs\\Plugin Loading.log" -f $Version)),
    (Join-Path $env:APPDATA ("Adobe\\After Effects\\{0}\\Log Files\\Plugin Loading.log" -f $Version)),
    (Join-Path $env:APPDATA ("Adobe\\After Effects\\{0}\\logs\\Log Files\\Plugin Loading.log" -f $Version))
  )
  foreach ($candidate in $candidates) {
    if (Test-Path -LiteralPath $candidate -PathType Leaf) {
      return (Resolve-Path -LiteralPath $candidate).Path
    }
  }

  $root = Join-Path $env:APPDATA "Adobe\\After Effects"
  if (-not (Test-Path -LiteralPath $root -PathType Container)) {
    return $null
  }

  $latest = Get-ChildItem -LiteralPath $root -Recurse -Filter "Plugin Loading.log" -File -ErrorAction SilentlyContinue |
    Sort-Object -Property LastWriteTimeUtc -Descending |
    Select-Object -First 1
  if ($null -eq $latest) {
    return $null
  }
  return $latest.FullName
}

function Get-ZsodaPluginCache {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Version
  )

  $result = [ordered]@{
    RootPath = "HKCU:\\Software\\Adobe\\After Effects\\$Version\\PluginCache\\en_US"
    Keys = @()
  }

  if (-not (Test-Path -LiteralPath $result.RootPath)) {
    return $result
  }

  $items = Get-ChildItem -LiteralPath $result.RootPath -ErrorAction SilentlyContinue |
    Where-Object { $_.PSChildName -like "ZSoda.aex_*" } |
    Sort-Object -Property PSChildName

  foreach ($item in $items) {
    $props = Get-ItemProperty -LiteralPath $item.PSPath -ErrorAction Stop
    $valueMap = [ordered]@{}
    foreach ($prop in $props.PSObject.Properties) {
      if ($prop.Name -notmatch '^PS') {
        $valueMap[$prop.Name] = $prop.Value
      }
    }
    $result.Keys += [ordered]@{
      Name = $item.PSChildName
      Path = $item.Name
      Values = $valueMap
    }
  }
  return $result
}

function Get-SafeStem {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path
  )

  $stem = $Path -replace '[:\\/\s]+', '_'
  $stem = $stem -replace '[^A-Za-z0-9._-]', '_'
  if ([string]::IsNullOrWhiteSpace($stem)) {
    return "aex"
  }
  return $stem
}

function Invoke-DumpbinToFile {
  param(
    [Parameter(Mandatory = $true)]
    [string]$DumpbinPath,

    [Parameter(Mandatory = $true)]
    [string[]]$Arguments,

    [Parameter(Mandatory = $true)]
    [string]$OutputPath
  )

  $output = & $DumpbinPath @Arguments 2>&1 | Out-String
  Set-Content -LiteralPath $OutputPath -Value $output -Encoding UTF8
}

function Test-LoadLibrary {
  param(
    [Parameter(Mandatory = $true)]
    [string]$AexPath
  )

  Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class ZSodaNativeProbe {
  [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
  public static extern IntPtr LoadLibraryW(string lpFileName);
  [DllImport("kernel32.dll", SetLastError = true)]
  public static extern bool FreeLibrary(IntPtr hModule);
}
"@ -ErrorAction SilentlyContinue

  $module = [ZSodaNativeProbe]::LoadLibraryW($AexPath)
  if ($module -eq [IntPtr]::Zero) {
    $errorCode = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    return [ordered]@{
      Success = $false
      LastError = $errorCode
    }
  }

  [void][ZSodaNativeProbe]::FreeLibrary($module)
  return [ordered]@{
    Success = $true
    LastError = 0
  }
}

$outputRootAbs = Resolve-AbsolutePath -Path $OutputRoot -AllowMissing
$sessionDir = Join-Path $outputRootAbs ("ae_loader_diag_{0}" -f (Get-Date -Format "yyyyMMdd_HHmmss"))
$cacheDir = Join-Path $sessionDir "plugin_cache"
$logDir = Join-Path $sessionDir "logs"
$aexDir = Join-Path $sessionDir "aex"

New-Directory -Path $sessionDir
New-Directory -Path $cacheDir
New-Directory -Path $logDir
New-Directory -Path $aexDir

$summary = New-Object System.Collections.Generic.List[string]
$summary.Add(("timestamp={0}" -f (Get-Date -Format o)))
$summary.Add(("after_effects_version={0}" -f $AfterEffectsVersion))
$summary.Add(("output_dir={0}" -f $sessionDir))

$cacheInfo = Get-ZsodaPluginCache -Version $AfterEffectsVersion
$cacheJsonPath = Join-Path $cacheDir "zsoda_plugin_cache.json"
$cacheTxtPath = Join-Path $cacheDir "zsoda_plugin_cache.txt"
$cacheInfo | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $cacheJsonPath -Encoding UTF8

$cacheLines = New-Object System.Collections.Generic.List[string]
$cacheLines.Add(("root={0}" -f $cacheInfo.RootPath))
$cacheLines.Add(("key_count={0}" -f $cacheInfo.Keys.Count))
foreach ($key in $cacheInfo.Keys) {
  $cacheLines.Add("")
  $cacheLines.Add(("[{0}]" -f $key.Name))
  foreach ($entry in $key.Values.GetEnumerator()) {
    $cacheLines.Add(("  {0}={1}" -f $entry.Key, $entry.Value))
  }
}
$cacheLines | Set-Content -LiteralPath $cacheTxtPath -Encoding UTF8
$summary.Add(("plugin_cache_keys={0}" -f $cacheInfo.Keys.Count))
$summary.Add(("plugin_cache_json={0}" -f $cacheJsonPath))

$pluginLoadingLogPath = Find-PluginLoadingLog -Version $AfterEffectsVersion
if ($null -ne $pluginLoadingLogPath) {
  $logCopyPath = Join-Path $logDir "Plugin Loading.log"
  Copy-Item -LiteralPath $pluginLoadingLogPath -Destination $logCopyPath -Force

  $contextPath = Join-Path $logDir "plugin_loading_zsoda_context.txt"
  $matches = Select-String -Path $pluginLoadingLogPath -Pattern "ZSoda|No loaders recognized" -Context $ContextLines, $ContextLines
  if ($matches.Count -eq 0) {
    Set-Content -LiteralPath $contextPath -Value "No ZSoda-related lines found." -Encoding UTF8
  } else {
    $contextLinesOut = New-Object System.Collections.Generic.List[string]
    foreach ($match in $matches) {
      $contextLinesOut.Add(("===== line {0} =====" -f $match.LineNumber))
      foreach ($pre in $match.Context.PreContext) {
        $contextLinesOut.Add(("  {0}" -f $pre))
      }
      $contextLinesOut.Add(("> {0}" -f $match.Line))
      foreach ($post in $match.Context.PostContext) {
        $contextLinesOut.Add(("  {0}" -f $post))
      }
      $contextLinesOut.Add("")
    }
    $contextLinesOut | Set-Content -LiteralPath $contextPath -Encoding UTF8
  }
  $summary.Add(("plugin_loading_log={0}" -f $pluginLoadingLogPath))
  $summary.Add(("plugin_loading_context={0}" -f $contextPath))
} else {
  $summary.Add("plugin_loading_log=<not found>")
}

$dumpbin = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
if ($null -eq $dumpbin) {
  $dumpbin = Get-Command dumpbin -ErrorAction SilentlyContinue
}
if ($null -ne $dumpbin) {
  $summary.Add(("dumpbin={0}" -f $dumpbin.Source))
} else {
  $summary.Add("dumpbin=<not found>")
}

foreach ($aexPath in $AexPaths) {
  $exists = Test-Path -LiteralPath $aexPath -PathType Leaf
  $stem = Get-SafeStem -Path $aexPath
  $metaPath = Join-Path $aexDir ("{0}.meta.txt" -f $stem)

  $meta = New-Object System.Collections.Generic.List[string]
  $meta.Add(("path={0}" -f $aexPath))
  $meta.Add(("exists={0}" -f $exists))

  if (-not $exists) {
    $meta | Set-Content -LiteralPath $metaPath -Encoding UTF8
    continue
  }

  $resolved = (Resolve-Path -LiteralPath $aexPath).Path
  $file = Get-Item -LiteralPath $resolved
  $hash = Get-FileHash -LiteralPath $resolved -Algorithm SHA256
  $probe = Test-LoadLibrary -AexPath $resolved

  $meta.Add(("resolved={0}" -f $resolved))
  $meta.Add(("size={0}" -f $file.Length))
  $meta.Add(("last_write_utc={0}" -f $file.LastWriteTimeUtc.ToString("o")))
  $meta.Add(("sha256={0}" -f $hash.Hash.ToLowerInvariant()))
  $meta.Add(("loadlibrary_success={0}" -f $probe.Success))
  $meta.Add(("loadlibrary_last_error={0}" -f $probe.LastError))
  $meta | Set-Content -LiteralPath $metaPath -Encoding UTF8

  if ($null -ne $dumpbin) {
    Invoke-DumpbinToFile -DumpbinPath $dumpbin.Source -Arguments @("/exports", $resolved) -OutputPath (Join-Path $aexDir ("{0}.exports.txt" -f $stem))
    Invoke-DumpbinToFile -DumpbinPath $dumpbin.Source -Arguments @("/headers", $resolved) -OutputPath (Join-Path $aexDir ("{0}.headers.txt" -f $stem))
    Invoke-DumpbinToFile -DumpbinPath $dumpbin.Source -Arguments @("/dependents", $resolved) -OutputPath (Join-Path $aexDir ("{0}.dependents.txt" -f $stem))
    Invoke-DumpbinToFile -DumpbinPath $dumpbin.Source -Arguments @("/rawdata:.rsrc", $resolved) -OutputPath (Join-Path $aexDir ("{0}.rsrc.txt" -f $stem))
  }
}

$summaryPath = Join-Path $sessionDir "summary.txt"
$summary | Set-Content -LiteralPath $summaryPath -Encoding UTF8

Write-Host ("AE loader diagnostics collected: {0}" -f $sessionDir)
Write-Host ("Summary: {0}" -f $summaryPath)
