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

#include <cassert>
#include <unordered_map>
#include <chrono>
#include <vector>
#include <memory>
#include <atomic>
#include <future>
#include <mutex>
#include <list>
#include <utility>

#include "DV3000.h"
#include "DV3003.h"
#include "TCSocket.h"
#include "AGC.h"
#include "NoiseSuppress.h"
#include "AmbeGain.h"
#include "CodecPlugin.h"
#include "CodecChoice.h"
#include "TcdStats.h"

class CController
{
public:
	std::mutex dstar_mux, dmrst_mux;

	CController();
	bool Start();
	void Stop();
	void ListDevices();
	// from: the plugin codec that decoded a D-Star packet, for Transcode (nullptr: a device did)
	// decoded: from's decode succeeded (a failed one gets no transcode, only its End)
	void RouteDstPacket(std::shared_ptr<CTranscoderPacket> packet, class CSoftCodec *from = nullptr, bool decoded = true);
	void RouteDmrPacket(std::shared_ptr<CTranscoderPacket> packet);
	void ReconfigureAGC();  // re-read AGC params from g_Stats.config
	void Dump(const std::shared_ptr<CTranscoderPacket> packet, const std::string &title) const;

protected:
	std::atomic<bool> keep_running;
	std::future<void> reflectorFuture, c2Future, imbeFuture, usrpFuture;
	std::unordered_map<char, int16_t[160]> audio_store;
	std::unordered_map<char, uint8_t[8]> data_store;
	CTCClient tcClient;
	// an AMBE device in use: how to open it again, its channels' codecs, and
	// whether it answers. A lost device is left to the codec plugins and opened
	// again when it is back (DeviceWatchThread)
	struct SDeviceSlot
	{
		std::string node, desc;		// device node, /dev/serial/by-id name
		Edvtype type;
		unsigned long baud;
		Encoding enc;			// the device's codec
		Encoding ch_enc[3];		// per channel, a DV3003 in mixed mode has both
		bool per_channel;		// a DV3003 with ch_enc set
		int8_t in_gain, out_gain;	// for enc, the other codec's come from tcd.ini
		const char *role;		// for the monitor: dstar, dmr, mixed
		std::shared_ptr<CDVDevice> dev;
		std::atomic<bool> up{false};
		std::atomic<bool> draining{false};	// phased out from the monitor: no new streams
		std::atomic<bool> out{false};		// phased out and closed, not opened again
		bool closed = false;		// the lost device's threads are joined
		int stats = -1;			// g_Stats.devices
		std::chrono::steady_clock::time_point next_try;
		bool told = false;		// the watch said why it cannot open it yet
		int compand = 0;		// the DeviceCompand it was opened with
		std::atomic<bool> recompand{false};	// to be opened again for another DeviceCompand
	};
	std::vector<std::unique_ptr<SDeviceSlot>> slots;
	// the AMBE channels for D-Star and for DMR (indexed by kChoiceDStar and
	// kChoiceDMR): fixed, channel i belongs to the i-th module of Modules; with
	// DeviceChannels = pool a stream takes a free one, whatever its module
	struct SHwChannel
	{
		int slot;
		uint8_t ch;
		bool busy = false;		// a stream has it
		char module = ' ';
		uint16_t streamid = 0;
		std::chrono::steady_clock::time_point last;
	};
	std::vector<SHwChannel> hw_channels[kNumChoices];
	// DeviceChannels for the streams that start from now on (the monitor switches it)
	static bool Pool(void) { return g_Stats.config.device_pool.load(std::memory_order_relaxed) != 0; }
	std::future<void> watchFuture;
	// the device nodes with "mixed" after their DeviceSerial
	std::vector<std::string> mixed_nodes;
	// opens the device of slots[i] on node and sets its channels up, nullptr when it fails
	std::shared_ptr<CDVDevice> OpenSlot(int i, const std::string &node);
	void AddSlot(const std::string &node, const std::string &desc, Edvtype type, unsigned long baud, Encoding enc, const char *role, const Encoding *ch_enc = nullptr);
	void AddChannels(int k, int slot, std::initializer_list<uint8_t> channels);
	// dev: the lost device, ignored when it is no longer the slot's
	void DeviceLost(int slot, const CDVDevice *dev);
	void DeviceWatchThread();
	void PhaseOut(int slot);
	// its channels in use, or quiet for less than a second, or frames still on the device
	bool SlotBusy(int slot);
	bool SlotBusyLocked(int slot);	// with pinned_mux
	int UncoveredChoice(int slot) const;
	// the channel of codec k for a stream, -1 for none (under pinned_mux)
	int TakeChannel(int k, char module, uint16_t streamid);
	bool HasChannel(int k, int hw, char module, uint16_t streamid) const;
	bool HwAny(int k) const { return !hw_channels[k].empty(); }
	// a name for channel c of codec k in the log: DV3003 ch1
	std::string ChannelName(int k, size_t c) const;

	CPacketQueue codec2_queue;

