[CmdletBinding()]
param(
    [switch] $RequireCleanHistory
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$allowedSerials = @("SN-PUB-ATC", "SN-PUB-MCP", "SN-PUB-LGT")
$mapPath = Join-Path $repositoryRoot ".local\mobiflight-serials.json"

function Get-RepositoryRelativePath([string] $FullPath) {
    $rootPrefix = $repositoryRoot.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    $normalized = [IO.Path]::GetFullPath($FullPath)
    if (-not $normalized.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Path is outside the repository: $normalized"
    }
    return $normalized.Substring($rootPrefix.Length).Replace('\', '/')
}

$trackedMcc = @(& git -C $repositoryRoot ls-files -- "*.mcc")
if ($LASTEXITCODE -ne 0) {
    throw "Unable to list tracked MobiFlight configs."
}
if ($trackedMcc.Count -eq 0) {
    throw "No tracked MobiFlight configs were found."
}

$trackedPrivate = @($trackedMcc | Where-Object { $_ -notlike "*.public.mcc" })
if ($trackedPrivate.Count -gt 0) {
    throw "Private .mcc files are still tracked: $($trackedPrivate -join ', ')"
}

$counts = @{}
foreach ($serial in $allowedSerials) {
    $counts[$serial] = 0
}

foreach ($relative in $trackedMcc) {
    $path = Join-Path $repositoryRoot $relative
    $text = [IO.File]::ReadAllText($path)
    $document = New-Object Xml.XmlDocument
    $document.PreserveWhitespace = $true
    $document.LoadXml($text)

    $assigned = [regex]::Matches(
        $text,
        'serial="[^"]*/\s*(SN-[A-Za-z0-9]{3}-[A-Za-z0-9]{3})"',
        [Text.RegularExpressions.RegexOptions]::IgnoreCase
    )
    foreach ($match in $assigned) {
        $serial = $match.Groups[1].Value
        if ($allowedSerials -notcontains $serial) {
            throw "Non-public assigned serial found in $relative."
        }
        $counts[$serial]++
    }
}

foreach ($serial in $allowedSerials) {
    if ($counts[$serial] -eq 0) {
        throw "Expected public placeholder $serial was not found."
    }
}

$privateFiles = @()
foreach ($relativeRoot in @("archive", "configs")) {
    $root = Join-Path $repositoryRoot $relativeRoot
    if (Test-Path -LiteralPath $root) {
        $privateFiles += Get-ChildItem -LiteralPath $root -Recurse -File -Filter "*.mcc" |
            Where-Object { $_.Name -notlike "*.public.mcc" }
    }
}
foreach ($file in $privateFiles) {
    $relative = Get-RepositoryRelativePath $file.FullName
    & git -C $repositoryRoot check-ignore --quiet -- $relative
    if ($LASTEXITCODE -ne 0) {
        throw "Private config is not ignored: $relative"
    }
}

if (Test-Path -LiteralPath $mapPath) {
    $mapObject = Get-Content -LiteralPath $mapPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $privateSerials = @()
    foreach ($placeholder in $allowedSerials) {
        $property = $mapObject.PSObject.Properties[$placeholder]
        if ($null -eq $property) {
            throw "Private serial map is missing $placeholder."
        }
        $privateSerials += [string] $property.Value
    }

    foreach ($relative in $trackedMcc) {
        $publicPath = Join-Path $repositoryRoot $relative
        $privateRelative = [regex]::Replace($relative, '\.public\.mcc$', '.mcc', 'IgnoreCase')
        $privatePath = Join-Path $repositoryRoot $privateRelative
        if (-not (Test-Path -LiteralPath $privatePath)) {
            continue
        }

        $expected = [IO.File]::ReadAllText($privatePath)
        for ($index = 0; $index -lt $allowedSerials.Count; $index++) {
            $expected = [regex]::Replace(
                $expected,
                [regex]::Escape($privateSerials[$index]),
                $allowedSerials[$index],
                [Text.RegularExpressions.RegexOptions]::IgnoreCase
            )
        }
        $actual = [IO.File]::ReadAllText($publicPath)
        if ($actual -cne $expected) {
            throw "Public config differs from its private source by more than serial anonymization: $relative"
        }
    }

    $historyLeak = $false
    $commits = @(& git -C $repositoryRoot rev-list --all)
    foreach ($privateSerial in $privateSerials) {
        foreach ($commit in $commits) {
            & git -C $repositoryRoot grep -q -F -e $privateSerial $commit -- 2>$null
            if ($LASTEXITCODE -eq 0) {
                $historyLeak = $true
                break
            }
        }
        if ($historyLeak) {
            break
        }
    }

    if ($historyLeak) {
        if ($RequireCleanHistory) {
            throw "Private controller serials remain in Git history. Rewrite the private history before publication."
        }
        Write-Warning "Working files are clean, but private controller serials remain in Git history."
    }
} elseif ($RequireCleanHistory) {
    throw "Cannot verify history without the ignored private serial map."
}

$summary = $allowedSerials | ForEach-Object { "$_=$($counts[$_])" }
Write-Host "Validated $($trackedMcc.Count) public MobiFlight configs ($($summary -join ', '))."
