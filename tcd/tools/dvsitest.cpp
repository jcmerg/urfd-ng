// dvsitest -- encodes or decodes a file with a DVSI DV3003 or DV3000, set up as tcd
// sets it up, to compare the codec plugins with the hardware
//
//   dvsitest <device node> enc <dstar|dmr> <in.raw> <out.frames>
//   dvsitest <device node> dec <dstar|dmr> <in.frames> <out.raw>
//   dvsitest <device node> info
//   dvsitest <device node> batch <jobs>
//
// Raw PCM is 8 kHz s16le, frames are 9 bytes as tcd has them. enc/dec use channel 0,
// one frame at a time. batch (DV3003 only) reads one job per line from <jobs>
// ("enc|dec dstar|dmr <in> <out>", "-" for stdin) and runs three of them at a time,
// one per channel, each channel with one frame in flight; a channel is set up again
// for each job it takes, so every file starts from a reset vocoder as with enc/dec. tcd must not run (it holds the device). Every mode prints
// the product ID and version of the chip, "info" only that.
//
// A DV3003 (921600 baud) by default; DVSI_TYPE=3000 in the environment for a single
// channel DV3000/ThumbDV (460800 baud, its packet format without the channel field,
// as tcd's CDV3000 has it). Use the rate of the device: bytes at a wrong rate can
// leave an AMBE3003 deaf until it is unplugged and plugged in again.
//
// Built with "make" here (or: g++ -O2 -std=c++17 -I.. dvsitest.cpp ../DeviceIO_posix.cpp -o dvsitest).

#include <arpa/inet.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <chrono>

#include "DeviceIO.h"
#include "DVSIPacket.h"

static std::unique_ptr<IDeviceIO> dev;
static bool dv3000 = false;

static bool Get(SDV_Packet &p)
{
	for (int i = 0; ; i++)
	{
		if (dev->Read(&p.start_byte, 1) != 1 || i > 2000)
			return false;
		if (p.start_byte == PKT_HEADER)
			break;
	}
	uint8_t *d = reinterpret_cast<uint8_t *>(&p.header);
	for (size_t left = sizeof(p.header); left; )
	{
		ssize_t n = dev->Read(d, left);
		if (n <= 0) return false;
		left -= n; d += n;
	}
	size_t left = ntohs(p.header.payload_length);
	if (left > 1 + sizeof(p.payload)) return false;
	d = &p.field_id;
	while (left)
	{
		ssize_t n = dev->Read(d, left);
		if (n <= 0) return false;
		left -= n; d += n;
	}
	return true;
}

static bool Put(const SDV_Packet &p)
{
	const ssize_t n = packet_size(p);
	return dev->Write(&p, n) == n;
}

static bool SetupDevice(const std::string &node);
static bool SetupChannel(int ch, bool dstar);

static bool Setup(const std::string &node, bool dstar)
{
	if (!SetupDevice(node)) return false;
	if (!SetupChannel(0, dstar)) return false;
	fprintf(stderr, "%s channel 0: %s\n", node.c_str(), dstar ? "D-Star" : "DMR");
	return true;
}

