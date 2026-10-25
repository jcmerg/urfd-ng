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

#include <unistd.h>
#include <climits>
#include <cstdlib>
#include <libgen.h>
#include <cstring>
#include <dirent.h>
#include <sys/select.h>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <thread>
#include <queue>
#include <vector>
#include "TranscoderPacket.h"
#include "CodecPlugins.h"
#include "Controller.h"
#include "Configure.h"
#include "TcdStats.h"
#include "StatsLogger.h"
#include "AmbeGain.h"

extern CTcdStats g_Stats;

extern CConfigure g_Conf;

static std::string ReadFirstLine(const std::string &path)
{
	std::ifstream f(path);
	std::string s;
	std::getline(f, s);
	while (!s.empty() && (s.back()=='\n' || s.back()=='\r')) s.pop_back();
	return s;
}

// sysfs directory of the USB device a tty (ttyUSB0, ttyACM0) belongs to: the first
// directory above its tty device that has an idVendor, never a hub further up
static std::string UsbDeviceDir(const std::string &tty)
{
	char real[PATH_MAX];
	const std::string path = "/sys/class/tty/" + tty + "/device";
	if (nullptr == realpath(path.c_str(), real))
		return std::string();
	std::string dir(real);
	for (int i = 0; i < 3 && dir.size() > 1; i++)
	{
		if (0 == access((dir + "/idVendor").c_str(), F_OK))
			return dir;
		dir = dir.substr(0, dir.rfind('/'));
	}
	return std::string();
}

static std::string GetUsbSerialForDevnode(const std::string &devnode)
{
	const std::string dir = UsbDeviceDir(devnode.substr(devnode.find_last_of('/') + 1));
	return dir.empty() ? std::string() : ReadFirstLine(dir + "/serial");
}

struct SSerialDevice
{
	std::string devnode;	// /dev/ttyUSB0
	std::string desc;		// its name in /dev/serial/by-id
	std::string serial;		// USB serial number, empty if the device has none
	char port;				// port letter as libftd2xx named the ports of multi-port chips, A for the first
};

// the USB serial devices udev lists in /dev/serial/by-id, false if there is no such directory
static bool ListSerialDevices(std::vector<SSerialDevice> &devices)
{
	const std::string byid = "/dev/serial/by-id";
	DIR *dir = opendir(byid.c_str());
	if (nullptr == dir)
		return false;

	struct dirent *entry;
	while ((entry = readdir(dir)) != nullptr)
	{
		if (entry->d_name[0] == '.')
			continue;
		char target[PATH_MAX];
		const ssize_t len = readlink((byid + "/" + entry->d_name).c_str(), target, sizeof(target)-1);
		if (len <= 0)
			continue;
		target[len] = '\0';

		SSerialDevice dev;
		dev.devnode = std::string("/dev/") + basename(target);
		dev.desc = entry->d_name;
		dev.serial = GetUsbSerialForDevnode(dev.devnode);
		// by-id names end in -ifNN-portM, NN being the USB interface
		const auto pos = dev.desc.rfind("-if");
		dev.port = (std::string::npos == pos) ? 'A' : char('A' + std::atoi(dev.desc.c_str() + pos + 3));
		devices.push_back(dev);
	}
	closedir(dir);
	return true;
}

// DeviceSerial = matches the USB serial, the libftd2xx style serial with the port
// letter (DA1234A), the by-id name or the device node, exactly
static bool IsWhitelisted(const SSerialDevice &dev, const std::vector<std::string> &whitelist)
{
	for (const auto &ws : whitelist)
	{
		if ((!dev.serial.empty() && (ws == dev.serial || ws == dev.serial + dev.port)) || ws == dev.desc || ws == dev.devnode)
			return true;
	}
	return false;
}

// libftd2xx builds of tcd unbound the AMBE devices from ftdi_sio, and they stay
// unbound until replugged or rebooted. Bind the FTDI interfaces without a driver
// back, so an upgraded tcd finds them.
static void BindFtdiSio(const std::vector<std::string> &whitelist)
{
	const std::string usb = "/sys/bus/usb/devices/";
	const std::string bind = "/sys/bus/usb/drivers/ftdi_sio/bind";
	DIR *dir = opendir(usb.c_str());
	if (nullptr == dir || 0 != access(bind.c_str(), W_OK))
	{
		if (dir)
			closedir(dir);
		return;
	}

	int bound = 0;
	struct dirent *entry;
	while ((entry = readdir(dir)) != nullptr)
	{
		const std::string intf(entry->d_name);
		if (std::string::npos == intf.find(':'))
			continue;	// a device, not an interface
		const std::string devdir = usb + intf.substr(0, intf.find(':'));
		if (ReadFirstLine(devdir + "/idVendor") != "0403" || 0 == access((usb + intf + "/driver").c_str(), F_OK))
			continue;	// not FTDI, or it has a driver
		const std::string serial = ReadFirstLine(devdir + "/serial");
		bool wanted = whitelist.empty();
		for (const auto &ws : whitelist)
			wanted = wanted || (!serial.empty() && 0 == ws.compare(0, serial.size(), serial));
		if (!wanted)
			continue;
		std::ofstream b(bind);
		if (b << intf << std::flush)
		{
			std::cout << "Bound " << intf << " (SN=" << serial << ") to ftdi_sio" << std::endl;
			bound++;
		}
	}
	closedir(dir);

	// give udev a moment for the /dev/serial/by-id links
	if (bound)
		std::this_thread::sleep_for(std::chrono::seconds(2));
}

// a single channel AMBE3000 device, by its /dev/serial/by-id name
static bool IsDV3000(const std::string &desc)
{
	for (const char *name : { "ThumbDV", "DVstick-30", "USB-3000", "FT230X_Basic_UART", "FT232R_USB_UART" })
		if (std::string::npos != desc.find(name))
			return true;
	return false;
}

int32_t CController::calcNumerator(int32_t db) const
{
	float num = 256.0f * powf(10.0f, (float(db)/20.0f));

	return int32_t(roundf(num));
}

void CController::ApplyGain(int16_t *samples, int count, int32_t num)
{
	if (num != 256)
	{
		for (int i = 0; i < count; i++)
		{
			int32_t v = ((int32_t)samples[i] * num) >> 8;
			if (v > 32767) v = 32767;
			else if (v < -32768) v = -32768;
			samples[i] = (int16_t)v;
		}
	}
}

