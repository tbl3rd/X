ifneq ($(filter Darwin,${shell uname -s}),)  # if mac
  CC = /opt/local/bin/gcc-mp-4.6
endif

ECHO := echo
RM := rm -rf

CFLAGS = -std=gnu99 -D_GNU_SOURCE -g -O0

SOURCES := $(shell echo *.c)
OBJECTS := $(SOURCES:%.c=%.o)

all: switch tester driver

switch: control.o forward.o process.o route.o switch.o tap.o tilera.o util.o
	$(CC) $(CFLAGS) -o $@ $^ -lpthread -lnetio -ltmc

tester: packets.o process.o route.o tap.o tester.o tilera.o util.o
	$(CC) $(CFLAGS) -o $@ $^ -lpthread -lnetio -ltmc

# The driver program should not depend on Tilera libraries.
#
driver: driver.o route.o util.o
	$(CC) $(CFLAGS) -o $@ $^

control.o: control.c control.h tilera.h util.h

driver.o: driver.c route.h util.h

forward.o: forward.c forward.h tilera.h util.h

packets.o: packets.c packets.h process.h tilera.h util.h

process.o: process.c process.h forward.h tap.h tilera.h util.h

route.o: route.c route.h tilera.h util.h

switch.o: switch.c route.h tilera.h util.h

tap.o: tap.c tap.h util.h

tester.o: tester.c packets.h process.h tilera.h util.h

tilera.o: tilera.c tilera.h util.h

util.o: util.c util.h

.PHONY: objects
objects: $(OBJECTS)

.PHONY: test
PORTS:= 1024 1025 1026 1027 1028 1029 1030 1031 1032 1033 1034 1035 1036 1037
PORTS:= 1024 1025 1026
HOSTNAME := $(shell hostname)
HOSTNAME := dhcp-1-193
HOSTNAME := dhcp-1-198
HOSTNAME := $(shell hostname -s)
LOCALHOST := $(shell hostname)
HOSTNAME := dhcp-1-207
HOSTNAME := dhcp-1-184
TV_PORT := 1234
VIDEOSERVER_PORT := 4321
CLIPS := /lab/photon/clips/HouseOfFlyingDaggers
CLIP := Daggers_VTS_02_2.ffmpeg-cbr3180.ts

# $(MAKE_MPEGTS) starts the video $(CLIP) streaming from the videoserver
# on $(LOCALHOST) at $(VIDEOSERVER_PORT) to each destination read from
# stdin ... I hope.
#
define MAKE_MPEGTS
    python -m videoclient.createstream --debug --repeat \
    --api-port $(VIDEOSERVER_PORT) $(LOCALHOST) $(CLIP)
endef

# $(call MAKE_DESTINATION,$(HOST),$(PORT)) to create a JSON string
# describing a route to $(HOST):$(PORT) to drive the switch.
#
define MAKE_DESTINATION
{ "host" : "$(1)", "port" : $(2) },
endef

define MAKE_ROUTE
{ "from" : $(1), "port" : $(2), "ip" : "$(3)", "mac" : "$(4)" }
endef

# $(call MAKE_ROUTES,$(FILE),$(HOST),$(PORTS)) to create a file named
# $(FILE) containing JSON route directives to $(HOST), one per port
# number in $(PORTS).
#
define MAKE_ROUTES
$(RM) $(1);
$(ECHO) '[' >> $(1);
$(foreach P,$(3),$(ECHO) '$(call MAKE_DESTINATION,$(2),$(P))' >> $(1);)
$(ECHO) ']' >> $(1);
endef

test: switch tvs videoserver routes.txt
	./switch routes.txt > tbl-0 &
	sleep 1;
	$(MAKE_MPEGTS) < tbl-0
	sleep 6000;


routes.txt: Makefile
	$(call MAKE_ROUTES,$@,$(HOSTNAME),$(PORTS))


.PHONY: tvs notvs
# $(call MAKE_TV,TV_PORT) to start a VLC monitor on $(TV_PORT).
#
define MAKE_TV
  vlc 'udp://@:$(1)' &
  sleep 1;
endef
tvs: notvs
	$(foreach P,$(PORTS),$(call MAKE_TV,$(P)))

notvs:
	-killall VLC


.PHONY: tags TAGS
TAGS_TMP := $(shell echo ./TAGS.tmp.$$$$)
TAGS tags:
	rm -f $(TAGS_TMP)
	find . -type f -print |\
	grep -i -e '\.[ch]$$' |\
	xargs etags -a -o $(TAGS_TMP)
	cmp -s ./TAGS $(TAGS_TMP) ||\
	rm -f ./TAGS &&\
	mv $(TAGS_TMP) ./TAGS &&\
	rm -f $(TAGS_TMP)

.PHONY: clean
clean:
	rm -rf switch switch.tar switch.tar.gz *.o *.dSYM TAGS

switch.tar.gz: clean
	rm -f /tmp/switch.tar
	tar -cvhf /tmp/switch.tar --exclude .git .
	mv /tmp/switch.tar .
	gzip switch.tar

.PHONY: tar
tar: switch.tar.gz

# Start a videoserver for $(CLIPS) listening on $(VIDEOSERVER_PORT)
#
videoserver: ../videoserver/jars/videoserver.jar
	java -jar ../videoserver/jars/videoserver.jar $(CLIPS) $(VIDEOSERVER_PORT) &
	sleep 5

../videoserver/jars/videoserver.jar:
	$(MAKE) -C ../videoserver
