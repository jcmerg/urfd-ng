#pragma once

// Noise suppression for analog input (SVX and USRP) with the speexdsp
// preprocessor, before the AGC analog. FM audio carries the receiver's noise
// under the speech and between words; the vocoders, AMBE above all, model
// speech and turn that noise into a warbling, robotic sound. Removing most of
// it before encoding keeps the vocoder on the voice.
//
// One preprocessor per module: a module carries one stream at a time, and its
// noise estimate carries over to the next transmission from the same node, so
// the first syllables are already clean. speexdsp keeps adapting the estimate,
// a different node on the module is followed within a second or two.
//
// Thread safe: Process runs in the USRP thread, Configure in the reflector
// thread (every second, from the monitor settings).

#include <cstdint>
#include <cmath>
#include <mutex>
#include <speex/speex_preprocess.h>

class CNoiseSuppress
{
public:
	~CNoiseSuppress()
	{
		for (auto &st : m_states)
			if (st)
				speex_preprocess_state_destroy(st);
	}

	// suppressDb: most attenuation of the noise in dB (-40 to 0), 0 switches it off
	void Configure(float suppressDb)
	{
		int db = (int)lroundf(suppressDb);
		db = db < -40 ? -40 : db > 0 ? 0 : db;
		std::lock_guard<std::mutex> lock(m_mutex);
		if (db == m_suppressDb)
			return;
		m_suppressDb = db;
		for (auto &st : m_states)
			if (st)
				speex_preprocess_ctl(st, SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &m_suppressDb);
	}

	// one 20 ms frame (160 samples at 8 kHz) of the module, in place
	void Process(int16_t *samples, char module)
	{
		int i = module - 'A';
		if (i < 0 || i >= 26)
			return;
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_suppressDb == 0)
			return;
		if (!m_states[i])
		{
			m_states[i] = speex_preprocess_state_init(160, 8000);
			if (!m_states[i])
				return;
			// AGC, VAD and dereverb stay at their default, off: the level is
			// the AGC's job (setting VAD at all makes speexdsp print a warning)
			int on = 1;
			speex_preprocess_ctl(m_states[i], SPEEX_PREPROCESS_SET_DENOISE, &on);
			speex_preprocess_ctl(m_states[i], SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &m_suppressDb);
		}
		speex_preprocess_run(m_states[i], samples);
	}

private:
	SpeexPreprocessState *m_states[26] = {};
	int m_suppressDb = 0;
	std::mutex m_mutex;
};