// counts a call of a codec plugin for the monitor
static void SwStat(const CSoftCodec *sw, char module, std::atomic<uint32_t> SwVocoderStats::*counter)
{
	if (!sw || sw->stats < 0)
		return;
	auto &st = g_Stats.sw[sw->stats];
	(st.*counter).fetch_add(1, std::memory_order_relaxed);
	st.active_module.store(module, std::memory_order_relaxed);
	st.last_active_ms.store(
		(uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count(),
		std::memory_order_relaxed);
}

CController::CController() : keep_running(true), ambe_in_num(256), ambe_out_num(256), dmr_reencode_num(256), outgain_dstar_num(256), outgain_dmr_num(256), outgain_usrp_num(256), outgain_imbe_num(256), outgain_m17_num(256) {}

bool CController::Start()
{
	dmr_reencode_num = calcNumerator(g_Conf.GetGain(EGainType::dmrreencode));
	outgain_dstar_num = calcNumerator(g_Conf.GetGain(EGainType::outgain_dstar));
	outgain_dmr_num = calcNumerator(g_Conf.GetGain(EGainType::outgain_dmr));
	outgain_usrp_num = calcNumerator(g_Conf.GetGain(EGainType::outgain_usrp));
	outgain_imbe_num = calcNumerator(g_Conf.GetGain(EGainType::outgain_imbe));
	outgain_m17_num = calcNumerator(g_Conf.GetGain(EGainType::outgain_m17));
	m_agcDigital.Configure(g_Conf.GetAGCEnabled(), g_Conf.GetAGCTarget(), g_Conf.GetAGCAttack(), g_Conf.GetAGCRelease(), g_Conf.GetAGCDigitalMaxGainUp(), g_Conf.GetAGCMaxGainDown(), g_Conf.GetAGCDigitalNoiseGate(), g_Conf.GetAGCDigitalGateFloor());
	m_agcAnalog.Configure(g_Conf.GetAGCEnabled(), g_Conf.GetAGCTarget(), g_Conf.GetAGCAttack(), g_Conf.GetAGCRelease(), g_Conf.GetAGCAnalogMaxGainUp(), g_Conf.GetAGCMaxGainDown(), g_Conf.GetAGCAnalogNoiseGate(), g_Conf.GetAGCAnalogGateFloor());
	m_nsAnalog.Configure(g_Conf.GetAGCAnalogNoiseSuppress());

	tcClient.SetPassword(g_Conf.GetPassword());
	tcClient.SetStandby(g_Conf.GetStandbyMods());
	if (InitVocoders() || tcClient.Open(g_Conf.GetAddress(), g_Conf.GetTCMods(), g_Conf.GetPort()))
	{
		keep_running = false;
		return true;
	}
	if (dstar_device) dstar_device->SetOutputGains(outgain_dstar_num, PcmDmrGainNum());
	if (dmrsf_device) dmrsf_device->SetOutputGains(outgain_dstar_num, PcmDmrGainNum());

	reflectorFuture = std::async(std::launch::async, &CController::ReadReflectorThread, this);
	c2Future        = std::async(std::launch::async, &CController::ProcessC2Thread,     this);
	imbeFuture      = std::async(std::launch::async, &CController::ProcessIMBEThread,   this);
	usrpFuture      = std::async(std::launch::async, &CController::ProcessUSRPThread,   this);
	// the threads of the codec plugins, when there is one for the codec
	if (std::count(dmr_choices.begin(), dmr_choices.end(), nullptr) < (long)dmr_choices.size())
		swambe2Future = std::async(std::launch::async, &CController::ProcessSWAMBE2Thread, this);
	if (std::count(dstar_choices.begin(), dstar_choices.end(), nullptr) < (long)dstar_choices.size())
		swdstarFuture = std::async(std::launch::async, &CController::ProcessSWDStarThread, this);
	return false;
}

void CController::ReconfigureAGC()
{
	m_agcDigital.Configure(
		g_Stats.config.agc_enabled.load(),
		g_Stats.config.agc_target.load(),
		g_Stats.config.agc_attack.load(),
		g_Stats.config.agc_release.load(),
		g_Stats.config.agc_digital_maxgain_up.load(),
		g_Stats.config.agc_maxgain_down.load(),
		g_Stats.config.agc_digital_noisegate.load(),
		g_Stats.config.agc_digital_gatefloor.load()
	);
	m_agcAnalog.Configure(
		g_Stats.config.agc_enabled.load(),
		g_Stats.config.agc_target.load(),
		g_Stats.config.agc_attack.load(),
		g_Stats.config.agc_release.load(),
		g_Stats.config.agc_analog_maxgain_up.load(),
		g_Stats.config.agc_maxgain_down.load(),
		g_Stats.config.agc_analog_noisegate.load(),
		g_Stats.config.agc_analog_gatefloor.load()
	);
	m_nsAnalog.Configure(g_Stats.config.agc_analog_noisesuppress.load());
	// Also update gain numerators from live config
	ambe_in_num = calcNumerator(g_Stats.config.gain_dmr_in.load());
	ambe_out_num = calcNumerator(g_Stats.config.gain_dmr_out.load());
	dstar_in_num = calcNumerator(g_Stats.config.gain_dstar_in.load());
	dstar_out_num = calcNumerator(g_Stats.config.gain_dstar_out.load());
	dmr_reencode_num = calcNumerator(g_Stats.config.gain_dmr_reencode.load());
	outgain_dstar_num = calcNumerator(g_Stats.config.outgain_dstar.load());
	outgain_dmr_num = calcNumerator(g_Stats.config.outgain_dmr.load());
	outgain_usrp_num = calcNumerator(g_Stats.config.outgain_usrp.load());
	outgain_imbe_num = calcNumerator(g_Stats.config.outgain_imbe.load());
	outgain_m17_num = calcNumerator(g_Stats.config.outgain_m17.load());
	if (dstar_device) dstar_device->SetOutputGains(outgain_dstar_num, PcmDmrGainNum());
	if (dmrsf_device) dmrsf_device->SetOutputGains(outgain_dstar_num, PcmDmrGainNum());
}

void CController::Stop()
{
	keep_running = false;

	if (reflectorFuture.valid())
		reflectorFuture.get();
	if (c2Future.valid())
		c2Future.get();

	tcClient.Close();
	if (dstar_device) dstar_device->CloseDevice();
	if (dmrsf_device) dmrsf_device->CloseDevice();
	dstar_device.reset();
	dmrsf_device.reset();
}

void CController::ListDevices()
{
	BindFtdiSio(g_Conf.GetDeviceSerials());
	std::vector<SSerialDevice> devices;
	if (!ListSerialDevices(devices) || devices.empty())
	{
		std::cout << "No USB serial devices found (no /dev/serial/by-id), is the AMBE device plugged in?" << std::endl;
		return;
	}

	std::cout << "Detected USB serial devices:" << std::endl;
	int idx = 0;
	for (const auto &dev : devices)
		std::cout << "  [" << idx++ << "] Serial: " << (dev.serial.empty() ? "(none)" : dev.serial)
		          << "  " << dev.desc << " -> " << dev.devnode << (IsAmbeDevice(dev.desc) ? "  (AMBE)" : "") << std::endl;
	std::cout << std::endl;
	std::cout << "Use 'DeviceSerial = <serial>' in tcd.ini to select your AMBE device." << std::endl;
}

bool CController::IsAmbeDevice(const std::string &desc)
{
	// Known AMBE/DVSI device descriptions
	// Generic FTDI names (FT232R, FT230X) are included because many
	// ThumbDV/DVstick clones report these generic names.
	static const std::vector<std::string> known = {
		"ThumbDV", "DVstick-30", "DVstick-33",
		"USB-3000", "USB-3003", "USB-3006", "USB-3012",
		"FT232R_USB_UART", "FT230X_Basic_UART",
		"DF2ET"
	};
	// Devices with empty description are not AMBE devices
	if (desc.empty())
		return false;
	for (const auto &k : known)
	{
		if (std::string::npos != desc.find(k))
			return true;
	}
	return false;
}

bool CController::DiscoverFtdiDevices(std::list<std::pair<std::string, std::string>> &found)
{
	// the AMBE devices are used through the kernel's ftdi_sio driver
	const auto &whitelist = g_Conf.GetDeviceSerials();
	BindFtdiSio(whitelist);

	std::vector<SSerialDevice> devices;
	if (!ListSerialDevices(devices))
	{
		std::cerr << "No USB serial devices found (no /dev/serial/by-id), is the AMBE device plugged in?" << std::endl;
		return false;
	}

	for (const auto &dev : devices)
	{
		const std::string sn = dev.serial.empty() ? "(none)" : dev.serial;
		if (whitelist.empty() ? IsAmbeDevice(dev.desc) : IsWhitelisted(dev, whitelist))
		{
			std::cout << "Found AMBE device" << (whitelist.empty() ? "" : " (whitelisted)") << ": " << dev.desc << " (SN=" << sn << ") -> " << dev.devnode << std::endl;
			found.emplace_back(std::make_pair(dev.devnode, dev.desc));
		}
		else
		{
			std::cout << "Skipping serial device" << (whitelist.empty() ? "" : " (not whitelisted)") << ": " << dev.desc << " (SN=" << sn << ")" << std::endl;
		}
	}
	return false;
}

bool CController::InitVocoders()
{
	// M17 "devices", one for each module
	const std::string modules(g_Conf.GetTCMods());
	for (unsigned int i=0; i<modules.size(); i++)
	{
		auto c = modules.at(i);
		if (c < 'A' || c > 'Z') {
			std::cerr << "Transcoded modules[" << i << "] is not an uppercase letter!" << std::endl;
			return true;
		}
		c2_16[c] = std::unique_ptr<CCodec2>(new CCodec2(false));
		c2_32[c] = std::unique_ptr<CCodec2>(new CCodec2(true));
	}

	// the software vocoders of the codec plugins: CodecDStar / CodecDMR in tcd.ini
	// put the codec in software, else the devices come first and a plugin only
	// does what no device does (and the DMR re-encode)
	// ("dvsi" names the device: the default)
	const std::string dstar_name = g_Conf.GetCodecDStar() == "dvsi" ? std::string() : g_Conf.GetCodecDStar();
	const std::string dmr_name = g_Conf.GetCodecDMR() == "dvsi" ? std::string() : g_Conf.GetCodecDMR();
	const bool dstar_forced = !dstar_name.empty();
	const bool dmr_forced = !dmr_name.empty();
	CSoftCodec *dstar_plugin = g_Codecs.Get(TCD_CODEC_DSTAR, dstar_name);
	dmr_sw = g_Codecs.Get(TCD_CODEC_DMR, dmr_name);
	if ((dstar_forced && !dstar_plugin) || (dmr_forced && !dmr_sw))
	{
		std::cerr << "ERROR: CodecDStar/CodecDMR names no loaded codec plugin that does that codec" << std::endl;
		return true;
	}

	// the 3000 or 3003 devices
	std::list<std::pair<std::string, std::string>> deviceset;
	if (DiscoverFtdiDevices(deviceset))
		return true;

	// the devices in use, for the monitor: device node, description, role
	std::vector<std::tuple<std::string, std::string, CDVDevice *, const char *>> used;

	if (dstar_forced && dmr_forced)
	{
		// everything in software, no device
		if (!deviceset.empty())
			std::cout << "CodecDStar and CodecDMR are set: the AMBE devices stay unused" << std::endl;
	}
	else if (dstar_forced || dmr_forced || deviceset.size() == 1)
	{
		// one device for one of the two, a plugin for the other
		if (deviceset.empty())
		{
			std::cerr << "Could not find an AMBE device for " << (dstar_forced ? "DMR" : "D-Star") << "!" << std::endl;
			std::cerr << "Run 'tcd --list-devices' to see the connected USB serial devices." << std::endl;
			return true;
		}
		// without a choice in tcd.ini the device does D-Star, a DMR plugin DMR
		const bool device_does_dmr = dstar_forced || (!dmr_forced && !dmr_sw && dstar_plugin);
		if (!device_does_dmr && !dmr_sw)
		{
			std::cerr << "ERROR: A single DVSI device needs a codec plugin for DMR (CodecPlugin = ... in tcd.ini)." << std::endl;
			std::cerr << "       Without one, two devices are needed: 2x DV3000, 2x DV3003," << std::endl;
			std::cerr << "       or one DV3000 + one DV3003 (mixed mode)." << std::endl;
			return true;
		}
		const auto &dev = deviceset.front();
		const Edvtype dvtype = IsDV3000(dev.second) ? Edvtype::dv3000 : Edvtype::dv3003;
		const Encoding enc = device_does_dmr ? Encoding::dmrsf : Encoding::dstar;
		auto &device = device_does_dmr ? dmrsf_device : dstar_device;
		if (Edvtype::dv3000 == dvtype)
			device = std::unique_ptr<CDVDevice>(new CDV3000(enc));
		else
			device = std::unique_ptr<CDVDevice>(new CDV3003(enc));
		if (device_does_dmr)
		{
			if (device->OpenDevice(dev.first, dev.second, dvtype, int8_t(g_Conf.GetGain(EGainType::dmrin)), int8_t(g_Conf.GetGain(EGainType::dmrout))))
				return true;
		}
		else
		{
			// the DMR plugin gets DmrGainIn/Out in software
			ambe_in_num = calcNumerator(g_Conf.GetGain(EGainType::dmrin));
			ambe_out_num = calcNumerator(g_Conf.GetGain(EGainType::dmrout));
			if (device->OpenDevice(dev.first, dev.second, dvtype, int8_t(g_Conf.GetGain(EGainType::dstarin)), int8_t(g_Conf.GetGain(EGainType::dstarout))))
				return true;
		}
		used.emplace_back(dev.first, dev.second, device.get(), device_does_dmr ? "dmr" : "dstar");
		(device_does_dmr ? dmr_hw_modules : dstar_hw_modules) = modules.substr(0, Edvtype::dv3000 == dvtype ? 1 : 3);
		std::cout << "Using " << dev.second << " for " << (device_does_dmr ? "DMR" : "D-Star") << std::endl;
	}
	else if (deviceset.size() >= 2)
	{
		// Classify each device by type
		std::pair<std::string,std::string> dv3000_dev, dv3003_dev;
		for (const auto &dev : deviceset)
		{
			if (IsDV3000(dev.second))
				dv3000_dev = dev;
			else
				dv3003_dev = dev;
		}

		// Mixed mode: DV3000 (D-Star) + DV3003 (1x D-Star + 2x DMR) = 2 modules
		if (!dv3000_dev.first.empty() && !dv3003_dev.first.empty())
		{
			std::cout << "Mixed mode: " << dv3000_dev.second << " (" << dv3000_dev.first << ") + "
			          << dv3003_dev.second << " (" << dv3003_dev.first << ")" << std::endl;

			// DV3000: D-Star vocoder for module A (first module)
			dstar_device = std::unique_ptr<CDVDevice>(new CDV3000(Encoding::dstar));
			if (dstar_device->OpenDevice(dv3000_dev.first, dv3000_dev.second, Edvtype::dv3000,
					int8_t(g_Conf.GetGain(EGainType::dstarin)), int8_t(g_Conf.GetGain(EGainType::dstarout))))
				return true;

			// DV3003: ch0=D-Star (module B), ch1=DMR (module A), ch2=DMR (module B)
			auto *dv3003 = new CDV3003(Encoding::dmrsf);
			dmrsf_device = std::unique_ptr<CDVDevice>(dv3003);

			// Set per-channel encoding BEFORE OpenDevice so ConfigureVocoder uses the right codec
			dv3003->SetChannelEncoding(0, Encoding::dstar);
			dv3003->SetChannelEncoding(1, Encoding::dmrsf);
			dv3003->SetChannelEncoding(2, Encoding::dmrsf);

			// Open with DMR gains (ConfigureVocoder will use per-channel encoding)
			if (dmrsf_device->OpenDevice(dv3003_dev.first, dv3003_dev.second, Edvtype::dv3003,
					int8_t(g_Conf.GetGain(EGainType::dmrin)), int8_t(g_Conf.GetGain(EGainType::dmrout))))
				return true;

			// Map modules to DV3003 channels
			if (modules.size() >= 2)
			{
				char modA = modules[0], modB = modules[1];
				dv3003->SetModuleDStarChannel(modB, 0);  // D-Star module B → ch0
				dv3003->SetModuleDMRChannel(modA, 1);    // DMR module A → ch1
				dv3003->SetModuleDMRChannel(modB, 2);    // DMR module B → ch2
				mixed_mode = true;
				mixed_dstar_module = modB;
				mixed_dv3003 = dv3003;
				std::cout << "  Module " << modA << ": D-Star(DV3000) + DMR(DV3003 ch1)" << std::endl;
				std::cout << "  Module " << modB << ": D-Star(DV3003 ch0) + DMR(DV3003 ch2)" << std::endl;
			}
			used.emplace_back(dv3000_dev.first, dv3000_dev.second, dstar_device.get(), "dstar");
			used.emplace_back(dv3003_dev.first, dv3003_dev.second, dmrsf_device.get(), mixed_mode ? "mixed" : "dmr");
			dstar_hw_modules = dmr_hw_modules = modules.substr(0, 2);
		}
		// Two same-type devices
		else
		{
			const auto dev1 = deviceset.front();
			deviceset.pop_front();
			const auto dev2 = deviceset.front();
			const Edvtype dvtype = IsDV3000(dev1.second) ? Edvtype::dv3000 : Edvtype::dv3003;

			if (Edvtype::dv3000 == dvtype) {
				dstar_device = std::unique_ptr<CDVDevice>(new CDV3000(Encoding::dstar));
				dmrsf_device = std::unique_ptr<CDVDevice>(new CDV3000(Encoding::dmrsf));
			} else {
				dstar_device = std::unique_ptr<CDVDevice>(new CDV3003(Encoding::dstar));
				dmrsf_device = std::unique_ptr<CDVDevice>(new CDV3003(Encoding::dmrsf));
			}

			if (dstar_device->OpenDevice(dev1.first, dev1.second, dvtype,
					int8_t(g_Conf.GetGain(EGainType::dstarin)), int8_t(g_Conf.GetGain(EGainType::dstarout))))
				return true;
			if (dmrsf_device->OpenDevice(dev2.first, dev2.second, dvtype,
					int8_t(g_Conf.GetGain(EGainType::dmrin)), int8_t(g_Conf.GetGain(EGainType::dmrout))))
				return true;
			used.emplace_back(dev1.first, dev1.second, dstar_device.get(), "dstar");
			used.emplace_back(dev2.first, dev2.second, dmrsf_device.get(), "dmr");
			dstar_hw_modules = dmr_hw_modules = modules.substr(0, Edvtype::dv3000 == dvtype ? 1 : 3);
		}
	}
	else if (dstar_plugin && dmr_sw)
	{
		std::cout << "No AMBE device: D-Star and DMR in software" << std::endl;
	}
	else
	{
		std::cerr << "Could not find an AMBE device!" << std::endl;
		std::cerr << "Run 'tcd --list-devices' to see the connected USB serial devices." << std::endl;
		std::cerr << "Use 'DeviceSerial = <serial>' in tcd.ini to select your AMBE device," << std::endl;
		std::cerr << "or load codec plugins for D-Star and DMR (CodecPlugin = ...)." << std::endl;
		return true;
	}

	// what may do D-Star and DMR: the device first, then every plugin that has the
	// codec; the monitor switches between them, CodecDStar/CodecDMR is the start
	dstar_in_num = calcNumerator(g_Conf.GetGain(EGainType::dstarin));
	dstar_out_num = calcNumerator(g_Conf.GetGain(EGainType::dstarout));
	for (auto [codec, device, choices, options, choice, name, label] : {
			std::make_tuple(TCD_CODEC_DSTAR, dstar_device.get(), &dstar_choices, &g_Stats.dstar_options, &g_Stats.config.codec_dstar, dstar_name, "D-Star"),
			std::make_tuple(TCD_CODEC_DMR, dmrsf_device.get(), &dmr_choices, &g_Stats.dmr_options, &g_Stats.config.codec_dmr, dmr_name, "DMR") })
	{
		if (device)
		{
			choices->push_back(nullptr);
			options->push_back("dvsi");
		}
		for (auto *sw : g_Codecs.All(codec))
		{
			choices->push_back(sw);
			options->push_back(sw->GetName());
			if (g_Stats.num_sw < CTcdStats::MAX_SW)
			{
				auto &st = g_Stats.sw[g_Stats.num_sw];
				st.name = sw->GetName();
				st.codec = codec == TCD_CODEC_DSTAR ? "dstar" : "dmr";
				sw->stats = g_Stats.num_sw++;
			}
		}
		int start = 0;
		for (size_t i = 0; i < options->size(); i++)
			if (!name.empty() && (*options)[i] == name)
				start = int(i);
		choice->store(start, std::memory_order_relaxed);
		std::string list;
		for (size_t i = 0; i < options->size(); i++)
			list += (i ? ", " : "") + (*options)[i] + (int(i) == start ? " (now)" : "");
		std::cout << label << ": " << list << std::endl;
	}
	// the modules beyond the devices' channels go to a plugin
	for (auto [hw, choices, label] : { std::make_tuple(&dstar_hw_modules, &dstar_choices, "D-Star"), std::make_tuple(&dmr_hw_modules, &dmr_choices, "DMR") })
	{
		std::string rest;
		for (char m : modules)
			if (std::string::npos == hw->find(m))
				rest += m;
		const bool plugin = std::count(choices->begin(), choices->end(), nullptr) < (long)choices->size();
		if (!hw->empty() && !rest.empty())
			std::cout << label << ": modules " << *hw << " on the devices, " << rest << (plugin ? " on a codec plugin" : "") << std::endl;
		if (!rest.empty() && !plugin)
		{
			std::cerr << "ERROR: the devices do " << label << " for " << (hw->empty() ? std::string("no module") : "modules " + *hw) << " only; modules " << rest
			          << " need a codec plugin for " << label << " (CodecPlugin = ...) or fewer Modules" << std::endl;
			return true;
		}
	}
	if (dmr_sw && dmrsf_device)
		std::cout << "Codec plugin " << dmr_sw->GetName() << " re-encodes DMR with DmrGainMode = reencode" << std::endl;
	g_Stats.reencode_available.store(dmr_sw && dmrsf_device, std::memory_order_relaxed);
	if (!dmr_sw && g_Stats.config.dmr_gain_mode.load(std::memory_order_relaxed) == 2)
	{
		std::cerr << "WARNING: DmrGainMode=reencode needs a codec plugin for DMR. Falling back to ambe." << std::endl;
		g_Stats.config.dmr_gain_mode.store(1, std::memory_order_relaxed);
	}

	if (dstar_device)
		dstar_device->Start();
	if (dmrsf_device)
		dmrsf_device->Start();

	// Register DVSI devices in monitoring stats
	{
		int di = 0;
		for (const auto &[node, desc, device, role] : used)
		{
			if (di >= CTcdStats::MAX_DEVICES)
				break;
			// node is the device node, show the USB serial number
			const std::string usb = GetUsbSerialForDevnode(node);
			g_Stats.devices[di].serial = usb.empty() ? node : usb;
			g_Stats.devices[di].type = IsDV3000(desc) ? "DV3000" : "DV3003";
			g_Stats.devices[di].role = role;
			g_Stats.devices[di].online.store(true);
			device->SetStatsIndex(di);
			di++;
		}
		g_Stats.num_devices = di;
	}

	return false;
}

// Encapsulate the incoming STCPacket into a CTranscoderPacket and push it into the appropriate queue
// based on packet's codec_in.
void CController::ReadReflectorThread()
{
	uint32_t reconf_counter = 0;
	while (keep_running)
	{
		// preemptively check the connection(s)... (a standby module waits while a
		// stream runs here, see CTCClient::PauseStandby)
		if (! g_Conf.GetStandbyMods().empty())
		{
			bool streaming = false;
			for (char mod : g_Conf.GetTCMods())
				if (mod >= 'A' && mod <= 'Z' && g_Stats.modules[mod - 'A'].last_activity.load(std::memory_order_relaxed) < 1.5f)
					streaming = true;
			tcClient.PauseStandby(streaming);
		}
		tcClient.ReConnect();

		// Every ~1s: re-read config + age idle modules
		if (++reconf_counter >= 10)
		{
			reconf_counter = 0;
			ReconfigureAGC();

			// Age idle timers and reset stale modules
			for (char mod : g_Conf.GetTCMods())
			{
				int idx = mod - 'A';
				if (idx < 0 || idx >= CTcdStats::MAX_MODULES) continue;
				auto &ms = g_Stats.modules[idx];
				float idle = ms.last_activity.load(std::memory_order_relaxed);
				if (idle < 1.0e6f)  // whole seconds stay exact in a float far beyond this
					ms.last_activity.store(idle + 1.0f, std::memory_order_relaxed);
				// Reset display after 2s of no packets
				if (idle >= 2.0f && ms.codec_in.load(std::memory_order_relaxed) != 0)
				{
					ms.codec_in.store(0, std::memory_order_relaxed);
					ms.stream_id.store(0, std::memory_order_relaxed);
					ms.rms_in.store(-100.0f, std::memory_order_relaxed);
					ms.peak_in.store(-100.0f, std::memory_order_relaxed);
					ms.rms_out.store(-100.0f, std::memory_order_relaxed);
					ms.peak_out.store(-100.0f, std::memory_order_relaxed);
					ms.agc_gain_db.store(0.0f, std::memory_order_relaxed);
					ms.agc_gate.store(false, std::memory_order_relaxed);
					ms.agc_gate_count.store(0, std::memory_order_relaxed);
				}
			}

			// Sync DVSI device stats (buf_depth, active channels)
			{
				auto now_ms = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now().time_since_epoch()).count();
				for (int di = 0; di < g_Stats.num_devices; di++)
				{
					auto &ds = g_Stats.devices[di];
					// Reset stale channels (2s timeout)
					for (int ch = 0; ch < DeviceStats::MAX_CHANNELS; ch++)
					{
						uint64_t last = ds.ch_last_ms[ch].load(std::memory_order_relaxed);
						if (last > 0 && (now_ms - last) > 2000)
							ds.ch_module[ch].store(' ', std::memory_order_relaxed);
					}
				}
				int di = 0;
				auto syncDev = [&](CDVDevice *dev) {
					if (!dev || di >= CTcdStats::MAX_DEVICES) return;
					g_Stats.devices[di].buf_depth.store(dev->GetBufferDepth(), std::memory_order_relaxed);
					di++;
				};
				if (dstar_device) syncDev(dstar_device.get());
				if (dmrsf_device) syncDev(dmrsf_device.get());
			}

			// Sync the stats of the codec plugins
			{
				auto now_ms = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now().time_since_epoch()).count();
				const bool reencode = dmr_sw && dmrsf_device &&
					(g_Stats.config.dmr_gain_mode.load(std::memory_order_relaxed) == 2) &&
					(m_agcDigital.IsEnabled() || outgain_dmr_num != 256 || dmr_reencode_num != 256);
				for (auto *choices : { &dstar_choices, &dmr_choices })
					for (auto *sw : *choices)
					{
						if (!sw || sw->stats < 0)
							continue;
						auto &st = g_Stats.sw[sw->stats];
						uint64_t last = st.last_active_ms.load(std::memory_order_relaxed);
						if (last > 0 && (now_ms - last) > 2000)
							st.active_module.store(' ', std::memory_order_relaxed);
						st.cached_streams.store(sw->Streams(), std::memory_order_relaxed);
						st.reencode_active.store(reencode && sw == dmr_sw, std::memory_order_relaxed);
					}
				PurgePinned();
			}
		}

		// Check if at least one module has an open FD
		{
			bool conn = false;
			for (char m : g_Conf.GetTCMods())
			{
				const bool linked = tcClient.GetFD(m) >= 0;
				conn = conn || linked;
				if (m >= 'A' && m <= 'Z')
					g_Stats.modules[m - 'A'].linked.store(linked, std::memory_order_relaxed);
			}
			g_Stats.reflector.connected.store(conn, std::memory_order_relaxed);
		}

		std::queue<std::unique_ptr<STCPacket>> queue;
		// wait up to 100 ms to read something on the unix port
		tcClient.Receive(queue, 100);
		// If no module is connected, poll returns instantly (all FDs -1).
		// Sleep to avoid busy-spinning the loop.
		if (queue.empty())
		{
			bool any_connected = false;
			for (char m : g_Conf.GetTCMods())
				if (tcClient.GetFD(m) >= 0) { any_connected = true; break; }
			if (!any_connected)
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
		while (! queue.empty())
		{
			// create a shared pointer to a new packet
			// there is only one CTranscoderPacket created for each new STCPacket received from the reflector
			auto packet = std::make_shared<CTranscoderPacket>(*queue.front());
			queue.pop();
			g_Stats.PacketIn(packet->GetModule(), packet->GetStreamId(), packet->GetCodecIn());
			g_Stats.reflector.packets_rx.fetch_add(1, std::memory_order_relaxed);
			// Idle reset is handled by 2s timeout in reconf loop
			// (not here, because the packet still flows through the pipeline
			// and Route functions would overwrite the reset values)
			switch (packet->GetCodecIn())
			{
			case ECodecType::dstar:
				ToDStar(packet);
				break;
			case ECodecType::dmr:
				ToDmr(packet);
				break;
			case ECodecType::p25:
				imbe_queue.push(packet);
				break;
			case ECodecType::usrp:
			case ECodecType::svx:
				usrp_queue.push(packet);
				break;
			case ECodecType::c2_1600:
			case ECodecType::c2_3200:
				codec2_queue.push(packet);
				break;
			default:
				Dump(packet, "ERROR: Received a reflector packet with unknown Codec:");
				break;
			}
		}
	}
}

