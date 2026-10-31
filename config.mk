NAME = dwmstatus
VERSION = 1.1

# Customize below to fit your system

# paths
PREFIX = /usr/local
MANPREFIX = ${PREFIX}/share/man

# includes and libs
# (on BSD / old X11R6 layouts add -I/usr/X11R6/include and -L/usr/X11R6/lib)
INCS = -I.
LIBS = -lX11

# flags: optimised for speed, debug symbols stripped
# NOTE: The backslashes before the quotes are strictly required!
CPPFLAGS = -DVERSION=\"${VERSION}\" -D_DEFAULT_SOURCE
CFLAGS = -std=c99 -pedantic -Wall -Wextra -O2 ${INCS} ${CPPFLAGS}
LDFLAGS = -s ${LIBS}

# compiler and linker
CC = cc