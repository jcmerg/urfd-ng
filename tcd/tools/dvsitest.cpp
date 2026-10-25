// dvsitest -- encodes or decodes a file with a DVSI DV3003, set up as tcd sets
// it up, to compare the codec plugins with the hardware
//
//   dvsitest <device node> enc <dstar|dmr> <in.raw> <out.frames>
//   dvsitest <device node> dec <dstar|dmr> <in.frames> <out.raw>
//
// Raw PCM is 8 kHz s16le, frames are 9 bytes as tcd has them. Channel 0 only,
// one frame at a time. tcd must not run (it holds the device).
//
// Built with "make" here (or: g++ -O2 -std=c++17 -I.. dvsitest.cpp ../DeviceIO_posix.cpp -o dvsitest).

#include <arpa/inet.h>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <chrono>

#include "DeviceIO.h"
#include "DVSIPacket.h"

static std::unique_ptr<IDeviceIO> dev;

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

static bool Setup(const std::string &node, bool dstar)
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
	dev->SetBaudRate(921600);
	dev->SetReadTimeout(10);

	uint8_t zeros[350] = {};
	dev->Write(zeros, sizeof(zeros));
	dev->Purge();
	std::this_thread::sleep_for(std::chrono::milliseconds(100));

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

	// channel 0 as tcd's ConfigureVocoder sets it, gains 0
	const uint8_t ecmode[] { PKT_ECMODE, 0x0, 0x0 };
	const uint8_t dcmode[] { PKT_DCMODE, 0x0, 0x0 };
	const uint8_t  rdstar[] { PKT_RATEP, 0x01U, 0x30U, 0x07U, 0x63U, 0x40U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x48U };
	const uint8_t    rdmr[] { PKT_RATEP, 0x04U, 0x31U, 0x07U, 0x54U, 0x24U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x6FU, 0x48U };
	const uint8_t  chfmt[] { PKT_CHANFMT, 0x0, 0x0 };
	const uint8_t  spfmt[] { PKT_SPCHFMT, 0x0, 0x0 };
	const uint8_t   gain[] { PKT_GAIN, 0, 0 };
	const uint8_t   init[] { PKT_INIT, 0x3U };
	c.header.payload_length = htons(1 + sizeof(SDV_Packet::payload.codec));
	c.field_id = PKT_CHANNEL0;
	memcpy(c.payload.codec.ecmode, ecmode, 3);
	memcpy(c.payload.codec.dcmode, dcmode, 3);
	memcpy(c.payload.codec.ratep, dstar ? rdstar : rdmr, 13);
	memcpy(c.payload.codec.chfmt, chfmt, 3);
	memcpy(c.payload.codec.spfmt, spfmt, 3);
	memcpy(c.payload.codec.gain, gain, 3);
	memcpy(c.payload.codec.init, init, 2);
	if (!Put(c) || !Get(r) || r.field_id != PKT_CHANNEL0) { fprintf(stderr, "channel config failed\n"); return false; }

	// linear PCM, as tcd forces it on a DV3003
	c.header.payload_length = htons(3);
	c.payload.ctrl.data.paritymode[0] = PKT_COMPAND;
	c.payload.ctrl.data.paritymode[1] = 0x0;
	if (dev->Write(&c, 7) != 7 || !Get(r)) { fprintf(stderr, "compand off failed\n"); return false; }
	fprintf(stderr, "%s channel 0: %s\n", node.c_str(), dstar ? "D-Star" : "DMR");
	return true;
}

int main(int argc, char *argv[])
{
	if (argc != 6) { fprintf(stderr, "usage: %s <device> enc|dec dstar|dmr <in> <out>\n", argv[0]); return 1; }
	const bool enc = !strcmp(argv[2], "enc"), dstar = !strcmp(argv[3], "dstar");
	if (!Setup(argv[1], dstar)) return 1;
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
			p.header.payload_length = htons(1 + sizeof(p.payload.audio));
			p.header.packet_type = PKT_SPEECH;
			p.payload.audio.speechd = PKT_SPEECHD;
			p.payload.audio.num_samples = 160;
			for (int i = 0; i < 160; i++) p.payload.audio.samples[i] = htons(pcm[i]);
			if (!Put(p) || !Get(r) || r.header.packet_type != PKT_CHANNEL) { fprintf(stderr, "frame %d: no AMBE back\n", frames); return 1; }
			fwrite(r.payload.ambe.data, 1, 9, out);
		}
		else
		{
			uint8_t f[9];
			if (fread(f, 1, 9, in) != 9) break;
			p.header.payload_length = htons(1 + sizeof(p.payload.ambe));
			p.header.packet_type = PKT_CHANNEL;
			p.payload.ambe.chand = PKT_CHAND;
			p.payload.ambe.num_bits = 72;
			memcpy(p.payload.ambe.data, f, 9);
			if (!Put(p) || !Get(r) || r.header.packet_type != PKT_SPEECH) { fprintf(stderr, "frame %d: no speech back\n", frames); return 1; }
			int16_t pcm[160];
			for (int i = 0; i < 160; i++) pcm[i] = ntohs(r.payload.audio.samples[i]);
			fwrite(pcm, sizeof(int16_t), 160, out);
		}
		frames++;
	}
	const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	fprintf(stderr, "%d frames, %.2f ms per frame round trip\n", frames, frames ? ms / frames : 0);
	dev->Close();
	return 0;
}
