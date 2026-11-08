"""Run the real vocal admission and quarantine tests against a stock DLL."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


BIN = Path(os.environ["LMMS_UNQUALIFIED_TEST_BIN_DIR"])
STOCK = Path(os.environ.get("LMMS_UNQUALIFIED_RUNTIME_PATH", "C:/msys64/ucrt64/bin/libsamplerate-0.dll"))
QUALIFIED = "bf275a0636dc7dadac8a691842c06b460109efc213c68b5ef6576a216e55d734"


class UnqualifiedVocalRuntimeTest(unittest.TestCase):
    def check(self, executable, case):
        digest = hashlib.sha256(STOCK.read_bytes()).hexdigest()
        self.assertNotEqual(digest, QUALIFIED, "The negative fixture must be an unqualified runtime")
        with tempfile.TemporaryDirectory(prefix="lmms-vocal-unqualified-") as temp:
            root = Path(temp)
            copied = root / executable
            shutil.copy2(BIN / executable, copied)
            shutil.copy2(STOCK, root / "libsamplerate-0.dll")
            env = os.environ.copy()
            env["PATH"] = f"C:/msys64/ucrt64/bin;C:/msys64/usr/bin;{env['PATH']}"
            env["QT_QPA_PLATFORM"] = "offscreen"
            env["LMMS_TEST_EXPECT_UNQUALIFIED_VOCAL_RUNTIME"] = digest
            report = root / "result.txt"
            result = subprocess.run([str(copied), case, "-o", f"{report.as_posix()},txt"],
                                    env=env, capture_output=True, text=True, timeout=45)
            details = report.read_text(encoding="utf-8", errors="replace") if report.exists() else ""
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr + details)
            self.assertIn("0 failed", details)
            self.assertIn("0 skipped", details)

    def test_planning_rejects_the_stock_runtime_without_mutation(self):
        self.check("AiBridgeVocalAssetActionsTest.exe", "unqualifiedRuntimeRejectsVocalEditWithoutMutation")

    def test_existing_variant_is_quarantined_and_revert_remains_available(self):
        self.check("SampleClipManagedVocalVariantTest.exe", "unqualifiedRuntimeQuarantinesVariantAndAllowsRevert")


if __name__ == "__main__":
    unittest.main(verbosity=2)
