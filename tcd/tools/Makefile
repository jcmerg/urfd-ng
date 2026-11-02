# test tools for codec plugins and DVSI devices, not installed (see "Codec
# plugins" in ../README.md)

CXXFLAGS = -O2 -W -Werror -std=c++17 -I..

all : codectest decode dvsitest ambecheck

codectest : codectest.cpp ../CodecPlugin.h
	$(CXX) $(CXXFLAGS) $< -o $@ -ldl

decode : decode.cpp ../CodecPlugin.h
	$(CXX) $(CXXFLAGS) $< -o $@ -ldl

dvsitest : dvsitest.cpp ../DeviceIO_posix.cpp ../DeviceIO.h
	$(CXX) $(CXXFLAGS) dvsitest.cpp ../DeviceIO_posix.cpp -o $@

ambecheck : ambecheck.cpp ../AmbeFrameCheck.h ../Golay24128.cpp
	$(CXX) $(CXXFLAGS) ambecheck.cpp ../Golay24128.cpp -o $@

clean :
	$(RM) codectest decode dvsitest ambecheck
