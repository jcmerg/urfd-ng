#Copyright (C) 2021 by Thomas A. Early, N7TAE

include tcd.mk

GCC = g++

# Shared source files from urfd, in the same repository
URFD_DIR ?= ../reflector
URFD_FILES = IP.cpp IP.h TCPacketDef.h TCSocket.cpp TCSocket.h Timer.h Golay24128.cpp Golay24128.h YSFDefines.h SHA256.cpp SHA256.h RejectLog.cpp RejectLog.h

ifeq ($(debug), true)
CFLAGS = -ggdb3 -W -Werror -Icodec2 -MMD -MD -std=c++17
else
CFLAGS = -O2 -W -Werror -Icodec2 -MMD -MD -std=c++17
endif

CFLAGS += -Iimbe

# a single DVSI device needs a codec plugin for DMR (CodecPlugin.h), loaded with dlopen
LDFLAGS = -lspeexdsp -lwebsockets -pthread -latomic -ldl

# Explicit source list (includes urfd shared files)
SRCS = CodecPlugins.cpp Configure.cpp Controller.cpp DV3000.cpp DV3003.cpp DVSIDevice.cpp \
		 IP.cpp Main.cpp RejectLog.cpp SHA256.cpp TCSocket.cpp TranscoderPacket.cpp \
		 DeviceIO_posix.cpp \
       MonitorServer.cpp StatsLogger.cpp monitor_html.cpp Golay24128.cpp \
       codec2/codebooks.cpp codec2/codec2.cpp codec2/kiss_fft.cpp \
       codec2/lpc.cpp codec2/nlp.cpp codec2/pack.cpp codec2/qbase.cpp \
       codec2/quantise.cpp
# the P25 IMBE vocoder from OP25, see imbe/README.md
IMBESRCS = $(wildcard imbe/*.cc)
OBJS = $(SRCS:.cpp=.o) $(IMBESRCS:.cc=.o)
DEPS = $(SRCS:.cpp=.d) $(IMBESRCS:.cc=.d)
EXE = tcd
MON = tcdmon

# Copy urfd files first, generate HTML embed, then build
all : urfd-files monitor_html.cpp
	$(MAKE) $(EXE) $(MON)

$(EXE) : $(OBJS)
	$(GCC) $(OBJS) $(LDFLAGS) -o $@

$(MON) : tcdmon.o
	$(GCC) tcdmon.o -lncurses -o $@

tcdmon.o : tcdmon.cpp
	$(GCC) $(CFLAGS) -c $< -o $@

monitor_html.cpp : monitor.html embed_html.sh $(wildcard ../dashboard/fonts/saira-latin.woff2)
	bash embed_html.sh

%.o : %.cpp
	$(GCC) $(CFLAGS) -c $< -o $@

# its own code, built as it comes, without -W -Werror
imbe/%.o : imbe/%.cc
	$(GCC) -O2 -MMD -MD -std=c++17 -c $< -o $@

urfd-files :
	@for f in $(URFD_FILES); do \
		if [ ! -f $$f ] || [ $(URFD_DIR)/$$f -nt $$f ]; then \
			cp $(URFD_DIR)/$$f . && echo "Copied $$f from urfd"; \
		fi \
	done

clean :
	$(RM) $(EXE) $(MON) $(OBJS) $(DEPS) $(URFD_FILES) tcdmon.o monitor_html.cpp

-include $(DEPS)

# The install and uninstall targets need to be run by root
install : all
	cp $(EXE) $(MON) $(BINDIR)
	cp config/$(EXE).service /etc/systemd/system/
	systemctl enable $(EXE)
	systemctl daemon-reload
	systemctl start $(EXE)

uninstall :
	systemctl stop $(EXE)
	systemctl disable $(EXE)
	systemctl daemon-reload
	rm -f /etc/systemd/system/$(EXE).service
	rm -f $(BINDIR)/$(EXE)
	systemctl daemon-reload
