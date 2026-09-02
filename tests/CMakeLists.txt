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
	src/tracks/AutomationTrackTest.cpp
)

foreach(LMMS_TEST_SRC IN LISTS LMMS_TESTS)
	# TODO CMake 3.20: Use cmake_path
	get_filename_component(LMMS_TEST_NAME ${LMMS_TEST_SRC} NAME_WE)

	add_executable(${LMMS_TEST_NAME} ${LMMS_TEST_SRC})
	add_test(NAME ${LMMS_TEST_NAME} COMMAND ${LMMS_TEST_NAME})

	# TODO CMake 3.12: Propagate usage requirements by linking to lmmsobjs
	target_include_directories(${LMMS_TEST_NAME} PRIVATE $<TARGET_PROPERTY:lmmsobjs,INCLUDE_DIRECTORIES>)
	if(LMMS_TEST_NAME MATCHES "^AiBridge(Actions|Context|WindowContext|Protocol|IpcServer)Test$")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge)
	endif()
	if(LMMS_TEST_NAME STREQUAL "AiBridgeMutationTypesTest")
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge)
		target_sources(${LMMS_TEST_NAME} PRIVATE
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
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeWindowActions.cpp)
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
			${CMAKE_SOURCE_DIR}/plugins/AiBridge/AiBridgeWindowActions.cpp)
		target_include_directories(${LMMS_TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/AiBridge)
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
