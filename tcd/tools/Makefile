# test tools for codec plugins and DVSI devices, not installed (see "Codec
# plugins" in ../README.md)

CXXFLAGS = -O2 -W -Werror -std=c++17 -I..

all : codectest decode dvsitest

codectest : codectest.cpp ../CodecPlugin.h
	$(CXX) $(CXXFLAGS) $< -o $@ -ldl

decode : decode.cpp ../CodecPlugin.h
	$(CXX) $(CXXFLAGS) $< -o $@ -ldl

dvsitest : dvsitest.cpp ../DeviceIO_posix.cpp ../DeviceIO.h
	$(CXX) $(CXXFLAGS) dvsitest.cpp ../DeviceIO_posix.cpp -o $@

clean :
	$(RM) codectest decode dvsitest
