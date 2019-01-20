CC      = clang
CFLAGS  = -lgit2 -lcurses -Wall -Wextra -Wpedantic \
          -Wformat=2 -Wno-unused-parameter -Wshadow \
          -Wwrite-strings -Wstrict-prototypes -Wold-style-definition \
          -Wredundant-decls -Wnested-externs -Wmissing-include-dirs \
	  -std=gnu11

RM      = rm -f

ifeq ($(PREFIX),)
    PREFIX := /usr/local
endif

default: all

all: gitsi

release: src/main.c
	$(CC) $(CFLAGS) -O2 -o gitsi src/main.c

gitsi: src/main.c
	$(CC) $(CFLAGS) -g -DDEBUG -o gitsi src/main.c

install:
	install -d $(DESTDIR)$(PREFIX)/bin/
	install -m 755 gitsi $(DESTDIR)$(PREFIX)/bin/
	install -d $(DESTDIR)$(PREFIX)/share/man/
	install -m 644 res/gitsi.1 $(DESTDIR)$(PREFIX)/share/man/man1/

clean veryclean:
	$(RM) gitsi
	$(RM) -R gitsi.dSYM
