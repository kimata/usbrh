# $Id: Makefile 40 2009-09-04 16:13:44Z kimata $
PACKAGE     	:= usbrh
VERSION     	:= 0.0.8
MODULE      	:= usbrh.ko

PKGDIR   		:= $(PACKAGE)-$(VERSION)
DISTFILE 		:= $(PKGDIR).tgz

all:
	$(MAKE) -C src $@

install:
	$(MAKE) -C src $@
	cp -f util/10-usbrh.rules /etc/udev/rules.d/
	cp -f util/usbrh.conf /etc/modprobe.d/
	cp -f util/usbrh.sh /lib/udev/

doc:
	$(MAKE) -C doc

dist: TMPDIR := $(shell mktemp -d -t $(PACKAGE).XXXXXX 2>/dev/null)
dist:
	svn export . $(TMPDIR)/$(PKGDIR)
	rm -rf $(TMPDIR)/$(PKGDIR)/pseudo
	touch -t `date "+%m%d"`0000 `find $(TMPDIR)`
	tar zcvf $(DISTFILE) -C $(TMPDIR) $(PKGDIR)
	rm -Rf $(TMPDIR)

clean:
	$(MAKE) -C src $@
	$(MAKE) -C doc $@
	rm -f $(MODULE)

.PHONY: doc