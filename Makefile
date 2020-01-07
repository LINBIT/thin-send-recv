all-src = Makefile README thin_delta_scanner.fl thin_delta_scanner.h thin_send_recv.c
VERSION = 1.0
all-obj = thin_send_recv.o thin_delta_scanner.o
CFLAGS = -o2 -Wall -DVERSION=\"$(VERSION)\"

all:	thin_send thin_recv

thin_send thin_recv: thin_send_recv
	ln -f -s $^ $@

thin_send_recv: $(all-obj)
	$(LINK.c) $(LDFLAGS) -o $@ $^

thin_delta_scanner.c: thin_delta_scanner.fl thin_delta_scanner.h
	flex -s -othin_delta_scanner.c thin_delta_scanner.fl

install: thin_send_recv
	install -t $(PREFIX)/usr/bin thin_send_recv
	cd $(PREFIX)/usr/bin ; ln -f -s thin_send_recv thin_send
	cd $(PREFIX)/usr/bin ; ln -f -s thin_send_recv thin_recv

tgz: $(all-src)
	tar --transform="flags=rSh;s,^,thin-send-recv-$(VERSION)/," \
		--owner=0 --group=0 -czf - $(all-src) > thin_send_recv-$(VERSION).tar.gz

clean:
	rm -rf $(all-obj) thin_delta_scanner.c *~ thin_send_recv thin_send thin_recv
