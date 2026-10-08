cmake_minimum_required(VERSION 3.16)

project(UdpBandwidthTester VERSION 1.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# Works with Qt 6 or Qt 5.15+
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core Network Widgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Network Widgets)

function(udpbw_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endif()
endfunction()

# ---------------------------------------------------------------------------
# Core: everything that builds, sends, receives and checks UDP datagrams.
# Independent of any GUI: links only Qt Core + Network, so a widget include here
# fails to build. Front ends use UdpSender / UdpReceiver.
# ---------------------------------------------------------------------------
add_library(udpbw_core STATIC
    src/core/protocol.h
    src/core/format.h           src/core/format.cpp
    src/core/payload.h          src/core/payload.cpp
    src/core/framelayout.h      src/core/framelayout.cpp
    src/core/netutil.h          src/core/netutil.cpp
    src/core/senderworker.h     src/core/senderworker.cpp
    src/core/receiverworker.h   src/core/receiverworker.cpp
    src/core/udpsender.h        src/core/udpsender.cpp
    src/core/udpreceiver.h      src/core/udpreceiver.cpp
)
target_include_directories(udpbw_core PUBLIC src/core)
target_link_libraries(udpbw_core PUBLIC Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Network)
if(WIN32)
    target_link_libraries(udpbw_core PUBLIC winmm)
endif()
udpbw_warnings(udpbw_core)

# ---------------------------------------------------------------------------
# GUI front end (widgets only, no networking code)
# ---------------------------------------------------------------------------
add_executable(UdpBandwidthTester WIN32
    src/gui/main.cpp
    src/gui/common.h            src/gui/common.cpp
    src/gui/ratechart.h         src/gui/ratechart.cpp
    src/gui/frameview.h         src/gui/frameview.cpp
    src/gui/senderpage.h        src/gui/senderpage.cpp
    src/gui/receiverpage.h      src/gui/receiverpage.cpp
)
target_link_libraries(UdpBandwidthTester PRIVATE udpbw_core Qt${QT_VERSION_MAJOR}::Widgets)
udpbw_warnings(UdpBandwidthTester)

# ---------------------------------------------------------------------------
# Command-line front end (no networking code)
# ---------------------------------------------------------------------------
add_executable(udpbw-cli src/cli/main.cpp)
target_link_libraries(udpbw-cli PRIVATE udpbw_core)
udpbw_warnings(udpbw-cli)