static bool SetupDevice(const std::string &node)
{
	dev.reset(CreatePosixDevice());
	std::string desc;
	if (!dev->Open(node, desc)) { fprintf(stderr, "cannot open %s\n", node.c_str()); return false; }
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	dev->Purge();
	dev->SetDataCharacteristics(8, 1, 0);
	dev->SetFlowControl(1);
	dev->SetRTS(true);
	dev->SetDTR(false);
	if (!dev->SetBaudRate(dv3000 ? 460800 : 921600)) { fprintf(stderr, "cannot set the baud rate\n"); return false; }
	dev->SetReadTimeout(10);

	if (!dv3000)
	{
		uint8_t zeros[350] = {};
		dev->Write(zeros, sizeof(zeros));
		dev->Drain(100);	// the zeros out first, then the input dropped
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		dev->PurgeInput();
	}

	SDV_Packet c, r;
	c.start_byte = PKT_HEADER;
	c.header.payload_length = htons(3);
	c.header.packet_type = PKT_CONTROL;
	c.field_id = PKT_RESET;
	c.payload.ctrl.data.paritymode[0] = PKT_PARITYBYTE;
	c.payload.ctrl.data.paritymode[1] = 0x3U ^ PKT_RESET ^ PKT_PARITYBYTE;
	if (dev->Write(&c, 7) != 7 || !Get(r) || r.field_id != PKT_READY) { fprintf(stderr, "reset failed\n"); return false; }

	c.header.payload_length = htons(4);
	c.field_id = PKT_PARITYMODE;
	c.payload.ctrl.data.paritymode[0] = 0;
	c.payload.ctrl.data.paritymode[1] = PKT_PARITYBYTE;
	c.payload.ctrl.data.paritymode[2] = 0x4U ^ PKT_PARITYMODE ^ PKT_PARITYBYTE;
	if (dev->Write(&c, 8) != 8 || !Get(r) || r.field_id != PKT_PARITYMODE) { fprintf(stderr, "parity off failed\n"); return false; }

	// product ID and version, as tcd's InitDevice asks for them
	c.header.payload_length = htons(1);
	for (uint8_t f : { uint8_t(PKT_PRODID), uint8_t(PKT_VERSTRING) })
	{
		c.field_id = f;
		if (dev->Write(&c, 5) != 5 || !Get(r) || r.field_id != f) { fprintf(stderr, "product ID/version query failed\n"); return false; }
		fprintf(stderr, "%s %s: %s\n", node.c_str(), f == PKT_PRODID ? "product ID" : "version", r.payload.ctrl.data.version);
	}
	return true;
}

// channel ch as tcd's ConfigureVocoder sets it, gains 0
static bool SetupChannel(int ch, bool dstar)
{
	SDV_Packet c, r;
	c.start_byte = PKT_HEADER;
	c.header.packet_type = PKT_CONTROL;

	const uint8_t ecmode[] { PKT_ECMODE, 0x0, 0x0 };
	const uint8_t dcmode[] { PKT_DCMODE, 0x0, 0x0 };
	const uint8_t  rdstar[] { PKT_RATEP, 0x01U, 0x30U, 0x07U, 0x63U, 0x40U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x48U };
	const uint8_t    rdmr[] { PKT_RATEP, 0x04U, 0x31U, 0x07U, 0x54U, 0x24U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x6FU, 0x48U };
	const uint8_t  chfmt[] { PKT_CHANFMT, 0x0, 0x0 };
	const uint8_t  spfmt[] { PKT_SPCHFMT, 0x0, 0x0 };
	const uint8_t   gain[] { PKT_GAIN, 0, 0 };
	const uint8_t   init[] { PKT_INIT, 0x3U };
	c.header.payload_length = htons(1 + sizeof(SDV_Packet::payload.codec));
	c.field_id = PKT_CHANNEL0 + ch;	// tcd sends it to a DV3000 too
	memcpy(c.payload.codec.ecmode, ecmode, 3);
	memcpy(c.payload.codec.dcmode, dcmode, 3);
	memcpy(c.payload.codec.ratep, dstar ? rdstar : rdmr, 13);
	memcpy(c.payload.codec.chfmt, chfmt, 3);
	memcpy(c.payload.codec.spfmt, spfmt, 3);
	memcpy(c.payload.codec.gain, gain, 3);
	memcpy(c.payload.codec.init, init, 2);
	if (!Put(c) || !Get(r) || r.field_id != PKT_CHANNEL0 + ch) { fprintf(stderr, "channel %d config failed\n", ch); return false; }

	// linear PCM, as tcd forces it on a DV3003
	if (!dv3000)
	{
		c.header.payload_length = htons(3);
		c.payload.ctrl.data.paritymode[0] = PKT_COMPAND;
		c.payload.ctrl.data.paritymode[1] = 0x0;
		if (dev->Write(&c, 7) != 7 || !Get(r)) { fprintf(stderr, "compand off failed\n"); return false; }
	}
	return true;
}

// batch: up to three jobs at a time, one per DV3003 channel, in lock step: every
// busy channel gets its next frame, then all answers are read and routed by channel.
// Channels are only set up between rounds, when nothing is in flight.
struct SJob { bool enc, dstar; std::string in, out; };
struct SChan { FILE *in = nullptr, *out = nullptr; bool enc = false; int frames = 0; std::string name; };