// This is only called when codec_in was dstar or dmr. Obviously, the incoming
// ambe packet was already decoded to audio.
// This might complete the packet. If so, send it back to the reflector
void CController::AudiotoCodec2(std::shared_ptr<CTranscoderPacket> packet)
{
	int16_t gained[160];
	memcpy(gained, packet->GetAudioSamples(), sizeof(gained));
	ApplyGain(gained, 160, outgain_m17_num);

	uint8_t m17data[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0x00, 0x01, 0x43, 0x09, 0xe4, 0x9c, 0x08, 0x21 };
	const auto m = packet->GetModule();
	if (packet->IsSecond())
	{
		memcpy(m17data, data_store[packet->GetModule()], 8);
		c2_32[m]->codec2_encode(m17data+8, gained);
		packet->SetM17Data(m17data);
	}
	else
	{
		c2_32[m]->codec2_encode(m17data, gained);
		memcpy(data_store[packet->GetModule()], m17data, 8);
		packet->SetM17Data(m17data);
	}

	// we might be all done...
	send_mux.lock();
	if (packet->AllCodecsAreSet() && packet->HasNotBeenSent()) SendToReflector(packet);
	send_mux.unlock();
}

// The original incoming coded was M17, so we will calculate the audio and then
// push the packet onto both the dstar and the dmr queue.
void CController::Codec2toAudio(std::shared_ptr<CTranscoderPacket> packet)
{
	uint8_t imbe[11];

	if (packet->IsSecond())
	{
		if (packet->GetCodecIn() == ECodecType::c2_1600)
		{
			int16_t tmp[160];
			memcpy(tmp, audio_store[packet->GetModule()], sizeof(tmp));
			g_Stats.UpdateLevelsIn(packet->GetModule(), tmp, 160);
			float agc_db; bool agc_gate;
			m_agcDigital.Process(tmp, 160, packet->GetStreamId(), agc_db, agc_gate);
			g_Stats.UpdateAGC(packet->GetModule(), agc_db, agc_gate);
			packet->SetAudioSamples(tmp, false);
		}
		else /* codec_in is ECodecType::c2_3200 */
		{
			int16_t tmp[160];
			c2_32[packet->GetModule()]->codec2_decode(tmp, packet->GetM17Data()+8);
			g_Stats.UpdateLevelsIn(packet->GetModule(), tmp, 160);
			float agc_db; bool agc_gate;
			m_agcDigital.Process(tmp, 160, packet->GetStreamId(), agc_db, agc_gate);
			g_Stats.UpdateAGC(packet->GetModule(), agc_db, agc_gate);
			packet->SetAudioSamples(tmp, false);
		}
	}
	else /* it's a "first packet" */
	{
		const auto m = packet->GetModule();
		if (packet->GetCodecIn() == ECodecType::c2_1600)
		{
			int16_t tmp[320];
			c2_16[m]->codec2_decode(tmp, packet->GetM17Data());
			g_Stats.UpdateLevelsIn(packet->GetModule(), tmp, 160);
			float agc_db; bool agc_gate;
			m_agcDigital.Process(tmp, 160, packet->GetStreamId(), agc_db, agc_gate);
			g_Stats.UpdateAGC(packet->GetModule(), agc_db, agc_gate);
			packet->SetAudioSamples(tmp, false);
			// the second half goes through the AGC with the second packet
			memcpy(audio_store[packet->GetModule()], &(tmp[160]), 320);
		}
		else /* codec_in is ECodecType::c2_3200 */
		{
			int16_t tmp[160];
			c2_32[m]->codec2_decode(tmp, packet->GetM17Data());
			g_Stats.UpdateLevelsIn(packet->GetModule(), tmp, 160);
			float agc_db; bool agc_gate;
			m_agcDigital.Process(tmp, 160, packet->GetStreamId(), agc_db, agc_gate);
			g_Stats.UpdateAGC(packet->GetModule(), agc_db, agc_gate);
			packet->SetAudioSamples(tmp, false);
		}
	}
	g_Stats.UpdateLevelsOut(packet->GetModule(), packet->GetAudioSamples(), 160);
	{
		int i = packet->GetModule() - 'A';
		if (i >= 0 && i < CTcdStats::MAX_MODULES) {
			auto &ms = g_Stats.modules[i];
			g_StatsLog.LogFrame(packet->GetModule(), packet->GetStreamId(), "codec2",
				ms.rms_in.load(), ms.peak_in.load(), ms.rms_out.load(), ms.peak_out.load(),
				ms.agc_gain_db.load(), ms.agc_gate.load());
		}
	}
	// D-Star: DVSI (gain applied in FeedDevice) or a codec plugin
	ToDStar(packet);

	// DMR/YSF: DVSI (gain applied in FeedDevice) or a codec plugin
	ToDmr(packet);

	// IMBE/P25
	{
		int16_t imbe_buf[160];
		memcpy(imbe_buf, packet->GetAudioSamples(), sizeof(imbe_buf));
		ApplyGain(imbe_buf, 160, outgain_imbe_num);
		std::lock_guard<std::mutex> lock(p25vocoder_mux);
		p25vocoder.encode_4400(imbe_buf, imbe);
	}
	packet->SetP25Data(imbe);

	// USRP
	{
		int16_t usrp_buf[160];
		memcpy(usrp_buf, packet->GetAudioSamples(), sizeof(usrp_buf));
		ApplyGain(usrp_buf, 160, outgain_usrp_num);
		packet->SetUSRPData(usrp_buf);
	}
}

