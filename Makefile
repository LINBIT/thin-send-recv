all-obj = thin_send_recv.o thin_delta_scanner.o
CFLAGS = -o2 -Wall

all:	thin_send_recv

thin_send_recv: $(all-obj)
	$(LINK.c) $(LDFLAGS) -o $@ $^

thin_delta_scanner.c: thin_delta_scanner.fl thin_delta_scanner.h
	flex -othin_delta_scanner.c thin_delta_scanner.fl

clean:
	rm -rf $(all-obj) thin_delta_scanner.c *~ thin_send_recv
