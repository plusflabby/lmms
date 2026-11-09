[CmdletBinding()]
param(
	[string]$WorkspaceRoot,
	[string]$OutputRoot,
	[string]$SourceArchivePath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($WorkspaceRoot))
{
	$WorkspaceRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
}
$WorkspaceRoot = [IO.Path]::GetFullPath($WorkspaceRoot)
$nativeRoot = $WorkspaceRoot
if ([string]::IsNullOrWhiteSpace($OutputRoot))
{
	$OutputRoot = Join-Path $nativeRoot 'build-phase3r-samplerate-dev'
}
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
$relativeOutput = [IO.Path]::GetRelativePath($nativeRoot, $OutputRoot)
if ($relativeOutput -eq '.' -or $relativeOutput.StartsWith('..', [StringComparison]::Ordinal) -or [IO.Path]::IsPathRooted($relativeOutput))
{
	throw "OutputRoot must be a new project-local directory below $nativeRoot"
}
if (Test-Path -LiteralPath $OutputRoot)
{
	throw "OutputRoot already exists; this build script never overwrites it: $OutputRoot"
}

$ucrtBin = 'C:\msys64\ucrt64\bin'
$usrBin = 'C:\msys64\usr\bin'
$cmake = Join-Path $ucrtBin 'cmake.exe'
$cc = Join-Path $ucrtBin 'cc.exe'
$ninja = Join-Path $ucrtBin 'ninja.exe'
$bash = Join-Path $usrBin 'bash.exe'
$cygpath = Join-Path $usrBin 'cygpath.exe'
$systemDll = Join-Path $ucrtBin 'libsamplerate-0.dll'
foreach ($required in @($cmake, $cc, $ninja, $bash, $cygpath, $systemDll))
{
	if (-not (Test-Path -LiteralPath $required)) { throw "Required UCRT64 tool or baseline DLL is missing: $required" }
}

$toolCmake = ((& $cmake --version | Select-Object -First 1) -replace '^cmake version ', '').Trim()
$toolCc = ((& $cc --version | Select-Object -First 1) -replace '.*?([0-9]+\.[0-9]+\.[0-9]+).*', '$1').Trim()
$toolNinja = (& $ninja --version).Trim()
if ($toolCmake -ne '4.4.2' -or $toolCc -ne '16.2.0' -or $toolNinja -ne '1.13.2')
{
	throw "UCRT64 toolchain mismatch: CMake=$toolCmake GCC=$toolCc Ninja=$toolNinja"
}

$sourceUrl = 'https://github.com/libsndfile/libsamplerate/releases/download/0.2.2/libsamplerate-0.2.2.tar.xz'
$expectedArchiveHash = '3258DA280511D24B49D6B08615BBE824D0CACC9842B0E4CAF11C52CF2B043893'
$expectedPatchHash = '065DD14B04C2795FDB0D33EB5539AC62C4714A7248552A41199451DF8B595FA7'
$expectedSourceHash = '12658E85973565BE762DF03F08BD815749DE699FC281FD7FB9C2BF1C5CEA529F'
$expectedDllHash = 'BF275A0636DC7DADAC8A691842C06B460109EFC213C68B5EF6576A216E55D734'
$sourceCommit = 'c96f5e3de9c4488f4e6c97f59f5245f22fda22f7'
$sourceDateEpoch = '1630842492'
$patchPath = Join-Path $WorkspaceRoot 'cmake\dependencies\samplerate\libsamplerate-0.2.2-linear-history.patch'
if (-not (Test-Path -LiteralPath $patchPath)) { throw "Committed dependency patch is missing: $patchPath" }
if ((Get-FileHash -LiteralPath $patchPath -Algorithm SHA256).Hash -ne $expectedPatchHash)
{
	throw 'Dependency patch SHA256 mismatch'
}

