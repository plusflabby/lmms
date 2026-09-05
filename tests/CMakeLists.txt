include(CTest)
find_package(Qt${QT_VERSION_MAJOR} COMPONENTS Network REQUIRED)

set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_AUTOMOC ON)

set(LMMS_TESTS
	src/core/ArrayVectorTest.cpp
	src/core/AudioBufferTest.cpp
	src/core/AutomatableModelTest.cpp
	src/core/MathTest.cpp
	src/core/ProjectVersionTest.cpp
	src/core/RelativePathsTest.cpp
	src/core/TimelineTest.cpp
	src/plugins/AiBridgeActionsTest.cpp
	src/plugins/AiBridgeContextTest.cpp
	src/plugins/AiBridgeWindowContextTest.cpp
	src/plugins/AiBridgeProtocolTest.cpp
	src/plugins/AiBridgeIpcServerTest.cpp
	src/plugins/AiBridgeViewTest.cpp
	src/plugins/AiBridgeMutationTypesTest.cpp
	src/plugins/AiBridgeProjectJournalTest.cpp
	src/plugins/AiBridgeConstructionTypesTest.cpp
	src/plugins/AiBridgeAssetTypesTest.cpp
	src/plugins/AiBridgeAssetCatalogTest.cpp
	src/plugins/AiBridgeAssetAdaptersTest.cpp
	src/plugins/AiBridgeAssetActionsTest.cpp
	src/plugins/AiBridgeMixerTypesTest.cpp
	src/plugins/AiBridgeMixerRegistryTest.cpp
	src/plugins/AiBridgeMixerContextTest.cpp
	 src/plugins/AiBridgeMixerActionsTest.cpp
	 src/plugins/AiBridgeEffectCatalogTest.cpp
	 src/plugins/AiBridgeEffectRegistryTest.cpp
	 src/plugins/AiBridgeEffectActionsTest.cpp
	 src/plugins/AiBridgeEffectAdaptersTest.cpp
	 src/plugins/AiBridgeEffectParameterActionsTest.cpp
	 src/plugins/AiBridgeMeterPublicationTest.cpp
	 src/plugins/AiBridgeMeterActionsTest.cpp
	 src/plugins/AiBridgeMixerProtocolTest.cpp
	 src/plugins/AiBridgeEffectProtocolTest.cpp
	 src/plugins/AiBridgeLifecycleTest.cpp
	 src/plugins/AiBridgeSecurityTest.cpp
	 src/plugins/AiBridgeFrameSizeTest.cpp
	src/tracks/AutomationTrackTest.cpp
)

