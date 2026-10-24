#pragma once

// Single-band AGC with noise gate and peak limiter for PCM audio
// (160 samples / 20ms frames). Applied between decode and encode
// in the transcoder pipeline.
//
// 1. 75 Hz high-pass (3rd order Butterworth, 18 dB/octave) on the audio, so
//    DC and hum do not drive the level measurement or reach the encoder
// 2. Speech level L in dB from the louder 10 ms half of each frame, over the
//    whole band: it follows the upper envelope of the speech, close to the
//    vowel peaks, not the mean
// 3. Asymmetric level tracking: a jump of more than 10 dB is caught by half
//    the gap per frame, smaller rises with the attack time; the level only
//    falls (the gain only rises) on clearly voiced frames well above the noise
//    floor, at most 10 dB per release time constant. Breath, fricatives, FM
//    hiss and pauses hold the gain at the speaker's value: no pumping
// 4. Voicing: normalized autocorrelation in the pitch range (80-400 Hz) on a
//    300 Hz high-passed copy, so a CTCSS tone does not count as voice
// 5. Adaptive noise floor N per stream, in the same 300 Hz band, so a CTCSS
//    tone or hum does not raise it: follows the level on frames without
//    speech, a minimum over the last 2.4 s catches noise that rose suddenly.
//    The boost is scaled by min(1, (L - N) / 20 dB): weak, noisy signals are
//    not lifted into their noise, clean sources keep the full gain
// 6. A new stream acquires its level fast on its first voiced frames, so the
//    first words of a quiet speaker are not too quiet
// 7. Gate with hold and fade: frames below the threshold or close to the noise
//    floor are no speech; 240ms later (bridging the gaps between words) the
//    gain fades to at most the gate floor (unity for digital sources, below
//    unity for analog FM audio) instead of switching, so word endings are not
//    chopped and noise is not amplified
// 8. Asymmetric gain limits — amplification limited more than attenuation
// 9. Peak limiter acts at once on the applied gain
// 10. Per-sample gain ramp — the gain moves linearly across each frame
//    instead of stepping every 20ms (no zipper noise)
//
// Thread safe: the decode paths of all codecs call it from their own threads.

#include <cstdint>
#include <cmath>
#include <algorithm>
#include <unordered_map>
#include <mutex>
#include <chrono>

namespace agc
{
	static constexpr int   MIN_LAG = 20;         // 400 Hz highest pitch
	static constexpr int   MAX_LAG = 100;        // 80 Hz lowest pitch
	static constexpr int   MAX_COUNT = 320;      // longest frame Process takes
	static constexpr int   MIN_SUB = 20;         // minimum statistics: 0.4 s per sub-window
	static constexpr int   MIN_WINDOWS = 6;      // 2.4 s in all
	static constexpr float kPi = 3.14159265f;

	// second order section, transposed direct form II
	struct Biquad
	{
		float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;

		static Biquad Highpass(float f, float q)
		{
			const float w = 2.0f * kPi * f / 8000.0f;
			const float alpha = sinf(w) / (2.0f * q), c = cosf(w), a0 = 1.0f + alpha;
			Biquad s;
			s.b0 = (1.0f + c) / 2.0f / a0;
			s.b1 = -(1.0f + c) / a0;
			s.b2 = s.b0;
			s.a1 = -2.0f * c / a0;
			s.a2 = (1.0f - alpha) / a0;
			return s;
		}

		float Run(float x, float z[2]) const
		{
			const float y = b0 * x + z[0];
			z[0] = b1 * x - a1 * y + z[1];
			z[1] = b2 * x - a2 * y;
			return y;
		}
	};

	struct Filters
	{
		// 75 Hz, 3rd order Butterworth: 1st order section and a 2nd order one with Q = 1
		float hp1b0, hp1a1;
		Biquad hp2 = Biquad::Highpass(75.0f, 1.0f);
		// 300 Hz, 4th order Butterworth, for the voicing measure
		Biquad vhp[2] = { Biquad::Highpass(300.0f, 0.5412f), Biquad::Highpass(300.0f, 1.3066f) };

		Filters()
		{
			const float k = tanf(kPi * 75.0f / 8000.0f);
			hp1b0 = 1.0f / (1.0f + k);
			hp1a1 = (k - 1.0f) / (k + 1.0f);
		}
	};
}

