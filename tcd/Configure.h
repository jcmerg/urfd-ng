/*
 *   Copyright (c) 2023 by Thomas A. Early N7TAE
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <regex>
#include <utility>

#include "CodecChoice.h"

enum class EGainType { dmrin, dmrout, dstarin, dstarout, dmrreencode, outgain_dstar, outgain_dmr, outgain_usrp, outgain_imbe, outgain_m17 };

#define IS_TRUE(a) ((a)=='t' || (a)=='T' || (a)=='1')

class CConfigure
{
public:
	bool ReadData(const std::string &path);
	int GetGain(EGainType gt) const;
	// Modules and then StandbyModules
	std::string GetTCMods(void) const { return tcmods; }
	// StandbyModules: modules taken only while no other tcd has them
	const std::string &GetStandbyMods(void) const { return standbymods; }
	std::string GetAddress(void) const { return address; }
	const std::string &GetPassword(void) const { return password; }
	unsigned GetPort(void) const { return port; }
	const std::vector<std::string> &GetDeviceSerials(void) const { return device_serials; }

	// DMR/YSF gain mode: off, ambe (bitstream b2), reencode (PCM through a codec plugin)
	enum class EDmrGainMode { off, ambe, reencode };
	EDmrGainMode GetDmrGainMode(void) const { return dmr_gain_mode; }
	int  GetAmbeGainDb(void) const          { return ambe_gain_db; }

	// AGC
	bool  GetAGCEnabled(void) const    { return agc_enabled; }
	float GetAGCTarget(void) const     { return agc_target; }
	float GetAGCAttack(void) const     { return agc_attack; }
	float GetAGCRelease(void) const    { return agc_release; }
	float GetAGCMaxGainDown(void) const { return agc_maxgain_down; }
	// AGC digital: D-Star, DMR/YSF/NXDN, M17, P25 input
	float GetAGCDigitalNoiseGate(void) const { return agc_digital_noisegate; }
	float GetAGCDigitalMaxGainUp(void) const { return agc_digital_maxgain_up; }
	float GetAGCDigitalGateFloor(void) const { return agc_digital_gatefloor; }
	// AGC analog: SVX and USRP input, FM audio with a noise floor in its pauses
	float GetAGCAnalogNoiseGate(void) const  { return agc_analog_noisegate; }
	float GetAGCAnalogMaxGainUp(void) const  { return agc_analog_maxgain_up; }
	float GetAGCAnalogGateFloor(void) const  { return agc_analog_gatefloor; }
	float GetAGCAnalogNoiseSuppress(void) const { return agc_analog_noisesuppress; }

	// Monitor
	const std::string &GetMonitorBindAddress(void) const { return monitor_bind; }
	uint16_t GetMonitorHttpPort(void) const  { return monitor_http_port; }
	uint16_t GetMonitorStatsPort(void) const { return monitor_stats_port; }
	// the monitor's module view: 0 = auto (compact from five modules on), 1 = cards, 2 = compact
	int      GetMonitorView(void) const      { return monitor_view; }
	bool     GetMonitorEnabled(void) const   { return monitor_enabled; }
	// the monitor's password: the web page always asks for it, tcdmon from other hosts than 127.0.0.1; empty = none
	const std::string &GetMonitorPassword(void) const { return monitor_password; }
	const std::string &GetIniPath(void) const { return ini_path; }
	// the part of OutputGainDMR (dB) that non-DMR sources get through b2 in the AMBE2+ frame after the encoder
	int GetOutputGainDMRBitstream(void) const { return outgain_dmr_bitstream; }

	// Stats logging
	bool               GetStatsLogEnabled(void) const { return statslog_enabled; }
	const std::string &GetStatsLogDir(void) const     { return statslog_dir; }
	int                GetStatsLogRetain(void) const   { return statslog_retain; }

	// codec plugins (CodecPlugin.h): the CodecPlugin lines, and the lines tcd does not know, for them
	const std::vector<std::string> &GetCodecPlugins(void) const { return codec_plugins; }
	const std::vector<std::pair<std::string, std::string>> &GetUnknownLines(void) const { return unknown_lines; }
	// the line of a vocoder choice (CodecChoice.h): a codec plugin's name, the
	// device ("dvsi") or tcd's own vocoder ("op25", "codec2"); empty without one.
	// For D-Star or DMR a plugin's name puts the codec in software, even when a
	// device could do it
	const std::string &GetCodecChoice(int choice) const { return codec_choice[choice]; }

private:
	std::string tcmods, standbymods, address, password, ini_path;
	std::vector<std::string> device_serials;
	std::vector<std::string> codec_plugins;
	std::string codec_choice[kNumChoices];
	std::vector<std::pair<std::string, std::string>> unknown_lines;
	uint16_t port;
	int dstar_in = 0, dstar_out = 0, dmr_in = 0, dmr_out = 0, dmr_reencode = 0;
	int outgain_dstar = 0, outgain_dmr = 0, outgain_usrp = 0, outgain_imbe = 0, outgain_m17 = 0;
	int outgain_dmr_bitstream = -6;  // clean at -4 and -6 since 0.7.1, more crackles
	EDmrGainMode dmr_gain_mode = EDmrGainMode::ambe;
	int  ambe_gain_db = 0;
	bool  agc_enabled = false;
	float agc_target  = -16.0f;
	float agc_attack  = 50.0f;
	float agc_release = 500.0f;
	float agc_maxgain_down = 24.0f;   // max attenuation (dB)
	float agc_digital_noisegate  = -55.0f;  // gate threshold (dBFS)
	float agc_digital_maxgain_up = 30.0f;   // max amplification (dB)
	float agc_digital_gatefloor  = 0.0f;    // gain with the gate closed (dB)
	float agc_analog_noisegate  = -40.0f;  // SVX/USRP: above the FM noise floor of a good signal
	float agc_analog_maxgain_up = 12.0f;   // SVX/USRP: max amplification (dB)
	float agc_analog_gatefloor  = 0.0f;    // SVX/USRP: gain with the gate closed (dB), the noise suppression lowers the pauses
	float agc_analog_noisesuppress = -10.0f; // SVX/USRP: speexdsp noise suppression (dB, 0 = off)
	bool     monitor_enabled    = true;
	int      monitor_view       = 0;
	std::string monitor_bind    = "127.0.0.1";
	std::string monitor_password;
	uint16_t monitor_http_port  = 8080;
	uint16_t monitor_stats_port = 8081;
	bool        statslog_enabled = false;
	std::string statslog_dir     = "/tmp/tcd-stats";
	int         statslog_retain  = 24;

	int getSigned(const std::string &key, const std::string &value) const;
};
