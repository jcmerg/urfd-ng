// decode -- decodes a file of frames with a tcd codec plugin to raw PCM
//   decode <plugin.so> <dmr|dstar|p25> <in.frames> <out.raw> [option=value ...]
// The file is one transmission (start before the first frame, stop after the last).
#include <cstdio>
#include <dlfcn.h>
#include <string>
#include <vector>
#include "CodecPlugin.h"

int main(int argc, char *argv[])
{
	if (argc < 5) { fprintf(stderr, "usage: %s <plugin.so> <dmr|dstar|p25> <in.frames> <out.raw> [option=value ...]\n", argv[0]); return 1; }
	void *h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	if (!h) { fprintf(stderr, "%s\n", dlerror()); return 1; }
	auto func = reinterpret_cast<tcd_codec_plugin_func>(dlsym(h, "tcd_codec_plugin"));
	const tcd_codec_plugin *p = func ? func() : nullptr;
	if (!p || p->api != TCD_CODEC_API) { fprintf(stderr, "no tcd codec plugin\n"); return 1; }
	const std::string c(argv[2]);
	const uint32_t codec = c == "dmr" ? TCD_CODEC_DMR : c == "dstar" ? TCD_CODEC_DSTAR : TCD_CODEC_P25;
	const size_t fsize = codec == TCD_CODEC_P25 ? 11 : 9;
	for (int i = 5; i < argc; i++)
	{
		std::string kv(argv[i]);
		const auto eq = kv.find('=');
		if (eq == std::string::npos || !p->option || p->option(kv.substr(0, eq).c_str(), kv.substr(eq + 1).c_str()) != 1) { fprintf(stderr, "option %s not taken\n", argv[i]); return 1; }
	}
	if (p->init()) return 1;
	FILE *in = fopen(argv[3], "rb"), *out = fopen(argv[4], "wb");
	if (!in || !out) { perror("open"); return 1; }
	void *st = p->open(codec);
	if (!st) { fprintf(stderr, "open failed\n"); return 1; }
	if (p->start) p->start(st);
	std::vector<uint8_t> f(fsize);
	int16_t pcm[160];
	while (fread(f.data(), 1, fsize, in) == fsize)
	{
		p->decode(st, f.data(), pcm);
		fwrite(pcm, sizeof(int16_t), 160, out);
	}
	if (p->stop) p->stop(st);
	p->close(st);
	if (p->shutdown) p->shutdown();
	return 0;
}
