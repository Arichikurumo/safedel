CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -D_GNU_SOURCE -Wall -Wextra
# sysfs paths are far below PATH_MAX; snprintf truncation warnings there are noise
CFLAGS  += -Wno-format-truncation
PREFIX  ?= /usr/local

SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)
HDR := $(wildcard src/*.h)

safedel: $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ)

src/%.o: src/%.c $(HDR)
	$(CC) $(CFLAGS) -c -o $@ $<

install: safedel
	install -Dm755 safedel $(DESTDIR)$(PREFIX)/sbin/safedel

clean:
	rm -f safedel $(OBJ)

.PHONY: install clean
