// codectest -- runs a tcd codec plugin on raw PCM (8 kHz, s16le, mono)
//
//   codectest <plugin.so> <codec: dmr|dstar|p25> <in.raw> <out.frames> <out.raw> [option=value ...]
//   codectest <plugin.so> <from>:<to> <in.raw> <out.frames> <out.raw> [option=value ...]
//
// The second form (plugin API 4, tcd 0.20.0) shows the direct path: the audio
// is encoded as <from> and decoded, and every decoded frame is made into a <to>
// frame by the plugin's transcode, as tcd does with Transcode = direct; it
// writes the <to> frames and their decoded audio, and prints the time per
// frame of transcode against decoding and encoding again. It checks neither
// the level nor the stream separation of the direct path (the plugin's own
// tests do); compare the audio it writes for that.
//
// Encodes every 160 samples to a frame, decodes the frames again, writes both and
// prints the time per frame. Two streams are interleaved (the same audio) to show
// that their states stay apart: their frames must be identical. Each state gets
// start before its first frame and stop after its last, as tcd does.
//
// Built with "make" here (or: g++ -O2 -std=c++17 -I.. codectest.cpp -o codectest -ldl).

#include <chrono>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <vector>

#include "CodecPlugin.h"

static std::vector<int16_t> ReadPcm(const char *path)
{
	std::vector<int16_t> pcm;
	FILE *in = fopen(path, "rb");
	if (!in) { perror(path); return pcm; }
	int16_t buf[160];
	while (fread(buf, sizeof(int16_t), 160, in) == 160)
		pcm.insert(pcm.end(), buf, buf + 160);
	fclose(in);
	return pcm;
}

// from:to, the direct path: encode as `from`, decode, transcode each decoded
// frame into `to` and decode that; the same audio also through decode and a
// `to` encoder, for the time per frame
static int Transcode(const tcd_codec_plugin *p, uint32_t from, uint32_t to, const char *in, const char *out_frames, const char *out_raw)
{
	const std::vector<int16_t> pcm = ReadPcm(in);
	const size_t frames = pcm.size() / 160, tsize = to == TCD_CODEC_P25 ? 11 : 9;
	const size_t fsize = from == TCD_CODEC_P25 ? 11 : 9;
	void *enc = p->open(from), *dec = p->open(from), *xc = p->open(to), *reenc = p->open(to), *out = p->open(to);
	if (!enc || !dec || !xc || !reenc || !out) { fprintf(stderr, "open failed\n"); return 1; }
	for (void *s : { enc, dec, xc, reenc, out })
		if (p->start) p->start(s);
	std::vector<uint8_t> f(fsize), t(tsize), r(tsize), tf(frames * tsize);
	std::vector<int16_t> mid(160), res(pcm.size());
	double ttc = 0, tre = 0;
	int errors = 0;
	for (size_t i = 0; i < frames; i++)
	{
		errors += p->encode(enc, &pcm[i * 160], f.data()) != 0;
		errors += p->decode(dec, f.data(), mid.data()) != 0;
		auto t0 = std::chrono::steady_clock::now();
		errors += p->transcode(dec, xc, 0.0f, t.data()) != 0;
		auto t1 = std::chrono::steady_clock::now();
		errors += p->encode(reenc, mid.data(), r.data()) != 0;
		auto t2 = std::chrono::steady_clock::now();
		ttc += std::chrono::duration<double, std::milli>(t1 - t0).count();
		tre += std::chrono::duration<double, std::milli>(t2 - t1).count();
		memcpy(&tf[i * tsize], t.data(), tsize);
		errors += p->decode(out, t.data(), &res[i * 160]) != 0;
	}
	for (void *s : { enc, dec, xc, reenc, out })
	{
		if (p->stop) p->stop(s);
		p->close(s);
	}
	if (p->shutdown) p->shutdown();
	FILE *ff = fopen(out_frames, "wb"); if (ff) { fwrite(tf.data(), 1, tf.size(), ff); fclose(ff); }
	FILE *fo = fopen(out_raw, "wb"); if (fo) { fwrite(res.data(), sizeof(int16_t), res.size(), fo); fclose(fo); }
	printf("%zu frames, transcode %.3f ms, encoding the decoded audio %.3f ms per frame, %d errors\n",
		frames, frames ? ttc / frames : 0, frames ? tre / frames : 0, errors);
	return errors != 0;
}

