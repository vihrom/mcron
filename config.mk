VERSION = 1.1.0

PREFIX = /usr/local
MANPREFIX = ${PREFIX}/share/man

TARGET = mcron

CC = cc
STDFLAG = -std=c99

CPPFLAGS = -DVERSION=\"${VERSION}\"

BASE_CFLAGS = ${STDFLAG} -Wall -Wextra -Wpedantic -Wshadow -Werror

# Default mode is PROD
MODE = PROD

# Flag definitions for each mode
PROD_CFLAGS = ${BASE_CFLAGS} -O2 -DNDEBUG -fstack-protector-strong -D_FORTIFY_SOURCE=3 -fPIE
PROD_LDFLAGS = -pie -s -Wl,-z,relro,-z,now

DEV_CFLAGS = ${BASE_CFLAGS} -g -O0 -fno-omit-frame-pointer
DEV_LDFLAGS = 

ASAN_CFLAGS = ${BASE_CFLAGS} -g -O1 -fno-omit-frame-pointer -fsanitize=address -fsanitize=undefined -fno-sanitize-recover=all
ASAN_LDFLAGS = -fsanitize=address -fsanitize=undefined

# Nested macro expansion
CFLAGS = ${${MODE}_CFLAGS}
LDFLAGS = ${${MODE}_LDFLAGS}
