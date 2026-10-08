# qmake project for the command-line tool (alternative to CMakeLists.txt)
QT = core network
CONFIG += c++17 console
CONFIG -= app_bundle
TARGET = udpbw-cli
TEMPLATE = app

include(src/core/core.pri)   # UDP engine, independent of the GUI

SOURCES += src/cli/main.cpp