void CController::ProcessC2Thread()
{
	while (keep_running)
	{
		auto packet = codec2_queue.pop();

		switch (packet->GetCodecIn())
		{
			case ECodecType::c2_1600:
			case ECodecType::c2_3200:
				// this is an original M17 packet, so decode it to audio
				// Codec2toAudio will send it on for AMBE processing
				Codec2toAudio(packet);
				break;

			case ECodecType::dstar:
			case ECodecType::dmr:
			case ECodecType::p25:
			case ECodecType::usrp:
			case ECodecType::svx:
				// codec_in was AMBE, so we need to calculate the the M17 data
				AudiotoCodec2(packet);
				break;
		}
	}
}

void CController::AudiotoSWAMBE2(std::shared_ptr<CTranscoderPacket> packet)
{
	uint8_t ambe2[9] = {};
	int16_t gained[160];
	memcpy(gained, packet->GetAudioSamples(), sizeof(gained));
	ApplyGain(gained, 160, PcmDmrGainNum());
	if (ambe_in_num != 256)
	{
		for (int i = 0; i < 160; ++i)
			gained[i] = int16_t((gained[i] * ambe_in_num) >> 8);
	}
	auto *sw = SwCodec(false, packet);
	sw->Encode(packet->GetStreamId(), packet->GetSequence(), packet->IsLast(), gained, ambe2);
	SwStat(sw, packet->GetModule(), &SwVocoderStats::encodes);
	BitstreamDmrGain(ambe2, packet->GetStreamId());
	packet->SetDMRData(ambe2);

	// we might be all done...
	send_mux.lock();
	if (packet->AllCodecsAreSet() && packet->HasNotBeenSent()) SendToReflector(packet);
	send_mux.unlock();
}

