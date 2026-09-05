INCLUDE(BuildPlugin)

find_package(Qt${QT_VERSION_MAJOR} COMPONENTS Network REQUIRED)

BUILD_PLUGIN(aibridge AiBridge.cpp AiBridge.h AiBridgeActions.cpp AiBridgeActions.h AiBridgeContext.cpp AiBridgeContext.h AiBridgeIpcServer.cpp AiBridgeIpcServer.h AiBridgeProtocol.cpp AiBridgeProtocol.h AiBridgeWindowContext.cpp AiBridgeWindowContext.h AiBridgeWindowActions.cpp AiBridgeWindowActions.h AiBridgeProjectContext.cpp AiBridgeProjectContext.h AiBridgeProjectActions.cpp AiBridgeProjectActions.h AiBridgeMutationTypes.cpp AiBridgeMutationTypes.h AiBridgeConstructionTypes.cpp AiBridgeConstructionTypes.h AiBridgeAssetTypes.cpp AiBridgeAssetTypes.h AiBridgeAssetCatalog.cpp AiBridgeAssetCatalog.h AiBridgeAssetAdapters.cpp AiBridgeAssetAdapters.h AiBridgeMixerTypes.cpp AiBridgeMixerTypes.h AiBridgeMixerRegistry.cpp AiBridgeMixerRegistry.h AiBridgeMixerContext.cpp AiBridgeMixerContext.h AiBridgeMixerActions.cpp AiBridgeMixerActions.h AiBridgeEffectTypes.cpp AiBridgeEffectTypes.h AiBridgeEffectCatalog.cpp AiBridgeEffectCatalog.h AiBridgeEffectRegistry.cpp AiBridgeEffectRegistry.h AiBridgeEffectActions.cpp AiBridgeEffectActions.h AiBridgeEffectAdapters.cpp AiBridgeEffectAdapters.h AiBridgeMeterSnapshot.cpp AiBridgeMeterSnapshot.h AiBridgeMeterActions.cpp AiBridgeMeterActions.h MOCFILES AiBridge.h)
target_link_libraries(aibridge Qt${QT_VERSION_MAJOR}::Network)
target_include_directories(aibridge PRIVATE
	${CMAKE_SOURCE_DIR}/plugins/Amplifier
	${CMAKE_SOURCE_DIR}/plugins/Compressor
	${CMAKE_SOURCE_DIR}/plugins/CrossoverEQ
	${CMAKE_SOURCE_DIR}/plugins/Delay
	${CMAKE_SOURCE_DIR}/plugins/ReverbSC)
target_compile_definitions(aibridge PRIVATE LMMS_PHASE3E_SOURCE_ROOT="${CMAKE_SOURCE_DIR}")