struct AGCState
{
	// filter states
	float hp1x = 0.0f, hp1y = 0.0f;
	float hp2[2] = {0.0f, 0.0f};
	float vhp[2][2] = {{0.0f, 0.0f}, {0.0f, 0.0f}};
	float vhist[agc::MAX_LAG] = {};  // end of the last frame for the autocorrelation

	int   frames = 0;          // frames of this stream so far
	float level = 0.0f;        // tracked speech level L in dBFS (set on the first frame)
	float noise = -90.0f;      // noise floor N in dBFS
	int   voicedFrames = 0;    // confident voiced frames, for the fast acquisition

	// minimum statistics of the frame level for the noise floor
	float minCur = 0.0f;
	float minWin[agc::MIN_WINDOWS] = {};
	int   minCount = 0;        // full sub-windows (up to MIN_WINDOWS)
	int   minIdx = 0;
	int   minFrames = 0;       // frames in the current sub-window

	float applied = 1.0f;      // gain applied at the end of the last frame

	// gate tracking
	bool open = false;         // gate open (speech or hold)
	int holdFrames = 0;        // frames without speech while still open

	std::chrono::steady_clock::time_point lastUsed{}; // to drop ended streams
};

class CAGC
{
public:
	void Configure(bool enable, float targetDbfs, float attackMs, float releaseMs,
	               float maxGainUpDb, float maxGainDownDb, float noiseGateDbfs = -55.0f,
	               float gateFloorDb = 0.0f)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_gateFloor = powf(10.0f, std::min(gateFloorDb, 0.0f) / 20.0f);
		m_enabled = enable;
		m_targetDb = targetDbfs + ENVELOPE_OFFSET;
		m_maxGainUpDb = maxGainUpDb;
		m_maxGainDownDb = maxGainDownDb;
		m_attackAlpha  = (attackMs  > 0.0f) ? 1.0f - expf(-20.0f / attackMs)  : 1.0f;
		m_releaseAlpha = (releaseMs > 0.0f) ? 1.0f - expf(-20.0f / releaseMs) : 1.0f;
		m_noiseGateDb = noiseGateDbfs;
	}

	void Configure(bool enable, float targetDbfs, float attackMs, float releaseMs, float maxGainDb)
	{
		Configure(enable, targetDbfs, attackMs, releaseMs, maxGainDb, maxGainDb);
	}