void CController::SWAMBE2toAudio(std::shared_ptr<CTranscoderPacket> packet)
{
	int16_t tmp[160] = {};
	auto *sw = SwCodec(false, packet);
	sw->Decode(packet->GetStreamId(), packet->GetSequence(), packet->IsLast(), packet->GetDMRData(), tmp);
	SwStat(sw, packet->GetModule(), &SwVocoderStats::decodes);
	if (ambe_out_num != 256)
	{
		for (int i=0; i<160; i++)
			tmp[i] = (tmp[i] * ambe_out_num) >> 8;
	}
	g_Stats.UpdateLevelsIn(packet->GetModule(), tmp, 160);
	float agc_db = 0.0f;
	{
		bool agc_gate;
		m_agcDigital.Process(tmp, 160, packet->GetStreamId(), agc_db, agc_gate);
		g_Stats.UpdateAGC(packet->GetModule(), agc_db, agc_gate);
	}
	packet->SetAudioSamples(tmp, false);
	// Bitstream gain on DMR -> DMR/YSF, as in RouteDmrPacket
	if (g_Stats.config.dmr_gain_mode.load(std::memory_order_relaxed) == 1)
	{
		const float gain_db = agc_db + (float)g_Stats.config.ambe_gain_db.load(std::memory_order_relaxed)
			+ (float)g_Stats.config.outgain_dmr.load(std::memory_order_relaxed);
		uint8_t dmr_data[9];
		memcpy(dmr_data, packet->GetDMRData(), 9);
		m_ambeGain.Process(dmr_data, packet->GetStreamId(), gain_db);
		packet->SetDMRData(dmr_data);
	}
	g_Stats.UpdateLevelsOut(packet->GetModule(), tmp, 160);
	{
		int i = packet->GetModule() - 'A';
		if (i >= 0 && i < CTcdStats::MAX_MODULES) {
			auto &ms = g_Stats.modules[i];
			g_StatsLog.LogFrame(packet->GetModule(), packet->GetStreamId(), "dmr",
				ms.rms_in.load(), ms.peak_in.load(), ms.rms_out.load(), ms.peak_out.load(),
				ms.agc_gain_db.load(), ms.agc_gate.load());
		}
	}
	ToDStar(packet);
	codec2_queue.push(packet);
	imbe_queue.push(packet);
	usrp_queue.push(packet);
}

