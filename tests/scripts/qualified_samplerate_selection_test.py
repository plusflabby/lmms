"""Exercise the production CMake finder against the real UCRT64 packages."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


NATIVE_ROOT = Path(__file__).resolve().parents[2]
UCRT = Path("C:/msys64/ucrt64")
PATCHED = Path(os.environ["LMMS_QUALIFIED_SAMPLERATE_TEST_PREFIX"])
MODULE_DIR = Path(os.environ.get("LMMS_TEST_CMAKE_MODULE_DIR", str(NATIVE_ROOT / "cmake/modules")))


class QualifiedSamplerateSelectionTest(unittest.TestCase):
    def configure(self, prefix, module_dir=MODULE_DIR, qualified=True, simulated_platform="", include_prefix=True):
        with tempfile.TemporaryDirectory(prefix="lmms-samplerate-selection-") as temp:
            root = Path(temp)
            (root / "CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 3.13)\n'
                'project(SamplerateSelection LANGUAGES CXX)\n'
                f'list(PREPEND CMAKE_MODULE_PATH "{module_dir.as_posix()}")\n'
                'set(LMMS_BUILD_WIN32 TRUE)\n'
                'if(LMMS_TEST_PLATFORM STREQUAL "msvc")\n  set(MSVC TRUE)\n'
                'elseif(LMMS_TEST_PLATFORM STREQUAL "arm64")\n  set(CMAKE_SYSTEM_PROCESSOR ARM64)\nendif()\n'
                'find_package(Samplerate 0.1.8 MODULE REQUIRED)\n',
                encoding="utf-8",
            )
            env = os.environ.copy()
            env["PATH"] = f"{UCRT / 'bin'};C:/msys64/usr/bin;{env['PATH']}"
            args = [str(UCRT / "bin/cmake.exe"), "-S", str(root), "-B", str(root / "build"),
                    "-G", "Ninja", f"-DCMAKE_CXX_COMPILER={UCRT.as_posix()}/bin/c++.exe",
                    f"-DCMAKE_MAKE_PROGRAM={UCRT.as_posix()}/bin/ninja.exe",
                    f"-DSampleRate_DIR={prefix.as_posix()}/lib/cmake/SampleRate",
                    f"-DLMMS_REQUIRE_QUALIFIED_VOCAL_RUNTIME={'ON' if qualified else 'OFF'}",
                    f"-DLMMS_TEST_PLATFORM={simulated_platform}"]
            if qualified and include_prefix:
                args.append(f"-DLMMS_QUALIFIED_SAMPLERATE_PREFIX={prefix.as_posix()}")
            return subprocess.run(args, env=env, capture_output=True, text=True, timeout=60)

    def test_system_samplerate_is_rejected_at_configuration(self):
        result = self.configure(UCRT)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_qualified_profile_without_prefix_cannot_fall_back_to_system(self):
        result = self.configure(UCRT, include_prefix=False)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_unqualified_windows_build_keeps_the_existing_dependency(self):
        result = self.configure(UCRT, qualified=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_unqualified_msvc_configuration_does_not_require_the_ucrt_prefix(self):
        result = self.configure(UCRT, qualified=False, simulated_platform="msvc")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_unqualified_arm64_configuration_does_not_require_the_ucrt_prefix(self):
        result = self.configure(UCRT, qualified=False, simulated_platform="arm64")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_qualified_ucrt_profile_cannot_be_enabled_for_msvc(self):
        result = self.configure(PATCHED, qualified=True, simulated_platform="msvc")
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_qualified_ucrt_profile_cannot_be_enabled_for_arm64(self):
        result = self.configure(PATCHED, qualified=True, simulated_platform="arm64")
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_unqualified_build_stays_disabled_even_if_patched_dll_is_on_path(self):
        with tempfile.TemporaryDirectory(prefix="lmms-vocal-build-profile-") as temp:
            root = Path(temp)
            (root / "lmms_export.h").write_text("#define LMMS_EXPORT\n", encoding="utf-8")
            (root / "lmmsconfig.h").write_text("/* unqualified build */\n", encoding="utf-8")
            (root / "main.cpp").write_text(
                '#include <QCoreApplication>\n'
                '#include <samplerate.h>\n'
                '#include "VocalRuntimeQualification.h"\n'
                'int main(int argc, char** argv) { QCoreApplication app(argc, argv); '
                '(void)src_get_version(); return lmms::qualifiedVocalSamplerateRuntime() ? 1 : 0; }\n', encoding="utf-8")
            (root / "CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 3.13)\n'
                'project(VocalBuildProfile LANGUAGES CXX)\n'
                'find_package(Qt5 COMPONENTS Core REQUIRED)\n'
                f'list(PREPEND CMAKE_MODULE_PATH "{MODULE_DIR.as_posix()}")\n'
                'set(LMMS_BUILD_WIN32 TRUE)\n'
                'find_package(Samplerate 0.1.8 MODULE REQUIRED)\n'
                f'add_executable(probe main.cpp "{(NATIVE_ROOT / "src/core/vocal/VocalRuntimeQualification.cpp").as_posix()}")\n'
                f'target_include_directories(probe PRIVATE "${{CMAKE_CURRENT_SOURCE_DIR}}" "{(NATIVE_ROOT / "src/core/vocal").as_posix()}")\n'
                'target_link_libraries(probe PRIVATE Qt5::Core SampleRate::samplerate)\n', encoding="utf-8")
            env = os.environ.copy()
            env["PATH"] = f"{UCRT / 'bin'};C:/msys64/usr/bin;{env['PATH']}"
            cmake = str(UCRT / "bin/cmake.exe")
            build = root / "build"
            config = subprocess.run([cmake, "-S", str(root), "-B", str(build), "-G", "Ninja",
                                     f"-DCMAKE_CXX_COMPILER={UCRT.as_posix()}/bin/c++.exe",
                                     f"-DCMAKE_MAKE_PROGRAM={UCRT.as_posix()}/bin/ninja.exe",
                                     f"-DCMAKE_PREFIX_PATH={UCRT.as_posix()}",
                                     "-DLMMS_REQUIRE_QUALIFIED_VOCAL_RUNTIME=OFF"],
                                    env=env, capture_output=True, text=True, timeout=60)
            self.assertEqual(config.returncode, 0, config.stdout + config.stderr)
            built = subprocess.run([cmake, "--build", str(build)], env=env,
                                   capture_output=True, text=True, timeout=60)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            shutil.copy2(PATCHED / "bin/libsamplerate-0.dll", build / "libsamplerate-0.dll")
            result = subprocess.run([str(build / "probe.exe")], env=env,
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, "unqualified build admitted vocal editing")

    def test_approved_patched_samplerate_is_accepted(self):
        result = self.configure(PATCHED)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_modified_dll_is_rejected_even_with_matching_package_config(self):
        with tempfile.TemporaryDirectory(prefix="lmms-samplerate-wrong-") as temp:
            prefix = Path(temp) / "prefix"
            shutil.copytree(PATCHED, prefix)
            with (prefix / "bin/libsamplerate-0.dll").open("ab") as dll:
                dll.write(b"not the qualified runtime")
            result = self.configure(prefix)
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_missing_dll_does_not_fall_back_to_system(self):
        with tempfile.TemporaryDirectory(prefix="lmms-samplerate-missing-") as temp:
            prefix = Path(temp) / "prefix"
            shutil.copytree(PATCHED, prefix, ignore=shutil.ignore_patterns("libsamplerate-0.dll"))
            result = self.configure(prefix)
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_modified_import_library_cannot_be_linked(self):
        with tempfile.TemporaryDirectory(prefix="lmms-samplerate-import-") as temp:
            prefix = Path(temp) / "prefix"
            shutil.copytree(PATCHED, prefix)
            with (prefix / "lib/libsamplerate.dll.a").open("ab") as library:
                library.write(b"not the qualified import library")
            result = self.configure(prefix)
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_package_config_cannot_redirect_headers_to_system(self):
        with tempfile.TemporaryDirectory(prefix="lmms-samplerate-header-") as temp:
            prefix = Path(temp) / "prefix"
            shutil.copytree(PATCHED, prefix)
            config = prefix / "lib/cmake/SampleRate/SampleRateTargets.cmake"
            original = config.read_text(encoding="utf-8")
            redirected = original.replace('${_IMPORT_PREFIX}/include', 'C:/msys64/ucrt64/include')
            self.assertNotEqual(redirected, original)
            config.write_text(redirected, encoding="utf-8")
            result = self.configure(prefix)
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_package_config_cannot_inject_an_unqualified_link_library(self):
        with tempfile.TemporaryDirectory(prefix="lmms-samplerate-link-") as temp:
            prefix = Path(temp) / "prefix"
            shutil.copytree(PATCHED, prefix)
            config = prefix / "lib/cmake/SampleRate/SampleRateTargets.cmake"
            original = config.read_text(encoding="utf-8")
            redirected = original.replace('INTERFACE_INCLUDE_DIRECTORIES ',
                                          'INTERFACE_LINK_LIBRARIES "unqualified_samplerate"\n  INTERFACE_INCLUDE_DIRECTORIES ', 1)
            self.assertNotEqual(redirected, original)
            config.write_text(redirected, encoding="utf-8")
            result = self.configure(prefix)
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_modified_approved_patch_is_rejected(self):
        with tempfile.TemporaryDirectory(prefix="lmms-samplerate-patch-") as temp:
            cmake = Path(temp) / "cmake"
            modules = cmake / "modules"
            dependencies = cmake / "dependencies/samplerate"
            modules.mkdir(parents=True)
            shutil.copytree(NATIVE_ROOT / "cmake/dependencies/samplerate", dependencies)
            for name in ("FindSamplerate.cmake", "ImportedTargetHelpers.cmake", "VerifyQualifiedSamplerate.cmake"):
                shutil.copy2(MODULE_DIR / name, modules / name)
            with (dependencies / "libsamplerate-0.2.2-linear-history.patch").open("ab") as patch:
                patch.write(b"changed patch")
            result = self.configure(PATCHED, modules)
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_runtime_changed_after_configuration_cannot_be_built(self):
        with tempfile.TemporaryDirectory(prefix="lmms-samplerate-revalidate-") as temp:
            root = Path(temp)
            prefix = root / "prefix"
            shutil.copytree(PATCHED, prefix)
            project = root / "project"
            project.mkdir()
            (project / "main.cpp").write_text("int main() { return 0; }\n", encoding="utf-8")
            (project / "CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 3.13)\n'
                'project(SamplerateRevalidation LANGUAGES CXX)\n'
                f'list(PREPEND CMAKE_MODULE_PATH "{MODULE_DIR.as_posix()}")\n'
                'set(LMMS_BUILD_WIN32 TRUE)\n'
                'find_package(Samplerate 0.1.8 MODULE REQUIRED)\n'
                'add_executable(probe main.cpp)\n'
                'target_link_libraries(probe PRIVATE SampleRate::samplerate)\n'
                'if(COMMAND lmms_require_qualified_samplerate)\n'
                '  lmms_require_qualified_samplerate(probe)\n'
                'endif()\n', encoding="utf-8")
            env = os.environ.copy()
            env["PATH"] = f"{UCRT / 'bin'};C:/msys64/usr/bin;{env['PATH']}"
            cmake = str(UCRT / "bin/cmake.exe")
            build = root / "build"
            config = subprocess.run([cmake, "-S", str(project), "-B", str(build), "-G", "Ninja",
                                     f"-DCMAKE_CXX_COMPILER={UCRT.as_posix()}/bin/c++.exe",
                                     f"-DCMAKE_MAKE_PROGRAM={UCRT.as_posix()}/bin/ninja.exe",
                                     f"-DLMMS_QUALIFIED_SAMPLERATE_PREFIX={prefix.as_posix()}",
                                     "-DLMMS_REQUIRE_QUALIFIED_VOCAL_RUNTIME=ON"],
                                    env=env, capture_output=True, text=True, timeout=60)
            self.assertEqual(config.returncode, 0, config.stdout + config.stderr)
            with (prefix / "bin/libsamplerate-0.dll").open("ab") as dll:
                dll.write(b"changed after configuration")
            built = subprocess.run([cmake, "--build", str(build)], env=env,
                                   capture_output=True, text=True, timeout=60)
            self.assertNotEqual(built.returncode, 0, built.stdout + built.stderr)

    def check_build_runtime_staging(self, existing=None, expect_existing_failure=True):
        with tempfile.TemporaryDirectory(prefix="lmms-samplerate-bundle-") as temp:
            root = Path(temp)
            project = root / "project"
            project.mkdir()
            (project / "main.cpp").write_text("int main() { return 0; }\n", encoding="utf-8")
            (project / "CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 3.13)\n'
                'project(SamplerateBundle LANGUAGES CXX)\n'
                f'list(PREPEND CMAKE_MODULE_PATH "{MODULE_DIR.as_posix()}")\n'
                'set(LMMS_BUILD_WIN32 TRUE)\n'
                'find_package(Samplerate 0.1.8 MODULE REQUIRED)\n'
                'add_executable(probe main.cpp)\n'
                'if(COMMAND lmms_stage_qualified_samplerate)\n'
                '  lmms_stage_qualified_samplerate(probe)\n'
                'endif()\n', encoding="utf-8")
            env = os.environ.copy()
            env["PATH"] = f"{UCRT / 'bin'};C:/msys64/usr/bin;{env['PATH']}"
            env["PSModulePath"] = str(root / "empty-module-path")
            cmake = str(UCRT / "bin/cmake.exe")
            build = root / "build"
            config = subprocess.run([cmake, "-S", str(project), "-B", str(build), "-G", "Ninja",
                                     f"-DCMAKE_CXX_COMPILER={UCRT.as_posix()}/bin/c++.exe",
                                     f"-DCMAKE_MAKE_PROGRAM={UCRT.as_posix()}/bin/ninja.exe",
                                     f"-DLMMS_QUALIFIED_SAMPLERATE_PREFIX={PATCHED.as_posix()}",
                                     "-DLMMS_REQUIRE_QUALIFIED_VOCAL_RUNTIME=ON"],
                                    env=env, capture_output=True, text=True, timeout=60)
            self.assertEqual(config.returncode, 0, config.stdout + config.stderr)
            target = build / "libsamplerate-0.dll"
            if existing is not None:
                target.write_bytes(existing)
            built = subprocess.run([cmake, "--build", str(build)], env=env,
                                   capture_output=True, text=True, timeout=60)
            if existing is not None and expect_existing_failure:
                self.assertNotEqual(built.returncode, 0, built.stdout + built.stderr)
                self.assertEqual(target.read_bytes(), existing, "existing file must be preserved")
            else:
                self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
                self.assertTrue(target.is_file(), "qualified runtime was not staged")
                self.assertEqual(target.read_bytes(), (PATCHED / "bin/libsamplerate-0.dll").read_bytes())

    def test_build_places_approved_runtime_beside_executable(self):
        self.check_build_runtime_staging()

    def test_build_does_not_overwrite_an_existing_different_runtime(self):
        self.check_build_runtime_staging(b"a pre-existing file to preserve")

    def test_build_accepts_existing_qualified_runtime_without_powershell_modules(self):
        self.check_build_runtime_staging((PATCHED / "bin/libsamplerate-0.dll").read_bytes(),
                                         expect_existing_failure=False)

    def test_stager_does_not_depend_on_powershell_module_autoload(self):
        with tempfile.TemporaryDirectory(prefix="lmms-samplerate-stager-") as temp:
            target = Path(temp) / "libsamplerate-0.dll"
            shutil.copy2(PATCHED / "bin/libsamplerate-0.dll", target)
            stager = MODULE_DIR / "StageQualifiedSamplerate.ps1"
            script = ("$PSModuleAutoLoadingPreference='None'; & '" + str(stager).replace("'", "''")
                      + "' -Source '" + str(PATCHED / "bin/libsamplerate-0.dll").replace("'", "''")
                      + "' -Destination '" + str(target).replace("'", "''") + "'")
            result = subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", script],
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_ctest_cannot_run_when_its_local_runtime_is_missing(self):
        with tempfile.TemporaryDirectory(prefix="lmms-samplerate-ctest-") as temp:
            root = Path(temp)
            project = root / "project"
            project.mkdir()
            (project / "CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 3.13)\n'
                'project(SamplerateCTest LANGUAGES CXX)\n'
                f'list(PREPEND CMAKE_MODULE_PATH "{MODULE_DIR.as_posix()}")\n'
                'set(LMMS_BUILD_WIN32 TRUE)\n'
                'find_package(Samplerate 0.1.8 MODULE REQUIRED)\n'
                'enable_testing()\n'
                'add_test(NAME probe COMMAND "${CMAKE_COMMAND}" -E true)\n'
                'if(COMMAND lmms_guard_qualified_test_runtime)\n'
                '  lmms_guard_qualified_test_runtime(probe)\n'
                'endif()\n', encoding="utf-8")
            env = os.environ.copy()
            env["PATH"] = f"{UCRT / 'bin'};C:/msys64/usr/bin;{env['PATH']}"
            cmake = str(UCRT / "bin/cmake.exe")
            build = root / "build"
            config = subprocess.run([cmake, "-S", str(project), "-B", str(build), "-G", "Ninja",
                                     f"-DCMAKE_CXX_COMPILER={UCRT.as_posix()}/bin/c++.exe",
                                     f"-DCMAKE_MAKE_PROGRAM={UCRT.as_posix()}/bin/ninja.exe",
                                     f"-DLMMS_QUALIFIED_SAMPLERATE_PREFIX={PATCHED.as_posix()}",
                                     "-DLMMS_REQUIRE_QUALIFIED_VOCAL_RUNTIME=ON"],
                                    env=env, capture_output=True, text=True, timeout=60)
            self.assertEqual(config.returncode, 0, config.stdout + config.stderr)
            result = subprocess.run([str(UCRT / "bin/ctest.exe"), "--test-dir", str(build),
                                     "-R", "^probe$", "--output-on-failure"],
                                    env=env, capture_output=True, text=True, timeout=60)
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_runtime_changed_after_configuration_cannot_be_installed(self):
        with tempfile.TemporaryDirectory(prefix="lmms-samplerate-install-") as temp:
            root = Path(temp)
            prefix = root / "prefix"
            shutil.copytree(PATCHED, prefix)
            project = root / "project"
            project.mkdir()
            (project / "CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 3.13)\n'
                'project(SamplerateInstall LANGUAGES CXX)\n'
                f'list(PREPEND CMAKE_MODULE_PATH "{MODULE_DIR.as_posix()}")\n'
                'set(LMMS_BUILD_WIN32 TRUE)\n'
                'find_package(Samplerate 0.1.8 MODULE REQUIRED)\n'
                'install(FILES "${LMMS_QUALIFIED_SAMPLERATE_PREFIX}/bin/libsamplerate-0.dll" DESTINATION bin)\n',
                encoding="utf-8")
            env = os.environ.copy()
            env["PATH"] = f"{UCRT / 'bin'};C:/msys64/usr/bin;{env['PATH']}"
            cmake = str(UCRT / "bin/cmake.exe")
            build, stage = root / "build", root / "stage"
            config = subprocess.run([cmake, "-S", str(project), "-B", str(build), "-G", "Ninja",
                                     f"-DCMAKE_CXX_COMPILER={UCRT.as_posix()}/bin/c++.exe",
                                     f"-DCMAKE_MAKE_PROGRAM={UCRT.as_posix()}/bin/ninja.exe",
                                     f"-DLMMS_QUALIFIED_SAMPLERATE_PREFIX={prefix.as_posix()}",
                                     "-DLMMS_REQUIRE_QUALIFIED_VOCAL_RUNTIME=ON"],
                                    env=env, capture_output=True, text=True, timeout=60)
            self.assertEqual(config.returncode, 0, config.stdout + config.stderr)
            with (prefix / "bin/libsamplerate-0.dll").open("ab") as dll:
                dll.write(b"changed before installation")
            installed = subprocess.run([cmake, "--install", str(build), "--prefix", str(stage)],
                                       env=env, capture_output=True, text=True, timeout=60)
            self.assertNotEqual(installed.returncode, 0, installed.stdout + installed.stderr)
            self.assertFalse((stage / "bin/libsamplerate-0.dll").exists())

    def test_install_detects_corruption_of_the_copied_runtime(self):
        with tempfile.TemporaryDirectory(prefix="lmms-samplerate-stage-") as temp:
            root = Path(temp)
            project = root / "project"
            project.mkdir()
            (project / "CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 3.13)\n'
                'project(SamplerateStage LANGUAGES CXX)\n'
                f'list(PREPEND CMAKE_MODULE_PATH "{MODULE_DIR.as_posix()}")\n'
                'set(LMMS_BUILD_WIN32 TRUE)\n'
                'find_package(Samplerate 0.1.8 MODULE REQUIRED)\n'
                'install(FILES "${LMMS_QUALIFIED_SAMPLERATE_PREFIX}/bin/libsamplerate-0.dll" DESTINATION bin)\n'
                'install(CODE "file(APPEND \\\"${CMAKE_INSTALL_PREFIX}/bin/libsamplerate-0.dll\\\" bad)")\n'
                'if(COMMAND lmms_verify_installed_samplerate)\n'
                '  lmms_verify_installed_samplerate(bin)\n'
                'endif()\n', encoding="utf-8")
            env = os.environ.copy()
            env["PATH"] = f"{UCRT / 'bin'};C:/msys64/usr/bin;{env['PATH']}"
            cmake = str(UCRT / "bin/cmake.exe")
            build, stage = root / "build", root / "stage"
            config = subprocess.run([cmake, "-S", str(project), "-B", str(build), "-G", "Ninja",
                                     f"-DCMAKE_CXX_COMPILER={UCRT.as_posix()}/bin/c++.exe",
                                     f"-DCMAKE_MAKE_PROGRAM={UCRT.as_posix()}/bin/ninja.exe",
                                     f"-DCMAKE_INSTALL_PREFIX={stage.as_posix()}",
                                     f"-DLMMS_QUALIFIED_SAMPLERATE_PREFIX={PATCHED.as_posix()}",
                                     "-DLMMS_REQUIRE_QUALIFIED_VOCAL_RUNTIME=ON"],
                                    env=env, capture_output=True, text=True, timeout=60)
            self.assertEqual(config.returncode, 0, config.stdout + config.stderr)
            installed = subprocess.run([cmake, "--install", str(build)], env=env,
                                       capture_output=True, text=True, timeout=60)
            self.assertNotEqual(installed.returncode, 0, installed.stdout + installed.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
