# qmake project for the GUI (alternative to CMakeLists.txt, e.g. for Qt Creator with qmake)
QT += widgets network
CONFIG += c++17
TARGET = UdpBandwidthTester
TEMPLATE = app

include(src/core/core.pri)   # UDP engine, independent of the GUI

INCLUDEPATH += src/gui

SOURCES += \
    src/gui/main.cpp \
    src/gui/common.cpp \
    src/gui/ratechart.cpp \
    src/gui/frameview.cpp \
    src/gui/senderpage.cpp \
    src/gui/receiverpage.cpp

HEADERS += \
    src/gui/common.h \
    src/gui/ratechart.h \
    src/gui/frameview.h \
    src/gui/senderpage.h \
    src/gui/receiverpage.h
