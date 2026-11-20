QT += core gui quick quickcontrols2 testlib dbus
CONFIG += c++17 testcase link_pkgconfig
PKGCONFIG += libarchive
TARGET = rufus-linux_tests
TEMPLATE = app

INCLUDEPATH += ../src

HEADERS += \
    ../src/backend.h \
    ../src/filepicker.h \
    ../src/isoanalyzer.h \
    ../src/portalfilepicker.h \
    ../src/theme.h \
    ../src/translator.h \
    ../src/unattend.h \
    ../src/worker.h

SOURCES += \
    rufus-linux_tests.cpp \
    ../src/backend.cpp \
    ../src/isoanalyzer.cpp \
    ../src/portalfilepicker.cpp \
    ../src/theme.cpp \
    ../src/translator.cpp \
    ../src/unattend.cpp \
    ../src/worker.cpp

RESOURCES += ../src/resources.qrc
