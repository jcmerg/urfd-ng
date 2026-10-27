#pragma once

// DMREmbeddedLC -- the link control carried in the embedded signalling of DMR
// voice bursts B to E, one LC per superframe, and the talker alias LCs
//
// The encoding follows CDMREmbeddedData of MMDVMHost by Jonathan Naylor G4KLX
// (GPL v2 or later).
//
// Copyright (C) 2026 Jens-Christian Merg (DL4JC)
// Licensed under the GNU General Public License v3 or later

#include <array>
#include <cstdint>
#include <string>
#include <vector>

class CDmrEmbeddedLC
{
public:
	// a 9 byte LC to send in the next superframe
	void Encode(const uint8_t *lc);
	// writes fragment n (1-4 for bursts B-E) into the embedded bits of a
	// 33 byte DMR frame, returns the LCSS for its EMB
	uint8_t Fragment(unsigned n, uint8_t *frame) const;

	// collects the embedded fragments of bursts B-E from a burst's 7 EMB and
	// embedded bytes (as CDvFramePacket keeps them); true once a whole LC
	// arrived and passed its checks, it is then in lc
	bool Collect(const uint8_t *sync, uint8_t *lc);

	// the LCs of a talker alias, UTF-16 like BrandMeister's: a header and up
	// to 3 blocks, at most 13 characters; UTF-8 text is converted
	static std::vector<std::array<uint8_t, 9>> TalkerAlias(const std::string &utf8);
	// the text of a talker alias from its LCs, header first, as UTF-8
	static std::string TalkerAliasText(const std::vector<std::array<uint8_t, 9>> &lcs);

private:
	bool m_Raw[128] = {};
	bool m_In[128] = {};
	unsigned m_InCount = 0;	// fragments collected, 0 = waiting for a first one
};

// the talker alias a DMR source sends (BrandMeister, the radio), gathered from
// the embedded LCs of its voice bursts
class CDmrTalkerAliasIn
{
public:
	// feed the 7 EMB and embedded bytes of burst n (1-4 for B-E); true once,
	// when the whole alias arrived, its text is then in utf8
	bool Collect(unsigned burst, const uint8_t *sync, std::string &utf8);
	bool Done(void) const { return m_bDone; }

private:
	CDmrEmbeddedLC m_LC;
	std::array<std::array<uint8_t, 9>, 4> m_TA {};
	uint8_t m_uiMask = 0;	// bit n: LC n of the alias arrived
	bool m_bDone = false;
};