int main(int argc, char *argv[])
{
	if (argc < 6)
	{
		fprintf(stderr, "usage: %s <plugin.so> <dmr|dstar|p25 | from:to> <in.raw> <out.frames> <out.raw> [option=value ...]\n", argv[0]);
		return 1;
	}
	void *h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	if (!h) { fprintf(stderr, "%s\n", dlerror()); return 1; }
	auto func = reinterpret_cast<tcd_codec_plugin_func>(dlsym(h, "tcd_codec_plugin"));
	const tcd_codec_plugin *p = func ? func() : nullptr;
	if (!p || p->api < TCD_CODEC_API_MIN || p->api > TCD_CODEC_API) { fprintf(stderr, "no tcd codec plugin of API %d to %d\n", TCD_CODEC_API_MIN, TCD_CODEC_API); return 1; }

	auto codec_of = [](const std::string &c) -> uint32_t { return c == "dmr" ? TCD_CODEC_DMR : c == "dstar" ? TCD_CODEC_DSTAR : c == "p25" ? TCD_CODEC_P25 : 0; };
	const std::string c(argv[2]);
	const auto colon = c.find(':');
	const uint32_t codec = codec_of(c.substr(0, colon));
	const uint32_t target = colon == std::string::npos ? 0 : codec_of(c.substr(colon + 1));
	const size_t fsize = codec == TCD_CODEC_P25 ? 11 : 9;
	if (!codec || (colon != std::string::npos && (!target || target == codec))) { fprintf(stderr, "codec: dmr, dstar or p25, or two different of them as from:to\n"); return 1; }
	if (!(p->codecs & codec) || (target && !(p->codecs & target))) { fprintf(stderr, "%s cannot do %s\n", p->name, argv[2]); return 1; }
	if (target && (p->api < 4 || !p->transcode)) { fprintf(stderr, "%s has no transcode (plugin API 4)\n", p->name); return 1; }

	for (int i = 6; i < argc; i++)
	{
		std::string kv(argv[i]);
		const auto eq = kv.find('=');
		if (eq == std::string::npos || !p->option || p->option(kv.substr(0, eq).c_str(), kv.substr(eq + 1).c_str()) != 1)
		{
			fprintf(stderr, "option %s not taken\n", argv[i]);
			return 1;
		}
	}
	if (p->init()) { fprintf(stderr, "init failed\n"); return 1; }
	printf("%s: %s\n", p->name, p->description ? p->description : "");
	if (target)
		return Transcode(p, codec, target, argv[3], argv[4], argv[5]);

	const std::vector<int16_t> pcm = ReadPcm(argv[3]);
	const size_t frames = pcm.size() / 160;

	void *s1 = p->open(codec), *s2 = p->open(codec);
	if (p->start) { p->start(s1); p->start(s2); }
	std::vector<uint8_t> f1(frames * fsize), f2(frames * fsize);
	std::vector<int16_t> out(pcm.size()), out2(pcm.size());
	double tenc = 0, tdec = 0;
	int errors = 0;
	for (size_t i = 0; i < frames; i++)
	{
		auto t0 = std::chrono::steady_clock::now();
		errors += p->encode(s1, &pcm[i * 160], &f1[i * fsize]) != 0;
		auto t1 = std::chrono::steady_clock::now();
		errors += p->encode(s2, &pcm[i * 160], &f2[i * fsize]) != 0;
		tenc += std::chrono::duration<double, std::milli>(t1 - t0).count();
	}
	void *d1 = p->open(codec), *d2 = p->open(codec);
	if (p->start) { p->start(d1); p->start(d2); }
	for (size_t i = 0; i < frames; i++)
	{
		auto t0 = std::chrono::steady_clock::now();
		errors += p->decode(d1, &f1[i * fsize], &out[i * 160]) != 0;
		auto t1 = std::chrono::steady_clock::now();
		errors += p->decode(d2, &f1[i * fsize], &out2[i * 160]) != 0;
		tdec += std::chrono::duration<double, std::milli>(t1 - t0).count();
	}
	if (p->stop) { p->stop(s1); p->stop(s2); p->stop(d1); p->stop(d2); }
	p->close(s1); p->close(s2); p->close(d1); p->close(d2);
	if (p->shutdown) p->shutdown();

	FILE *ff = fopen(argv[4], "wb"); fwrite(f1.data(), 1, f1.size(), ff); fclose(ff);
	FILE *fo = fopen(argv[5], "wb"); fwrite(out.data(), sizeof(int16_t), out.size(), fo); fclose(fo);

	printf("%zu frames, encode %.2f ms, decode %.2f ms per frame (a frame is 20 ms), %d errors\n",
		frames, frames ? tenc / frames : 0, frames ? tdec / frames : 0, errors);
	printf("interleaved streams %s\n", (f1 == f2 && out == out2) ? "identical" : "DIFFER");
	return errors || f1 != f2 || out != out2;
}