void CController::ProcessSWAMBE2Thread()
{
	while (keep_running)
	{
		auto packet = swambe2_queue.pop();

		switch (packet->GetCodecIn())
		{
			case ECodecType::c2_1600:
			case ECodecType::c2_3200:
			case ECodecType::dstar:
			case ECodecType::p25:
			case ECodecType::usrp:
			case ECodecType::svx:
				AudiotoSWAMBE2(packet);
				break;

			case ECodecType::dmr:
				SWAMBE2toAudio(packet);
				break;
		}
	}
}

// D-Star from a codec plugin, when no device does it

void CController::AudioToSWDStar(std::shared_ptr<CTranscoderPacket> packet)
{
	// OutputGainDStar and DStarGainIn, as a device applies them before its encoder
	int16_t pcm[160];
	memcpy(pcm, packet->GetAudioSamples(), sizeof(pcm));
	ApplyGain(pcm, 160, outgain_dstar_num);
	ApplyGain(pcm, 160, dstar_in_num);
	uint8_t frame[9] = {};
	auto *sw = SwCodec(true, packet);
	sw->Encode(packet->GetStreamId(), packet->GetSequence(), packet->IsLast(), pcm, frame);
	SwStat(sw, packet->GetModule(), &SwVocoderStats::encodes);
	packet->SetDStarData(frame);
	dstar_mux.lock();
	RouteDstPacket(packet);
	dstar_mux.unlock();
}

void CController::SWDStarToAudio(std::shared_ptr<CTranscoderPacket> packet)
{
	int16_t pcm[160] = {};
	auto *sw = SwCodec(true, packet);
	sw->Decode(packet->GetStreamId(), packet->GetSequence(), packet->IsLast(), packet->GetDStarData(), pcm);
	SwStat(sw, packet->GetModule(), &SwVocoderStats::decodes);
	ApplyGain(pcm, 160, dstar_out_num);
	packet->SetAudioSamples(pcm, false);
	dstar_mux.lock();
	RouteDstPacket(packet);
	dstar_mux.unlock();
}

void CController::ProcessSWDStarThread()
{
	while (keep_running)
	{
		auto packet = swdstar_queue.pop();
		if (ECodecType::dstar == packet->GetCodecIn())
			SWDStarToAudio(packet);
		else
			AudioToSWDStar(packet);
	}
}

// the choice of a stream, made at its first packet: the one for new streams, but a
// module the devices do not serve always goes to a plugin (the chosen one, else the first)
int CController::Pick(bool dstar, uint16_t streamid, char module)
{
	auto &choices = dstar ? dstar_choices : dmr_choices;
	auto &pinned = dstar ? dstar_pinned : dmr_pinned;
	std::lock_guard<std::mutex> lock(pinned_mux);
	const auto now = std::chrono::steady_clock::now();
	auto it = pinned.find(streamid);
	if (it == pinned.end())
	{
		int c = (dstar ? g_Stats.config.codec_dstar : g_Stats.config.codec_dmr).load(std::memory_order_relaxed);
		if (c < 0 || c >= int(choices.size()))
			c = 0;
		if (!choices[c] && std::string::npos == (dstar ? dstar_hw_modules : dmr_hw_modules).find(module))
			for (size_t i = 0; i < choices.size(); i++)
				if (choices[i])
				{
					c = int(i);
					break;
				}
		it = pinned.emplace(streamid, SPinned{ c, now }).first;
	}
	it->second.last = now;
	return it->second.choice;
}

// streams quiet for 30 s are forgotten
void CController::PurgePinned(void)
{
	std::lock_guard<std::mutex> lock(pinned_mux);
	const auto cutoff = std::chrono::steady_clock::now() - std::chrono::seconds(30);
	for (auto *pinned : { &dstar_pinned, &dmr_pinned })
		for (auto it = pinned->begin(); it != pinned->end(); )
			it = it->second.last < cutoff ? pinned->erase(it) : std::next(it);
}

