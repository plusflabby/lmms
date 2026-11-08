param([Parameter(Mandatory)][string]$Source, [Parameter(Mandatory)][string]$Destination)
$ErrorActionPreference = 'Stop'
$expected = 'BF275A0636DC7DADAC8A691842C06B460109EFC213C68B5EF6576A216E55D734'
$sourceInfo = [IO.FileInfo]::new($Source)
if (-not $sourceInfo.Exists -or $sourceInfo.Length -le 0 -or $sourceInfo.Length -gt 8388608) { throw 'Qualified runtime has invalid size' }
$bytes = [IO.File]::ReadAllBytes($Source)
$sha = [Security.Cryptography.SHA256]::Create()
try { $actual = [BitConverter]::ToString($sha.ComputeHash($bytes)).Replace('-', '') }
finally { $sha.Dispose() }
if ($actual -ne $expected) { throw 'Qualified runtime SHA-256 mismatch' }
if ([IO.Directory]::Exists($Destination)) { throw "Runtime destination is a directory: $Destination" }
if ([IO.File]::Exists($Destination))
{
	$destinationInfo = [IO.FileInfo]::new($Destination)
	if ($destinationInfo.Length -le 0 -or $destinationInfo.Length -gt 8388608)
	{ throw "A different existing runtime is preserved: $Destination" }
	$existing = [IO.File]::ReadAllBytes($Destination)
	$sha = [Security.Cryptography.SHA256]::Create()
	try { $existingHash = [BitConverter]::ToString($sha.ComputeHash($existing)).Replace('-', '') }
	finally { $sha.Dispose() }
	if ($existingHash -ne $expected) { throw "A different existing runtime is preserved: $Destination" }
	return
}
# CreateNew never replaces a file created between the existence check and open.
$output = [IO.File]::Open($Destination, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
try { $output.Write($bytes, 0, $bytes.Length); $output.Flush() }
finally { $output.Dispose() }
