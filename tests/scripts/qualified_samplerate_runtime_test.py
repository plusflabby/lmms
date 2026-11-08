"""Check unqualified runtimes do not prevent ordinary application startup."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


STAGE = Path(os.environ["LMMS_QUALIFIED_SAMPLERATE_TEST_STAGE"])
EXECUTABLE = Path(os.environ.get("LMMS_QUALIFIED_SAMPLERATE_TEST_EXE", str(STAGE / "lmms.exe")))
SYSTEM_DLL = Path("C:/msys64/ucrt64/bin/libsamplerate-0.dll")


class QualifiedSamplerateRuntimeTest(unittest.TestCase):
    def run_version(self, dll):
        with tempfile.TemporaryDirectory(prefix="lmms-samplerate-runtime-") as temp:
            root = Path(temp)
            shutil.copy2(EXECUTABLE, root / "lmms.exe")
            if dll is not None:
                shutil.copy2(dll, root / "libsamplerate-0.dll")
            env = os.environ.copy()
            env["PATH"] = f"{STAGE};C:/msys64/ucrt64/bin;C:/msys64/usr/bin;{env['PATH']}"
            return subprocess.run([str(root / "lmms.exe"), "--version"], env=env,
                                  capture_output=True, text=True, timeout=30)

    def test_approved_bundled_runtime_starts(self):
        result = self.run_version(STAGE / "libsamplerate-0.dll")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_unpatched_bundled_runtime_does_not_block_application(self):
        result = self.run_version(SYSTEM_DLL)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_missing_bundled_runtime_can_use_existing_path_for_application(self):
        result = self.run_version(None)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
