CXX := g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra
PREFIX ?= /usr/local

all: ../sender ../receiver

../sender: sender.cpp
	$(CXX) $(CXXFLAGS) -o ../sender sender.cpp

../receiver: receiver.cpp
	$(CXX) $(CXXFLAGS) -o ../receiver receiver.cpp

install: ../sender ../receiver
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 0755 ../sender $(DESTDIR)$(PREFIX)/bin/sender
	install -m 0755 ../receiver $(DESTDIR)$(PREFIX)/bin/receiver

clean:
	rm -f ../sender ../receiver

.PHONY: all install clean
