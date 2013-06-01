PACKAGE     	:= usbrh
VERSION     	:= 0.0.9
MODULE      	:= usbrh.ko

PKGDIR   	:= $(PACKAGE)-$(VERSION)
DISTFILE 	:= $(PKGDIR).tgz

all:
	$(MAKE) -C src $@

install:
	$(MAKE) -C src $@
	cp -f util/10-usbrh.rules /etc/udev/rules.d/
	cp -f util/usbrh.sh /lib/udev/

doc:
	$(MAKE) -C doc

dist:
	git archive --prefix $(PKGDIR)/ master | gzip > $(DISTFILE)

clean:
	$(MAKE) -C src $@
	$(MAKE) -C doc $@
	rm -f $(MODULE)

.PHONY: doc