foreach(LMMS_TEST_SRC IN LISTS LMMS_TESTS)
	# TODO CMake 3.20: Use cmake_path
	get_filename_component(LMMS_TEST_NAME ${LMMS_TEST_SRC} NAME_WE)

	add_executable(${LMMS_TEST_NAME} ${LMMS_TEST_SRC})
	add_test(NAME ${LMMS_TEST_NAME} COMMAND ${LMMS_TEST_NAME})

	# TODO CMake 3.12: Propagate usage requirements by linking to lmmsobjs
	target_include_directories(${LMMS_TEST_NAME} PRIVATE $<TARGET_PROPERTY:lmmsobjs,INCLUDE_DIRECTORIES>)
	if(LMMS_TEST_NAME MATCHES "^AiBridge(Actions|Context|WindowContext|Protocol|IpcServer|MixerProtocol|EffectProtocol|FrameSize|Lifecycle|Security)Test$")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeMutationTypesTest")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge)
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMutationTypes.cpp)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeMixerTypesTest")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge)
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMixerTypes.cpp)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeMixerRegistryTest")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge)
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMixerRegistry.cpp)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeMixerContextTest")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge)
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMixerContext.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMixerRegistry.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeEffectRegistry.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeProjectContext.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMutationTypes.cpp)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeMixerActionsTest")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge)
		target_sources(${LMMS_TEST_NAME} PRIVATE
		${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMixerActions.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMixerContext.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMixerRegistry.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeEffectRegistry.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeProjectContext.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMutationTypes.cpp)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeEffectCatalogTest")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge)
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeEffectTypes.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeEffectCatalog.cpp)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeEffectRegistryTest")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge)
		target_sources(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeEffectRegistry.cpp)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeEffectActionsTest")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge)
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeEffectActions.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeEffectRegistry.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeEffectCatalog.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeEffectTypes.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMixerContext.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMixerRegistry.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMixerActions.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeProjectContext.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMutationTypes.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeEffectAdapters.cpp)
		target_include_directories(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/Amplifier
			${CMAKE_SOURCE_DIR}/plugins/Compressor
			${CMAKE_SOURCE_DIR}/plugins/CrossoverEQ
			${CMAKE_SOURCE_DIR}/plugins/Delay
			${CMAKE_SOURCE_DIR}/plugins/ReverbSC)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeEffectAdaptersTest")
		target_compile_definitions(${LMMS_TEST_NAME} PRIVATE PLUGIN_NAME=Amplifier PLUGIN_STATIC_DEFINE)
		target_include_directories(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge
			${CMAKE_SOURCE_DIR}/plugins/Amplifier
		${CMAKE_SOURCE_DIR}/plugins/Compressor
			${CMAKE_SOURCE_DIR}/plugins/CrossoverEQ
			${CMAKE_SOURCE_DIR}/plugins/Delay
			${CMAKE_SOURCE_DIR}/plugins/ReverbSC
			${CMAKE_BINARY_DIR}/plugins/Compressor
			${CMAKE_BINARY_DIR}/plugins/CrossoverEQ
			${CMAKE_BINARY_DIR}/plugins/Delay
			${CMAKE_BINARY_DIR}/plugins/ReverbSC)
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeEffectAdapters.cpp
			${CMAKE_SOURCE_DIR}/plugins/Amplifier/AmplifierControls.cpp
			${CMAKE_SOURCE_DIR}/plugins/Amplifier/AmplifierControlDialog.cpp
			${CMAKE_SOURCE_DIR}/plugins/Compressor/CompressorControls.cpp
			${CMAKE_SOURCE_DIR}/plugins/Compressor/CompressorControlDialog.cpp
			${CMAKE_SOURCE_DIR}/plugins/Compressor/Compressor.cpp
			${CMAKE_SOURCE_DIR}/plugins/CrossoverEQ/CrossoverEQControls.cpp
			${CMAKE_SOURCE_DIR}/plugins/CrossoverEQ/CrossoverEQControlDialog.cpp
			${CMAKE_SOURCE_DIR}/plugins/CrossoverEQ/CrossoverEQ.cpp
			${CMAKE_SOURCE_DIR}/plugins/Delay/DelayControls.cpp
			${CMAKE_SOURCE_DIR}/plugins/Delay/DelayControlsDialog.cpp
			${CMAKE_SOURCE_DIR}/plugins/Delay/DelayEffect.cpp
			${CMAKE_SOURCE_DIR}/plugins/Delay/Lfo.cpp
			${CMAKE_SOURCE_DIR}/plugins/Delay/StereoDelay.cpp
			${CMAKE_SOURCE_DIR}/plugins/ReverbSC/ReverbSCControls.cpp
			${CMAKE_SOURCE_DIR}/plugins/ReverbSC/ReverbSCControlDialog.cpp
			${CMAKE_SOURCE_DIR}/plugins/ReverbSC/ReverbSC.cpp
			${CMAKE_SOURCE_DIR}/plugins/ReverbSC/base.c
			${CMAKE_SOURCE_DIR}/plugins/ReverbSC/revsc.c
			${CMAKE_SOURCE_DIR}/plugins/ReverbSC/dcblock.c
			${CMAKE_SOURCE_DIR}/plugins/Eq/EqFader.h)
		set_source_files_properties(
			${CMAKE_SOURCE_DIR}/plugins/Compressor/Compressor.cpp
			PROPERTIES COMPILE_DEFINITIONS "lmms_plugin_main=compressor_plugin_main")
		set_source_files_properties(
			${CMAKE_SOURCE_DIR}/plugins/CrossoverEQ/CrossoverEQ.cpp
			PROPERTIES COMPILE_DEFINITIONS "lmms_plugin_main=crossovereq_plugin_main")
		set_source_files_properties(
			${CMAKE_SOURCE_DIR}/plugins/Delay/DelayEffect.cpp
			PROPERTIES COMPILE_DEFINITIONS "lmms_plugin_main=delay_plugin_main")
		set_source_files_properties(
			${CMAKE_SOURCE_DIR}/plugins/ReverbSC/ReverbSC.cpp
			PROPERTIES COMPILE_DEFINITIONS "lmms_plugin_main=reverbsc_plugin_main")
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeEffectParameterActionsTest")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge
			${CMAKE_SOURCE_DIR}/plugins/Amplifier
			${CMAKE_SOURCE_DIR}/plugins/Compressor
			${CMAKE_SOURCE_DIR}/plugins/CrossoverEQ
			${CMAKE_SOURCE_DIR}/plugins/Delay
			${CMAKE_SOURCE_DIR}/plugins/ReverbSC)
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeEffectAdapters.cpp
			${CMAKE_SOURCE_DIR}/plugins/Amplifier/AmplifierControls.cpp
			${CMAKE_SOURCE_DIR}/plugins/Amplifier/AmplifierControlDialog.cpp)
		target_compile_definitions(${LMMS_TEST_NAME} PRIVATE PLUGIN_NAME=Amplifier)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeMeterPublicationTest")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge)
		target_sources(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMeterSnapshot.cpp)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeMeterActionsTest")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge)
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMeterActions.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMixerContext.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMixerRegistry.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeEffectRegistry.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeProjectContext.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMutationTypes.cpp)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeConstructionTypesTest")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge)
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeConstructionTypes.cpp)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeAssetTypesTest")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge)
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAssetTypes.cpp)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeAssetCatalogTest")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge)
		target_compile_definitions(${LMMS_TEST_NAME} PRIVATE LMMS_PHASE3E_SOURCE_ROOT="${CMAKE_SOURCE_DIR}")
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAssetCatalog.cpp)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeAssetAdaptersTest")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge
			${CMAKE_SOURCE_DIR}/plugins/AudioFileProcessor)
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAssetAdapters.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAssetCatalog.cpp)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeAssetActionsTest")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge)
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeProjectActions.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeProjectContext.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAssetAdapters.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAssetCatalog.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMutationTypes.cpp)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeActionsTest")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge)
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeProjectActions.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeProjectContext.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAssetAdapters.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAssetCatalog.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMutationTypes.cpp)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeProjectJournalTest")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeWindowContextTest")
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeWindowContext.cpp)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeContextTest")
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeActions.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeContext.cpp)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeProtocolTest")
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeProtocol.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMutationTypes.cpp)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeIpcServerTest")
			target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeActions.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeContext.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeIpcServer.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeProtocol.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeProjectContext.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeProjectActions.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAssetAdapters.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAssetCatalog.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMutationTypes.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeWindowContext.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeWindowActions.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMixerActions.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMixerContext.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMixerRegistry.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeEffectActions.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeEffectCatalog.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeEffectRegistry.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeEffectTypes.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeEffectAdapters.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMeterActions.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeProjectContext.cpp)
		target_include_directories(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/Amplifier
			${CMAKE_SOURCE_DIR}/plugins/Compressor
			${CMAKE_SOURCE_DIR}/plugins/CrossoverEQ
			${CMAKE_SOURCE_DIR}/plugins/Delay
			${CMAKE_SOURCE_DIR}/plugins/ReverbSC)
	endif()
	if(LMMS_TEST_NAME MATCHES "^AiBridge(MixerProtocol|EffectProtocol|FrameSize)Test$")
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeProtocol.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMutationTypes.cpp)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeLifecycleTest")
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMixerRegistry.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeEffectRegistry.cpp)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeSecurityTest")
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeEffectTypes.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeEffectCatalog.cpp)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeViewTest")
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridge.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeActions.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeContext.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeIpcServer.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeProtocol.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeProjectContext.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeProjectActions.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAssetAdapters.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAssetCatalog.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMutationTypes.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeWindowContext.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeWindowActions.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMixerActions.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMixerContext.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMixerRegistry.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeEffectActions.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeEffectCatalog.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeEffectRegistry.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeEffectTypes.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeEffectAdapters.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMeterActions.cpp)
		target_include_directories(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge
			${CMAKE_SOURCE_DIR}/plugins/Amplifier
			${CMAKE_SOURCE_DIR}/plugins/Compressor
			${CMAKE_SOURCE_DIR}/plugins/CrossoverEQ
			${CMAKE_SOURCE_DIR}/plugins/Delay
			${CMAKE_SOURCE_DIR}/plugins/ReverbSC)
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_BINARY_DIR}/plugins/AiBridge)
		target_compile_definitions(${LMMS_TEST_NAME} PRIVATE PLUGIN_NAME=aibridge PLUGIN_STATIC_DEFINE)
		target_link_libraries(${LMMS_TEST_NAME} PRIVATE Qt${QT_VERSION_MAJOR}::Network)
	endif()

	target_static_libraries("${LMMS_TEST_NAME}" PRIVATE lmmsobjs)
	target_link_libraries(${LMMS_TEST_NAME} PRIVATE
		${QT_LIBRARIES}
		${QT_QTTEST_LIBRARY}
	)
	if(LMMS_TEST_NAME STREQUAL "AiBridgeIpcServerTest")
		target_link_libraries(${LMMS_TEST_NAME} PRIVATE Qt${QT_VERSION_MAJOR}::Network)
	endif()

	target_compile_features(${LMMS_TEST_NAME} PRIVATE cxx_std_20)
	target_compile_definitions(${LMMS_TEST_NAME} PRIVATE LMMS_TESTING)
endforeach()
