######################################################################
# Automatically generated by qmake (2.01a) Thu Apr 26 12:38:34 2012
######################################################################

contains(QT_VERSION, ^4\\.[0-7]\\..*) {
	message("Cannot build InVpn with Qt version $${QT_VERSION}.")
	error("Use at least Qt 4.8.")
}

TEMPLATE = app
TARGET = 
DEPENDPATH += .
INCLUDEPATH += .
QT -= gui
QT += network

QMAKE_CXXFLAGS += -std=gnu++0x

target.path = /usr/sbin
INSTALLS += target

# Input
HEADERS += QTap.hpp InVpn.hpp InVpnSslServer.hpp InVpnNode.hpp
SOURCES += main.cpp QTap.cpp InVpn.cpp InVpnSslServer.cpp InVpnNode.cpp
