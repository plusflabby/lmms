include(CTest)
find_package(Qt${QT_VERSION_MAJOR} COMPONENTS Network REQUIRED)

if(LMMS_BUILD_WIN32 AND LMMS_REQUIRE_QUALIFIED_VOCAL_RUNTIME)
	find_package(Python3 COMPONENTS Interpreter QUIET)
	if(Python3_Interpreter_FOUND)
		add_test(NAME QualifiedSamplerateSelectionTest
			COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/qualified_samplerate_selection_test.py")
		set_tests_properties(QualifiedSamplerateSelectionTest PROPERTIES
			ENVIRONMENT "LMMS_QUALIFIED_SAMPLERATE_TEST_PREFIX=${LMMS_QUALIFIED_SAMPLERATE_PREFIX}")
		add_test(NAME QualifiedSamplerateRuntimeTest
			COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/qualified_samplerate_runtime_test.py")
		set_tests_properties(QualifiedSamplerateRuntimeTest PROPERTIES
			ENVIRONMENT "LMMS_QUALIFIED_SAMPLERATE_TEST_STAGE=$<TARGET_FILE_DIR:lmms>")
		add_test(NAME QualifiedSamplerateBuilderTest
			COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/qualified_samplerate_builder_test.py")
		add_test(NAME UnqualifiedVocalRuntimeTest
			COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/unqualified_vocal_runtime_test.py")
		set_tests_properties(UnqualifiedVocalRuntimeTest PROPERTIES
			ENVIRONMENT "LMMS_UNQUALIFIED_TEST_BIN_DIR=$<TARGET_FILE_DIR:AiBridgeVocalAssetActionsTest>")
	else()
		message(WARNING "Python 3 was not found; qualified libsamplerate regression tests are unavailable")
	endif()
endif()

set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_AUTOMOC ON)

if(MSVC AND NOT LMMS_REQUIRE_QUALIFIED_VOCAL_RUNTIME)
	find_package(Python3 3.9 COMPONENTS Interpreter QUIET)
	if(NOT Python3_Interpreter_FOUND)
		message(WARNING "Python 3 is needed to run the unqualified vocal regression tests")
		set(Python3_EXECUTABLE python)
	endif()
	add_test(NAME UnqualifiedVocalCiPolicyTest
		COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/unqualified_vocal_ci_policy_test.py")
endif()

set(LMMS_TESTS
	src/core/MidiExportPlannerTest.cpp
	src/core/MidiExportFixtureTest.cpp
	src/core/MidiExportOutputTest.cpp
	src/core/StandardMidiWriterTest.cpp
	src/core/RecordingOutputQueueTest.cpp
	src/core/RecordingSessionStateTest.cpp
	src/core/RecordingSessionManagerTest.cpp
	src/core/RecordingTransportControlTest.cpp
	src/core/RecordingClipOffsetPlaybackTest.cpp
	src/core/RecordingPlacementTest.cpp
	src/core/RecordingLibraryTest.cpp
	src/core/RecordedTakeSourceTest.cpp
	src/core/RecordingAttachmentTest.cpp
	src/core/RecordingRecoveryTest.cpp
	src/core/RecordingProjectIdentityTest.cpp
	src/core/ProjectSavePublicationTest.cpp
	src/core/RecordingRingTest.cpp
	src/core/RecordingPcm16Test.cpp
	src/core/RecordingInputRuntimeTest.cpp
	src/core/RecordingTakeJournalTest.cpp
	src/core/RecordingTakeReceiptTest.cpp
	src/core/RecordingWriterTest.cpp
	src/core/RecordingInputControllerTest.cpp
	src/core/RecordingServiceHostTest.cpp
	src/core/RecordingQualificationViewTest.cpp
	src/core/RecordingRecorderViewTest.cpp
	src/core/RecordingTakePreviewTest.cpp
	src/core/RecordingAudioBackendPolicyTest.cpp
	src/plugins/AiBridgeVocalJobSafetyTest.cpp
	src/plugins/AiBridgeVocalJobViewTest.cpp
	src/plugins/AiBridgeVocalOperationSafetyAuditTest.cpp
	src/plugins/AiBridgeRestrictedEqAdapterTest.cpp
	src/plugins/AiBridgeEqAdapterTest.cpp
	src/plugins/AiBridgeSampleTrackAutomationTest.cpp
	src/plugins/AiBridgeVocalTrackActionsTest.cpp
	src/plugins/AiBridgeVocalJobsTest.cpp
	src/plugins/AiBridgeMidiExportJobsTest.cpp
	src/plugins/AiBridgeVocalAnalysisTest.cpp
	src/plugins/AiBridgeVocalSourceTest.cpp
	src/core/PcmWaveStreamingTest.cpp
	src/core/ManagedVocalWaveReaderTest.cpp
	src/core/ManagedVocalAssetTypesTest.cpp
	src/core/ManagedVocalAssetStoreTest.cpp
	src/core/ManagedVocalPlaybackTest.cpp
	src/core/ManagedVocalPlaybackCacheTest.cpp
	src/core/SampleClipManagedVocalVariantTest.cpp
	src/plugins/AiBridgeVocalTypesTest.cpp
	src/plugins/AiBridgeVocalEditTypesTest.cpp
	src/plugins/AiBridgeVocalEditPlannerTest.cpp
	src/plugins/AiBridgeVocalEditWireTest.cpp
	src/plugins/AiBridgeVocalGainFadeTest.cpp
	src/plugins/AiBridgeVocalSignalsmithBackendTest.cpp
	src/plugins/AiBridgeVocalAssetActionsTest.cpp
	src/core/AudioSourceWindowPersistenceTest.cpp
	src/core/AudioSourceWindowV2PersistenceTest.cpp
	src/core/AudioWindowPlaybackTest.cpp
	src/core/AudioWindowV2PlaybackTest.cpp
	src/core/AudioWindowQuarantineTest.cpp
	src/core/AudioWindowV2SchedulerTest.cpp
	src/core/AudioWindowEndpointContextTest.cpp
	src/core/AudioWindowTerminalEligibilityTest.cpp
	src/plugins/AiBridgeAudioRangeNativeTest.cpp
	src/core/AudioFileAuthorizationTest.cpp
	src/core/AudioImportPolicyTest.cpp
	src/core/SampleClipAuthorizedAudioTest.cpp
	src/core/SampleClipAudioCopyTest.cpp
	src/plugins/AiBridgeAudioTypesTest.cpp
	src/plugins/AiBridgeAudioContextTest.cpp
	src/plugins/AiBridgeAudioWindowCompatibilityTest.cpp
	src/plugins/AiBridgeAudioLifecycleTest.cpp
	src/plugins/AiBridgeAudioActionsTest.cpp
	src/plugins/AiBridgeAudioPlannerTest.cpp
	src/plugins/AiBridgeAudioRangePlannerTest.cpp
	src/plugins/AiBridgeAudioRangeActionsTest.cpp
	src/plugins/AiBridgeAudioRangeProtocolTest.cpp
	src/plugins/AiBridgeAudioManagementTest.cpp
	src/core/SampleClipBoundedSourceTest.cpp
	src/core/SampleDecoderAdmissionTest.cpp
	src/core/SampleClipSourcePersistenceTest.cpp
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
	src/plugins/AiBridgeRecordingProtocolTest.cpp
	src/plugins/AiBridgeRecordingActionsTest.cpp
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
	src/plugins/AiBridgeTimelineTypesTest.cpp
	src/plugins/AiBridgeAutomationTypesTest.cpp
	src/plugins/AiBridgeArrangementContextTest.cpp
	src/plugins/AiBridgeAutomationRegistryTest.cpp
	src/plugins/AiBridgeAutomationTargetsTest.cpp
	src/plugins/AiBridgeAutomationReconcilerTest.cpp
	src/plugins/AiBridgeAutomationContextTest.cpp
	src/plugins/AiBridgeTimelineActionsTest.cpp
	src/plugins/AiBridgeAutomationActionsTest.cpp
	src/plugins/AiBridgeAutomationPointsTest.cpp
	src/plugins/AiBridgeArrangementPlannerTest.cpp
	src/plugins/AiBridgeArrangementExecutionTest.cpp
	src/plugins/AiBridgePhase3GProtocolTest.cpp
	src/plugins/AiBridgeMidiTypesTest.cpp
	src/plugins/AiBridgeMidiContextTest.cpp
	src/plugins/AiBridgeMidiAnalysisTest.cpp
	src/plugins/AiBridgeMidiQuantizePlannerTest.cpp
	src/plugins/AiBridgeMidiHumanizeGroovePlannerTest.cpp
	src/plugins/AiBridgeMidiDynamicsPlannerTest.cpp
	src/plugins/AiBridgeMidiCleanupScalePlannerTest.cpp
	src/plugins/AiBridgeMidiActionsTest.cpp
	src/plugins/AiBridgeMidiProtocolTest.cpp
	src/plugins/AiBridgeMidiLifecycleTest.cpp
	src/plugins/AiBridgePhase3HSecurityTest.cpp
	src/plugins/AiBridgePhase3HFrameSizeTest.cpp
	src/tracks/AutomationTrackTest.cpp
)

if(WIN32 AND LMMS_HAVE_PORTAUDIO)
	list(APPEND LMMS_TESTS src/core/RecordingPortAudioDiagnosticTest.cpp)
endif()

if(LMMS_BUILD_WIN32 AND LMMS_REQUIRE_QUALIFIED_VOCAL_RUNTIME)
	get_property(_samplerate_stager GLOBAL PROPERTY LMMS_SAMPLERATE_STAGER)
	add_custom_target(lmms_prepare_qualified_test_runtime
		COMMAND powershell.exe -NoProfile -NonInteractive -File "${_samplerate_stager}"
			-Source "${LMMS_QUALIFIED_SAMPLERATE_PREFIX}/bin/libsamplerate-0.dll"
			-Destination "${CMAKE_CURRENT_BINARY_DIR}/libsamplerate-0.dll"
		VERBATIM)
	add_dependencies(lmms_prepare_qualified_test_runtime lmms_verify_qualified_samplerate)
endif()

foreach(LMMS_TEST_SRC IN LISTS LMMS_TESTS)
	# TODO CMake 3.20: Use cmake_path
	get_filename_component(LMMS_TEST_NAME ${LMMS_TEST_SRC} NAME_WE)

	add_executable(${LMMS_TEST_NAME} ${LMMS_TEST_SRC})
	if(LMMS_BUILD_WIN32 AND LMMS_REQUIRE_QUALIFIED_VOCAL_RUNTIME)
		lmms_require_qualified_samplerate(${LMMS_TEST_NAME})
		add_dependencies(${LMMS_TEST_NAME} lmms_prepare_qualified_test_runtime)
	endif()
	if(MSVC AND NOT LMMS_REQUIRE_QUALIFIED_VOCAL_RUNTIME AND
		(LMMS_TEST_NAME STREQUAL "AiBridgeVocalAssetActionsTest" OR LMMS_TEST_NAME STREQUAL "SampleClipManagedVocalVariantTest"
			OR LMMS_TEST_NAME STREQUAL "AiBridgeVocalJobSafetyTest"))
		add_test(NAME ${LMMS_TEST_NAME} COMMAND "${Python3_EXECUTABLE}"
			"${CMAKE_CURRENT_SOURCE_DIR}/scripts/unqualified_vocal_ci.py" --executable "$<TARGET_FILE:${LMMS_TEST_NAME}>")
	else()
		add_test(NAME ${LMMS_TEST_NAME} COMMAND ${LMMS_TEST_NAME})
	endif()
	if(LMMS_BUILD_WIN32 AND LMMS_REQUIRE_QUALIFIED_VOCAL_RUNTIME)
		lmms_guard_qualified_test_runtime(${LMMS_TEST_NAME})
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeIpcServerTest")
		# This integration test loads the real instrument modules from build/plugins.
		set_target_properties(${LMMS_TEST_NAME} PROPERTIES ENABLE_EXPORTS ON)
		if(WIN32)
			# Native plugin import tables refer to lmms.exe; use the same module
			# name for this exported test host so Windows binds plugins to it.
			set_target_properties(${LMMS_TEST_NAME} PROPERTIES OUTPUT_NAME "lmms")
		endif()
		if(UNIX AND NOT APPLE)
			target_link_options(${LMMS_TEST_NAME} PRIVATE -Wl,--export-dynamic)
		endif()
	endif()

	# TODO CMake 3.12: Propagate usage requirements by linking to lmmsobjs
	target_include_directories(${LMMS_TEST_NAME} PRIVATE $<TARGET_PROPERTY:lmmsobjs,INCLUDE_DIRECTORIES>)
	if(LMMS_TEST_NAME STREQUAL "AiBridgeVocalEditTypesTest" OR LMMS_TEST_NAME STREQUAL "AiBridgeVocalEditWireTest" OR LMMS_TEST_NAME STREQUAL "AiBridgeVocalGainFadeTest")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeVocalEditTypesTest" OR LMMS_TEST_NAME STREQUAL "AiBridgeVocalGainFadeTest")
		target_sources(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalEditTypes.cpp)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeVocalSignalsmithBackendTest")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge
			${CMAKE_SOURCE_DIR}/src/3rdparty/signalsmith/stretch
			${CMAKE_SOURCE_DIR}/src/3rdparty/signalsmith/linear/include)
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalDspBackend.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalComp.cpp)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeVocalGainFadeTest")
		target_sources(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalGainFade.cpp)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeVocalAssetActionsTest")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge ${CMAKE_SOURCE_DIR}/src/core/vocal
			${CMAKE_SOURCE_DIR}/src/3rdparty/signalsmith/stretch
			${CMAKE_SOURCE_DIR}/src/3rdparty/signalsmith/linear/include)
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalAssetActions.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalVariantAttachment.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalTrackActions.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMixerRegistry.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalDspBackend.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalJobs.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalJobs.h
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalAnalysis.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalGainFade.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalComp.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalEditTypes.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalEditPlanner.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalTypes.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalContext.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalSource.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAudioTypes.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAudioContext.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeProjectContext.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMutationTypes.cpp)
	endif()
	if(LMMS_TEST_NAME STREQUAL "ManagedVocalWaveReaderTest" OR LMMS_TEST_NAME STREQUAL "ManagedVocalAssetTypesTest" OR LMMS_TEST_NAME STREQUAL "ManagedVocalAssetStoreTest" OR LMMS_TEST_NAME STREQUAL "ManagedVocalPlaybackTest" OR LMMS_TEST_NAME STREQUAL "ManagedVocalPlaybackCacheTest" OR LMMS_TEST_NAME STREQUAL "SampleClipManagedVocalVariantTest")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/src/core/vocal)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeVocalEditPlannerTest")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge)
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalEditTypes.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalEditPlanner.cpp)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeEqAdapterTest" OR LMMS_TEST_NAME STREQUAL "AiBridgeRestrictedEqAdapterTest")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/Eq ${CMAKE_BINARY_DIR}/plugins/Eq)
		target_compile_definitions(${LMMS_TEST_NAME} PRIVATE PLUGIN_NAME=eq PLUGIN_STATIC_DEFINE)
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/Eq/EqEffect.cpp
			${CMAKE_SOURCE_DIR}/plugins/Eq/EqControls.cpp ${CMAKE_SOURCE_DIR}/plugins/Eq/EqControls.h
			${CMAKE_SOURCE_DIR}/plugins/Eq/EqControlsDialog.cpp ${CMAKE_SOURCE_DIR}/plugins/Eq/EqControlsDialog.h
			${CMAKE_SOURCE_DIR}/plugins/Eq/EqCurve.cpp ${CMAKE_SOURCE_DIR}/plugins/Eq/EqCurve.h
			${CMAKE_SOURCE_DIR}/plugins/Eq/EqParameterWidget.cpp ${CMAKE_SOURCE_DIR}/plugins/Eq/EqParameterWidget.h
			${CMAKE_SOURCE_DIR}/plugins/Eq/EqFader.h
			${CMAKE_SOURCE_DIR}/plugins/Eq/EqSpectrumView.cpp ${CMAKE_SOURCE_DIR}/plugins/Eq/EqSpectrumView.h)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeVocalTrackActionsTest" OR LMMS_TEST_NAME STREQUAL "AiBridgeVocalOperationSafetyAuditTest")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge)
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalTypes.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalContext.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalTrackActions.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAudioTypes.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeProjectContext.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMutationTypes.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMixerRegistry.cpp)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeVocalAnalysisTest")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge)
		target_sources(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalAnalysis.cpp)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeMidiExportJobsTest")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge)
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeProjectContext.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMutationTypes.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMidiExportCapture.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMidiExportJobs.cpp)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeVocalJobsTest" OR LMMS_TEST_NAME STREQUAL "AiBridgeVocalJobSafetyTest" OR LMMS_TEST_NAME STREQUAL "AiBridgeVocalJobViewTest" OR LMMS_TEST_NAME STREQUAL "AiBridgeVocalSourceTest")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge)
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalTypes.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalContext.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalSource.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAudioTypes.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAudioContext.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeProjectContext.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMutationTypes.cpp)
		if(LMMS_TEST_NAME STREQUAL "AiBridgeVocalJobsTest" OR LMMS_TEST_NAME STREQUAL "AiBridgeVocalJobSafetyTest" OR LMMS_TEST_NAME STREQUAL "AiBridgeVocalJobViewTest")
			target_include_directories(${LMMS_TEST_NAME} PRIVATE
				${CMAKE_SOURCE_DIR}/src/3rdparty/signalsmith/stretch
				${CMAKE_SOURCE_DIR}/src/3rdparty/signalsmith/linear/include)
			target_sources(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalJobs.cpp
				${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalJobs.h ${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalAnalysis.cpp
				${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalAssetActions.cpp
				${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalVariantAttachment.cpp
				${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalDspBackend.cpp
				${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalGainFade.cpp
				${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalComp.cpp
				${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalEditTypes.cpp
				${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalEditPlanner.cpp)
		endif()
		if(LMMS_TEST_NAME STREQUAL "AiBridgeVocalJobSafetyTest" OR LMMS_TEST_NAME STREQUAL "AiBridgeVocalJobViewTest")
			target_sources(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalWire.cpp
				${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeProtocol.cpp
				${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeRecordingTypes.cpp
				${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalTrackActions.cpp
				${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMixerRegistry.cpp)
		endif()
		if(LMMS_TEST_NAME STREQUAL "AiBridgeVocalJobsTest")
			target_sources(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalTrackActions.cpp
				${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMixerRegistry.cpp)
		endif()
		if(LMMS_TEST_NAME STREQUAL "AiBridgeVocalJobViewTest")
			target_sources(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalJobView.cpp)
		endif()
		if(LMMS_TEST_NAME STREQUAL "AiBridgeVocalSourceTest")
			target_sources(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalTrackActions.cpp
				${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMixerRegistry.cpp)
		endif()
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeVocalTypesTest")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge)
		target_sources(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalTypes.cpp)
	endif()
	if(LMMS_TEST_NAME MATCHES "^AiBridge(Actions|Context|WindowContext|Protocol|IpcServer|MixerProtocol|EffectProtocol|FrameSize|Lifecycle|Security|Phase3HSecurity|Phase3HFrameSize)Test$")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeAudioTypesTest")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge)
		target_sources(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAudioTypes.cpp)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeAudioRangeProtocolTest")
		target_compile_definitions(${LMMS_TEST_NAME} PRIVATE LMMS_AUDIO_RANGE_TESTING)
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeAudioRangeActionsTest")
		target_compile_definitions(${LMMS_TEST_NAME} PRIVATE LMMS_AUDIO_RANGE_TESTING)
		target_sources(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAudioRangePlanner.cpp ${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAudioRangeActions.cpp ${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAudioPlanner.cpp ${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAudioEditActions.cpp)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeAudioRangePlannerTest")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge)
		target_sources(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAudioTypes.cpp ${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAudioRangePlanner.cpp)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeAudioPlannerTest")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge)
		target_sources(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAudioPlanner.cpp ${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAudioTypes.cpp)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeAudioRangeActionsTest" OR LMMS_TEST_NAME STREQUAL "AiBridgeAudioWindowCompatibilityTest" OR LMMS_TEST_NAME STREQUAL "AiBridgeAudioContextTest" OR LMMS_TEST_NAME STREQUAL "AiBridgeAudioLifecycleTest" OR LMMS_TEST_NAME STREQUAL "AiBridgeAudioActionsTest" OR LMMS_TEST_NAME STREQUAL "AiBridgeAudioManagementTest")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge)
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAudioTypes.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAudioContext.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeProjectContext.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMutationTypes.cpp)
		if(LMMS_TEST_NAME STREQUAL "AiBridgeAudioWindowCompatibilityTest" OR LMMS_TEST_NAME STREQUAL "AiBridgeAudioActionsTest" OR LMMS_TEST_NAME STREQUAL "AiBridgeAudioManagementTest")
			target_sources(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAudioActions.cpp)
		endif()
		if(LMMS_TEST_NAME STREQUAL "AiBridgeAudioWindowCompatibilityTest" OR LMMS_TEST_NAME STREQUAL "AiBridgeAudioManagementTest")
			target_sources(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAudioPlanner.cpp ${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAudioEditActions.cpp)
		endif()
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeMutationTypesTest")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge)
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMutationTypes.cpp)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeTimelineTypesTest")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge)
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeTimelineTypes.cpp)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeMidiTypesTest")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge)
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMidiTypes.cpp)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeMidiContextTest")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge)
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMidiTypes.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMidiContext.cpp)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeMidiAnalysisTest")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge)
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMidiTypes.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMidiAnalysis.cpp)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeMidiQuantizePlannerTest")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge)
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMidiTypes.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMidiPlanner.cpp)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeMidiHumanizeGroovePlannerTest")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge)
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMidiTypes.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMidiPlanner.cpp)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeMidiDynamicsPlannerTest")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge)
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMidiTypes.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMidiPlanner.cpp)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeMidiCleanupScalePlannerTest")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge)
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMidiTypes.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMidiPlanner.cpp)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeMidiActionsTest")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge)
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMidiTypes.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMidiContext.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeProjectContext.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMutationTypes.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMidiActions.cpp)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeMidiProtocolTest")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge)
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeProtocol.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeRecordingTypes.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMutationTypes.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMidiTypes.cpp)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeMidiLifecycleTest")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge)
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMidiTypes.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMidiContext.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeProjectContext.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMutationTypes.cpp)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeAutomationTypesTest")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge)
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAutomationTypes.cpp)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeArrangementContextTest")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge)
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeArrangementContext.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeProjectContext.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMutationTypes.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeTimelineTypes.cpp)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeAutomationRegistryTest")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge)
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAutomationRegistry.cpp)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeAutomationTargetsTest")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge)
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAutomationTargets.cpp)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeAutomationReconcilerTest" OR LMMS_TEST_NAME STREQUAL "AiBridgeSampleTrackAutomationTest")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge)
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAutomationReconciler.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAutomationTargets.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAutomationRegistry.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMixerRegistry.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeEffectRegistry.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeEffectAdapters.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeProjectContext.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMutationTypes.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAutomationTypes.cpp)
		target_include_directories(${LMMS_TEST_NAME} PRIVATE
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
		target_compile_definitions(${LMMS_TEST_NAME} PRIVATE PLUGIN_NAME=Amplifier PLUGIN_STATIC_DEFINE)
		if(LMMS_TEST_NAME STREQUAL "AiBridgeSampleTrackAutomationTest")
			target_sources(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAutomationActions.cpp
				${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAutomationContext.cpp)
		endif()
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeAutomationContextTest")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge)
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAutomationContext.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAutomationRegistry.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAutomationTargets.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAutomationTypes.cpp)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeTimelineActionsTest")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge)
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeTimelineActions.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeProjectContext.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMutationTypes.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeTimelineTypes.cpp)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeAutomationActionsTest")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge)
		 target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAutomationActions.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAutomationRegistry.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAutomationTargets.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAutomationTypes.cpp)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeAutomationPointsTest")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge)
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAutomationActions.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAutomationRegistry.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAutomationTargets.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAutomationTypes.cpp)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeArrangementPlannerTest")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge)
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeArrangementPlanner.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeTimelineTypes.cpp)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeArrangementExecutionTest")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge)
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeArrangementActions.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeArrangementPlanner.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeTimelineTypes.cpp)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgePhase3GProtocolTest")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge)
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeProtocol.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeRecordingTypes.cpp
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
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeRecordingTypes.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMutationTypes.cpp)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeRecordingProtocolTest")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge)
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeProtocol.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMutationTypes.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeRecordingTypes.cpp)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeRecordingActionsTest")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge)
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeRecordingActions.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeRecordingTypes.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeProtocol.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMutationTypes.cpp)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeProtocolTest"
		OR LMMS_TEST_NAME STREQUAL "AiBridgeMidiProtocolTest"
		OR LMMS_TEST_NAME STREQUAL "AiBridgePhase3GProtocolTest"
		OR LMMS_TEST_NAME STREQUAL "AiBridgeIpcServerTest"
		OR LMMS_TEST_NAME STREQUAL "AiBridgeRecordingProtocolTest"
		OR LMMS_TEST_NAME STREQUAL "AiBridgeRecordingActionsTest"
		OR LMMS_TEST_NAME STREQUAL "AiBridgeAudioRangeProtocolTest"
		OR LMMS_TEST_NAME STREQUAL "AiBridgeViewTest"
		OR LMMS_TEST_NAME STREQUAL "AiBridgeVocalJobSafetyTest"
		OR LMMS_TEST_NAME STREQUAL "AiBridgeVocalJobViewTest"
		OR LMMS_TEST_NAME MATCHES "^AiBridge(MixerProtocol|EffectProtocol|FrameSize|Phase3HSecurity|Phase3HFrameSize)Test$")
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeProjectMutationGuard.cpp)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeIpcServerTest" OR LMMS_TEST_NAME STREQUAL "AiBridgeAudioRangeProtocolTest" OR LMMS_TEST_NAME STREQUAL "AiBridgeViewTest")
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMidiExportCapture.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMidiExportJobs.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMidiExportWire.cpp)
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalWire.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalTypes.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalContext.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalSource.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalAnalysis.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalJobs.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalJobs.h
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalTrackActions.cpp)
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAudioWire.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAudioRangePlanner.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAudioRangeActions.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAudioActions.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAudioEditActions.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAudioPlanner.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAudioContext.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAudioTypes.cpp)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeIpcServerTest" OR LMMS_TEST_NAME STREQUAL "AiBridgeAudioRangeProtocolTest" OR LMMS_TEST_NAME STREQUAL "AiBridgeViewTest")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/src/3rdparty/signalsmith/stretch
			${CMAKE_SOURCE_DIR}/src/3rdparty/signalsmith/linear/include)
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalAssetActions.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalVariantAttachment.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalDspBackend.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalGainFade.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalComp.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalEditTypes.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalEditPlanner.cpp)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeIpcServerTest" OR LMMS_TEST_NAME STREQUAL "AiBridgeAudioRangeProtocolTest")
			target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeActions.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeContext.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeIpcServer.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeProtocol.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeRecordingActions.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeRecordingTypes.cpp
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
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeProjectContext.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeTimelineTypes.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAutomationTypes.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeArrangementContext.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAutomationRegistry.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAutomationTargets.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAutomationReconciler.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAutomationContext.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeTimelineActions.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAutomationActions.cpp)
			target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMidiTypes.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMidiContext.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMidiAnalysis.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMidiPlanner.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMidiActions.cpp)
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeArrangementPlanner.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeArrangementActions.cpp)
		target_include_directories(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/Amplifier
			${CMAKE_SOURCE_DIR}/plugins/Compressor
			${CMAKE_SOURCE_DIR}/plugins/CrossoverEQ
			${CMAKE_SOURCE_DIR}/plugins/Delay
			${CMAKE_SOURCE_DIR}/plugins/ReverbSC)
	endif()
	if(LMMS_TEST_NAME MATCHES "^AiBridge(MixerProtocol|EffectProtocol|FrameSize|Phase3HSecurity|Phase3HFrameSize)Test$")
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeProtocol.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeRecordingTypes.cpp
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
		target_sources(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalJobView.cpp)
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalTypes.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalContext.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalSource.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalAnalysis.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalJobs.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeVocalJobs.h)
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridge.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeActions.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeContext.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeIpcServer.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeProtocol.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeRecordingActions.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeRecordingTypes.cpp
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
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeTimelineTypes.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAutomationTypes.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeArrangementContext.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAutomationRegistry.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAutomationTargets.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAutomationReconciler.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAutomationContext.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeTimelineActions.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeAutomationActions.cpp)
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeArrangementPlanner.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeArrangementActions.cpp)
		target_sources(${LMMS_TEST_NAME} PRIVATE
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMidiTypes.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMidiContext.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMidiAnalysis.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMidiPlanner.cpp
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeMidiActions.cpp)
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
	if(LMMS_TEST_NAME STREQUAL "AiBridgeIpcServerTest" OR LMMS_TEST_NAME STREQUAL "AiBridgeAudioRangeProtocolTest")
		target_link_libraries(${LMMS_TEST_NAME} PRIVATE Qt${QT_VERSION_MAJOR}::Network)
	endif()

	target_compile_features(${LMMS_TEST_NAME} PRIVATE cxx_std_20)
	target_compile_definitions(${LMMS_TEST_NAME} PRIVATE LMMS_TESTING)
	if(LMMS_TEST_NAME STREQUAL "RecordingAudioBackendPolicyTest" AND LMMS_ENABLE_RECORDING_QUALIFICATION)
		target_compile_definitions(${LMMS_TEST_NAME} PRIVATE LMMS_EXPECT_RECORDING_QUALIFICATION)
	endif()
endforeach()
