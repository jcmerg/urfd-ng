// BuiltinCodecs -- tcd's own vocoders as codec plugins, see BuiltinCodecs.h
//
// Copyright (C) 2026
// Licensed under the GNU General Public License v3 or later

#include <cstring>
#include <new>
#include <imbe_vocoder_api.h>

#include "codec2.h"
#include "BuiltinCodecs.h"

// both keep their state in an object made afresh at the start of a
// transmission: IMBE quantises each frame against the one before, Codec2
// carries its filters and the last frame's parameters. open makes none (a new
// state gets a start before its first frame), stop frees it after the last
// frame (a state stays up to 30 s after its stream); without one, after the
// allocation failed, a frame is an error and tcd gives silence

// OP25: P25 IMBE 4400, 11 bytes
struct SOp25State { imbe_vocoder *enc_dec; };

static int Op25Init(void) { return 0; }

static void *Op25Open(uint32_t codec)
{
	if (codec != TCD_CODEC_P25)
		return nullptr;
	return new (std::nothrow) SOp25State{ nullptr };
}

static void Op25Close(void *state)
{
	auto *s = static_cast<SOp25State *>(state);
	delete s->enc_dec;
	delete s;
}

static void Op25Start(void *state)
{
	auto *s = static_cast<SOp25State *>(state);
	delete s->enc_dec;
	s->enc_dec = nullptr;
	try { s->enc_dec = new imbe_vocoder(); } catch (const std::bad_alloc &) {}	// it allocates inside too
}

static void Op25Stop(void *state)
{
	auto *s = static_cast<SOp25State *>(state);
	delete s->enc_dec;
	s->enc_dec = nullptr;
}

static int Op25Encode(void *state, const int16_t *pcm, uint8_t *frame)
{
	auto *s = static_cast<SOp25State *>(state);
	if (!s->enc_dec)
		return -1;
	int16_t buf[160];	// encode_4400 takes a non-const buffer
	memcpy(buf, pcm, sizeof(buf));
	s->enc_dec->encode_4400(buf, frame);
	return 0;
}

static int Op25Decode(void *state, const uint8_t *frame, int16_t *pcm)
{
	auto *s = static_cast<SOp25State *>(state);
	if (!s->enc_dec)
		return -1;
	uint8_t buf[11];	// decode_4400 takes a non-const frame
	memcpy(buf, frame, sizeof(buf));
	s->enc_dec->decode_4400(pcm, buf);
	return 0;
}

static int Op25FrameFormat(uint32_t codec, uint32_t *bytes, uint32_t *samples)
{
	if (codec != TCD_CODEC_P25)
		return -1;
	*bytes = 11;
	*samples = 160;
	return 0;
}

const tcd_codec_plugin g_BuiltinOp25 = {
	TCD_CODEC_API, "op25", "the OP25 IMBE vocoder, built in", TCD_CODEC_P25,
	nullptr, Op25Init, nullptr, Op25Open, Op25Close, Op25Encode, Op25Decode,
	Op25Start, Op25Stop, nullptr, Op25FrameFormat, nullptr
};

// Codec2: M17 3200 (8 bytes, 160 samples) and 1600 (8 bytes, 320 samples)
struct SCodec2State { CCodec2 *c2; bool is3200; };

static int Codec2Init(void) { return 0; }

static void *Codec2Open(uint32_t codec)
{
	if (codec != TCD_CODEC_M17 && codec != TCD_CODEC_M17_1600)
		return nullptr;
	return new (std::nothrow) SCodec2State{ nullptr, codec == TCD_CODEC_M17 };
}

static void Codec2Close(void *state)
{
	auto *s = static_cast<SCodec2State *>(state);
	delete s->c2;
	delete s;
}

static void Codec2Start(void *state)
{
	auto *s = static_cast<SCodec2State *>(state);
	delete s->c2;
	s->c2 = nullptr;
	try { s->c2 = new CCodec2(s->is3200); } catch (const std::bad_alloc &) {}
}

static void Codec2Stop(void *state)
{
	auto *s = static_cast<SCodec2State *>(state);
	delete s->c2;
	s->c2 = nullptr;
}

static int Codec2Encode(void *state, const int16_t *pcm, uint8_t *frame)
{
	auto *s = static_cast<SCodec2State *>(state);
	if (!s->c2)
		return -1;
	s->c2->codec2_encode(frame, pcm);
	return 0;
}

static int Codec2Decode(void *state, const uint8_t *frame, int16_t *pcm)
{
	auto *s = static_cast<SCodec2State *>(state);
	if (!s->c2)
		return -1;
	s->c2->codec2_decode(pcm, frame);
	return 0;
}

static int Codec2FrameFormat(uint32_t codec, uint32_t *bytes, uint32_t *samples)
{
	if (codec != TCD_CODEC_M17 && codec != TCD_CODEC_M17_1600)
		return -1;
	*bytes = 8;
	*samples = codec == TCD_CODEC_M17 ? 160 : 320;
	return 0;
}

const tcd_codec_plugin g_BuiltinCodec2 = {
	TCD_CODEC_API, "codec2", "Codec2 3200 and 1600, built in", TCD_CODEC_M17 | TCD_CODEC_M17_1600,
	nullptr, Codec2Init, nullptr, Codec2Open, Codec2Close, Codec2Encode, Codec2Decode,
	Codec2Start, Codec2Stop, nullptr, Codec2FrameFormat, nullptr
};