void CController::ToDStar(std::shared_ptr<CTranscoderPacket> packet)
{
	if (SwCodec(true, packet))
		swdstar_queue.push(packet);
	else if (mixed_mode && packet->GetModule() == mixed_dstar_module)
		mixed_dv3003->AddPacketToChannel(packet, 0);	// ch0 = D-Star
	else
		dstar_device->AddPacket(packet);
}

void CController::ToDmr(std::shared_ptr<CTranscoderPacket> packet)
{
	if (SwCodec(false, packet))
		swambe2_queue.push(packet);
	else if (mixed_mode)
	{
		const auto pos = std::string(g_Conf.GetTCMods()).find(packet->GetModule());
		if (pos != std::string::npos)
			mixed_dv3003->AddPacketToChannel(packet, (pos == 0) ? 1 : 2);	// ch1/ch2 = DMR of module A/B
	}
	else
		dmrsf_device->AddPacket(packet);
}

void CController::AudiotoIMBE(std::shared_ptr<CTranscoderPacket> packet)
{
	uint8_t imbe[11];
	int16_t gained[160];
	memcpy(gained, packet->GetAudioSamples(), sizeof(gained));
	ApplyGain(gained, 160, outgain_imbe_num);

	{
		std::lock_guard<std::mutex> lock(p25vocoder_mux);
		p25vocoder.encode_4400(gained, imbe);
	}
	packet->SetP25Data(imbe);
	// we might be all done...
	send_mux.lock();
	if (packet->AllCodecsAreSet() && packet->HasNotBeenSent()) SendToReflector(packet);
	send_mux.unlock();
}

void CController::IMBEtoAudio(std::shared_ptr<CTranscoderPacket> packet)
{
	int16_t tmp[160] = { 0 };
	{
		std::lock_guard<std::mutex> lock(p25vocoder_mux);
		p25vocoder.decode_4400(tmp, (uint8_t*)packet->GetP25Data());
	}
	g_Stats.UpdateLevelsIn(packet->GetModule(), tmp, 160);
	{
		float agc_db; bool agc_gate;
		m_agcDigital.Process(tmp, 160, packet->GetStreamId(), agc_db, agc_gate);
		g_Stats.UpdateAGC(packet->GetModule(), agc_db, agc_gate);
	}
	packet->SetAudioSamples(tmp, false);
	g_Stats.UpdateLevelsOut(packet->GetModule(), tmp, 160);
	{
		int i = packet->GetModule() - 'A';
		if (i >= 0 && i < CTcdStats::MAX_MODULES) {
			auto &ms = g_Stats.modules[i];
			g_StatsLog.LogFrame(packet->GetModule(), packet->GetStreamId(), "imbe",
				ms.rms_in.load(), ms.peak_in.load(), ms.rms_out.load(), ms.peak_out.load(),
				ms.agc_gain_db.load(), ms.agc_gate.load());
		}
	}
	ToDStar(packet);
	codec2_queue.push(packet);

	ToDmr(packet);

	usrp_queue.push(packet);
}

void CController::ProcessIMBEThread()
{
	while (keep_running)
	{
		auto packet = imbe_queue.pop();

		switch (packet->GetCodecIn())
		{
			case ECodecType::c2_1600:
			case ECodecType::c2_3200:
			case ECodecType::dstar:
			case ECodecType::dmr:
			case ECodecType::usrp:
			case ECodecType::svx:
				AudiotoIMBE(packet);
				break;

			case ECodecType::p25:
				IMBEtoAudio(packet);
				break;
		}
	}
}

void CController::AudiotoUSRP(std::shared_ptr<CTranscoderPacket> packet)
{
	int16_t tmp[160];
	memcpy(tmp, packet->GetAudioSamples(), sizeof(tmp));
	ApplyGain(tmp, 160, outgain_usrp_num);
	packet->SetUSRPData(tmp);


	// we might be all done...
	send_mux.lock();
	if (packet->AllCodecsAreSet() && packet->HasNotBeenSent()) SendToReflector(packet);
	send_mux.unlock();
}

void CController::USRPtoAudio(std::shared_ptr<CTranscoderPacket> packet)
{
	int16_t tmp[160];
	memcpy(tmp, packet->GetUSRPData(), sizeof(tmp));
	g_Stats.UpdateLevelsIn(packet->GetModule(), tmp, 160);
	{
		float agc_db; bool agc_gate;
		m_nsAnalog.Process(tmp, packet->GetModule());
		m_agcAnalog.Process(tmp, 160, packet->GetStreamId(), agc_db, agc_gate);
		g_Stats.UpdateAGC(packet->GetModule(), agc_db, agc_gate);
	}
	packet->SetAudioSamples(tmp, false);
	g_Stats.UpdateLevelsOut(packet->GetModule(), tmp, 160);
	{
		int i = packet->GetModule() - 'A';
		if (i >= 0 && i < CTcdStats::MAX_MODULES) {
			auto &ms = g_Stats.modules[i];
			g_StatsLog.LogFrame(packet->GetModule(), packet->GetStreamId(), "usrp",
				ms.rms_in.load(), ms.peak_in.load(), ms.rms_out.load(), ms.peak_out.load(),
				ms.agc_gain_db.load(), ms.agc_gate.load());
		}
	}
	ToDStar(packet);
	codec2_queue.push(packet);

	ToDmr(packet);

	imbe_queue.push(packet);
}

void CController::ProcessUSRPThread()
{
	while (keep_running)
	{
		auto packet = usrp_queue.pop();

		switch (packet->GetCodecIn())
		{
			case ECodecType::c2_1600:
			case ECodecType::c2_3200:
			case ECodecType::dstar:
			case ECodecType::dmr:
			case ECodecType::p25:
				AudiotoUSRP(packet);
				break;

			case ECodecType::usrp:
				USRPtoAudio(packet);
				break;

			case ECodecType::svx:
				SvxToAudio(packet);
				break;
		}
	}
}

void CController::SvxToAudio(std::shared_ptr<CTranscoderPacket> packet)
{
	const int16_t *p = packet->GetUSRPData();
	int16_t tmp[160];
	memcpy(tmp, p, sizeof(tmp));

	g_Stats.UpdateLevelsIn(packet->GetModule(), tmp, 160);
	{
		float agc_db; bool agc_gate;
		m_nsAnalog.Process(tmp, packet->GetModule());
		m_agcAnalog.Process(tmp, 160, packet->GetStreamId(), agc_db, agc_gate);
		g_Stats.UpdateAGC(packet->GetModule(), agc_db, agc_gate);
	}
	packet->SetAudioSamples(tmp, false);
	g_Stats.UpdateLevelsOut(packet->GetModule(), tmp, 160);
	{
		int i = packet->GetModule() - 'A';
		if (i >= 0 && i < CTcdStats::MAX_MODULES) {
			auto &ms = g_Stats.modules[i];
			g_StatsLog.LogFrame(packet->GetModule(), packet->GetStreamId(), "svx",
				ms.rms_in.load(), ms.peak_in.load(), ms.rms_out.load(), ms.peak_out.load(),
				ms.agc_gain_db.load(), ms.agc_gate.load());
		}
	}
	ToDStar(packet);
	codec2_queue.push(packet);

	ToDmr(packet);

	imbe_queue.push(packet);
}

