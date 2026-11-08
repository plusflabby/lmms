# Copyright (c) 2023 Dominic Clark
#
# Redistribution and use is allowed according to the terms of the New BSD license.
# For details see the accompanying COPYING-CMAKE-SCRIPTS file.

include(ImportedTargetHelpers)

option(LMMS_REQUIRE_QUALIFIED_VOCAL_RUNTIME "Require the approved UCRT64 Phase 3J-B runtime" OFF)
if(LMMS_REQUIRE_QUALIFIED_VOCAL_RUNTIME AND NOT LMMS_BUILD_WIN32)
	message(FATAL_ERROR "The qualified vocal runtime profile is only available for Windows UCRT64")
endif()
if(LMMS_BUILD_WIN32 AND LMMS_REQUIRE_QUALIFIED_VOCAL_RUNTIME)
	if(CMAKE_VERSION VERSION_LESS "3.21")
		message(FATAL_ERROR "Qualified Windows packaging requires CMake 3.21 or newer")
	endif()
	string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _vocal_target_processor)
	if(MSVC OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8
		OR NOT _vocal_target_processor MATCHES "^(amd64|x86_64)$")
		message(FATAL_ERROR "The qualified vocal runtime profile requires UCRT64 x86-64 GCC")
	endif()
	# Only the pinned UCRT64 image has passed the Phase 3J-B endpoint and
	# playback qualification. Never let config-mode or pkg-config fall back to
	# a system package if the explicitly selected prefix is absent or wrong.
	if(NOT LMMS_QUALIFIED_SAMPLERATE_PREFIX)
		message(FATAL_ERROR "Set LMMS_QUALIFIED_SAMPLERATE_PREFIX to the approved patched UCRT64 libsamplerate prefix")
	endif()
	get_filename_component(_qualified_prefix "${LMMS_QUALIFIED_SAMPLERATE_PREFIX}" REALPATH)
	set(_qualified_config "${_qualified_prefix}/lib/cmake/SampleRate")
	if(SampleRate_DIR)
		get_filename_component(_requested_config "${SampleRate_DIR}" REALPATH)
		if(NOT _requested_config STREQUAL _qualified_config)
			message(FATAL_ERROR "SampleRate_DIR differs from the qualified libsamplerate prefix")
		endif()
	endif()
	if(TARGET SampleRate::samplerate)
		message(FATAL_ERROR "libsamplerate was selected before its qualified prefix could be verified")
	endif()
	set_property(GLOBAL PROPERTY LMMS_SAMPLERATE_VERIFIER "${CMAKE_CURRENT_LIST_DIR}/VerifyQualifiedSamplerate.cmake")
	set_property(GLOBAL PROPERTY LMMS_SAMPLERATE_STAGER "${CMAKE_CURRENT_LIST_DIR}/StageQualifiedSamplerate.ps1")
	set_property(GLOBAL PROPERTY LMMS_SAMPLERATE_TEST_VERIFIER "${CMAKE_CURRENT_LIST_DIR}/VerifyQualifiedTestRuntime.cmake")
	include("${CMAKE_CURRENT_LIST_DIR}/VerifyQualifiedSamplerate.cmake")
	# Register this before other install rules, including component installs.
	# A prefix changed after configure must fail before files are published.
	install(CODE "set(LMMS_QUALIFIED_SAMPLERATE_PREFIX [==[${_qualified_prefix}]==])\ninclude([==[${CMAKE_CURRENT_LIST_DIR}/VerifyQualifiedSamplerate.cmake]==])" ALL_COMPONENTS)
	set(SampleRate_DIR "${_qualified_config}" CACHE PATH "Qualified libsamplerate config" FORCE)
	find_package(SampleRate CONFIG REQUIRED PATHS "${_qualified_config}" NO_DEFAULT_PATH)
	get_target_property(_selected_headers SampleRate::samplerate INTERFACE_INCLUDE_DIRECTORIES)
	get_filename_component(_selected_headers "${_selected_headers}" REALPATH)
	get_filename_component(_expected_headers "${_qualified_prefix}/include" REALPATH)
	if(NOT _selected_headers STREQUAL _expected_headers)
		message(FATAL_ERROR "Imported libsamplerate target does not use the qualified headers")
	endif()
	foreach(_property IMPORTED_LOCATION_RELEASE IMPORTED_IMPLIB_RELEASE)
		get_target_property(_selected SampleRate::samplerate "${_property}")
		if(_property STREQUAL "IMPORTED_LOCATION_RELEASE")
			set(_expected_path "${_qualified_prefix}/bin/libsamplerate-0.dll")
		else()
			set(_expected_path "${_qualified_prefix}/lib/libsamplerate.dll.a")
		endif()
		get_filename_component(_selected "${_selected}" REALPATH)
		get_filename_component(_expected_path "${_expected_path}" REALPATH)
		if(NOT _selected STREQUAL _expected_path)
			message(FATAL_ERROR "Imported libsamplerate target does not use the qualified ${_property}")
		endif()
	endforeach()
	# The pinned header and binary establish the version without executing a
	# configure-time probe that could load a different same-named DLL.
	set(Samplerate_VERSION "0.2.2")
	function(lmms_require_qualified_samplerate target)
		get_property(_verifier GLOBAL PROPERTY LMMS_SAMPLERATE_VERIFIER)
		if(NOT TARGET lmms_verify_qualified_samplerate)
			add_custom_target(lmms_verify_qualified_samplerate ALL
				COMMAND "${CMAKE_COMMAND}" "-DLMMS_QUALIFIED_SAMPLERATE_PREFIX=${LMMS_QUALIFIED_SAMPLERATE_PREFIX}" -P "${_verifier}"
				VERBATIM)
		endif()
		add_dependencies(${target} lmms_verify_qualified_samplerate)
	endfunction()
	function(lmms_stage_qualified_samplerate target)
		get_property(_stager GLOBAL PROPERTY LMMS_SAMPLERATE_STAGER)
		lmms_require_qualified_samplerate(${target})
		add_custom_command(TARGET ${target} POST_BUILD
			COMMAND powershell.exe -NoProfile -NonInteractive -File "${_stager}"
				-Source "${LMMS_QUALIFIED_SAMPLERATE_PREFIX}/bin/libsamplerate-0.dll"
				-Destination "$<TARGET_FILE_DIR:${target}>/libsamplerate-0.dll"
			VERBATIM)
	endfunction()
	function(lmms_guard_qualified_test_runtime test_name)
		if(NOT TEST QualifiedSamplerateTestRuntimeGate)
			get_property(_verifier GLOBAL PROPERTY LMMS_SAMPLERATE_TEST_VERIFIER)
			add_test(NAME QualifiedSamplerateTestRuntimeGate
				COMMAND "${CMAKE_COMMAND}" "-DLMMS_QUALIFIED_TEST_RUNTIME=${CMAKE_CURRENT_BINARY_DIR}/libsamplerate-0.dll" -P "${_verifier}")
			set_tests_properties(QualifiedSamplerateTestRuntimeGate PROPERTIES FIXTURES_SETUP QualifiedSamplerateTestRuntime)
		endif()
		set_tests_properties(${test_name} PROPERTIES
			FIXTURES_REQUIRED QualifiedSamplerateTestRuntime
			ENVIRONMENT "LMMS_EXPECTED_SAMPLERATE_SHA256=BF275A0636DC7DADAC8A691842C06B460109EFC213C68B5EF6576A216E55D734;LMMS_EXPECTED_SAMPLERATE_DIR=${CMAKE_CURRENT_BINARY_DIR}")
	endfunction()
	function(lmms_verify_installed_samplerate destination)
		set(_script [=[
			set(_installed "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/@destination@/libsamplerate-0.dll")
			if(NOT EXISTS "${_installed}")
				message(FATAL_ERROR "Installed qualified libsamplerate DLL is missing")
			endif()
			file(SHA256 "${_installed}" _installed_hash)
			string(TOUPPER "${_installed_hash}" _installed_hash)
			if(NOT _installed_hash STREQUAL "BF275A0636DC7DADAC8A691842C06B460109EFC213C68B5EF6576A216E55D734")
				message(FATAL_ERROR "Installed qualified libsamplerate DLL SHA-256 mismatch")
			endif()
		]=])
		string(CONFIGURE "${_script}" _script @ONLY)
		install(CODE "${_script}" ALL_COMPONENTS)
	endfunction()
endif()

find_package_config_mode_with_fallback(SampleRate SampleRate::samplerate
	LIBRARY_NAMES "samplerate" "libsamplerate" "libsamplerate-0"
	INCLUDE_NAMES "samplerate.h"
	PKG_CONFIG samplerate
	PREFIX Samplerate
)

determine_version_from_source(Samplerate_VERSION SampleRate::samplerate [[
	#include <iostream>
	#include <string_view>
	#include <samplerate.h>

	auto main() -> int
	{
		// Version string has the format "name-version copyright"
		const auto version = std::string_view{src_get_version()};
		const auto begin = version.find('-') + 1;
		const auto end = version.find(' ', begin);
		std::cout << version.substr(begin, end - begin);
	}
]])

include(FindPackageHandleStandardArgs)

find_package_handle_standard_args(Samplerate
	REQUIRED_VARS Samplerate_LIBRARY Samplerate_INCLUDE_DIRS
	VERSION_VAR Samplerate_VERSION
)
