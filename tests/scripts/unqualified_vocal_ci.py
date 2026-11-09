"""Fail-closed classification for qualified and unqualified vocal tests."""

import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import subprocess
import tempfile


def select_cases(available, unqualified, qualified_only):
    if unqualified & qualified_only:
        raise ValueError("overlap in case classification")
    if available - (unqualified | qualified_only):
        raise ValueError("unclassified vocal test cases")
    if (unqualified | qualified_only) - available:
        raise ValueError("missing vocal test cases")
    return sorted(unqualified)


def runtime_argument(argument, environment):
    value = argument or environment.get("LMMS_UNQUALIFIED_RUNTIME_PATH")
    if not value:
        raise ValueError("unqualified runtime path is required")
    return Path(value)


def successful_report(details, case):
    totals = re.search(r"^Totals:\s+\d+ passed, 0 failed, 0 skipped, 0 blacklisted\b", details, re.MULTILINE)
    passed = re.search(r"^PASS\s+: [^\r\n]*::" + re.escape(case) + r"\(\)", details, re.MULTILINE)
    return bool(totals and passed)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True, type=Path)
    parser.add_argument("--runtime", type=Path)
    args = parser.parse_args()
    manifest = json.loads(Path(__file__).with_name("unqualified_vocal_ci_cases.json").read_text(encoding="utf-8"))
    name = args.executable.stem
    policy = manifest[name]
    digest = hashlib.sha256(runtime_argument(args.runtime, os.environ).read_bytes()).hexdigest()
    env = os.environ.copy()
    env["LMMS_TEST_EXPECT_UNQUALIFIED_VOCAL_RUNTIME"] = digest
    env["QT_QPA_PLATFORM"] = "offscreen"
    listed = subprocess.run([str(args.executable), "-functions"], env=env, capture_output=True, text=True, check=True)
    available = {line.strip().removesuffix("()") for line in listed.stdout.splitlines() if line.strip()}
    chosen = select_cases(available, set(policy["unqualified"]), set(policy["qualified_only"]))
    print(f"{name}: running {len(chosen)} unqualified cases; {len(policy['qualified_only'])} qualified-only cases run in UCRT64 CI", flush=True)
    print("Qualified-only cases: " + ", ".join(sorted(policy["qualified_only"])), flush=True)
    for case in chosen:
        with tempfile.TemporaryDirectory(prefix="lmms-unqualified-ci-") as temp:
            report = Path(temp) / "result.txt"
            result = subprocess.run([str(args.executable), case, "-o", f"{report.as_posix()},txt"], env=env,
                                    capture_output=True, text=True, timeout=120)
            details = report.read_text(encoding="utf-8", errors="replace") if report.exists() else ""
            print(details, flush=True)
            if result.returncode != 0 or not successful_report(details, case):
                raise RuntimeError(f"{case} failed or skipped: {result.stdout} {result.stderr}")


if __name__ == "__main__":
    main()
