# Qualified Phase 3J-B libsamplerate profile

The qualified Windows profile uses the pinned UCRT64 x86-64 GCC build of
libsamplerate 0.2.2 described by `ucrt64-development-manifest.json` and the
adjacent linear-history patch. Build the isolated prefix from the native
checkout with PowerShell 7 and `tools/phase3jb/Build-PatchedSamplerate.ps1`. The builder checks
the source archive, patch, toolchain, upstream tests, output and license; it
does not replace the installed system DLL.

Configure a new UCRT64 LMMS build with both
`-DLMMS_REQUIRE_QUALIFIED_VOCAL_RUNTIME=ON` and
`-DLMMS_QUALIFIED_SAMPLERATE_PREFIX=<builder-output>/prefix`. Configuration,
build and installation reject missing or changed qualified files. The build
stages the qualified DLL beside the application and tests without replacing a
different existing file; the installed DLL is checked again after copying.

The default profile leaves existing platform dependency selection in place.
It does not qualify Phase 3J-B vocal editing or managed-variant playback.
An unqualified build or loaded runtime rejects vocal-edit operations and
quarantines managed variants; the original association is retained and an
explicit revert remains available. Existing non-vocal playback is unchanged.
The stock UCRT64 runtime has known legacy sample-offset endpoint failures;
disabling Phase 3J-B does not repair or qualify that generic playback path.
MSVC, Windows ARM64 and non-Windows Phase 3J-B runtimes have not been
qualified by this profile. A compatible build and authorized asset roots are
required to play saved managed variants.

The MSVC CTest configuration runs explicit rejection and quarantine cases
for the unqualified runtime, while the full positive vocal cases run in the
qualified UCRT64 CI job. The test runner also retains the runtime-independent
cases from those suites and fails if its case inventory changes without
classification. Set `LMMS_UNQUALIFIED_RUNTIME_PATH` to the exact samplerate
DLL loaded by the tests and provide Python 3.9 or newer when running this
MSVC test selection. The tests verify that the loaded DLL's SHA-256 matches
the supplied file. This configuration is not evidence of MSVC qualification.

The existing CI matrix explicitly quarantines `AiBridgeViewTest` and
`RecordingPortAudioDiagnosticTest` because their diagnostic execution is
unsafe; it continues to run `AiBridgeAudioRangeNativeTest` and report its
result. The qualified UCRT64 safe-suite job also excludes the separately
documented legacy AudioRange exception. Historical failures and exclusions
must not be represented as successful test runs.

The qualified CI job uses the hosted runner's MSYS2 installation. Its builder
requires the exact manifest toolchain versions and DLL hash; runner or package
drift is reported as a failed check rather than accepted as qualification.
