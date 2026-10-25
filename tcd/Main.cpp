// tcd - a hybrid transcoder using DVSI hardware and Codec2 software
// Copyright © 2021,2023 Thomas A. Early N7TAE
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include <unistd.h>
#include <cstring>
#include <csignal>
#include <iostream>
#include <thread>
#include <chrono>

#include "Controller.h"
#include "Configure.h"
#include "TcdStats.h"
#include "MonitorServer.h"
#include "StatsLogger.h"
#include "Version.h"
#include "CodecPlugins.h"

// the global objects
CConfigure     g_Conf;
CController    g_Cont;
CTcdStats      g_Stats;
CMonitorServer g_Monitor;

// SIGTERM and SIGINT only end pause() in main, which then stops tcd. Without a
// handler SIGTERM ended tcd at once, without stopping the devices, and in a
// container, where tcd is PID 1, not at all: the kernel ignores it there, and
// docker killed tcd after 10 s
static void OnSignal(int) {}

int main(int argc, char *argv[])
{
	// Ignore SIGPIPE so send() on a broken connection returns EPIPE
	// instead of killing the process. Needed for clean reconnection.
	signal(SIGPIPE, SIG_IGN);
	if (argc == 2 && 0 == strcmp(argv[1], "--list-devices"))
	{
		g_Cont.ListDevices();
		return EXIT_SUCCESS;
	}

	if (2 != argc)
	{
		std::cerr << "Usage: " << argv[0] << " PATHTOINIFILE" << std::endl;
		std::cerr << "       " << argv[0] << " --list-devices" << std::endl;
		return EXIT_FAILURE;
	}

	if (g_Conf.ReadData(argv[1]))
		return EXIT_FAILURE;

	// the codec plugins, before the controller looks for a DMR codec
	if (g_Codecs.Load(g_Conf.GetCodecPlugins(), g_Conf.GetUnknownLines()))
		return EXIT_FAILURE;

	if (g_Cont.Start())
		return EXIT_FAILURE;

	// The live config: the controller reapplies it every second, edited by the
	// monitor if it runs, so it is filled from the ini in any case
	g_Stats.config.agc_enabled.store(g_Conf.GetAGCEnabled());
	g_Stats.config.agc_target.store(g_Conf.GetAGCTarget());
	g_Stats.config.agc_attack.store(g_Conf.GetAGCAttack());
	g_Stats.config.agc_release.store(g_Conf.GetAGCRelease());
	g_Stats.config.agc_maxgain_down.store(g_Conf.GetAGCMaxGainDown());
	g_Stats.config.agc_digital_noisegate.store(g_Conf.GetAGCDigitalNoiseGate());
	g_Stats.config.agc_digital_maxgain_up.store(g_Conf.GetAGCDigitalMaxGainUp());
	g_Stats.config.agc_digital_gatefloor.store(g_Conf.GetAGCDigitalGateFloor());
	g_Stats.config.agc_analog_noisegate.store(g_Conf.GetAGCAnalogNoiseGate());
	g_Stats.config.agc_analog_maxgain_up.store(g_Conf.GetAGCAnalogMaxGainUp());
	g_Stats.config.agc_analog_gatefloor.store(g_Conf.GetAGCAnalogGateFloor());
	g_Stats.config.agc_analog_noisesuppress.store(g_Conf.GetAGCAnalogNoiseSuppress());
	g_Stats.config.gain_dstar_in.store(g_Conf.GetGain(EGainType::dstarin));
	g_Stats.config.gain_dstar_out.store(g_Conf.GetGain(EGainType::dstarout));
	g_Stats.config.gain_dmr_in.store(g_Conf.GetGain(EGainType::dmrin));
	g_Stats.config.gain_dmr_out.store(g_Conf.GetGain(EGainType::dmrout));
	g_Stats.config.gain_dmr_reencode.store(g_Conf.GetGain(EGainType::dmrreencode));
	g_Stats.config.outgain_dstar.store(g_Conf.GetGain(EGainType::outgain_dstar));
	g_Stats.config.outgain_dmr.store(g_Conf.GetGain(EGainType::outgain_dmr));
	g_Stats.config.outgain_dmr_bitstream.store(g_Conf.GetOutputGainDMRBitstream());
	g_Stats.config.outgain_usrp.store(g_Conf.GetGain(EGainType::outgain_usrp));
	g_Stats.config.outgain_imbe.store(g_Conf.GetGain(EGainType::outgain_imbe));
	g_Stats.config.outgain_m17.store(g_Conf.GetGain(EGainType::outgain_m17));
	{
		auto mode = g_Conf.GetDmrGainMode();
		// Downgrade reencode to ambe without a codec plugin for DMR
		if (mode == CConfigure::EDmrGainMode::reencode && !g_Stats.reencode_available.load(std::memory_order_relaxed))
		{
			std::cerr << "Warning: DmrGainMode=reencode needs a codec plugin for DMR, falling back to ambe" << std::endl;
			mode = CConfigure::EDmrGainMode::ambe;
		}
		g_Stats.config.dmr_gain_mode.store((int)mode);
	}
	g_Stats.config.ambe_gain_db.store(g_Conf.GetAmbeGainDb());
	g_Stats.config.monitor_view.store(g_Conf.GetMonitorView());

	// Start monitor server (web dashboard + stats socket)
	if (g_Conf.GetMonitorEnabled())
	{
		g_Stats.module_letters = g_Conf.GetTCMods();
		for (char m : g_Conf.GetStandbyMods())
			g_Stats.modules[m - 'A'].standby = true;
		g_Monitor.Start(g_Conf.GetMonitorBindAddress(), g_Conf.GetMonitorHttpPort(), g_Conf.GetMonitorStatsPort(), g_Conf.GetMonitorPassword());
	}

	// Start stats CSV logger
	g_StatsLog.Configure(g_Conf.GetStatsLogEnabled(), g_Conf.GetStatsLogDir(), g_Conf.GetStatsLogRetain());
	g_StatsLog.Start();

	std::cout << "Hybrid Transcoder version " TCD_VERSION " successfully started" << std::endl;

	struct sigaction sa = {};
	sa.sa_handler = OnSignal;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGTERM, &sa, nullptr);
	sigaction(SIGINT, &sa, nullptr);
	pause();

	// a stop that hangs still ends (a thread: as PID 1 in a container tcd would
	// ignore SIGALRM's default action as well)
	std::thread([] {
		std::this_thread::sleep_for(std::chrono::seconds(5));
		std::cerr << "tcd did not stop within 5 s, ending it" << std::endl;
		_exit(EXIT_FAILURE);
	}).detach();
	std::cout << "Stopping tcd" << std::endl;
	g_StatsLog.Stop();
	g_Monitor.Stop();
	g_Cont.Stop();

	return EXIT_SUCCESS;
}