static int Batch(const std::string &node, const char *list)
{
	FILE *jf = strcmp(list, "-") ? fopen(list, "r") : stdin;
	if (!jf) { perror(list); return 1; }
	std::vector<SJob> jobs;
	char line[4096], m[8], c[8], in[2048], out[2048];
	while (fgets(line, sizeof(line), jf))
	{
		if (sscanf(line, "%7s %7s %2047s %2047s", m, c, in, out) != 4) continue;
		jobs.push_back({ !strcmp(m, "enc"), !strcmp(c, "dstar"), in, out });
	}
	if (jf != stdin) fclose(jf);
	if (!SetupDevice(node)) return 1;
	SChan ch[3];
	size_t next = 0;
	int failed = 0, done = 0, total = 0;
	auto t0 = std::chrono::steady_clock::now();
	for (;;)
	{
		for (int k = 0; k < 3; k++)
			while (!ch[k].in && next < jobs.size())
			{
				const SJob &j = jobs[next++];
				if (!SetupChannel(k, j.dstar)) return 1;
				ch[k].in = fopen(j.in.c_str(), "rb");
				ch[k].out = ch[k].in ? fopen(j.out.c_str(), "wb") : nullptr;
				if (!ch[k].in || !ch[k].out) { perror(j.in.c_str()); if (ch[k].in) fclose(ch[k].in); ch[k].in = nullptr; failed++; continue; }
				ch[k].enc = j.enc; ch[k].frames = 0; ch[k].name = j.out;
			}
		int sent = 0;
		for (int k = 0; k < 3; k++)
		{
			if (!ch[k].in) continue;
			SDV_Packet p;
			p.start_byte = PKT_HEADER;
			p.field_id = PKT_CHANNEL0 + k;
			if (ch[k].enc)
			{
				int16_t pcm[160];
				if (fread(pcm, sizeof(int16_t), 160, ch[k].in) == 160)
				{
					p.header.packet_type = PKT_SPEECH;
					p.header.payload_length = htons(1 + sizeof(p.payload.audio));
					p.payload.audio.speechd = PKT_SPEECHD;
					p.payload.audio.num_samples = 160;
					for (int i = 0; i < 160; i++) p.payload.audio.samples[i] = htons(pcm[i]);
					if (!Put(p)) { fprintf(stderr, "write failed\n"); return 1; }
					sent++;
					continue;
				}
			}
			else
			{
				uint8_t f[9];
				if (fread(f, 1, 9, ch[k].in) == 9)
				{
					p.header.packet_type = PKT_CHANNEL;
					p.header.payload_length = htons(1 + sizeof(p.payload.ambe));
					p.payload.ambe.chand = PKT_CHAND;
					p.payload.ambe.num_bits = 72;
					memcpy(p.payload.ambe.data, f, 9);
					if (!Put(p)) { fprintf(stderr, "write failed\n"); return 1; }
					sent++;
					continue;
				}
			}
			fclose(ch[k].in); fclose(ch[k].out); ch[k].in = nullptr;
			fprintf(stderr, "ch%d: %d frames -> %s\n", k, ch[k].frames, ch[k].name.c_str());
			done++;
		}
		if (!sent)
		{
			if (next >= jobs.size()) break;
			continue;
		}
		for (int n = 0; n < sent; n++)
		{
			SDV_Packet r;
			if (!Get(r)) { fprintf(stderr, "no answer (%d of %d)\n", n, sent); return 1; }
			const int k = r.field_id - PKT_CHANNEL0;
			if (k < 0 || k > 2 || !ch[k].in) { fprintf(stderr, "answer for channel field 0x%02x\n", r.field_id); return 1; }
			if (ch[k].enc)
			{
				if (r.header.packet_type != PKT_CHANNEL) { fprintf(stderr, "ch%d: no AMBE back\n", k); return 1; }
				fwrite(r.payload.ambe.data, 1, 9, ch[k].out);
			}
			else
			{
				if (r.header.packet_type != PKT_SPEECH) { fprintf(stderr, "ch%d: no speech back\n", k); return 1; }
				int16_t pcm[160];
				for (int i = 0; i < 160; i++) pcm[i] = ntohs(r.payload.audio.samples[i]);
				fwrite(pcm, sizeof(int16_t), 160, ch[k].out);
			}
			ch[k].frames++;
			total++;
		}
	}
	const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	fprintf(stderr, "%d jobs, %d failed, %d frames, %.2f ms per frame\n", done, failed, total, total ? ms / total : 0);
	dev->Close();
	return failed ? 1 : 0;
}

