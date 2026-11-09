"""The standalone dependency builder must reject an untrusted source archive."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
BUILDER = ROOT / "tools/phase3jb/Build-PatchedSamplerate.ps1"
PATCH = ROOT / "cmake/dependencies/samplerate/libsamplerate-0.2.2-linear-history.patch"


class QualifiedSamplerateBuilderTest(unittest.TestCase):
    def test_wrong_archive_is_rejected_before_creating_output(self):
        with tempfile.TemporaryDirectory(prefix="lmms-samplerate-builder-") as temp:
            root = Path(temp)
            patch = root / "cmake/dependencies/samplerate/libsamplerate-0.2.2-linear-history.patch"
            patch.parent.mkdir(parents=True)
            shutil.copy2(PATCH, patch)
            archive = root / "bad.tar.xz"
            archive.write_bytes(b"not the pinned upstream source")
            output = root / "new-build"
            result = subprocess.run(["pwsh.exe", "-NoProfile", "-File", str(BUILDER),
                                     "-WorkspaceRoot", str(root), "-OutputRoot", str(output),
                                     "-SourceArchivePath", str(archive)],
                                    capture_output=True, text=True, timeout=30)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Source archive SHA256 mismatch", result.stdout + result.stderr)
            self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