private:
	// the upper envelope L of active speech reads this much above its RMS
	static constexpr float ENVELOPE_OFFSET = 3.0f;

	// Core AGC processing, called with m_mutex held
	void ProcessCore(int16_t *samples, int count, AGCState &state, float &gain_db_out, bool &gate_out)
	{
		using namespace agc;
		static constexpr int   HOLD_FRAMES = 12;     // 240ms before the gate closes
		static constexpr float GATE_FADE = 0.5f;     // -6 dB per frame when the gate is closed
		static constexpr float JUMP_DB = 10.0f;      // rises above this are caught by half per frame
		static constexpr float MAX_FALL_DB = 10.0f;  // most negative step into the level tracker
		static constexpr float VOICED = 0.6f;        // autocorrelation of clearly voiced speech
		static constexpr float WEAK_VOICED = 0.4f;
		static constexpr float RELEASE_SNR = 25.0f;  // full release rate above this distance to N
		static constexpr float WEAK_SNR = 10.0f;     // a fifth of it above this, hold below
		static constexpr float GATE_SNR = 9.0f;      // speech: this far above N, or voiced
		static constexpr float NOISE_SNR = 6.0f;     // noise floor update below this
		static constexpr float SNR_SPAN = 20.0f;     // boost scaled by (L - N) / SNR_SPAN
		static constexpr int   ACQUIRE_FRAMES = 10;  // fast release on the first voiced frames
		static constexpr float ACQUIRE_ALPHA = 0.3f;

		count = std::min(count, MAX_COUNT);
		const auto &f = m_filters;

		// High-pass the audio in place at 75 Hz; the speech level is measured on
		// it. Noise floor, gate and voicing run on a copy high-passed at 300 Hz,
		// the band the radios transmit: a CTCSS tone or hum below it neither
		// raises the noise floor nor counts as voice. (A level from the 300 Hz
		// band too depends on the voice: on FM-Funknetz traffic it spread the
		// stations twice as far apart as the whole band.)
		float x[MAX_COUNT], v[MAX_LAG + MAX_COUNT];
		std::copy(state.vhist, state.vhist + MAX_LAG, v);
		float *cur = v + MAX_LAG;
		float sumSq[2] = {0.0f, 0.0f}, sumSqBand[2] = {0.0f, 0.0f}, peak = 0.0f;
		const int half = count / 2;
		for (int i = 0; i < count; i++)
		{
			const float s = samples[i];
			state.hp1y = f.hp1b0 * (s - state.hp1x) - f.hp1a1 * state.hp1y;
			state.hp1x = s;
			x[i] = f.hp2.Run(state.hp1y, state.hp2);
			peak = std::max(peak, fabsf(x[i]));
			sumSq[i < half ? 0 : 1] += x[i] * x[i];
			cur[i] = f.vhp[1].Run(f.vhp[0].Run(x[i], state.vhp[0]), state.vhp[1]);
			sumSqBand[i < half ? 0 : 1] += cur[i] * cur[i];
		}
		std::copy(v + count, v + count + MAX_LAG, state.vhist);

		// levels of the louder 10 ms half: L over the whole band, E in the 300 Hz band
		auto louderHalf = [half, count](const float sq[2]) {
			const float ms = std::max(sq[0] / std::max(half, 1), sq[1] / std::max(count - half, 1));
			return std::max(10.0f * log10f(ms / (32767.0f * 32767.0f) + 1e-12f), -90.0f);
		};
		const float L = louderHalf(sumSq);
		const float E = louderHalf(sumSqBand);

		const float r = Voicing(cur, count);

		// Noise floor: anything below it is evidence at once, frames close to it
		// update it, and the minimum of the last 2.4 s lifts it when the noise rose
		if (state.frames == 0)
		{
			state.level = m_targetDb;  // start at unity gain
			state.minCur = E;
		}
		if (E < state.noise)
			state.noise += 0.5f * (E - state.noise);
		else if (E - state.noise < NOISE_SNR && r < VOICED)
			state.noise += 0.2f * (E - state.noise);
		state.minCur = std::min(state.minCur, E);
		if (++state.minFrames >= MIN_SUB)
		{
			state.minWin[state.minIdx] = state.minCur;
			state.minIdx = (state.minIdx + 1) % MIN_WINDOWS;
			state.minCount = std::min(state.minCount + 1, MIN_WINDOWS);
			state.minFrames = 0;
			state.minCur = E;
			if (state.minCount == MIN_WINDOWS)
			{
				const float minStat = *std::min_element(state.minWin, state.minWin + MIN_WINDOWS);
				state.noise = std::max(state.noise, minStat);
			}
		}
		state.frames++;

		const float snr = E - state.noise;
		const bool voiced = r >= VOICED;
		const bool speech = E >= m_noiseGateDb && (snr >= GATE_SNR || (voiced && snr >= 3.0f));

		// Track the speech level, asymmetric: fast up, down only on voiced speech
		if (speech)
		{
			const float d = std::max(L - state.level, -MAX_FALL_DB);
			float alpha = 0.0f;
			if (d > JUMP_DB)
				alpha = 0.5f;
			else if (d > 0.0f)
				alpha = m_attackAlpha;
			else if (voiced && state.voicedFrames < ACQUIRE_FRAMES && snr >= 15.0f)
				alpha = std::max(m_releaseAlpha, ACQUIRE_ALPHA);
			else if (voiced && snr >= RELEASE_SNR)
				alpha = m_releaseAlpha;
			else if (r >= WEAK_VOICED && snr >= WEAK_SNR)
				alpha = 0.2f * m_releaseAlpha;
			state.level += alpha * d;
			if (voiced && snr >= 15.0f)
				state.voicedFrames++;
		}

		// Gain toward the target, the boost scaled down on noisy signals
		float gainDb = std::clamp(m_targetDb - state.level, -m_maxGainDownDb, m_maxGainUpDb);
		if (gainDb > 0.0f)
			gainDb *= std::clamp((state.level - state.noise) / SNR_SPAN, 0.0f, 1.0f);
		const float gain = powf(10.0f, gainDb / 20.0f);

		// Peak limiter: the gain that keeps this frame below clipping
		const float peakLimit = 32000.0f / std::max(peak, 1.0f);

		// Gate with hold: opens on speech, closes after HOLD_FRAMES without it
		if (speech)
		{
			state.open = true;
			state.holdFrames = 0;
		}
		else if (state.open && ++state.holdFrames > HOLD_FRAMES)
		{
			state.open = false;
		}

		float target;
		if (state.open)
		{
			target = gain;
		}
		else
		{
			// Fade the applied gain to at most the gate floor: no noise amplification,
			// for analog sources the noise between words is lowered
			target = std::max(std::min(gain, m_gateFloor), state.applied * GATE_FADE);
			target = std::min(target, state.applied);  // a fade never raises the gain
		}

		// Ramp from the last applied gain to the target across the frame;
		// both ends below the peak limit keep every sample below clipping
		target = std::min(target, peakLimit);
		float start = std::min(state.applied, peakLimit);
		float step = (target - start) / count;
		for (int i = 0; i < count; i++)
		{
			float s = x[i] * (start + step * (i + 1));
			samples[i] = (int16_t)std::clamp(s, -32767.0f, 32767.0f);
		}
		state.applied = target;

		gain_db_out = 20.0f * log10f(std::max(state.applied, 0.001f));
		gate_out = !state.open;
	}

	// Highest normalized autocorrelation in the pitch range of the 300 Hz
	// high-passed frame cur, with MAX_LAG samples of history before it:
	// about 1 on vowels, below 0.3 on noise
	static float Voicing(const float *cur, int count)
	{
		using namespace agc;
		float e0 = 0.0f;
		for (int i = 0; i < count; i++)
			e0 += cur[i] * cur[i];
		if (e0 < 1.0f * count)  // below about -90 dBFS
			return 0.0f;

		// energy of the lagged window, moved one sample per lag
		float el = 0.0f;
		for (int i = 0; i < count; i++)
			el += cur[i - MIN_LAG] * cur[i - MIN_LAG];
		float best = 0.0f;
		for (int lag = MIN_LAG; lag <= MAX_LAG; lag++)
		{
			if (lag > MIN_LAG)
				el += cur[-lag] * cur[-lag] - cur[count - lag] * cur[count - lag];
			float c = 0.0f;
			for (int i = 0; i < count; i++)
				c += cur[i] * cur[i - lag];
			if (c > 0.0f && el > 0.0f)
				best = std::max(best, c / sqrtf(e0 * el));
		}
		return std::min(best, 1.0f);
	}