	CPacketQueue imbe_queue;
	CPacketQueue usrp_queue;
	std::mutex send_mux;
	// per vocoder choice (CodecChoice.h), what may do it: nullptr = the AMBE
	// devices, else a codec of tcd's own vocoders (OP25, Codec2) or of a codec
	// plugin (CodecPlugins.h), as g_Stats.codec_options names them; a stream keeps
	// the one it started with (Pick), config.codec_choice is for new streams.
	// m17_1600_choices runs alongside choices[kChoiceM17]: the same plugin's M17
	// 1600 codec, else tcd's own Codec2 when it is loaded, else nullptr
	std::vector<class CSoftCodec *> choices[kNumChoices], m17_1600_choices;
	// hw: the stream's AMBE channel (hw_channels[k]), -1 for none
	struct SPinned { int choice; int hw; std::chrono::steady_clock::time_point last; };
	// StreamLog: one line per stream at its end with the vocoder of every codec
	// it used and, on an AMBE device, how its frames fared by their FEC
	// (AmbeFrameCheck.h): decoded frames with corrected errors (fec) or an
	// uncorrectable first partition (unc), encoded frames that fail their FEC
	// (bad). A part is done at the stream's last frame, on a device that
	// encodes at the device's answer to it; the line goes out when every part
	// is done, or when the stream has been quiet for 5 s (its last frame lost)
	struct SStreamPart { int k; std::string by; bool device, decode, done; uint32_t frames, fec, unc, bad; };
	struct SStreamLog { char module; std::chrono::steady_clock::time_point first, last; std::vector<SStreamPart> parts; };
	std::unordered_map<uint32_t, SStreamLog> stream_log;	// by GetStreamKey, under pinned_mux
	SStreamPart *StreamLogPart(int k, const std::shared_ptr<CTranscoderPacket> &packet, int choice, int hw, bool note);
	void StreamLogCount(int k, const std::shared_ptr<CTranscoderPacket> &packet);
	void StreamLogFlush(uint32_t key);
	void DeviceFrameIn(int k, int hw, const std::shared_ptr<CTranscoderPacket> &packet);
	void DeviceFrameOut(int k, const std::shared_ptr<CTranscoderPacket> &packet);
	std::mutex pinned_mux;
	std::unordered_map<uint32_t, SPinned> pinned[kNumChoices];	// by GetStreamKey
	// the modules the devices do D-Star and DMR for; the other modules go to a
	// plugin (empty for the choices with an own vocoder, which does every module)
	std::string hw_modules[kNumChoices];
	// the index of a stream's choice k (CodecChoice.h) into a list of
	// list_size, pinned for the stream; for a device its AMBE channel in
	// dev and ch (dev stays empty for a plugin or tcd's own vocoder)
	// count: the frame counts for the stream's log (false for a lookup besides the frame's own)
	int Pick(int k, size_t list_size, const std::shared_ptr<CTranscoderPacket> &packet, std::shared_ptr<CDVDevice> *dev = nullptr, uint8_t *ch = nullptr, bool count = true);
	class CSoftCodec *PluginChoice(uint32_t codec, const std::shared_ptr<CTranscoderPacket> &packet);
	// codec: TCD_CODEC_DSTAR, _DMR, _P25, _M17 or _M17_1600 (the two M17 share the choice)
	// count: the frame counts for StreamLog here (false where ToDStar/ToDmr counted it already)
	class CSoftCodec *SwCodec(uint32_t codec, const std::shared_ptr<CTranscoderPacket> &packet, std::shared_ptr<CDVDevice> *dev = nullptr, uint8_t *ch = nullptr, bool count = true)
	{
		const int k = ChoiceOfCodec(codec);
		assert(k >= 0);
		const auto &list = codec == TCD_CODEC_M17_1600 ? m17_1600_choices : choices[k];
		return list.empty() ? nullptr : list[Pick(k, list.size(), packet, dev, ch, count)];
	}
	// P25 with the stream's choice: OP25 or a plugin. An error gives IMBE
	// silence (encode) or silent samples (decode)
	void EncodeIMBE(const std::shared_ptr<CTranscoderPacket> &packet, const int16_t *gained, uint8_t *imbe);
	// the codec that decoded, nullptr for none; decoded: its decode succeeded
	class CSoftCodec *DecodeIMBE(const std::shared_ptr<CTranscoderPacket> &packet, int16_t *pcm, bool *decoded = nullptr);
	// Transcode = direct: the frame `from` (a plugin codec) just decoded, coded into
	// codec `to` by the same plugin from the frame itself, when the stream's choice
	// for `to` is that plugin and it can (CodecPlugin.h api 4); the AGC's gain and
	// the source's output gain in source_db, the target's gains added here. Sets
	// the packet's frame of `to` and its bit in direct; false leaves `to` to the
	// audio path
	bool Transcode(uint32_t to, class CSoftCodec *from, float source_db, const std::shared_ptr<CTranscoderPacket> &packet, uint8_t *direct);
	// M17 with the stream's choice: tcd's own Codec2 or a plugin. Encode: 3200,
	// 160 samples into 8 bytes; decode: 8 bytes into 160 samples (3200) or 320
	// (1600). An error gives Codec2 silence or silent samples
	void EncodeM17(const std::shared_ptr<CTranscoderPacket> &packet, const int16_t *gained, uint8_t *bits);
	void DecodeM17(const std::shared_ptr<CTranscoderPacket> &packet, const uint8_t *bits, bool is1600, int16_t *pcm);
	void PurgePinned(void);
	// the DMR plugin for DmrGainMode = reencode, nullptr without one
	class CSoftCodec *dmr_sw = nullptr;
	// the gains as numerators (256 = 0 dB): written by ReconfigureAGC (the monitor's
	// changes, from the reflector thread) and read by every codec thread
	std::atomic<int32_t> dstar_in_num{256}, dstar_out_num{256};	// DStarGainIn/Out for a D-Star plugin
	std::atomic<int32_t> ambe_in_num{256}, ambe_out_num{256}, dmr_reencode_num{256};
	std::atomic<int32_t> outgain_dstar_num{256}, outgain_dmr_num{256}, outgain_usrp_num{256}, outgain_imbe_num{256}, outgain_m17_num{256};
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
	void BitstreamDmrGain(uint8_t *ambe2, uint32_t streamKey);
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
