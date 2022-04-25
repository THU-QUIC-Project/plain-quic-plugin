PROJECT(plain_quic_plugin C)

CMAKE_MINIMUM_REQUIRED(VERSION 3.15)
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

find_package(Wireshark REQUIRED PATHS $ENV{WIRESHARK_PATH})

MESSAGE(STATUS "Wireshark_FOUND: ${Wireshark_FOUND}")
MESSAGE(STATUS "Wireshark_VERSION_MAJOR: ${Wireshark_VERSION_MAJOR}")
MESSAGE(STATUS "Wireshark_VERSION_MINOR: ${Wireshark_VERSION_MINOR}")
MESSAGE(STATUS "Wireshark_VERSION_PATCH: ${Wireshark_VERSION_PATCH}")
MESSAGE(STATUS "Wireshark_VERSION: ${Wireshark_VERSION}")
MESSAGE(STATUS "Wireshark_VERSION_STRING: ${Wireshark_VERSION_STRING}")
MESSAGE(STATUS "Wireshark_INSTALL_DIR: ${Wireshark_INSTALL_DIR}")
MESSAGE(STATUS "Wireshark_PLUGIN_INSTALL_DIR: ${Wireshark_PLUGIN_INSTALL_DIR}")
MESSAGE(STATUS "Wireshark_LIB_DIR: ${Wireshark_LIB_DIR}")
MESSAGE(STATUS "Wireshark_LIBRARY: ${Wireshark_LIBRARY}")
MESSAGE(STATUS "Wireshark_INCLUDE_DIR: ${Wireshark_INCLUDE_DIR}")
MESSAGE(STATUS "Wireshark_CMAKE_MODULES_DIR: ${Wireshark_CMAKE_MODULES_DIR}")

INCLUDE(UseMakePluginReg)

set(GLIB2_MIN_VERSION 2.4.0)
find_package(GLIB2)
include_directories(${GLIB2_INCLUDE_DIRS})

include_directories(${Wireshark_INCLUDE_DIR})

IF(APPLE)
  LINK_DIRECTORIES(/usr/local/lib)
ENDIF()

set(LINK_MODE_LIB SHARED)
set(LINK_MODE_MODULE MODULE)

set(PLUGIN_VERSION "0.0.1")
add_definitions(-DPLUGIN_VERSION=\"${PLUGIN_VERSION}\")

set(DISSECTOR_SRC
	src/packet-plain-quic.c
)

set(PLUGIN_FILES
	plugin.c
	${DISSECTOR_SRC}
)

include_directories(${CMAKE_CURRENT_SOURCE_DIR})

register_plugin_files(plugin.c
	plugin
	${DISSECTOR_SRC}
)

add_library(plain_quic ${LINK_MODE_MODULE}
	${PLUGIN_FILES}
	${PLUGIN_RC_FILE}
)

message("WS_LINK_FLAGS: ${WS_LINK_FLAGS}")

target_link_directories(plain_quic PUBLIC ${Wireshark_LIB_DIR})

target_link_libraries(plain_quic wireshark wsutil wiretap)
