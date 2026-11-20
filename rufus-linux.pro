QT += core gui qml quick quickcontrols2 dbus

CONFIG += c++17 release link_pkgconfig
PKGCONFIG += libarchive
TARGET = rufus-linux
TEMPLATE = app

HEADERS += \
    src/backend.h \
    src/filepicker.h \
    src/isoanalyzer.h \
    src/portalfilepicker.h \
    src/theme.h \
    src/translator.h \
    src/unattend.h \
    src/worker.h

SOURCES += \
    src/main.cpp \
    src/backend.cpp \
    src/isoanalyzer.cpp \
    src/portalfilepicker.cpp \
    src/theme.cpp \
    src/translator.cpp \
    src/unattend.cpp \
    src/worker.cpp

RESOURCES += src/resources.qrc
