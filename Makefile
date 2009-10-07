INSTALL ?= install

PREFIX ?= /usr/local
MANDIR ?= $(PREFIX)/share/man/man8
BINDIR ?= $(PREFIX)/bin

AUDIO ?= portaudio

alsa_SRCS = alsa.c
portaudio_SRCS = portaudio.c

SRCS = $($(AUDIO)_SRCS) \
	cli.c \
	espeak.c \
	espeakup.c  \
	queue.c \
	signal.c \
		softsynth.c

ifeq ($(AUDIO),alsa)
SOUNDLIB = -lasound
endif

OBJS = $(SRCS:.c=.o)

LDLIBS = -lespeak $(SOUNDLIB)

all: espeakup

install: espeakup
	$(INSTALL) -d $(DESTDIR)$(BINDIR)
	$(INSTALL) -d $(DESTDIR)$(MANDIR)
	$(INSTALL) $< $(DESTDIR)$(BINDIR)
	$(INSTALL) espeakup.8 $(DESTDIR)$(MANDIR)

clean:
	$(RM) *.d *.o

distclean: clean
	$(RM) espeakup

espeakup: $(OBJS)

%.o: %.c
	$(COMPILE.c) -MMD -Wall $(OUTPUT_OPTION) $<

-include $(SRCS:.c=.d)
