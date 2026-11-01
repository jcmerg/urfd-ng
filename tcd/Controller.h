#pragma once

// tcd - a hybid transcoder using DVSI hardware and Codec2 software
// Copyright © 2021 Thomas A. Early N7TAE
// Copyright © 2021 Doug McLain AD8DP

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include <unordered_map>
#include <chrono>
#include <vector>
#include <memory>
#include <atomic>
#include <future>
#include <mutex>
#include <list>
#include <utility>
#include <imbe_vocoder_api.h>

#include "codec2.h"
#include "DV3000.h"
#include "DV3003.h"
#include "TCSocket.h"
#include "AGC.h"
#include "NoiseSuppress.h"
#include "AmbeGain.h"
#include "CodecPlugin.h"

class CController
{
public:
	std::mutex dstar_mux, dmrst_mux;

	CController();
	bool Start();
	void Stop();
	void ListDevices();
	void RouteDstPacket(std::shared_ptr<CTranscoderPacket> packet);
	void RouteDmrPacket(std::shared_ptr<CTranscoderPacket> packet);
	void ReconfigureAGC();  // re-read AGC params from g_Stats.config
	void Dump(const std::shared_ptr<CTranscoderPacket> packet, const std::string &title) const;

protected:
	std::atomic<bool> keep_running;
	std::future<void> reflectorFuture, c2Future, imbeFuture, usrpFuture;
	std::unordered_map<char, int16_t[160]> audio_store;
	std::unordered_map<char, uint8_t[8]> data_store;
	CTCClient tcClient;
	std::unordered_map<char, std::unique_ptr<CCodec2>> c2_16, c2_32;
	// the stream and frame tcd's own Codec2 and IMBE encoder and decoder last did
	// per module: a new stream id or a lower count is a new transmission, which
	// starts the vocoder afresh (Begin: true when it did)
	struct SStreamPos { bool known; uint16_t streamid; uint32_t sequence; };
	std::unordered_map<char, SStreamPos> c2_16_stream, c2_32_stream, imbe_enc_stream, imbe_dec_stream;
	static bool Begin(std::unordered_map<char, SStreamPos> &pos, const std::shared_ptr<CTranscoderPacket> &packet);
	// the module's Codec2 of the mode the packet needs, afresh for a new transmission
	CCodec2 &Codec2For(const std::shared_ptr<CTranscoderPacket> &packet, bool is1600);
	std::unique_ptr<CDVDevice> dstar_device, dmrsf_device;
	bool mixed_mode = false;
	char mixed_dstar_module = ' ';
	CDV3003 *mixed_dv3003 = nullptr;  // cached pointer, owned by dmrsf_device

	CPacketQueue codec2_queue;

