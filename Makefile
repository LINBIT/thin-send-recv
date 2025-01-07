all-src = Makefile README.md thin_delta_scanner.fl thin_delta_scanner.h thin_send_recv.c thin_send_recv.spec
all-src += run_tests.sh $(addprefix tests/,00-volume-copy.sh 01-volume-diff.sh 02-volume-diff-with-discards.sh 03-sig-while-metadata-locked.sh)
VERSION = $(shell sed -ne '/^Version:/{s/Version: \(.*\)/\1/;p;q;}' thin_send_recv.spec)
all-obj = thin_send_recv.o thin_delta_scanner.o
CFLAGS  ?= -o2 -Wall
CFLAGS  += -DVERSION=\"$(VERSION)\" $(EXTRA_CFLAGS)

# globs are messy, would need dh_clean, better name the ones we need
DEBFILES = rules copyright source/format changelog compat control

all:	thin_send thin_recv

thin_send thin_recv: thin_send_recv
	ln -f -s $^ $@

thin_send_recv: $(all-obj)
	$(LINK.c) $(LDFLAGS) -o $@ $^

thin_delta_scanner.c: thin_delta_scanner.fl thin_delta_scanner.h
	flex -s -othin_delta_scanner.c thin_delta_scanner.fl

install: thin_send_recv
	mkdir -p $(DESTDIR)/usr/bin
	install -D thin_send_recv $(DESTDIR)/usr/bin/thin_send_recv
	cd $(DESTDIR)/usr/bin ; ln -f -s thin_send_recv thin_send
	cd $(DESTDIR)/usr/bin ; ln -f -s thin_send_recv thin_recv

tgz: $(all-src)
	tar --transform="flags=rSh;s,^,thin-send-recv-$(VERSION)/," \
		--owner=0 --group=0 -czf - $(all-src) \
		$(if $(PRESERVE_DEBIAN), $(addprefix debian/, $(DEBFILES)),) \
		> thin-send-recv-$(VERSION).tar.gz

release: tgz

debrelease:
	make tgz PRESERVE_DEBIAN=1

clean:
	rm -rf $(all-obj) thin_delta_scanner.c *~ thin_send_recv thin_send thin_recv

# test target is used by packaging tools, but this needs a VG, so keep it out and use tests as target name
tests: all
	./run_tests.sh
