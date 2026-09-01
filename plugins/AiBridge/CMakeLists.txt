INCLUDE(BuildPlugin)

find_package(Qt${QT_VERSION_MAJOR} COMPONENTS Network REQUIRED)

BUILD_PLUGIN(aibridge AiBridge.cpp AiBridge.h AiBridgeActions.cpp AiBridgeActions.h AiBridgeContext.cpp AiBridgeContext.h AiBridgeIpcServer.cpp AiBridgeIpcServer.h AiBridgeProtocol.cpp AiBridgeProtocol.h MOCFILES AiBridge.h)
target_link_libraries(aibridge Qt${QT_VERSION_MAJOR}::Network)
