all-obj = lvm_send_recv.o thin_delta_scanner.o

all:	lvm_send_recv

lvm_send_recv: $(all-obj)
	$(LINK.c) $(LDFLAGS) -o $@ $^

thin_delta_scanner.c: thin_delta_scanner.fl thin_delta_scanner.h
	flex -othin_delta_scanner.c thin_delta_scanner.fl

clean:
	rm -rf $(all-obj) thin_delta_scanner.c
