CC = gcc
CFLAGS = -std=c99 -Wall -Wextra -pedantic -Wformat -Wformat=2 -Wconversion -Werror=format-security -Werror=implicit -Werror=incompatible-pointer-types -Werror=int-conversion -pipe
LDLIBS = -lsystemd -lcurl

PROG = syslog-remote_to_journal-remote

# all: dev
all: release

dev: ${PROG}
dev: CFLAGS += -Werror

release: ${PROG}
release: CFLAGS += -s -O2 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3 -fPIE -pie -Wl,-z,relro -Wl,-z,now -fstack-protector-strong -fstack-clash-protection -fcf-protection=full -Wl,-z,noexecstack -Wl,-z,nodlopen

clean:
	${RM} ${PROG}

ifeq ($(PREFIX),)
    PREFIX := /usr/local
endif

install: ${PROG}
	install --compare --preserve-timestamps --mode=755 ${PROG} $(DESTDIR)$(PREFIX)/sbin/
	install --compare --preserve-timestamps --mode=644 ${PROG}.socket $(DESTDIR)/etc/systemd/system/
	install --compare --preserve-timestamps --mode=644 ${PROG}.service $(DESTDIR)/etc/systemd/system/