int main(int argc, char *argv[])
{
	const bool info = argc == 3 && !strcmp(argv[2], "info");
	const bool batch = argc == 4 && !strcmp(argv[2], "batch");
	if (argc != 6 && !info && !batch) { fprintf(stderr, "usage: %s <device> enc|dec dstar|dmr <in> <out>\n       %s <device> batch <jobs|->   (DV3003, three channels)\n       %s <device> info\n       (DVSI_TYPE=3000 for a DV3000)\n", argv[0], argv[0], argv[0]); return 1; }
	const char *t = getenv("DVSI_TYPE");
	dv3000 = t && !strcmp(t, "3000");
	if (batch)
	{
		if (dv3000) { fprintf(stderr, "batch needs a DV3003\n"); return 1; }
		return Batch(argv[1], argv[3]);
	}
	const bool enc = !strcmp(argv[2], "enc"), dstar = info || !strcmp(argv[3], "dstar");
	if (!Setup(argv[1], dstar)) return 1;
	if (info) { dev->Close(); return 0; }
	FILE *in = fopen(argv[4], "rb"), *out = fopen(argv[5], "wb");
	if (!in || !out) { perror("open"); return 1; }
	int frames = 0;
	auto t0 = std::chrono::steady_clock::now();
	for (;;)
	{
		SDV_Packet p, r;
		p.start_byte = PKT_HEADER;
		p.field_id = PKT_CHANNEL0;
		if (enc)
		{
			int16_t pcm[160];
			if (fread(pcm, sizeof(int16_t), 160, in) != 160) break;
			p.header.packet_type = PKT_SPEECH;
			if (dv3000)
			{
				p.header.payload_length = htons(1 + sizeof(p.payload.audio3k));
				p.field_id = PKT_SPEECHD;
				p.payload.audio3k.num_samples = 160;
				for (int i = 0; i < 160; i++) p.payload.audio3k.samples[i] = htons(pcm[i]);
			}
			else
			{
				p.header.payload_length = htons(1 + sizeof(p.payload.audio));
				p.payload.audio.speechd = PKT_SPEECHD;
				p.payload.audio.num_samples = 160;
				for (int i = 0; i < 160; i++) p.payload.audio.samples[i] = htons(pcm[i]);
			}
			if (!Put(p) || !Get(r) || r.header.packet_type != PKT_CHANNEL) { fprintf(stderr, "frame %d: no AMBE back\n", frames); return 1; }
			fwrite(dv3000 ? r.payload.ambe3k.data : r.payload.ambe.data, 1, 9, out);
		}
		else
		{
			uint8_t f[9];
			if (fread(f, 1, 9, in) != 9) break;
			p.header.packet_type = PKT_CHANNEL;
			if (dv3000)
			{
				p.header.payload_length = htons(1 + sizeof(p.payload.ambe3k));
				p.field_id = PKT_CHAND;
				p.payload.ambe3k.num_bits = 72;
				memcpy(p.payload.ambe3k.data, f, 9);
			}
			else
			{
				p.header.payload_length = htons(1 + sizeof(p.payload.ambe));
				p.payload.ambe.chand = PKT_CHAND;
				p.payload.ambe.num_bits = 72;
				memcpy(p.payload.ambe.data, f, 9);
			}
			if (!Put(p) || !Get(r) || r.header.packet_type != PKT_SPEECH) { fprintf(stderr, "frame %d: no speech back\n", frames); return 1; }
			int16_t pcm[160];
			for (int i = 0; i < 160; i++) pcm[i] = ntohs(dv3000 ? r.payload.audio3k.samples[i] : r.payload.audio.samples[i]);
			fwrite(pcm, sizeof(int16_t), 160, out);
		}
		frames++;
	}
	const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	fprintf(stderr, "%d frames, %.2f ms per frame round trip\n", frames, frames ? ms / frames : 0);
	dev->Close();
	return 0;
}
