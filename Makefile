# Top-level Makefile
.PHONY: all clean install

all:
	$(MAKE) -C src all

clean:
	$(MAKE) -C src clean

install:
	$(MAKE) -C src install
