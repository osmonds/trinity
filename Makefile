VERSION=1.4pre

INSTALL_PREFIX ?= $(DESTDIR)
INSTALL_PREFIX ?= $(HOME)

ifeq ($(CC),"")
CC := gcc
endif
CC := $(CROSS_COMPILE)$(CC)
LD := $(CROSS_COMPILE)$(LD)

CFLAGS += -Wall -W -g -O2 -I. -Iinclude/ -Wimplicit -D_FORTIFY_SOURCE=2 -D_GNU_SOURCE -D__linux__
ifneq ($(SYSROOT),)
CFLAGS += --sysroot=$(SYSROOT)
endif
#CFLAGS += $(shell if $(CC) -m32 -S -o /dev/null -xc /dev/null >/dev/null 2>&1; then echo "-m32"; fi)
CFLAGS += -Wdeclaration-after-statement
CFLAGS += -Wformat=2
CFLAGS += -Winit-self
CFLAGS += -Wlogical-op
CFLAGS += -Wnested-externs
CFLAGS += -Wpacked
CFLAGS += -Wshadow
CFLAGS += -Wstrict-aliasing=3
CFLAGS += -Wswitch-enum
CFLAGS += -Wundef
CFLAGS += -Wwrite-strings
CFLAGS += -Wno-format-nonliteral
CFLAGS += -Wstrict-prototypes -Wmissing-prototypes

# Only enabled during development, and on gcc 4.8+
CPP_MAJOR := $(shell $(CPP) -dumpversion 2>&1 | cut -d'.' -f1)
CPP_MINOR := $(shell $(CPP) -dumpversion 2>&1 | cut -d'.' -f2)
DEVEL	:= $(shell grep VERSION Makefile | head -n1 | grep pre | wc -l)
CFLAGS	+= $(shell if [ $(CPP_MAJOR) -eq 4 -a $(CPP_MINOR) -ge 8 -a $(DEVEL) -eq 1 ] ; then echo "-Werror"; else echo ""; fi)

V	= @
Q	= $(V:1=)
QUIET_CC = $(Q:@=@echo    '  CC	'$@;)


all: trinity

test:
	@if [ ! -f config.h ]; then  echo "[1;31mRun configure.sh first.[0m" ; exit; fi


MACHINE		= $(shell $(CC) -dumpmachine)
SYSCALLS_ARCH	= $(shell case "$(MACHINE)" in \
		  (sh*) echo syscalls/sh/*.c ;; \
		  (ia64*) echo syscalls/ia64/*.c ;; \
		  esac)

HEADERS		= $(patsubst %.h,%.h,$(wildcard *.h)) $(patsubst %.h,%.h,$(wildcard syscalls/*.h)) $(patsubst %.h,%.h,$(wildcard ioctls/*.h))

SRCS		= $(wildcard *.c) \
		  $(wildcard children/*.c) \
		  $(wildcard ioctls/*.c) \
		  $(wildcard net/*.c) \
		  $(wildcard syscalls/*.c) \
		  $(SYSCALLS_ARCH)

OBJS		= $(sort $(patsubst %.c,%.o,$(wildcard *.c))) \
		  $(sort $(patsubst %.c,%.o,$(wildcard children/*.c))) \
		  $(sort $(patsubst %.c,%.o,$(wildcard ioctls/*.c))) \
		  $(sort $(patsubst %.c,%.o,$(wildcard net/*.c))) \
		  $(sort $(patsubst %.c,%.o,$(wildcard syscalls/*.c))) \
		  $(sort $(patsubst %.c,%.o,$(SYSCALLS_ARCH)))

DEPDIR= .deps

-include $(SRCS:%.c=$(DEPDIR)/%.d)

trinity: test $(OBJS) $(HEADERS)
	$(QUIET_CC)$(CC) $(CFLAGS) $(LDFLAGS) -o trinity $(OBJS)
	@mkdir -p tmp

df = $(DEPDIR)/$(*D)/$(*F)

%.o : %.c
	$(QUIET_CC)$(CC) $(CFLAGS) $(LDFLAGS) -o $@ -c $<
	@mkdir -p $(DEPDIR)/$(*D)
	@gcc -MM $(CFLAGS) $*.c > $(df).d
	@mv -f $(df).d $(df).d.tmp
	@sed -e 's|.*:|$*.o:|' <$(df).d.tmp > $(df).d
	@sed -e 's/.*://' -e 's/\\$$//' < $(df).d.tmp | fmt -1 | \
	  sed -e 's/^ *//' -e 's/$$/:/' >> $(df).d
	@rm -f $(df).d.tmp

clean:
	@rm -f $(OBJS)
	@rm -f core.*
	@rm -f trinity
	@rm -f tags
	@rm -rf $(DEPDIR)/*

devel:
	@perl -p -i -e 's/DEVEL=0/DEVEL=1/' configure.sh
	git commit Makefile configure.sh -m "Enable -Werror & devel mode"

release:
	@perl -p -i -e 's/DEVEL=1/DEVEL=0/' configure.sh
	git commit Makefile configure.sh -m "Disable -Werror & devel mode"

tag:
	git tag -a $(VERSION) -m "$(VERSION) release."

tarball:
	git repack -a -d
	git prune-packed
	git archive --format=tar.gz --prefix=trinity-$(VERSION)/ HEAD > trinity-$(VERSION).tgz

install: trinity
	install -d -m 755 $(INSTALL_PREFIX)/bin
	install trinity $(INSTALL_PREFIX)/bin

tags:	$(SRCS)
	@ctags -R --exclude=tmp

scan:
	@scan-build --use-analyzer=/usr/bin/clang make

coverity:
	rm -rf cov-int trinity-coverity.tgz
	cov-build --dir cov-int make
	tar czvf trinity-coverity.tgz cov-int

