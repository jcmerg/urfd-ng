// ambecheck -- the FEC of a file of 9-byte AMBE frames, as tcd counts it for the AMBE devices
//   ambecheck dstar|dmr <in.frames> [-v]
#include <cstdio>
#include <cstring>
#include "AmbeFrameCheck.h"
int main(int argc, char *argv[])
{
	if (argc < 3) { fprintf(stderr, "usage: %s dstar|dmr <in.frames> [-v]\n", argv[0]); return 1; }
	const bool dstar = !strcmp(argv[1], "dstar"), verbose = argc > 3;
	FILE *in = fopen(argv[2], "rb");
	if (!in) { perror("open"); return 1; }
	uint8_t f[9];
	unsigned frames = 0, with_errors = 0, errors = 0, uncorrectable = 0;
	while (fread(f, 1, 9, in) == 9)
	{
		const SAmbeFec r = dstar ? AmbeFrameCheck::DStar(f) : AmbeFrameCheck::Dmr(f);
		frames++;
		if (r.errors) with_errors++;
		errors += r.errors;
		if (r.uncorrectable) uncorrectable++;
		if (verbose && (r.errors || r.uncorrectable))
			printf("frame %u: %d errors%s\n", frames - 1, r.errors, r.uncorrectable ? ", uncorrectable" : "");
	}
	fclose(in);
	printf("%u frames, %u with corrected errors (%u bits), %u uncorrectable\n", frames, with_errors, errors, uncorrectable);
	return 0;
}