	CPacketQueue imbe_queue;
	CPacketQueue usrp_queue;
	std::mutex send_mux;
	std::mutex p25vocoder_mux;
	// what does D-Star, DMR, P25 and M17: nullptr = the device (P25: tcd's own OP25
	// vocoder, M17: tcd's own Codec2), else a codec plugin's codec (CodecPlugin.h), as
	// g_Stats.dstar_options/dmr_options/p25_options/m17_options name them; a stream
	// keeps the one it started with (Pick), config.codec_dstar/codec_dmr/codec_p25/
	// codec_m17 is for new streams. m17_1600_choices runs alongside m17_choices: the
	// same plugin's M17 1600 codec, nullptr where tcd's own Codec2 decodes 1600
	std::vector<class CSoftCodec *> dstar_choices, dmr_choices, p25_choices, m17_choices, m17_1600_choices;
	struct SPinned { int choice; std::chrono::steady_clock::time_point last; };
	std::mutex pinned_mux;
	std::unordered_map<uint16_t, SPinned> dstar_pinned, dmr_pinned, p25_pinned, m17_pinned;
	// the modules the devices do D-Star and DMR for; the other modules go to a plugin
	std::string dstar_hw_modules, dmr_hw_modules;
	std::vector<class CSoftCodec *> &Choices(uint32_t codec)
	{
		return codec == TCD_CODEC_DSTAR ? dstar_choices : codec == TCD_CODEC_DMR ? dmr_choices : codec == TCD_CODEC_P25 ? p25_choices
		     : codec == TCD_CODEC_M17 ? m17_choices : m17_1600_choices;
	}
	// codec: TCD_CODEC_DSTAR, _DMR, _P25, _M17 or _M17_1600 (the two M17 share the choice)
	int Pick(uint32_t codec, uint16_t streamid, char module);
	class CSoftCodec *SwCodec(uint32_t codec, const std::shared_ptr<CTranscoderPacket> &packet)
	{
		auto &c = Choices(codec);
		return c.empty() ? nullptr : c[Pick(codec, packet->GetStreamId(), packet->GetModule())];
	}
	// P25 with the stream's choice: OP25 or a plugin. A plugin's error gives IMBE
	// silence (encode) or silent samples (decode)
	void EncodeIMBE(const std::shared_ptr<CTranscoderPacket> &packet, const int16_t *gained, uint8_t *imbe);
	void DecodeIMBE(const std::shared_ptr<CTranscoderPacket> &packet, int16_t *pcm);
	// M17 with the stream's choice: tcd's own Codec2 or a plugin. Encode: 3200,
	// 160 samples into 8 bytes; decode: 8 bytes into 160 samples (3200) or 320
	// (1600). A plugin's error gives Codec2 silence or silent samples
	void EncodeM17(const std::shared_ptr<CTranscoderPacket> &packet, const int16_t *gained, uint8_t *bits);
	void DecodeM17(const std::shared_ptr<CTranscoderPacket> &packet, const uint8_t *bits, bool is1600, int16_t *pcm);
	void PurgePinned(void);
	// the DMR plugin for DmrGainMode = reencode, nullptr without one
	class CSoftCodec *dmr_sw = nullptr;
	// the monitor's entries of tcd's own vocoders, -1 without one
	int op25_stats = -1, codec2_stats = -1;
	int32_t dstar_in_num = 256, dstar_out_num = 256;	// DStarGainIn/Out for a D-Star plugin
	int32_t ambe_in_num, ambe_out_num, dmr_reencode_num;
	int32_t outgain_dstar_num, outgain_dmr_num, outgain_usrp_num, outgain_imbe_num, outgain_m17_num;
	// P25: IMBE quantises each frame against the one before, so every module has its
	// own encoder and decoder (streams of two modules must not share one state)
	std::unordered_map<char, std::unique_ptr<imbe_vocoder>> imbe_enc, imbe_dec;
	// P25 to M17: a 300 Hz high-pass in front of the Codec2 encoder, per module
	struct SHighPass { float z[2][2]; uint16_t streamid; };
	std::unordered_map<char, SHighPass> c2_highpass;
	void HighPass(char module, uint16_t streamid, int16_t *samples);
	CAGC m_agcDigital;  // D-Star, DMR/YSF/NXDN, M17, P25
	CAGC m_agcAnalog;   // SVX and USRP: FM audio, gate above its noise floor
	CNoiseSuppress m_nsAnalog;  // SVX and USRP: FM noise removed before the AGC
	CAmbeGain m_ambeGain;  // DmrGainMode = ambe: AGC and gains in the AMBE2+ bitstream

	int32_t calcNumerator(int32_t db) const;
	static void ApplyGain(int16_t *samples, int count, int32_t num);
	int BitstreamDmrDb(void) const;
	int32_t PcmDmrGainNum(void) const;
	void BitstreamDmrGain(uint8_t *ambe2, uint16_t streamId);
	bool DiscoverFtdiDevices(std::list<std::pair<std::string, std::string>> &found);
	bool IsAmbeDevice(const std::string &desc);
	bool InitVocoders();
	// processing threads
	void ReadReflectorThread();
	void ProcessC2Thread();

	void ProcessIMBEThread();
	void ProcessUSRPThread();
	void Codec2toAudio(std::shared_ptr<CTranscoderPacket> packet);
	void AudiotoCodec2(std::shared_ptr<CTranscoderPacket> packet);
	void IMBEtoAudio(std::shared_ptr<CTranscoderPacket> packet);
	void AudiotoIMBE(std::shared_ptr<CTranscoderPacket> packet);
	void USRPtoAudio(std::shared_ptr<CTranscoderPacket> packet);
	void AudiotoUSRP(std::shared_ptr<CTranscoderPacket> packet);
	void SvxToAudio(std::shared_ptr<CTranscoderPacket> packet);
	void SendToReflector(std::shared_ptr<CTranscoderPacket> packet);
	std::future<void> swambe2Future, swdstarFuture;
	CPacketQueue swambe2_queue, swdstar_queue;
	void ProcessSWAMBE2Thread();
	void SWAMBE2toAudio(std::shared_ptr<CTranscoderPacket> packet);
	void AudiotoSWAMBE2(std::shared_ptr<CTranscoderPacket> packet);
	void ProcessSWDStarThread();
	void SWDStarToAudio(std::shared_ptr<CTranscoderPacket> packet);
	void AudioToSWDStar(std::shared_ptr<CTranscoderPacket> packet);
	// a packet to the D-Star or the DMR codec: a DVSI device or a codec plugin
	void ToDStar(std::shared_ptr<CTranscoderPacket> packet);
	void ToDmr(std::shared_ptr<CTranscoderPacket> packet);
};
