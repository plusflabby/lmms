# Verify the exact UCRT64 files in the approved development manifest. This
# file is usable both at configure time and as a build/install script.
if(NOT LMMS_QUALIFIED_SAMPLERATE_PREFIX)
	message(FATAL_ERROR "The qualified libsamplerate prefix is required")
endif()
set(_samplerate_provenance "${CMAKE_CURRENT_LIST_DIR}/../dependencies/samplerate")
foreach(_entry
	"libsamplerate-0.2.2-linear-history.patch|065DD14B04C2795FDB0D33EB5539AC62C4714A7248552A41199451DF8B595FA7"
	"ucrt64-development-manifest.json|1F1D80FF8FA4AA2B9117AE9C24DCD19FEEF0A166E267D2FBAF5248A0DF2C0908")
	string(REPLACE "|" ";" _parts "${_entry}")
	list(GET _parts 0 _relative)
	list(GET _parts 1 _expected)
	if(NOT EXISTS "${_samplerate_provenance}/${_relative}")
		message(FATAL_ERROR "Qualified libsamplerate provenance is missing: ${_relative}")
	endif()
	file(SHA256 "${_samplerate_provenance}/${_relative}" _actual)
	string(TOUPPER "${_actual}" _actual)
	if(NOT _actual STREQUAL _expected)
		message(FATAL_ERROR "Qualified libsamplerate provenance SHA-256 mismatch: ${_relative}")
	endif()
endforeach()
get_filename_component(_qualified_prefix "${LMMS_QUALIFIED_SAMPLERATE_PREFIX}" REALPATH)
foreach(_entry
	"bin/libsamplerate-0.dll|BF275A0636DC7DADAC8A691842C06B460109EFC213C68B5EF6576A216E55D734"
	"lib/libsamplerate.dll.a|CB98B78576FA6D70008EE8271A731946FABC8AEBFC3A17A6D075B62B2DDC671B"
	"include/samplerate.h|DFE5F4107782BCF6E9B8421A9E7C4772E19446A4C18727C73B22286D3FBB6626"
	"lib/cmake/SampleRate/SampleRateConfig.cmake|EB49B74E97FED7C053C0B7D84165959718782D3A49C8AB75AB2DAE919FCF9653"
	"lib/cmake/SampleRate/SampleRateTargets.cmake|CAF5148E936977BBD79452B5C740FFEDA45AEEDD688FFBC32DF7C4F289CB237B"
	"lib/cmake/SampleRate/SampleRateTargets-release.cmake|57814BA6689DD6B292109C48543490F6EC453670E11F3FB3F90D72E464CD6826"
	"share/licenses/libsamplerate/COPYING|2C1F76CE2EFFDDDB425018405D5690C0B1AB4E6976E35296B0A6DB65C5E1A55D")
	string(REPLACE "|" ";" _parts "${_entry}")
	list(GET _parts 0 _relative)
	list(GET _parts 1 _expected)
	set(_file "${_qualified_prefix}/${_relative}")
	if(NOT EXISTS "${_file}")
		message(FATAL_ERROR "Qualified libsamplerate file is missing: ${_relative}")
	endif()
	file(SHA256 "${_file}" _actual)
	string(TOUPPER "${_actual}" _actual)
	if(NOT _actual STREQUAL _expected)
		message(FATAL_ERROR "Qualified libsamplerate SHA-256 mismatch: ${_relative}")
	endif()
endforeach()
