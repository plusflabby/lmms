SET(PLUGIN_FILES "")
IF(LMMS_BUILD_WIN32)
        INSTALL(FILES $<TARGET_FILE:Qt${QT_VERSION_MAJOR}::QWindowsIntegrationPlugin> DESTINATION platforms)
        INSTALL(FILES $<TARGET_FILE:Qt${QT_VERSION_MAJOR}::QSvgIconPlugin> DESTINATION iconengines)
        INSTALL(FILES $<TARGET_FILE:Qt${QT_VERSION_MAJOR}::QSvgPlugin> DESTINATION imageformats)
        INSTALL(FILES $<TARGET_FILE:Qt${QT_VERSION_MAJOR}::Svg> DESTINATION .)
ENDIF()

IF(LMMS_BUILD_WIN32 OR LMMS_INSTALL_DEPENDENCIES)
	include(InstallTargetDependencies)

	# Collect directories to search for DLLs
	GET_FILENAME_COMPONENT(QTBIN_DIR "${QT_QMAKE_EXECUTABLE}" PATH)
	set(LIB_DIRS "${QTBIN_DIR}")

	GET_PROPERTY(PLUGINS_BUILT GLOBAL PROPERTY PLUGINS_BUILT)

	IF(LMMS_BUILD_WIN32)
		SET(LMMS_DEP_DESTINATION ${BIN_DIR})
		SET(PLUGIN_DEP_DESTINATION ${BIN_DIR})
	ELSE()
		SET(LMMS_DEP_DESTINATION ${LIB_DIR})
		SET(PLUGIN_DEP_DESTINATION ${LIB_DIR})
	ENDIF()

	INSTALL_TARGET_DEPENDENCIES(
			NAME "main_binary"
			TARGETS lmms
			DESTINATION "${LMMS_DEP_DESTINATION}"
			LIB_DIRS  ${LIB_DIRS}
	)

	INSTALL_TARGET_DEPENDENCIES(
			NAME "plugins"
			TARGETS ${PLUGINS_BUILT}
			DESTINATION ${PLUGIN_DEP_DESTINATION}
			LIB_DIRS ${LIB_DIRS} "${PLUGIN_DIR}" "${PLUGIN_DIR}/optional"
			SEARCH_PATHS "${PLUGIN_DIR}" "${PLUGIN_DIR}/optional"
	)
ENDIF()

# The Windows dependency scanner can find the installed system DLL even when
# LMMS was linked against an explicitly selected project-local SampleRate
# package. Install the imported target's own runtime image after dependency
# collection so the staged package uses the same qualified DLL as the link.
if(LMMS_BUILD_WIN32 AND TARGET SampleRate::samplerate)
	get_target_property(SAMPLERATE_RUNTIME_IMAGE SampleRate::samplerate IMPORTED_LOCATION_RELEASE)
	if(NOT SAMPLERATE_RUNTIME_IMAGE)
		get_target_property(SAMPLERATE_RUNTIME_IMAGE SampleRate::samplerate IMPORTED_LOCATION)
	endif()
	if(NOT SAMPLERATE_RUNTIME_IMAGE OR NOT EXISTS "${SAMPLERATE_RUNTIME_IMAGE}")
		message(FATAL_ERROR "The selected SampleRate target has no installable runtime/import image")
	endif()

	get_filename_component(SAMPLERATE_LIBRARY_DIRECTORY "${SAMPLERATE_RUNTIME_IMAGE}" DIRECTORY)
	get_filename_component(SAMPLERATE_PREFIX_DIRECTORY "${SAMPLERATE_LIBRARY_DIRECTORY}" DIRECTORY)
	set(SAMPLERATE_LICENSE "${SAMPLERATE_PREFIX_DIRECTORY}/share/licenses/libsamplerate/COPYING")
	if(NOT EXISTS "${SAMPLERATE_LICENSE}")
		message(FATAL_ERROR "The selected SampleRate package has no BSD license at ${SAMPLERATE_LICENSE}")
	endif()

	if(SAMPLERATE_RUNTIME_IMAGE MATCHES "\\.[Dd][Ll][Ll]$")
		install(FILES "${SAMPLERATE_RUNTIME_IMAGE}" DESTINATION "${BIN_DIR}")
	endif()
	install(FILES "${SAMPLERATE_LICENSE}" DESTINATION "${DATA_DIR}/licenses/libsamplerate")
endif()

# Install STK rawwaves
if(LMMS_HAVE_STK AND (LMMS_BUILD_WIN32 OR LMMS_BUILD_APPLE))
	if(STK_RAWWAVE_ROOT)
		file(GLOB RAWWAVES "${STK_RAWWAVE_ROOT}/*.raw")
		list(SORT RAWWAVES)
		install(FILES ${RAWWAVES} DESTINATION "${DATA_DIR}/stk/rawwaves")
	else()
		message(WARNING "Can't find STK rawwave root!")
	endif()
endif()

# Ship licenses for the header-only Signalsmith backend with binary installers.
install(FILES "${CMAKE_SOURCE_DIR}/src/3rdparty/signalsmith/stretch/LICENSE.txt"
	DESTINATION "${DATA_DIR}/licenses/signalsmith" RENAME "SignalsmithStretch-LICENSE.txt")
install(FILES "${CMAKE_SOURCE_DIR}/src/3rdparty/signalsmith/linear/LICENSE.txt"
	DESTINATION "${DATA_DIR}/licenses/signalsmith" RENAME "SignalsmithLinear-LICENSE.txt")
install(FILES "${CMAKE_SOURCE_DIR}/src/3rdparty/signalsmith/README.md"
	DESTINATION "${DATA_DIR}/licenses/signalsmith" RENAME "README.txt")