void CController::SendToReflector(std::shared_ptr<CTranscoderPacket> packet)
{
	constexpr int max_retries = 5;
	for (int attempt = 0; attempt < max_retries; ++attempt)
	{
		if (!tcClient.Send(packet->GetTCPacket()))
		{
			packet->Sent();
			g_Stats.PacketOut(packet->GetModule());
			g_Stats.reflector.packets_tx.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		std::cerr << "SendToReflector: send failed, attempt " << (attempt + 1) << "/" << max_retries << std::endl;
		tcClient.ReConnect();
		std::this_thread::sleep_for(std::chrono::milliseconds(100 * (1 << attempt)));
	}
	std::cerr << "SendToReflector: dropping packet after " << max_retries << " retries" << std::endl;
	packet->Sent();
}

void CController::RouteDstPacket(std::shared_ptr<CTranscoderPacket> packet)
{
	if (ECodecType::dstar == packet->GetCodecIn())
	{
		// codec_in is dstar, the audio has just completed — apply AGC before encoding
		g_Stats.UpdateLevelsIn(packet->GetModule(), packet->GetAudioSamples(), 160);
		if (m_agcDigital.IsEnabled())
		{
			int16_t tmp[160];
			float agc_db; bool agc_gate;
			memcpy(tmp, packet->GetAudioSamples(), sizeof(tmp));
			m_agcDigital.Process(tmp, 160, packet->GetStreamId(), agc_db, agc_gate);
			packet->SetAudioSamples(tmp, false);
			g_Stats.UpdateAGC(packet->GetModule(), agc_db, agc_gate);
		}
		g_Stats.UpdateLevelsOut(packet->GetModule(), packet->GetAudioSamples(), 160);
		{
			int i = packet->GetModule() - 'A';
			if (i >= 0 && i < CTcdStats::MAX_MODULES) {
				auto &ms = g_Stats.modules[i];
				g_StatsLog.LogFrame(packet->GetModule(), packet->GetStreamId(), "dstar",
					ms.rms_in.load(), ms.peak_in.load(), ms.rms_out.load(), ms.peak_out.load(),
					ms.agc_gain_db.load(), ms.agc_gate.load());
			}
		}
		// Output gains applied in encode functions and DVSI FeedDevice
		codec2_queue.push(packet);
		imbe_queue.push(packet);
		usrp_queue.push(packet);
		ToDmr(packet);
	}
	else
	{
		send_mux.lock();
		if (packet->AllCodecsAreSet() && packet->HasNotBeenSent()) SendToReflector(packet);
		send_mux.unlock();
	}
}

void CController::RouteDmrPacket(std::shared_ptr<CTranscoderPacket> packet)
{
	if (ECodecType::dmr == packet->GetCodecIn())
	{
		// codec_in is dmr (hardware decode), apply AGC before encoding
		g_Stats.UpdateLevelsIn(packet->GetModule(), packet->GetAudioSamples(), 160);
		float agc_db = 0.0f;
		if (m_agcDigital.IsEnabled())
		{
			int16_t tmp[160];
			bool agc_gate;
			memcpy(tmp, packet->GetAudioSamples(), sizeof(tmp));
			m_agcDigital.Process(tmp, 160, packet->GetStreamId(), agc_db, agc_gate);
			packet->SetAudioSamples(tmp, false);
			g_Stats.UpdateAGC(packet->GetModule(), agc_db, agc_gate);
		}
		g_Stats.UpdateLevelsOut(packet->GetModule(), packet->GetAudioSamples(), 160);
		{
			int i = packet->GetModule() - 'A';
			if (i >= 0 && i < CTcdStats::MAX_MODULES) {
				auto &ms = g_Stats.modules[i];
				g_StatsLog.LogFrame(packet->GetModule(), packet->GetStreamId(), "dmr",
					ms.rms_in.load(), ms.peak_in.load(), ms.rms_out.load(), ms.peak_out.load(),
					ms.agc_gain_db.load(), ms.agc_gate.load());
			}
		}
		// Output gains applied in encode functions and DVSI FeedDevice
		// Re-encode DMR from AGC'd PCM with the codec plugin
		// Always re-encode when AGC or OutputGainDMR is active, otherwise the
		// original (un-gained) AMBE data passes through unchanged
		if (dmr_sw && (g_Stats.config.dmr_gain_mode.load(std::memory_order_relaxed) == 2) &&
		    (m_agcDigital.IsEnabled() || outgain_dmr_num != 256 || dmr_reencode_num != 256))
		{
			uint8_t ambe2[9];
			const int16_t *pcm = packet->GetAudioSamples();
			int16_t tmp[160];
			memcpy(tmp, pcm, sizeof(tmp));
			ApplyGain(tmp, 160, outgain_dmr_num);
			if (dmr_reencode_num != 256)
			{
				for (int i = 0; i < 160; i++)
					tmp[i] = (int16_t)((tmp[i] * dmr_reencode_num) >> 8);
			}
			// the plugin keeps a state per stream; on its error the original frame stays
			if (dmr_sw->Encode(packet->GetStreamId(), packet->GetSequence(), packet->IsLast(), tmp, ambe2))
				packet->SetDMRData(ambe2);
			SwStat(dmr_sw, packet->GetModule(), &SwVocoderStats::reencodes);
		}
		// Bitstream gain: the AGC's gain for this frame plus AmbeGainDb and
		// OutputGainDMR, applied through b2 (delta-gamma) in the AMBE2+ frame
		if (g_Stats.config.dmr_gain_mode.load(std::memory_order_relaxed) == 1)
		{
			const float gain_db = agc_db + (float)g_Stats.config.ambe_gain_db.load(std::memory_order_relaxed)
				+ (float)g_Stats.config.outgain_dmr.load(std::memory_order_relaxed);
			uint8_t dmr_data[9];
			memcpy(dmr_data, packet->GetDMRData(), 9);
			m_ambeGain.Process(dmr_data, packet->GetStreamId(), gain_db);
			packet->SetDMRData(dmr_data);
		}
		codec2_queue.push(packet);
		imbe_queue.push(packet);
		usrp_queue.push(packet);
		ToDStar(packet);
	}
	else
	{
		// the DVSI encoded audio of another codec: OutputGainDMR in the bitstream
		if (BitstreamDmrDb() != 0)
		{
			uint8_t dmr_data[9];
			memcpy(dmr_data, packet->GetDMRData(), 9);
			BitstreamDmrGain(dmr_data, packet->GetStreamId());
			packet->SetDMRData(dmr_data);
		}
		send_mux.lock();
		if (packet->AllCodecsAreSet() && packet->HasNotBeenSent()) SendToReflector(packet);
		send_mux.unlock();
	}
}

// OutputGainDMR for audio that is encoded to DMR (not DMR -> DMR, which has
// its own path), split in two: OutputGainDMRBitstream dB of it through b2 after
// the encoder, the rest as a PCM gain before it. The encoder works better with
// more level, but b2 moves quiet frames in coarse steps, so all of it in b2 can
// leave artifacts
int CController::BitstreamDmrDb(void) const
{
	const int total = g_Stats.config.outgain_dmr.load(std::memory_order_relaxed);
	const int bs = g_Stats.config.outgain_dmr_bitstream.load(std::memory_order_relaxed);
	// never more than OutputGainDMR itself, and only an attenuation
	return (total >= 0) ? 0 : std::max(bs, total);
}

int32_t CController::PcmDmrGainNum(void) const
{
	const int bs = BitstreamDmrDb();
	return bs ? calcNumerator(g_Stats.config.outgain_dmr.load(std::memory_order_relaxed) - bs) : outgain_dmr_num;
}

void CController::BitstreamDmrGain(uint8_t *ambe2, uint16_t streamId)
{
	const int bs = BitstreamDmrDb();
	if (bs != 0)
		m_ambeGain.Process(ambe2, streamId, (float)bs, 1.2f);  // at most 1.2 dB per frame: no crackling on quiet frames
}

void CController::Dump(const std::shared_ptr<CTranscoderPacket> p, const std::string &title) const
{
	std::stringstream line;
	line << title << " Mod='" << p->GetModule() << "' SID=" << std::showbase << std::hex << ntohs(p->GetStreamId()) << std::noshowbase;

	ECodecType in = p->GetCodecIn();
	if (p->DStarIsSet())
		line << " DStar";
	if (ECodecType::dstar == in)
		line << '*';
	if (p->DMRIsSet())
		line << " DMR";
	if (ECodecType::dmr == in)
		line << '*';
	if (p->M17IsSet())
		line << " M17";
	if (ECodecType::c2_1600 == in)
		line << "**";
	else if (ECodecType::c2_3200 == in)
		line << '*';
	if (p->P25IsSet())
		line << " P25";
	if (ECodecType::p25 == in)
		line << "*";
	if (p->USRPIsSet())
		line << " USRP";
	if (ECodecType::usrp == in)
		line << "*";
	if (ECodecType::svx == in)
		line << "(svx)";
	if (p->IsSecond())
		line << " IsSecond";
	if (p->IsLast())
		line << " IsLast";

	std::cout << line.str() << std::dec << std::endl;
}
