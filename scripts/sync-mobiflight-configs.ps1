[CmdletBinding()]
param(
    [ValidateSet("Publish", "Materialize")]
    [string] $Mode = "Publish",

    [string] $MapPath,

    [string] $MaterializedRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$utf8NoBom = New-Object Text.UTF8Encoding($false)
$utf8Bom = New-Object Text.UTF8Encoding($true)
$placeholders = [ordered]@{
    "SN-PUB-ATC" = "MobiFlight Mega"
    "SN-PUB-MCP" = "MF UNO MCP"
    "SN-PUB-LGT" = "MF UNO LGHTS"
}

if ([string]::IsNullOrWhiteSpace($MapPath)) {
    $MapPath = Join-Path $repositoryRoot ".local\mobiflight-serials.json"
} elseif (-not [IO.Path]::IsPathRooted($MapPath)) {
    $MapPath = Join-Path $repositoryRoot $MapPath
}
$MapPath = [IO.Path]::GetFullPath($MapPath)

if ([string]::IsNullOrWhiteSpace($MaterializedRoot)) {
    $MaterializedRoot = Join-Path $repositoryRoot ".local\materialized"
} elseif (-not [IO.Path]::IsPathRooted($MaterializedRoot)) {
    $MaterializedRoot = Join-Path $repositoryRoot $MaterializedRoot
}
$MaterializedRoot = [IO.Path]::GetFullPath($MaterializedRoot)

function Get-RepositoryRelativePath([string] $FullPath) {
    $rootPrefix = $repositoryRoot.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    $normalized = [IO.Path]::GetFullPath($FullPath)
    if (-not $normalized.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Path is outside the repository: $normalized"
    }
    return $normalized.Substring($rootPrefix.Length)
}

function Get-MccFiles([bool] $Public) {
    $files = @()
    foreach ($relativeRoot in @("archive", "configs")) {
        $root = Join-Path $repositoryRoot $relativeRoot
        if (Test-Path -LiteralPath $root) {
            $files += Get-ChildItem -LiteralPath $root -Recurse -File -Filter "*.mcc"
        }
    }

    if ($Public) {
        return @($files | Where-Object { $_.Name -like "*.public.mcc" } | Sort-Object FullName)
    }
    return @($files | Where-Object { $_.Name -notlike "*.public.mcc" } | Sort-Object FullName)
}

function Assert-WellFormedXml([string] $Text, [string] $Path) {
    try {
        $document = New-Object Xml.XmlDocument
        $document.PreserveWhitespace = $true
        $document.LoadXml($Text)
    } catch {
        throw "Generated invalid XML for $Path`: $($_.Exception.Message)"
    }
}

function Test-Utf8Bom([string] $Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    return $bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF
}

function Write-Utf8([string] $Path, [string] $Text, [bool] $EmitBom) {
    $directory = Split-Path -Parent $Path
    if (-not (Test-Path -LiteralPath $directory)) {
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
    }
    $encoding = if ($EmitBom) { $utf8Bom } else { $utf8NoBom }
    [IO.File]::WriteAllText($Path, $Text, $encoding)
}

function Read-PrivateMap {
    if (-not (Test-Path -LiteralPath $MapPath)) {
        throw "Private serial map not found at $MapPath. Run Publish first or create the ignored map locally."
    }

    $object = Get-Content -LiteralPath $MapPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $map = [ordered]@{}
    foreach ($placeholder in $placeholders.Keys) {
        $property = $object.PSObject.Properties[$placeholder]
        if ($null -eq $property) {
            throw "Private serial map is missing $placeholder."
        }
        $serial = [string] $property.Value
        if ($serial -notmatch '^SN-[A-Za-z0-9]{3}-[A-Za-z0-9]{3}$') {
            throw "Private serial mapped from $placeholder has an unexpected format."
        }
        if ($serial -eq $placeholder) {
            throw "Private serial mapped from $placeholder is still a public placeholder."
        }
        $map[$placeholder] = $serial
    }

    if (@($map.Values | Sort-Object -Unique).Count -ne $map.Count) {
        throw "Private serial map must contain three distinct controller serials."
    }
    return $map
}

if ($Mode -eq "Publish") {
    $privateFiles = @(Get-MccFiles $false)
    if ($privateFiles.Count -eq 0) {
        throw "No private .mcc files were found. Private bound exports are intentionally ignored by Git."
    }

    $privateTexts = @{}
    foreach ($file in $privateFiles) {
        $privateTexts[$file.FullName] = [IO.File]::ReadAllText($file.FullName)
    }
    $combined = [string]::Join("`n", @($privateTexts.Values))

    $map = [ordered]@{}
    foreach ($placeholder in $placeholders.Keys) {
        $label = $placeholders[$placeholder]
        $pattern = 'serial="' + [regex]::Escape($label) + '/\s*(SN-[A-Za-z0-9]{3}-[A-Za-z0-9]{3})"'
        $matches = [regex]::Matches($combined, $pattern, [Text.RegularExpressions.RegexOptions]::IgnoreCase)
        $serials = @($matches | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique)
        if ($serials.Count -ne 1) {
            throw "Expected exactly one private controller serial for role $label; found $($serials.Count)."
        }
        $map[$placeholder] = $serials[0]
    }

    if (@($map.Values | Sort-Object -Unique).Count -ne $map.Count) {
        throw "The three controller roles must resolve to distinct private serials."
    }

    $mapDirectory = Split-Path -Parent $MapPath
    if (-not (Test-Path -LiteralPath $mapDirectory)) {
        New-Item -ItemType Directory -Path $mapDirectory -Force | Out-Null
    }
    Write-Utf8 $MapPath (($map | ConvertTo-Json) + [Environment]::NewLine) $false

    $knownPrivateSerials = @($map.Values)
    $replacementCount = 0
    foreach ($file in $privateFiles) {
        $privateText = $privateTexts[$file.FullName]
        $assigned = [regex]::Matches(
            $privateText,
            'serial="[^"]*/\s*(SN-[A-Za-z0-9]{3}-[A-Za-z0-9]{3})"',
            [Text.RegularExpressions.RegexOptions]::IgnoreCase
        )
        $unknown = @(
            $assigned |
                ForEach-Object { $_.Groups[1].Value } |
                Where-Object { $knownPrivateSerials -notcontains $_ } |
                Sort-Object -Unique
        )
        if ($unknown.Count -gt 0) {
            throw "Unclassified assigned controller serial found in $($file.FullName); update the role mapping first."
        }

        $publicText = $privateText
        foreach ($placeholder in $map.Keys) {
            $privateSerial = $map[$placeholder]
            $replacementCount += [regex]::Matches(
                $publicText,
                [regex]::Escape($privateSerial),
                [Text.RegularExpressions.RegexOptions]::IgnoreCase
            ).Count
            $publicText = [regex]::Replace(
                $publicText,
                [regex]::Escape($privateSerial),
                $placeholder,
                [Text.RegularExpressions.RegexOptions]::IgnoreCase
            )
        }

        $remaining = @(
            [regex]::Matches(
                $publicText,
                'serial="[^"]*/\s*(SN-[A-Za-z0-9]{3}-[A-Za-z0-9]{3})"',
                [Text.RegularExpressions.RegexOptions]::IgnoreCase
            ) |
                ForEach-Object { $_.Groups[1].Value } |
                Where-Object { $placeholders.Keys -notcontains $_ } |
                Sort-Object -Unique
        )
        if ($remaining.Count -gt 0) {
            throw "A non-public assigned serial remains in the generated copy for $($file.FullName)."
        }

        $publicPath = [regex]::Replace($file.FullName, '\.mcc$', '.public.mcc', 'IgnoreCase')
        Assert-WellFormedXml $publicText $publicPath
        Write-Utf8 $publicPath $publicText (Test-Utf8Bom $file.FullName)
    }

    Write-Host "Generated $($privateFiles.Count) public MobiFlight configs with $replacementCount anonymized bindings."
    Write-Host "The private serial map is stored under the ignored .local directory."
    exit 0
}

$map = Read-PrivateMap
$publicFiles = @(Get-MccFiles $true)
if ($publicFiles.Count -eq 0) {
    throw "No .public.mcc files were found."
}

if (-not $MaterializedRoot.StartsWith(
    $repositoryRoot.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar,
    [StringComparison]::OrdinalIgnoreCase
)) {
    throw "MaterializedRoot must remain inside the repository so generated private files stay covered by local safeguards."
}

$replacementCount = 0
foreach ($file in $publicFiles) {
    $text = [IO.File]::ReadAllText($file.FullName)
    foreach ($placeholder in $map.Keys) {
        $replacementCount += [regex]::Matches($text, [regex]::Escape($placeholder)).Count
        $text = $text.Replace($placeholder, $map[$placeholder])
    }

    $relative = Get-RepositoryRelativePath $file.FullName
    $relative = [regex]::Replace($relative, '\.public\.mcc$', '.mcc', 'IgnoreCase')
    $destination = Join-Path $MaterializedRoot $relative
    Assert-WellFormedXml $text $destination
    Write-Utf8 $destination $text (Test-Utf8Bom $file.FullName)
}

Write-Host "Materialized $($publicFiles.Count) locally bound configs with $replacementCount restored bindings."
Write-Host "Output: $MaterializedRoot"
