#pragma once

// TcdStats — real-time metrics for monitoring (web dashboard + tcdmon)
// Updated by audio pipeline threads, read by MonitorServer.
// Uses atomics for lock-free access from monitor thread.

#include <atomic>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>

#include "TCPacketDef.h"
#include "CodecChoice.h"

struct ModuleStats
{
	// Audio levels in dBFS (updated per 20ms frame)
	std::atomic<float> rms_in{-100.0f};     // before AGC
	std::atomic<float> peak_in{-100.0f};
	std::atomic<float> rms_out{-100.0f};     // after AGC
	std::atomic<float> peak_out{-100.0f};

	// AGC state
	std::atomic<float> agc_gain_db{0.0f};    // current gain in dB
	std::atomic<bool>  agc_gate{false};       // noise gate active (with hysteresis)
	std::atomic<int>   agc_gate_count{0};     // consecutive gate frames (for hysteresis)

	// Stream info
	std::atomic<uint16_t> stream_id{0};
	std::atomic<uint8_t>  codec_in{0};        // ECodecType cast
	std::atomic<uint8_t>  last_codec{0};      // codec_in of the last stream, kept while idle

	// Packet counters
	std::atomic<uint32_t> packets_in{0};      // from reflector
	std::atomic<uint32_t> packets_out{0};     // to reflector

	// Timing
	std::atomic<float> last_activity{0.0f};   // seconds since last packet (0 = active)

	// the link to the reflector for this module; a standby module (StandbyModules) has
	// one only while no other tcd has the module
	std::atomic<bool> linked{false};
	bool standby = false;                     // set once at start
};

struct DeviceStats
{
	static constexpr int MAX_CHANNELS = 3;
	std::string serial;
	std::string type;       // "DV3000" or "DV3003"
	std::string role;       // "dstar", "dmr", "mixed"
	std::atomic<unsigned int> buf_depth{0};
	std::atomic<uint32_t> errors{0};
	std::atomic<bool> online{false};
	// phase out from the monitor: want_out asks for it; phase 0 in service, 1 taking
	// no new streams (its own still run), 2 closed, can be unplugged. can_out: a codec
	// plugin does its codecs, so it may go (set once at start)
	std::atomic<bool> want_out{false};
	std::atomic<int> phase{0};
	std::atomic<bool> can_out{false};
	// Per-channel activity: module letter + timestamp (ms since epoch)
	std::atomic<char> ch_module[MAX_CHANNELS] = {' ', ' ', ' '};
	std::atomic<uint64_t> ch_last_ms[MAX_CHANNELS] = {0, 0, 0};
};

// a codec of a codec plugin (CodecPlugin.h), or one of tcd's own vocoders (OP25, Codec2)
struct SwVocoderStats
{
	std::string name, codec;                  // the plugin's and "dstar"/"dmr"/"p25" ("m17" for Codec2), set once before the monitor starts
	bool builtin{false};                      // tcd's own vocoder, not a plugin
	std::atomic<bool> reencode_active{false}; // re-encode triggered by AGC/OutputGainDMR
	std::atomic<uint32_t> encodes{0};        // total encode calls
	std::atomic<uint32_t> decodes{0};        // total decode calls
	std::atomic<uint32_t> reencodes{0};      // total re-encode calls
	// how the decoded frames came out, from plugins of codec api 3 (0 otherwise)
	std::atomic<uint32_t> error_frames{0};   // frames with bit errors the FEC corrected
	std::atomic<uint32_t> repeats{0};        // frames played as a repeat of the last good one
	std::atomic<uint32_t> mutes{0};          // frames muted for errors or repeats
	std::atomic<int> cached_streams{0};      // streams with a vocoder state
	std::atomic<char> active_module{' '};    // last active module
	std::atomic<uint64_t> last_active_ms{0}; // timestamp of last operation
};

struct ReflectorStats
{
	std::atomic<bool> connected{false};
	std::atomic<uint32_t> packets_rx{0};
	std::atomic<uint32_t> packets_tx{0};
};

class CTcdStats
{
public:
	static constexpr int MAX_MODULES = 26;
	static constexpr int MAX_DEVICES = 16;	// boards of four DV3003 chips, more than one

	ModuleStats    modules[MAX_MODULES];    // indexed by module - 'A'
	DeviceStats    devices[MAX_DEVICES];
	static constexpr int MAX_SW = 16;
	SwVocoderStats sw[MAX_SW];              // the codecs of the codec plugins and tcd's own vocoders
	int            num_sw{0};
	// per vocoder choice (CodecChoice.h), what may do it: "dvsi" (a device) or tcd's
	// own vocoder ("op25", "codec2"), then the codec plugins' names. Set once at
	// start; config.codec_choice is the one new streams get
	std::vector<std::string> codec_options[kNumChoices];
	std::atomic<bool> reencode_available{false};	// a codec plugin for DmrGainMode = reencode
	std::atomic<bool> pool_available{false};	// AMBE channels, so DeviceChannels means something
	// the hardware (AMBE) channels of D-Star and DMR (kChoiceDStar, kChoiceDMR):
	// in service, and of them in use by a stream
	std::atomic<int> hw_total[kNumChoices] {}, hw_busy[kNumChoices] {};
	int            num_devices{0};
	ReflectorStats reflector;

