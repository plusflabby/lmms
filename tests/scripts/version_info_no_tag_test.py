"""A pull-request build without release tags keeps the internal version."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class VersionInfoNoTagTest(unittest.TestCase):
    def test_pull_request_without_tags_uses_internal_version(self):
        module = Path(__file__).resolve().parents[2] / "cmake/modules/VersionInfo.cmake"
        with tempfile.TemporaryDirectory(prefix="lmms-version-notags-") as temp:
            root = Path(temp)
            subprocess.run(["git", "init", "-q", str(root)], check=True, capture_output=True)
            script = root / "probe.cmake"
            script.write_text('set(VERSION "1.3.0-alpha")\n'
                              'set(VERSION_MAJOR "1")\nset(VERSION_MINOR "3")\n'
                              'set(VERSION_RELEASE "0")\nset(VERSION_STAGE "alpha")\n'
                              f'set(CMAKE_SOURCE_DIR "{root.as_posix()}")\n'
                              f'include("{module.as_posix()}")\n'
                              'if(NOT VERSION STREQUAL "1.3.0-alpha")\n'
                              '  message(FATAL_ERROR "Internal version was not preserved")\nendif()\n', encoding="utf-8")
            env = os.environ.copy()
            env["GITHUB_REF"] = "refs/pull/6/merge"
            result = subprocess.run(["cmake", "-P", str(script)], env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_tagged_pull_request_retains_version_and_metadata(self):
        module = Path(__file__).resolve().parents[2] / "cmake/modules/VersionInfo.cmake"
        with tempfile.TemporaryDirectory(prefix="lmms-version-tagged-") as temp:
            root = Path(temp)
            subprocess.run(["git", "init", "-q", str(root)], check=True, capture_output=True)
            subprocess.run(["git", "-C", str(root), "-c", "user.name=Test", "-c", "user.email=test@example.invalid",
                            "commit", "-q", "--allow-empty", "-m", "Merge abcdef1 into 1234567"], check=True)
            subprocess.run(["git", "-C", str(root), "tag", "v1.2.3"], check=True)
            script = root / "probe.cmake"
            script.write_text(f'set(CMAKE_SOURCE_DIR "{root.as_posix()}")\n'
                              f'include("{module.as_posix()}")\n'
                              'if(NOT VERSION STREQUAL "1.2.3+pr6")\n'
                              '  message(FATAL_ERROR "Tagged version or PR metadata changed: ${VERSION}")\n'
                              'endif()\n', encoding="utf-8")
            env = os.environ.copy()
            env["GITHUB_REF"] = "refs/pull/6/merge"
            result = subprocess.run(["cmake", "-P", str(script)], env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