$archivePath = $SourceArchivePath
if (-not [string]::IsNullOrWhiteSpace($archivePath))
{
	$archivePath = [IO.Path]::GetFullPath($archivePath)
	if (-not (Test-Path -LiteralPath $archivePath -PathType Leaf)) { throw "Source archive does not exist: $archivePath" }
	if ((Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash -ne $expectedArchiveHash)
	{
		throw 'Source archive SHA256 mismatch; no output directory was created'
	}
}

New-Item -ItemType Directory -Path $OutputRoot | Out-Null
$logs = Join-Path $OutputRoot 'logs'
$sourceContainer = Join-Path $OutputRoot 'source'
$sourceRoot = Join-Path $sourceContainer 'libsamplerate-0.2.2'
$staticBuild = Join-Path $OutputRoot 'build-static'
$sharedBuild = Join-Path $OutputRoot 'build-shared'
$prefix = Join-Path $OutputRoot 'prefix'
New-Item -ItemType Directory -Path $logs, $sourceContainer | Out-Null

if ([string]::IsNullOrWhiteSpace($archivePath))
{
	$archivePath = Join-Path $OutputRoot 'libsamplerate-0.2.2.tar.xz'
	Invoke-WebRequest -Uri $sourceUrl -OutFile $archivePath
	if ((Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash -ne $expectedArchiveHash)
	{
		throw 'Downloaded source archive SHA256 mismatch; retained the download for inspection'
	}
}
else
{
	Copy-Item -LiteralPath $archivePath -Destination (Join-Path $OutputRoot 'libsamplerate-0.2.2.tar.xz')
	$archivePath = Join-Path $OutputRoot 'libsamplerate-0.2.2.tar.xz'
}

function ConvertTo-MsysPath([string]$Path)
{
	$value = (& $cygpath -u $Path).Trim()
	if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($value)) { throw "Cannot convert path for MSYS2: $Path" }
	return $value
}

function Quote-Bash([string]$Value)
{
	return "'" + $Value.Replace("'", "'\''") + "'"
}

function Invoke-Logged([string]$Name, [string]$Executable, [string[]]$Arguments)
{
	$stdoutPath = Join-Path $logs ($Name + '.stdout.log')
	$stderrPath = Join-Path $logs ($Name + '.stderr.log')
	$exitPath = Join-Path $logs ($Name + '.exit.txt')
	& $Executable @Arguments 1> $stdoutPath 2> $stderrPath
	$exitCode = $LASTEXITCODE
	Set-Content -LiteralPath $exitPath -Value $exitCode
	if ($exitCode -ne 0) { throw "$Name failed with exit $exitCode; see $stdoutPath and $stderrPath" }
}

$oldPath = $env:PATH
$oldSystem = $env:MSYSTEM
$oldPrefix = $env:MINGW_PREFIX
$oldPackagePrefix = $env:MINGW_PACKAGE_PREFIX
$oldSourceDateEpoch = $env:SOURCE_DATE_EPOCH
$systemDllHashBefore = (Get-FileHash -LiteralPath $systemDll -Algorithm SHA256).Hash
try
{
	$env:PATH = "$ucrtBin;$usrBin;$oldPath"
	$env:MSYSTEM = 'UCRT64'
	$env:MINGW_PREFIX = '/ucrt64'
	$env:MINGW_PACKAGE_PREFIX = 'mingw-w64-ucrt-x86_64'
	$env:SOURCE_DATE_EPOCH = $sourceDateEpoch
	$archiveMsys = ConvertTo-MsysPath $archivePath
	$sourceMsys = ConvertTo-MsysPath $sourceContainer
	$patchMsys = ConvertTo-MsysPath $patchPath
	$extractCmd = 'tar -xf ' + (Quote-Bash $archiveMsys) + ' -C ' + (Quote-Bash $sourceMsys)
	Invoke-Logged 'extract-source' $bash @('-lc', $extractCmd)
	if (-not (Test-Path -LiteralPath (Join-Path $sourceRoot 'src\src_linear.c'))) { throw 'Unexpected source archive layout' }
	$patchCmd = 'cd ' + (Quote-Bash (ConvertTo-MsysPath $sourceRoot)) + ' && patch -p1 --batch -i ' + (Quote-Bash $patchMsys)
	Invoke-Logged 'apply-source-patch' $bash @('-lc', $patchCmd)
	if ((Get-FileHash -LiteralPath (Join-Path $sourceRoot 'src\src_linear.c') -Algorithm SHA256).Hash -ne $expectedSourceHash)
	{
		throw 'Patched source hash does not match the pinned release archive plus patch'
	}

	$common = @(
		'-S', $sourceRoot,
		'-G', 'Ninja',
		'-DCMAKE_BUILD_TYPE=Release',
		"-DCMAKE_C_COMPILER=$cc",
		"-DCMAKE_MAKE_PROGRAM=$ninja",
		"-DCMAKE_INSTALL_PREFIX=$prefix",
		'-DCMAKE_POLICY_VERSION_MINIMUM=3.5',
		'-DLIBSAMPLERATE_EXAMPLES=OFF',
		'-DLIBSAMPLERATE_INSTALL=ON',
		'-DLIBSAMPLERATE_COMPATIBLE_NAME=OFF',
		'-DLIBSAMPLERATE_INSTALL_PKGCONFIG_MODULE=ON',
		'-DBUILD_TESTING=ON'
	)
	Invoke-Logged 'configure-static' $cmake ($common + @('-B', $staticBuild, '-DBUILD_SHARED_LIBS=OFF'))
	Invoke-Logged 'build-static' $cmake @('--build', $staticBuild)
	Invoke-Logged 'test-static' (Join-Path $ucrtBin 'ctest.exe') @('--test-dir', $staticBuild, '--output-on-failure')
	if ((Get-Content -LiteralPath (Join-Path $logs 'test-static.stdout.log') -Raw) -notmatch '100% tests passed out of 13')
	{
		throw 'Static CTest did not report all 13 upstream tests passing'
	}

	Invoke-Logged 'configure-shared' $cmake ($common + @('-B', $sharedBuild, '-DBUILD_SHARED_LIBS=ON'))
	Invoke-Logged 'build-shared' $cmake @('--build', $sharedBuild)
	$sharedDll = Get-ChildItem -LiteralPath $sharedBuild -Recurse -Filter 'libsamplerate-0.dll' | Select-Object -First 1
	if (-not $sharedDll) { throw 'Shared build did not produce libsamplerate-0.dll' }
	$testDllDirectory = Join-Path $sharedBuild 'tests'
	$testDllPath = Join-Path $testDllDirectory 'libsamplerate-0.dll'
	if (Test-Path -LiteralPath $testDllPath) { throw 'Unexpected pre-existing samplerate DLL in shared test directory' }
	Copy-Item -LiteralPath $sharedDll.FullName -Destination $testDllPath
	if ((Get-FileHash -LiteralPath $testDllPath -Algorithm SHA256).Hash -ne $expectedDllHash)
	{
		throw 'Shared test directory does not contain the pinned candidate DLL'
	}
	Invoke-Logged 'test-shared' (Join-Path $ucrtBin 'ctest.exe') @('--test-dir', $sharedBuild, '--output-on-failure')
	if ((Get-Content -LiteralPath (Join-Path $logs 'test-shared.stdout.log') -Raw) -notmatch '100% tests passed out of 13')
	{
		throw 'Shared CTest did not report all 13 upstream tests passing'
	}

	Invoke-Logged 'install-static' $cmake @('--install', $staticBuild, '--prefix', $prefix)
	Invoke-Logged 'install-shared' $cmake @('--install', $sharedBuild, '--prefix', $prefix)
	$licensePath = Join-Path $prefix 'share\licenses\libsamplerate\COPYING'
	if (Test-Path -LiteralPath $licensePath) { throw 'Unexpected pre-existing staged COPYING file' }
	New-Item -ItemType Directory -Path (Split-Path -Parent $licensePath) -Force | Out-Null
	Copy-Item -LiteralPath (Join-Path $sourceRoot 'COPYING') -Destination $licensePath
	$licenseHash = (Get-FileHash -LiteralPath $licensePath -Algorithm SHA256).Hash
	if ($licenseHash -ne '2C1F76CE2EFFDDDB425018405D5690C0B1AB4E6976E35296B0A6DB65C5E1A55D')
	{
		throw 'Staged license hash does not match the upstream BSD-2-Clause COPYING file'
	}
	$stagedDll = Join-Path $prefix 'bin\libsamplerate-0.dll'
	$stagedHash = (Get-FileHash -LiteralPath $stagedDll -Algorithm SHA256).Hash
	if ($stagedHash -ne $expectedDllHash) { throw "Reproducible DLL hash mismatch: $stagedHash" }
	$systemDllHashAfter = (Get-FileHash -LiteralPath $systemDll -Algorithm SHA256).Hash
	if ($systemDllHashAfter -ne $systemDllHashBefore) { throw 'Global MSYS2 libsamplerate DLL changed during local build' }
	$stagedFiles = @(Get-ChildItem -LiteralPath $prefix -Recurse -File | Sort-Object FullName | ForEach-Object {
		[ordered]@{
			Path = $_.FullName.Substring($prefix.Length + 1).Replace('\', '/')
			Length = $_.Length
			SHA256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
		}
	})
	$manifest = [ordered]@{
		UpstreamTag = '0.2.2'
		UpstreamCommit = $sourceCommit
		SourceArchiveURL = $sourceUrl
		SourceArchiveSHA256 = $expectedArchiveHash
		SourceDateEpoch = [long]$sourceDateEpoch
		PatchSHA256 = $expectedPatchHash
		PatchedArchiveSourceSHA256 = $expectedSourceHash
		CandidateDLLSHA256 = $stagedHash
		CandidateDLLLength = (Get-Item -LiteralPath $stagedDll).Length
		OriginalSystemDLLSHA256Before = $systemDllHashBefore
		OriginalSystemDLLSHA256After = $systemDllHashAfter
		Toolchain = [ordered]@{ CMake = $toolCmake; GCC = $toolCc; Ninja = $toolNinja; BuildType = 'Release' }
		LicenseSHA256 = $licenseHash
		StagedFiles = $stagedFiles
	}
	$manifestPath = Join-Path $OutputRoot 'dependency-manifest.json'
	[IO.File]::WriteAllText($manifestPath, ($manifest | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))
	Write-Output "Patched dependency prefix: $prefix"
	Write-Output "Patched DLL SHA256: $stagedHash"
	Write-Output "Dependency manifest: $manifestPath"
}
finally
{
	$env:PATH = $oldPath
	$env:MSYSTEM = $oldSystem
	$env:MINGW_PREFIX = $oldPrefix
	$env:MINGW_PACKAGE_PREFIX = $oldPackagePrefix
	$env:SOURCE_DATE_EPOCH = $oldSourceDateEpoch
}