	// Configuration (readable by monitor, writable by REST)
	struct Config
	{
		std::atomic<bool>  agc_enabled{false};
		std::atomic<float> agc_target{-16.0f};
		std::atomic<float> agc_attack{50.0f};
		std::atomic<float> agc_release{500.0f};
		std::atomic<float> agc_maxgain_down{24.0f};
		std::atomic<float> agc_digital_noisegate{-55.0f};
		std::atomic<float> agc_digital_maxgain_up{30.0f};
		std::atomic<float> agc_digital_gatefloor{0.0f};
		std::atomic<float> agc_analog_noisegate{-40.0f};
		std::atomic<float> agc_analog_maxgain_up{12.0f};
		std::atomic<float> agc_analog_gatefloor{0.0f};
		std::atomic<float> agc_analog_noisesuppress{-10.0f};
		std::atomic<int>   gain_dstar_in{0};
		std::atomic<int>   gain_dstar_out{0};
		std::atomic<int>   gain_dmr_in{0};
		std::atomic<int>   gain_dmr_out{0};
		std::atomic<int>   gain_dmr_reencode{0};
		std::atomic<int>   outgain_dstar{0};
		std::atomic<int>   outgain_dmr{0};
		std::atomic<int>   outgain_dmr_bitstream{-6};   // dB of OutputGainDMR through b2 for non-DMR sources
		std::atomic<int>   outgain_usrp{0};
		std::atomic<int>   outgain_imbe{0};
		std::atomic<int>   outgain_m17{0};
		std::atomic<int>   dmr_gain_mode{1};   // 0=off, 1=ambe, 2=reencode
		std::atomic<int>   ambe_gain_db{-2};
		std::atomic<int>   codec_choice[kNumChoices]{};   // index into codec_options
		std::atomic<int>   monitor_view{0};    // 0 auto, 1 cards, 2 compact
		std::atomic<int>   device_pool{0};     // DeviceChannels: 0 fixed, 1 pool, for the streams that start from now on
	} config;

	// Module name list (set at startup)
	std::string module_letters;

	// Helper: compute dBFS from PCM samples
	static void MeasureLevels(const int16_t *samples, int count, float &rms_dbfs, float &peak_dbfs)
	{
		float sumSq = 0.0f;
		float peak = 0.0f;
		for (int i = 0; i < count; i++)
		{
			float s = (float)samples[i];
			sumSq += s * s;
			float a = fabsf(s);
			if (a > peak) peak = a;
		}
		float rms = sqrtf(sumSq / count);
		rms_dbfs  = (rms  > 0.0f) ? 20.0f * log10f(rms  / 32767.0f) : -100.0f;
		peak_dbfs = (peak > 0.0f) ? 20.0f * log10f(peak / 32767.0f) : -100.0f;
	}

	// Update pre-AGC levels for a module
	void UpdateLevelsIn(char module, const int16_t *samples, int count)
	{
		int idx = module - 'A';
		if (idx < 0 || idx >= MAX_MODULES) return;
		float rms, peak;
		MeasureLevels(samples, count, rms, peak);
		modules[idx].rms_in.store(rms, std::memory_order_relaxed);
		modules[idx].peak_in.store(peak, std::memory_order_relaxed);
	}

	// Update post-AGC levels for a module
	void UpdateLevelsOut(char module, const int16_t *samples, int count)
	{
		int idx = module - 'A';
		if (idx < 0 || idx >= MAX_MODULES) return;
		float rms, peak;
		MeasureLevels(samples, count, rms, peak);
		modules[idx].rms_out.store(rms, std::memory_order_relaxed);
		modules[idx].peak_out.store(peak, std::memory_order_relaxed);
	}

	// Update AGC state for a module (gate with hysteresis: 5 consecutive frames = ~100ms)
	void UpdateAGC(char module, float gain_db, bool gate)
	{
		int idx = module - 'A';
		if (idx < 0 || idx >= MAX_MODULES) return;
		modules[idx].agc_gain_db.store(gain_db, std::memory_order_relaxed);
		if (gate)
		{
			int cnt = modules[idx].agc_gate_count.fetch_add(1, std::memory_order_relaxed) + 1;
			modules[idx].agc_gate.store(cnt >= 5, std::memory_order_relaxed);  // ~100ms of silence
		}
		else
		{
			modules[idx].agc_gate_count.store(0, std::memory_order_relaxed);
			modules[idx].agc_gate.store(false, std::memory_order_relaxed);
		}
	}

	// Record incoming packet
	void PacketIn(char module, uint16_t sid, ECodecType codec)
	{
		int idx = module - 'A';
		if (idx < 0 || idx >= MAX_MODULES) return;
		modules[idx].packets_in.fetch_add(1, std::memory_order_relaxed);
		modules[idx].stream_id.store(sid, std::memory_order_relaxed);
		modules[idx].codec_in.store((uint8_t)codec, std::memory_order_relaxed);
		modules[idx].last_codec.store((uint8_t)codec, std::memory_order_relaxed);
		modules[idx].last_activity.store(0.0f, std::memory_order_relaxed);
	}

	// Record outgoing packet
	void PacketOut(char module)
	{
		int idx = module - 'A';
		if (idx < 0 || idx >= MAX_MODULES) return;
		modules[idx].packets_out.fetch_add(1, std::memory_order_relaxed);
	}
};

// Global instance
extern CTcdStats g_Stats;
