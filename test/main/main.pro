include( ../../pri/nbody.pri )

TEMPLATE	= app
TARGET		= nbody-simulation
MOC_DIR = ./.tmp/moc
DESTDIR = ./

CONFIG		+= qt
QT += opengl
win32:LIBS += -lGLU32
unix:LIBS += -lGLU

INCLUDEPATH += ../../nbody
LIBS += -L../../lib
LIBS += -lnbody

SOURCES	+= main.cpp

