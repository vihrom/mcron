include config.mk

SRC = mcron.c
OBJ = ${SRC:.c=.o}

all: ${TARGET}

${OBJ}: config.mk

.c.o:
	${CC} -c ${CPPFLAGS} ${CFLAGS} ${INCS} $<

${TARGET}: ${OBJ}
	${CC} -o $@ ${OBJ} ${LDFLAGS}

clean:
	rm -f ${TARGET} ${OBJ}

install: all
	mkdir -p ${DESTDIR}${PREFIX}/bin
	cp -f ${TARGET} ${DESTDIR}${PREFIX}/bin
	chmod 755 ${DESTDIR}${PREFIX}/bin/${TARGET}

	mkdir -p ${DESTDIR}${MANPREFIX}/man1
	cp -f ${TARGET}.1 ${DESTDIR}${MANPREFIX}/man1
	chmod 644 ${DESTDIR}${MANPREFIX}/man1/${TARGET}.1

	mkdir -p ${DESTDIR}/etc
	if [ ! -f ${DESTDIR}/etc/mcron.conf ]; then \
		cp mcron.conf.example ${DESTDIR}/etc/mcron.conf; \
		chmod 644 ${DESTDIR}/etc/mcron.conf; \
	fi

uninstall:
	rm -f ${DESTDIR}${PREFIX}/bin/${TARGET}
	rm -f ${DESTDIR}${MANPREFIX}/man1/${TARGET}.1

.PHONY: all clean install uninstall