public:
	// Process without monitoring output
	void Process(int16_t *samples, int count, uint16_t streamId)
	{
		float gain_db;
		bool gate;
		Process(samples, count, streamId, gain_db, gate);
	}

	// Process with monitoring output
	void Process(int16_t *samples, int count, uint16_t streamId, float &gain_db_out, bool &gate_out)
	{
		gate_out = false;
		gain_db_out = 0.0f;
		std::lock_guard<std::mutex> lock(m_mutex);
		if (!m_enabled) return;

		const auto now = std::chrono::steady_clock::now();
		auto it = m_streams.find(streamId);
		if (it == m_streams.end())
		{
			DropEndedStreams(now);
			it = m_streams.emplace(streamId, AGCState{}).first;
		}
		it->second.lastUsed = now;

		ProcessCore(samples, count, it->second, gain_db_out, gate_out);
	}

	bool IsEnabled() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_enabled;
	}

private:
	// streams not heard for 30 s have ended
	void DropEndedStreams(std::chrono::steady_clock::time_point now)
	{
		for (auto it = m_streams.begin(); it != m_streams.end(); )
		{
			if (now - it->second.lastUsed > std::chrono::seconds(30))
				it = m_streams.erase(it);
			else
				++it;
		}
	}

	const agc::Filters m_filters;
	bool  m_enabled = false;
	float m_targetDb = -20.0f + ENVELOPE_OFFSET;
	float m_maxGainUpDb = 20.0f;
	float m_maxGainDownDb = 24.0f;
	float m_attackAlpha = 0.33f;
	float m_releaseAlpha = 0.04f;
	float m_noiseGateDb = -55.0f;
	float m_gateFloor = 1.0f;          // gain limit with the gate closed (0 dB)
	std::unordered_map<uint16_t, AGCState> m_streams;
	mutable std::mutex m_mutex;
};
