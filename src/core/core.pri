# UDP engine shared by the GUI and the CLI. Needs only Qt Core + Network.
QT *= core network
INCLUDEPATH += $$PWD

SOURCES += \
    $$PWD/format.cpp \
    $$PWD/payload.cpp \
    $$PWD/framelayout.cpp \
    $$PWD/netutil.cpp \
    $$PWD/senderworker.cpp \
    $$PWD/receiverworker.cpp \
    $$PWD/udpsender.cpp \
    $$PWD/udpreceiver.cpp

HEADERS += \
    $$PWD/protocol.h \
    $$PWD/format.h \
    $$PWD/payload.h \
    $$PWD/framelayout.h \
    $$PWD/netutil.h \
    $$PWD/senderworker.h \
    $$PWD/receiverworker.h \
    $$PWD/udpsender.h \
    $$PWD/udpreceiver.h

win32: LIBS += -lwinmm
