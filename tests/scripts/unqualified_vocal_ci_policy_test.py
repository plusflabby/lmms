import unittest
import subprocess
import sys
from pathlib import Path

from unqualified_vocal_ci import select_cases


class SelectionTest(unittest.TestCase):
    def test_selects_negative_and_platform_independent_cases(self):
        available = {"reject", "pure", "positive"}
        self.assertEqual(select_cases(available, {"reject", "pure"}, {"positive"}),
                         ["pure", "reject"])

    def test_new_cases_must_be_classified(self):
        with self.assertRaisesRegex(ValueError, "unclassified"):
            select_cases({"reject", "new"}, {"reject"}, set())

    def test_missing_cases_cannot_silently_reduce_coverage(self):
        with self.assertRaisesRegex(ValueError, "missing"):
            select_cases({"reject"}, {"reject"}, {"positive"})

    def test_overlap_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "overlap"):
            select_cases({"same"}, {"same"}, {"same"})

    def test_runner_exposes_its_required_inputs(self):
        result = subprocess.run([sys.executable, str(Path(__file__).with_name("unqualified_vocal_ci.py")), "--help"],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0)
        self.assertIn("--runtime", result.stdout)
        self.assertIn("--executable", result.stdout)

    def test_runtime_may_be_provided_by_ci_environment(self):
        from unqualified_vocal_ci import runtime_argument
        self.assertEqual(runtime_argument(None, {"LMMS_UNQUALIFIED_RUNTIME_PATH": "fixture.dll"}),
                         Path("fixture.dll"))
        with self.assertRaises(ValueError):
            runtime_argument(None, {})

    def test_report_rejects_skips_and_multiple_digit_failures(self):
        from unqualified_vocal_ci import successful_report
        self.assertTrue(successful_report("PASS   : Suite::case()\nTotals: 3 passed, 0 failed, 0 skipped, 0 blacklisted", "case"))
        self.assertFalse(successful_report("Totals: 3 passed, 0 failed, 0 skipped, 0 blacklisted", "case"))
        self.assertFalse(successful_report("Totals: 3 passed, 0 failed, 10 skipped, 0 blacklisted", "case"))
        self.assertFalse(successful_report("Totals: 3 passed, 10 failed, 0 skipped, 0 blacklisted", "case"))
        self.assertFalse(successful_report("0 failed, 0 skipped", "case"))


if __name__ == "__main__":
    unittest.main()
