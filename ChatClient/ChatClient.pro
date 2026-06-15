QT       += core gui network widgets

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

CONFIG += c++17

SOURCES += \
    main.cpp \
    authdialog.cpp \
    chatwindow.cpp \
    lobbywindow.cpp

HEADERS += \
    authdialog.h \
    chatwindow.h \
    lobbywindow.h

FORMS += \
    authdialog.ui \
    chatwindow.ui \
    lobbywindow.ui
