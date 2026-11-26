<#
.SYNOPSIS
Prepares a release: bumps the version, rebuilds and tests, refreshes the README's screenshots (docs\editor.png,
docs\fx-chain.png, docs\fx-graph.png and docs\seq.png), then lists the commit, tag and push and asks whether to do them (only "yes"
does).

.EXAMPLE
.\release.ps1                # 0.6 -> 0.7: on "yes", commit "Version 0.7" on main, tag v0.7, push both
.\release.ps1 -Patch         # 0.6 -> 0.6.1 (0.6.1 -> 0.6.2): a patch release, on main or a branch of its own
.\release.ps1 -Version 1.0   # a version of your choosing (major.minor, or major.minor.patch)
.\release.ps1 -Screenshots   # only render the README's screenshots into docs, on any branch, committing nothing

A release with a new major or minor version is made from main. A patch release (a third number) may also be made
from another branch, such as release/0.14 branched from the tag v0.14 to fix an older version while main has moved
on: it is committed on that branch, and the branch and the tag are pushed. The workflow marks a release Latest and
puts its demo on GitHub Pages only when its tag is the newest version, so a fix to an older version leaves them be.

Steps: check the branch (main, or any for a patch release), a clean tree, the branch level with origin, the tag free
and the version above the current one; set the version in
CMakeLists.txt; build, run every test and both validators (build.ps1 -Validate, which also renders the editor's snapshots offscreen); copy the
snapshots the tests render over the README's, in the Spectrum theme: the Screamer preset's editor over docs\editor.png, the
Crystal Motion preset's effects' panel as a chain over docs\fx-chain.png, the Neon Pluck preset's effects' panel, a graph, over
docs\fx-graph.png, and the Future Plucks preset's SEQ view, at its full height, over docs\seq.png; ask; on "yes", commit them all, tag vX.Y (or vX.Y.Z), and
push the branch and the tag. The tag makes GitHub Actions build the release and the browser demo (see AGENTS.md,
"Releasing").

On anything but "yes", CMakeLists.txt and the screenshots go back as they were afterwards (so the script can run
again); the new screenshots stay in build\release\ui-shots to look at.

-Screenshots does only the screenshots: it builds and runs fmvibe-uishot (the UI snapshots, with their checks) in the
release build and copies its renders over docs\editor.png, docs\fx-chain.png, docs\fx-graph.png and docs\seq.png, leaving the
version, the other tests and git alone.
#>
param(
    [ValidatePattern('^\d+\.\d+(\.\d+)?$')] [string] $Version,
    [switch] $Patch,
    [switch] $Screenshots
)
$ErrorActionPreference = 'Stop'

function Invoke-Git {
    & git @args
    if ($LASTEXITCODE) { throw "git $($args -join ' ') failed (exit code $LASTEXITCODE)" }
}

# The README's screenshots, which the tests render in the Spectrum theme: the Screamer preset's routing view, the
# Crystal Motion preset's effects' panel as a chain, and the Neon Pluck preset's effects' panel, a graph split
# low and high.
function Copy-Screenshots {
    $shots = [ordered]@{
        'editor.png'   = '24c-screamer-example-routing.png'
        'fx-chain.png' = '24-example-fx-chain-panel.png'
        'fx-graph.png' = '24b-split-example-fx-graph-panel.png'
        'seq.png'      = '24d-seq-example-seq.png'
    }
    foreach ($doc in $shots.Keys) {
        $shot = "$PSScriptRoot\build\release\ui-shots\$($shots[$doc])"
        if (-not (Test-Path $shot) -or (Get-Item $shot).LastWriteTime -lt (Get-Date).AddHours(-1)) {
            throw "The tests did not render $shot."
        }
        Copy-Item $shot "$PSScriptRoot\docs\$doc" -Force
    }
}

Push-Location $PSScriptRoot
try {
    if ($Screenshots) {
        # cl, cmake and ninja come with Visual Studio: load its dev environment unless that already happened.
        if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
            $installer = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer"
            $vs = & "$installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
            if (-not $vs) { throw 'Visual Studio with the C++ build tools was not found.' }
            $env:PATH = "$installer;$env:PATH" # VsDevCmd calls vswhere by name
            Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
            Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
        }
        Write-Host '== build fmvibe-uishot' -ForegroundColor Cyan
        cmake --preset release
        if ($LASTEXITCODE) { throw "configure failed (exit code $LASTEXITCODE)" }
        cmake --build --preset release --target fmvibe-uishot
        if ($LASTEXITCODE) { throw "build failed (exit code $LASTEXITCODE)" }
        Write-Host '== render the snapshots' -ForegroundColor Cyan
        & "$PSScriptRoot\build\release\fmvibe-uishot.exe" "$PSScriptRoot\build\release\ui-shots"
        if ($LASTEXITCODE) { throw "fmvibe-uishot failed (exit code $LASTEXITCODE)" }
        Copy-Screenshots
        Write-Host '== rendered docs\editor.png, docs\fx-chain.png, docs\fx-graph.png and docs\seq.png (not committed)' -ForegroundColor Green
        return
    }

    if ($Patch -and $Version) { throw 'Give -Patch or -Version, not both.' }

    # --- The version: CMakeLists.txt's project() (major.minor, or major.minor.patch), one minor step up unless
    # -Patch or -Version says otherwise.
    $cmake = Get-Content CMakeLists.txt -Raw
    $pattern = '(?m)^(project\(FMVibe VERSION )(\d+)\.(\d+)(?:\.(\d+))?( LANGUAGES)'
    $m = [regex]::Match($cmake, $pattern)
    if (-not $m.Success) { throw 'Could not find the version in project() in CMakeLists.txt.' }
    $major = [int]$m.Groups[2].Value
    $minor = [int]$m.Groups[3].Value
    $patchNumber = if ($m.Groups[4].Success) { [int]$m.Groups[4].Value } else { 0 }
    $current = if ($m.Groups[4].Success) { "$major.$minor.$patchNumber" } else { "$major.$minor" }
    if ($Patch) { $Version = "$major.$minor.$($patchNumber + 1)" }
    elseif (-not $Version) { $Version = "$major.$($minor + 1)" }
    # [version] counts a missing third number as below any (0.14 < 0.14.0 < 0.14.1).
    if ([version]$Version -le [version]$current) { throw "The version $Version isn't above the current $current." }
    # A patch release names a third number above 0; X.Y.0 is a minor release like X.Y.
    $parts = $Version.Split('.')
    $isPatch = $parts.Count -eq 3 -and [int]$parts[2] -gt 0
    $tag = "v$Version"

    # --- Checks, before anything is touched.
    $branch = "$(& git branch --show-current)".Trim()
    if (-not $branch) { throw 'HEAD is detached; check out the branch to release from.' }
    if ($branch -ne 'main' -and -not $isPatch) {
        throw "Releases with a new major or minor version are made from main; this is '$branch'. A patch release (-Patch, or -Version X.Y.Z with Z above 0) may be made from another branch."
    }
    if (& git status --porcelain) { throw 'The working tree has uncommitted changes; commit or stash them first.' }
    Invoke-Git fetch origin --tags --quiet
    # A branch not on origin yet (a new release branch) is pushed with the tag.
    & git rev-parse --verify --quiet "refs/remotes/origin/$branch" | Out-Null
    if (-not $LASTEXITCODE -and (& git rev-list --count "HEAD..origin/$branch") -ne '0') {
        throw "$branch is behind origin/$branch; pull first."
    }
    if (& git tag --list $tag) { throw "The tag $tag exists already." }
    Write-Host "== version $current -> $Version on $branch" -ForegroundColor Cyan

    try {
        $utf8 = New-Object System.Text.UTF8Encoding($false)
        [IO.File]::WriteAllText("$PSScriptRoot\CMakeLists.txt", [regex]::Replace($cmake, $pattern, "`${1}$Version`$5"), $utf8)

        # --- Build and test with the new version (the tests render the editor's snapshots as they go).
        & "$PSScriptRoot\build.ps1" -Validate
        if ($LASTEXITCODE) { throw "build.ps1 failed (exit code $LASTEXITCODE)" }

        Copy-Screenshots

        Write-Host "== next (the new screenshots: docs\editor.png, docs\fx-chain.png, docs\fx-graph.png, docs\seq.png):" -ForegroundColor Yellow
        Write-Host "   git add CMakeLists.txt docs/editor.png docs/fx-chain.png docs/fx-graph.png docs/seq.png"
        Write-Host "   git commit -m `"Version $Version`""
        Write-Host "   git tag $tag"
        Write-Host "   git push origin $branch $tag"
        if ((Read-Host "Release $tag now? Type 'yes' to commit, tag and push") -ne 'yes') {
            Write-Host "== not released; CMakeLists.txt and the screenshots are put back as they were." -ForegroundColor Yellow
            return
        }

        # --- Commit, tag, push.
        Write-Host "== commit, tag $tag, push" -ForegroundColor Cyan
        Invoke-Git add CMakeLists.txt docs/editor.png docs/fx-chain.png docs/fx-graph.png docs/seq.png
        Invoke-Git commit -m "Version $Version"
        Invoke-Git tag $tag
        Invoke-Git push origin $branch $tag
        Write-Host "== released $tag" -ForegroundColor Green
    }
    finally {
        # Nothing was committed (not confirmed, or a step failed): put the files back.
        if ((& git log -1 --format=%s) -ne "Version $Version") {
            & git checkout -- CMakeLists.txt docs/editor.png docs/fx-chain.png docs/fx-graph.png docs/seq.png
        }
    }
}
finally {
    Pop-Location
}
