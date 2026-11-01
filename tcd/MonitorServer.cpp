// MonitorServer — HTTP dashboard + WebSocket live stats + TCP stats socket

#include <iostream>
#include <sstream>
#include <fstream>
#include <vector>
#include <cstring>
#include <strings.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <fcntl.h>
#include <cmath>

#include "MonitorServer.h"
#include "TcdStats.h"
#include "Configure.h"
#include "Version.h"
#include <libwebsockets.h>
#include <algorithm>
#include <chrono>
#include <thread>

extern CConfigure g_Conf;
extern CTcdStats g_Stats;
extern CMonitorServer g_Monitor;

// Embedded HTML dashboard (generated from monitor.html)
extern const char monitor_html[];
extern const unsigned int monitor_html_len;

// g_Monitor is defined in Main.cpp

CMonitorServer::CMonitorServer() {}

CMonitorServer::~CMonitorServer()
{
	Stop();
}

bool CMonitorServer::Start(const std::string &bind, uint16_t http_port, uint16_t stats_port, const std::string &password)
{
	m_Password = password;
	struct in_addr test;
	if (1 != inet_pton(AF_INET, bind.c_str(), &test))
	{
		std::cerr << "Monitor: MonitorBindAddress '" << bind << "' is not an IPv4 address, monitor not started" << std::endl;
		return false;
	}
	m_bind = bind;
	m_httpPort = http_port;
	m_statsPort = stats_port;
	m_running = true;

	m_httpFuture = std::async(std::launch::async, &CMonitorServer::HttpThread, this);
	m_statsFuture = std::async(std::launch::async, &CMonitorServer::StatsThread, this);

	std::cout << "Monitor: HTTP on " << m_bind << " port " << m_httpPort
	          << ", Stats on port " << m_statsPort << (m_Password.empty() ? "" : ", password required (tcdmon: not from 127.0.0.1)") << std::endl;
	return true;
}

void CMonitorServer::Stop()
{
	m_running = false;
	// the service thread waits in lws_service; the push timer wakes it within 200 ms
	if (m_httpFuture.valid())
		m_httpFuture.get();
	if (m_statsFuture.valid())
		m_statsFuture.get();
}

////////////////////////////////////////////////////////////////////////////////
// HTTP + WebSocket thread (libwebsockets)

static const char *s_codec_names[] = {
	"none", "D-Star", "DMR/YSF", "Codec2 1600", "Codec2 3200", "IMBE (P25, YSF VW)", "USRP", "SVX"
};

static const char *codec_name(uint8_t c)
{
	return (c < 8) ? s_codec_names[c] : "unknown";
}

std::string CMonitorServer::BuildStatsJson()
{
	std::ostringstream js;
	js << std::fixed;
	js.precision(1);

	js << "{\"version\":\"" TCD_VERSION "\",\"modules\":{";
	bool first = true;
	for (char mod : g_Stats.module_letters)
	{
		int idx = mod - 'A';
		if (idx < 0 || idx >= CTcdStats::MAX_MODULES) continue;
		auto &m = g_Stats.modules[idx];

		if (!first) js << ",";
		first = false;

		js << "\"" << mod << "\":{"
		   << "\"codec_in\":\"" << codec_name(m.codec_in.load(std::memory_order_relaxed)) << "\""
		   << ",\"last_codec\":\"" << codec_name(m.last_codec.load(std::memory_order_relaxed)) << "\""
		   << ",\"stream_id\":" << m.stream_id.load(std::memory_order_relaxed)
		   << ",\"rms_in\":" << m.rms_in.load(std::memory_order_relaxed)
		   << ",\"peak_in\":" << m.peak_in.load(std::memory_order_relaxed)
		   << ",\"rms_out\":" << m.rms_out.load(std::memory_order_relaxed)
		   << ",\"peak_out\":" << m.peak_out.load(std::memory_order_relaxed)
		   << ",\"agc_gain\":" << m.agc_gain_db.load(std::memory_order_relaxed)
		   << ",\"agc_gate\":" << (m.agc_gate.load(std::memory_order_relaxed) ? "true" : "false")
		   << ",\"pkts_in\":" << m.packets_in.load(std::memory_order_relaxed)
		   << ",\"pkts_out\":" << m.packets_out.load(std::memory_order_relaxed)
		   << ",\"idle\":" << m.last_activity.load(std::memory_order_relaxed)
		   << ",\"linked\":" << (m.linked.load(std::memory_order_relaxed) ? "true" : "false")
		   << ",\"standby\":" << (m.standby ? "true" : "false")
		   << "}";
	}
	js << "},\"devices\":[";
	for (int i = 0; i < g_Stats.num_devices; i++)
	{
		auto &d = g_Stats.devices[i];
		if (i > 0) js << ",";
		js << "{\"serial\":\"" << d.serial << "\""
		   << ",\"type\":\"" << d.type << "\""
		   << ",\"role\":\"" << d.role << "\""
		   << ",\"buf_depth\":" << d.buf_depth.load(std::memory_order_relaxed)
		   << ",\"errors\":" << d.errors.load(std::memory_order_relaxed)
		   << ",\"online\":" << (d.online.load(std::memory_order_relaxed) ? "true" : "false")
		   << ",\"phase\":" << d.phase.load(std::memory_order_relaxed)
		   << ",\"want_out\":" << (d.want_out.load(std::memory_order_relaxed) ? "true" : "false")
		   << ",\"can_out\":" << (d.can_out.load() ? "true" : "false")
		   << ",\"channels\":[";
		for (int ch = 0; ch < DeviceStats::MAX_CHANNELS; ch++)
		{
			char mod = d.ch_module[ch].load(std::memory_order_relaxed);
			if (ch > 0) js << ",";
			js << "{\"module\":\"" << (mod != ' ' ? std::string(1, mod) : "") << "\"}";
		}
		js << "]}";

	}
	// the codecs in software: of codec plugins and tcd's own (OP25, Codec2)
	js << "],\"swvocoders\":[";
	{
		bool first = true;
		for (int i = 0; i < g_Stats.num_sw; i++)
		{
			const auto *sw = &g_Stats.sw[i];
			const char mod = sw->active_module.load(std::memory_order_relaxed);
			js << (first ? "" : ",") << "{\"codec\":\"" << sw->codec << "\""
			   << ",\"name\":\"" << sw->name << "\""
			   << ",\"builtin\":" << (sw->builtin ? "true" : "false")
			   << ",\"reencode_active\":" << (sw->reencode_active.load(std::memory_order_relaxed) ? "true" : "false")
			   << ",\"encodes\":" << sw->encodes.load(std::memory_order_relaxed)
			   << ",\"decodes\":" << sw->decodes.load(std::memory_order_relaxed)
			   << ",\"reencodes\":" << sw->reencodes.load(std::memory_order_relaxed)
			   << ",\"error_frames\":" << sw->error_frames.load(std::memory_order_relaxed)
			   << ",\"repeats\":" << sw->repeats.load(std::memory_order_relaxed)
			   << ",\"mutes\":" << sw->mutes.load(std::memory_order_relaxed)
			   << ",\"cached_streams\":" << sw->cached_streams.load(std::memory_order_relaxed)
			   << ",\"active_module\":\"" << (mod != ' ' ? std::string(1, mod) : "") << "\"}";
			first = false;
		}
	}
	// what does each vocoder choice (D-Star, DMR, P25, M17) for new streams, and what could
	js << "],\"codecs\":{";
	for (int k = 0; k < kNumChoices; k++)
	{
		const auto &options = g_Stats.codec_options[k];
		js << (k ? "," : "") << "\"" << g_CodecChoices[k].key << "\":{\"options\":[";
		for (size_t i = 0; i < options.size(); i++)
			js << (i ? "," : "") << "\"" << options[i] << "\"";
		const int c = g_Stats.config.codec_choice[k].load(std::memory_order_relaxed);
		js << "],\"use\":\"" << (c >= 0 && c < int(options.size()) ? options[c] : std::string()) << "\"}";
	}
	js << "},\"reencode_available\":" << (g_Stats.reencode_available.load(std::memory_order_relaxed) ? "true" : "false");
	js << ",\"pool_available\":" << (g_Stats.pool_available.load(std::memory_order_relaxed) ? "true" : "false");
	js << ",\"hw_channels\":{\"dstar\":{\"total\":" << g_Stats.hw_total[kChoiceDStar].load() << ",\"busy\":" << g_Stats.hw_busy[kChoiceDStar].load()
	   << "},\"dmr\":{\"total\":" << g_Stats.hw_total[kChoiceDMR].load() << ",\"busy\":" << g_Stats.hw_busy[kChoiceDMR].load() << "}}";
	js << ",\"reflector\":{"
	   << "\"connected\":" << (g_Stats.reflector.connected.load(std::memory_order_relaxed) ? "true" : "false")
	   << ",\"pkts_rx\":" << g_Stats.reflector.packets_rx.load(std::memory_order_relaxed)
	   << ",\"pkts_tx\":" << g_Stats.reflector.packets_tx.load(std::memory_order_relaxed)
	   << "},\"config\":{"
	   << "\"monitor_view\":" << g_Stats.config.monitor_view.load()
	   << ",\"agc_enabled\":" << (g_Stats.config.agc_enabled.load() ? "true" : "false")
	   << ",\"agc_target\":" << g_Stats.config.agc_target.load()
	   << ",\"agc_attack\":" << g_Stats.config.agc_attack.load()
	   << ",\"agc_release\":" << g_Stats.config.agc_release.load()
	   << ",\"agc_maxgain_down\":" << g_Stats.config.agc_maxgain_down.load()
	   << ",\"agc_digital_noisegate\":" << g_Stats.config.agc_digital_noisegate.load()
	   << ",\"agc_digital_maxgain_up\":" << g_Stats.config.agc_digital_maxgain_up.load()
	   << ",\"agc_digital_gatefloor\":" << g_Stats.config.agc_digital_gatefloor.load()
	   << ",\"agc_analog_noisegate\":" << g_Stats.config.agc_analog_noisegate.load()
	   << ",\"agc_analog_maxgain_up\":" << g_Stats.config.agc_analog_maxgain_up.load()
	   << ",\"agc_analog_gatefloor\":" << g_Stats.config.agc_analog_gatefloor.load()
	   << ",\"agc_analog_noisesuppress\":" << g_Stats.config.agc_analog_noisesuppress.load()
	   << ",\"gain_dstar_in\":" << g_Stats.config.gain_dstar_in.load()
	   << ",\"gain_dstar_out\":" << g_Stats.config.gain_dstar_out.load()
	   << ",\"gain_dmr_in\":" << g_Stats.config.gain_dmr_in.load()
	   << ",\"gain_dmr_out\":" << g_Stats.config.gain_dmr_out.load()
	   << ",\"gain_dmr_reencode\":" << g_Stats.config.gain_dmr_reencode.load()
	   << ",\"outgain_dstar\":" << g_Stats.config.outgain_dstar.load()
	   << ",\"outgain_dmr\":" << g_Stats.config.outgain_dmr.load()
	   << ",\"outgain_dmr_bitstream\":" << g_Stats.config.outgain_dmr_bitstream.load()
	   << ",\"outgain_usrp\":" << g_Stats.config.outgain_usrp.load()
	   << ",\"outgain_imbe\":" << g_Stats.config.outgain_imbe.load()
	   << ",\"outgain_m17\":" << g_Stats.config.outgain_m17.load()
	   << ",\"dmr_gain_mode\":" << g_Stats.config.dmr_gain_mode.load()
	   << ",\"ambe_gain_db\":" << g_Stats.config.ambe_gain_db.load()
	   << ",\"device_pool\":" << g_Stats.config.device_pool.load()
	   << "}}";

	return js.str();
}

// Simple JSON value parser (no library dependency)
static bool json_get_int(const char *json, size_t /*len*/, const char *key, int &val)
{
	char search[64];
	snprintf(search, sizeof(search), "\"%s\":", key);
	const char *p = strstr(json, search);
	if (!p) return false;
	p += strlen(search);
	while (*p == ' ') p++;
	val = atoi(p);
	return true;
}

static bool json_get_float(const char *json, size_t /*len*/, const char *key, float &val)
{
	char search[64];
	snprintf(search, sizeof(search), "\"%s\":", key);
	const char *p = strstr(json, search);
	if (!p) return false;
	p += strlen(search);
	while (*p == ' ') p++;
	val = strtof(p, nullptr);
	return true;
}

static bool json_get_bool(const char *json, size_t /*len*/, const char *key, bool &val)
{
	char search[64];
	snprintf(search, sizeof(search), "\"%s\":", key);
	const char *p = strstr(json, search);
	if (!p) return false;
	p += strlen(search);
	while (*p == ' ') p++;
	val = (*p == 't' || *p == 'T' || *p == '1');
	return true;
}

// a request to /api/..., the answer is JSON; the HTTP status
int CMonitorServer::HandleRest(const std::string &uri, const std::string &body, std::string &reply)
{
	if (uri == "/api/stats")
	{
		std::string json = g_Monitor.BuildStatsJson();
		reply = json;
		return 200;
	}
	else if (uri == "/api/gain" && !body.empty())
	{
		// POST {"type": "dstar_in", "db": 8}
		int db = 0;
		if (json_get_int(body.c_str(), body.size(), "db", db))
		{
			if (db < -24) db = -24;
			if (db > 24) db = 24;

			// Find which gain to set
			char search[64];
			snprintf(search, sizeof(search), "\"type\":\"");
			const char *p = strstr(body.c_str(), search);
			if (p)
			{
				p += strlen(search);
				if      (strncmp(p, "dstar_in", 8) == 0)      g_Stats.config.gain_dstar_in.store(db);
				else if (strncmp(p, "dstar_out", 9) == 0)     g_Stats.config.gain_dstar_out.store(db);
				else if (strncmp(p, "dmr_in", 6) == 0)        g_Stats.config.gain_dmr_in.store(db);
				else if (strncmp(p, "dmr_out", 7) == 0)       g_Stats.config.gain_dmr_out.store(db);
				else if (strncmp(p, "dmr_reencode", 12) == 0)  g_Stats.config.gain_dmr_reencode.store(db);
				else if (strncmp(p, "outgain_dstar", 13) == 0) g_Stats.config.outgain_dstar.store(db);
				else if (strncmp(p, "outgain_dmr", 11) == 0)   g_Stats.config.outgain_dmr.store(db);
				else if (strncmp(p, "outgain_usrp", 12) == 0)  g_Stats.config.outgain_usrp.store(db);
				else if (strncmp(p, "outgain_imbe", 12) == 0)  g_Stats.config.outgain_imbe.store(db);
				else if (strncmp(p, "outgain_m17", 11) == 0)   g_Stats.config.outgain_m17.store(db);
			}
		}
		reply = "{\"status\":\"ok\"}";
		return 200;
	}
	else if (uri == "/api/agc" && !body.empty())
	{
		bool enabled;
		float target, attack, release;
		if (json_get_bool(body.c_str(), body.size(), "enabled", enabled))
			g_Stats.config.agc_enabled.store(enabled);
		int bitstream;
		if (json_get_int(body.c_str(), body.size(), "outgain_dmr_bitstream", bitstream))
			g_Stats.config.outgain_dmr_bitstream.store(std::clamp(bitstream, -24, 0));
		if (json_get_float(body.c_str(), body.size(), "target", target))
			g_Stats.config.agc_target.store(target);
		if (json_get_float(body.c_str(), body.size(), "attack", attack))
			g_Stats.config.agc_attack.store(attack);
		if (json_get_float(body.c_str(), body.size(), "release", release))
			g_Stats.config.agc_release.store(release);
		float maxgain_down;
		if (json_get_float(body.c_str(), body.size(), "maxgain_down", maxgain_down))
			g_Stats.config.agc_maxgain_down.store(maxgain_down);
		float digital;
		if (json_get_float(body.c_str(), body.size(), "digital_noisegate", digital))
			g_Stats.config.agc_digital_noisegate.store(digital);
		if (json_get_float(body.c_str(), body.size(), "digital_maxgain_up", digital))
			g_Stats.config.agc_digital_maxgain_up.store(digital);
		if (json_get_float(body.c_str(), body.size(), "digital_gatefloor", digital))
			g_Stats.config.agc_digital_gatefloor.store(std::min(digital, 0.0f));
		float analog;
		if (json_get_float(body.c_str(), body.size(), "analog_noisegate", analog))
			g_Stats.config.agc_analog_noisegate.store(analog);
		if (json_get_float(body.c_str(), body.size(), "analog_maxgain_up", analog))
			g_Stats.config.agc_analog_maxgain_up.store(analog);
		if (json_get_float(body.c_str(), body.size(), "analog_gatefloor", analog))
			g_Stats.config.agc_analog_gatefloor.store(std::min(analog, 0.0f));
		if (json_get_float(body.c_str(), body.size(), "analog_noisesuppress", analog))
			g_Stats.config.agc_analog_noisesuppress.store(std::clamp(analog, -40.0f, 0.0f));
		float dmr_gain_mode;
		if (json_get_float(body.c_str(), body.size(), "dmr_gain_mode", dmr_gain_mode))
		{
			int m = (int)dmr_gain_mode < 0 ? 0 : (int)dmr_gain_mode > 2 ? 2 : (int)dmr_gain_mode;
			if (m == 2 && !g_Stats.reencode_available.load())
				m = 1;  // re-encode needs a codec plugin for DMR, fall back to the bitstream gain
			g_Stats.config.dmr_gain_mode.store(m);
		}
		float ambe_db;
		if (json_get_float(body.c_str(), body.size(), "ambe_gain_db", ambe_db))
			g_Stats.config.ambe_gain_db.store((int)ambe_db < -30 ? -30 : (int)ambe_db > 10 ? 10 : (int)ambe_db);

		reply = "{\"status\":\"ok\"}";
		return 200;
	}
	else if (uri == "/api/codec" && !body.empty())
	{
		// POST {"codec": "dstar"|"dmr"|"p25"|"m17", "use": "dvsi"|"op25"|"codec2"|"<plugin>"}: for the streams that start from now on
		auto field = [&body](const char *name) {
			const std::string key = std::string("\"") + name + "\"";
			auto p = body.find(key);
			if (p == std::string::npos) return std::string();
			p = body.find('"', body.find(':', p + key.size()));
			if (p == std::string::npos) return std::string();
			const auto e = body.find('"', p + 1);
			return e == std::string::npos ? std::string() : body.substr(p + 1, e - p - 1);
		};
		const std::string codec = field("codec"), use = field("use");
		const int k = ChoiceOfKey(codec);
		int found = -1;
		if (k >= 0)
			for (size_t i = 0; i < g_Stats.codec_options[k].size(); i++)
				if (g_Stats.codec_options[k][i] == use)
					found = int(i);
		if (found < 0)
		{
			reply = "{\"status\":\"error\",\"message\":\"no such codec or choice\"}";
			return 400;
		}
		else
		{
			g_Stats.config.codec_choice[k].store(found);
			std::cout << "Monitor: new " << g_CodecChoices[k].label << " streams go to " << use << std::endl;
			reply = "{\"status\":\"ok\"}";
			return 200;
		}
	}
	else if (uri == "/api/pool" && !body.empty())
	{
		// POST {"pool": true|false}: DeviceChannels for the streams that start from now on; Save keeps it
		bool on;
		if (!json_get_bool(body.c_str(), body.size(), "pool", on))
		{
			reply = "{\"status\":\"error\",\"message\":\"pool: true or false expected\"}";
			return 400;
		}
		if (!g_Stats.pool_available.load())
		{
			reply = "{\"status\":\"error\",\"message\":\"no AMBE channels\"}";
			return 400;
		}
		g_Stats.config.device_pool.store(on ? 1 : 0);
		std::cout << "Monitor: new streams take " << (on ? "any free AMBE channel (pool)" : "the AMBE channels of their module (fixed)") << std::endl;
		reply = "{\"status\":\"ok\"}";
		return 200;
	}
	else if (uri == "/api/device" && !body.empty())
	{
		// POST {"device": <index in devices>, "out": true|false}: phase a device out (no new
		// streams, closed when its streams are done) or take it back; not kept by Save
		int i;
		bool out;
		if (!json_get_int(body.c_str(), body.size(), "device", i) || !json_get_bool(body.c_str(), body.size(), "out", out)
		    || i < 0 || i >= g_Stats.num_devices)
		{
			reply = "{\"status\":\"error\",\"message\":\"no such device\"}";
			return 400;
		}
		if (out && !g_Stats.devices[i].can_out.load())
		{
			reply = "{\"status\":\"error\",\"message\":\"no codec plugin takes its streams\"}";
			return 400;
		}
		g_Stats.devices[i].want_out.store(out);
		std::cout << "Monitor: " << g_Stats.devices[i].serial << (out ? " is to be phased out" : " is to be taken back") << std::endl;
		reply = "{\"status\":\"ok\"}";
		return 200;
	}
	else if (uri == "/api/view" && !body.empty())
	{
		// POST {"view": "auto"|"cards"|"compact"}: the module view for every browser; Save keeps it
		const int v = body.find("\"compact\"") != std::string::npos ? 2 : body.find("\"cards\"") != std::string::npos ? 1 : 0;
		g_Stats.config.monitor_view.store(v);
		reply = "{\"status\":\"ok\"}";
		return 200;
	}
	else if (uri == "/api/save")
	{
		const bool ok = g_Monitor.SaveConfig(g_Conf.GetIniPath());
		reply = std::string(ok ? "{\"status\":\"ok\",\"message\":\"saved to " : "{\"status\":\"error\",\"message\":\"failed to write ") + g_Conf.GetIniPath() + "\"}";
		return ok ? 200 : 500;
	}
	reply = "{\"status\":\"error\",\"message\":\"not found\"}";
	return 404;
}

////////////////////////////////////////////////////////////////////////////////
// MonitorPassword: a login gives a session cookie

#define SESSION_COOKIE   "tcd_session"
#define SESSION_DAYS     30
#define SESSION_MAX      64
#define LOGIN_TRIES      5      // failed logins from one address, then
#define LOGIN_PAUSE_S    60     // a pause of this many seconds
#define LOGIN_PEERS_MAX  4096   // addresses with failed logins kept
#define LOGIN_FORGET_S   600    // failed logins older than this many seconds do not count
#define AUTH_WAITING_MAX 8      // tcdmon connections in their password check at a time

// given against secret, in a time that does not depend on where they differ
static bool SameSecret(const std::string &given, const std::string &secret)
{
	size_t diff = given.size() ^ secret.size();
	for (size_t i = 0; i < given.size(); i++)
		diff |= (unsigned char)given[i] ^ (unsigned char)(i < secret.size() ? secret[i] : 0);
	return 0 == diff && !secret.empty();
}

static std::string NewToken(void)
{
	unsigned char raw[24];
	std::ifstream urandom("/dev/urandom", std::ios::binary);
	if (!urandom.read((char *)raw, sizeof(raw)))
		return std::string();
	static const char hex[] = "0123456789abcdef";
	std::string t;
	for (unsigned char c : raw)
	{
		t += hex[c >> 4];
		t += hex[c & 15];
	}
	return t;
}

// the value of a cookie in the request, empty when there is none
static std::string RequestCookie(struct lws *wsi, const char *name)
{
	// the whole header: behind a reverse proxy the cookies of the rest of the site come along
	const int len = lws_hdr_total_length(wsi, WSI_TOKEN_HTTP_COOKIE);
	if (len <= 0 || len > 16384)
		return std::string();
	std::vector<char> buf(size_t(len) + 1);
	if (lws_hdr_copy(wsi, buf.data(), len + 1, WSI_TOKEN_HTTP_COOKIE) <= 0)
		return std::string();
	const std::string all(buf.data()), key = std::string(name) + "=";
	size_t p = 0;
	while (p < all.size())
	{
		while (p < all.size() && (all[p] == ' ' || all[p] == ';'))
			p++;
		const size_t e = all.find(';', p);
		const std::string item = all.substr(p, e == std::string::npos ? std::string::npos : e - p);
		if (item.compare(0, key.size(), key) == 0)
			return item.substr(key.size());
		if (e == std::string::npos)
			break;
		p = e + 1;
	}
	return std::string();
}

// a string field of a small JSON body, with \" and \\ undone
static std::string JsonString(const std::string &body, const char *name)
{
	const std::string key = std::string("\"") + name + "\"";
	auto p = body.find(key);
	if (p == std::string::npos)
		return std::string();
	p = body.find('"', body.find(':', p + key.size()));
	if (p == std::string::npos)
		return std::string();
	std::string v;
	for (size_t i = p + 1; i < body.size() && body[i] != '"'; i++)
	{
		if (body[i] == '\\' && i + 1 < body.size())
			i++;
		v += body[i];
	}
	return v;
}

static std::string Peer(struct lws *wsi)
{
	char ip[64] = "";
	lws_get_peer_simple(wsi, ip, sizeof(ip));
	return ip;
}

// a reverse proxy on this host: its headers are trusted, nobody else's
static bool LoopbackPeer(const std::string &peer)
{
	return peer.compare(0, 4, "127.") == 0 || peer == "::1" || peer.compare(0, 11, "::ffff:127.") == 0;
}

// the address a login comes from: behind a reverse proxy on this host the last
// X-Forwarded-For entry, the one that proxy added, so one client's failed logins
// do not pause everybody's
static std::string PeerAddress(struct lws *wsi)
{
	const std::string peer = Peer(wsi);
	if (LoopbackPeer(peer))
	{
		char buf[256];
		if (lws_hdr_copy(wsi, buf, sizeof(buf), WSI_TOKEN_X_FORWARDED_FOR) > 0)
		{
			std::string fwd(buf);
			const auto comma = fwd.rfind(',');
			if (comma != std::string::npos)
				fwd.erase(0, comma + 1);
			fwd.erase(0, fwd.find_first_not_of(' '));
			fwd.erase(fwd.find_last_not_of(' ') + 1);
			if (!fwd.empty() && fwd.size() < 64)
				return fwd;
		}
	}
	return peer;
}

// the session cookie's attributes: Secure when a reverse proxy on this host says
// the page came over HTTPS (X-Forwarded-Proto), so the browser never sends it in
// the clear
static std::string CookieAttributes(struct lws *wsi)
{
	std::string attr = "; Path=/; HttpOnly; SameSite=Strict";
#if defined(LWS_WITH_CUSTOM_HEADERS)
	if (LoopbackPeer(Peer(wsi)))
	{
		char proto[16];
		int n = lws_hdr_custom_copy(wsi, proto, sizeof(proto), "x-forwarded-proto:", 18);
		if (n <= 0)
			n = lws_hdr_custom_copy(wsi, proto, sizeof(proto), "X-Forwarded-Proto:", 18);
		if (n == 5 && strncasecmp(proto, "https", 5) == 0)
			attr += "; Secure";
	}
#endif
	return attr;
}

// a valid session cookie, or no MonitorPassword
bool CMonitorServer::Authorized(struct lws *wsi)
{
	if (m_Password.empty())
		return true;
	const auto it = m_Tokens.find(RequestCookie(wsi, SESSION_COOKIE));
	if (it == m_Tokens.end())
		return false;
	const auto now = std::chrono::steady_clock::now();
	if (now - it->second > std::chrono::hours(24 * SESSION_DAYS))
	{
		m_Tokens.erase(it);
		return false;
	}
	it->second = now;	// sliding: 30 days after the last use
	return true;
}

// POST /api/login {"password": "..."}: a session cookie, or 401 (429 after too many tries)
int CMonitorServer::Login(struct lws *wsi, SSession &s)
{
	const auto now = std::chrono::steady_clock::now();
	const std::string peer = PeerAddress(wsi);
	// bounded: from the internet (a reverse proxy) the addresses do not end
	if (m_Fails.size() >= LOGIN_PEERS_MAX)
		for (auto it = m_Fails.begin(); it != m_Fails.end();)
			it = it->second.until > now ? std::next(it) : m_Fails.erase(it);
	auto &f = m_Fails[peer];
	if (now - f.last > std::chrono::seconds(LOGIN_FORGET_S))
		f.count = 0;	// the earlier failed logins are long ago
	if (f.until > now)
	{
		s.out = "{\"status\":\"error\",\"message\":\"too many failed logins, wait a minute\"}";
		return 429;
	}
	if (m_Password.empty() || !SameSecret(JsonString(s.body, "password"), m_Password))
	{
		f.last = now;
		if (++f.count >= LOGIN_TRIES)
		{
			f.count = 0;
			f.until = now + std::chrono::seconds(LOGIN_PAUSE_S);
			std::cerr << "Monitor: " << LOGIN_TRIES << " failed logins from " << peer << ", pausing logins from there for " << LOGIN_PAUSE_S << " s" << std::endl;
		}
		s.out = "{\"status\":\"error\",\"message\":\"wrong password\"}";
		return 401;
	}
	m_Fails.erase(peer);
	const std::string token = NewToken();
	if (token.empty())
	{
		s.out = "{\"status\":\"error\",\"message\":\"no random token\"}";
		return 500;
	}
	while (m_Tokens.size() >= SESSION_MAX)
	{
		auto oldest = m_Tokens.begin();
		for (auto it = m_Tokens.begin(); it != m_Tokens.end(); ++it)
			if (it->second < oldest->second)
				oldest = it;
		m_Tokens.erase(oldest);
	}
	m_Tokens[token] = now;
	s.cookie = std::string(SESSION_COOKIE "=") + token + CookieAttributes(wsi) + "; Max-Age=" + std::to_string(SESSION_DAYS * 86400);
	s.out = "{\"status\":\"ok\"}";
	return 200;
}

int CMonitorServer::Callback(struct lws *wsi, enum lws_callback_reasons reason, void *, void *in, size_t len)
{
	auto *self = static_cast<CMonitorServer *>(lws_context_user(lws_get_context(wsi)));
	if (!self)
		return 0;
	switch (reason)
	{
	case LWS_CALLBACK_HTTP:
	{
		auto &s = self->m_Sessions[wsi];
		s = SSession();
		s.uri.assign(static_cast<const char *>(in), len);
		char cl[16] = "";
		const bool post = lws_hdr_total_length(wsi, WSI_TOKEN_POST_URI) > 0;
		if (post && lws_hdr_copy(wsi, cl, sizeof(cl), WSI_TOKEN_HTTP_CONTENT_LENGTH) > 0 && atoi(cl) > 0)
			return 0;	// LWS_CALLBACK_HTTP_BODY follows
		return self->Respond(wsi);
	}
	case LWS_CALLBACK_HTTP_BODY:
	{
		auto &s = self->m_Sessions[wsi];
		if (s.body.size() + len > 65536)
			return -1;
		s.body.append(static_cast<const char *>(in), len);
		return 0;
	}
	case LWS_CALLBACK_HTTP_BODY_COMPLETION:
		return self->Respond(wsi);
	case LWS_CALLBACK_HTTP_WRITEABLE:
		return self->WriteBody(wsi);
	case LWS_CALLBACK_FILTER_PROTOCOL_CONNECTION:
		// the WebSocket only with a session when there is a MonitorPassword
		return self->Authorized(wsi) ? 0 : -1;
	case LWS_CALLBACK_ESTABLISHED:
		self->m_Sessions[wsi] = SSession();
		self->m_Sessions[wsi].ws = true;
		break;
	case LWS_CALLBACK_SERVER_WRITEABLE:
	{
		// the latest stats, every client gets the same
		if (self->m_Push.empty())
			break;
		std::vector<unsigned char> buf(LWS_PRE + self->m_Push.size());
		memcpy(buf.data() + LWS_PRE, self->m_Push.data(), self->m_Push.size());
		if (lws_write(wsi, buf.data() + LWS_PRE, self->m_Push.size(), LWS_WRITE_TEXT) < (int)self->m_Push.size())
			return -1;
		break;
	}
	case LWS_CALLBACK_EVENT_WAIT_CANCELLED:
		// woken by the push timer: stats to every WebSocket client
		if (self->m_PushDue.exchange(false))
		{
			bool any = false;
			for (const auto &[w, s] : self->m_Sessions)
				any = any || s.ws;
			if (any)
			{
				self->m_Push = self->BuildStatsJson();
				lws_callback_on_writable_all_protocol(lws_get_context(wsi), &self->m_Protocols[0]);
			}
		}
		break;
	case LWS_CALLBACK_WSI_DESTROY:
		self->m_Sessions.erase(wsi);
		break;
	default:
		break;
	}
	return 0;
}

// the response to the request of wsi: the page, or JSON from /api/...
int CMonitorServer::Respond(struct lws *wsi)
{
	auto &s = m_Sessions[wsi];
	if (s.responded)
		return 0;
	s.responded = true;
	int status = 200;
	const char *type = "application/json";
	if (s.uri == "/api/auth")
		s.out = std::string("{\"required\":") + (m_Password.empty() ? "false" : "true") + ",\"ok\":" + (Authorized(wsi) ? "true" : "false") + "}";
	else if (s.uri == "/api/login")
		status = Login(wsi, s);
	else if (s.uri == "/api/logout")
	{
		m_Tokens.erase(RequestCookie(wsi, SESSION_COOKIE));
		s.cookie = std::string(SESSION_COOKIE "=") + CookieAttributes(wsi) + "; Max-Age=0";
		s.out = "{\"status\":\"ok\"}";
	}
	else if (s.uri.compare(0, 5, "/api/") == 0)
	{
		if (Authorized(wsi))
			status = HandleRest(s.uri, s.body, s.out);
		else
		{
			status = 401;
			s.out = "{\"status\":\"error\",\"message\":\"login required\"}";
		}
	}
	else
	{
		s.out.assign(monitor_html, monitor_html_len);
		type = "text/html; charset=utf-8";
	}
	s.sent = 0;

	unsigned char headers[LWS_PRE + 1024];
	unsigned char *start = headers + LWS_PRE, *p = start, *end = headers + sizeof(headers) - 1;
	static const char cors[] = "*";
	if (lws_add_http_common_headers(wsi, (unsigned)status, type, (lws_filepos_t)s.out.size(), &p, end)
	 || (m_Password.empty() && lws_add_http_header_by_name(wsi, (const unsigned char *)"Access-Control-Allow-Origin:", (const unsigned char *)cors, sizeof(cors) - 1, &p, end))
	 || (!s.cookie.empty() && lws_add_http_header_by_name(wsi, (const unsigned char *)"Set-Cookie:", (const unsigned char *)s.cookie.c_str(), (int)s.cookie.size(), &p, end))
	 || lws_finalize_write_http_header(wsi, start, &p, end))
		return -1;
	lws_callback_on_writable(wsi);
	return 0;
}

int CMonitorServer::WriteBody(struct lws *wsi)
{
	auto it = m_Sessions.find(wsi);
	if (it == m_Sessions.end())
		return -1;
	auto &s = it->second;
	const size_t n = std::min<size_t>(s.out.size() - s.sent, 8192);
	const bool last = s.sent + n >= s.out.size();
	std::vector<unsigned char> buf(LWS_PRE + n);
	memcpy(buf.data() + LWS_PRE, s.out.data() + s.sent, n);
	if (lws_write(wsi, buf.data() + LWS_PRE, n, last ? LWS_WRITE_HTTP_FINAL : LWS_WRITE_HTTP) < (int)n)
		return -1;
	s.sent += n;
	if (!last)
	{
		lws_callback_on_writable(wsi);
		return 0;
	}
	// keep-alive: the connection waits for the next request
	return lws_http_transaction_completed(wsi) ? -1 : 0;
}

void CMonitorServer::HttpThread()
{
	memset(m_Protocols, 0, sizeof(m_Protocols));
	// the page, /api/... and the WebSocket /ws on one port; a browser asks for
	// no subprotocol, so its WebSocket gets this first one
	m_Protocols[0].name = "tcd-monitor";
	m_Protocols[0].callback = &CMonitorServer::Callback;
	lws_set_log_level(LLL_ERR, nullptr);

	struct lws_context_creation_info info;
	memset(&info, 0, sizeof(info));
	info.port = m_httpPort;
	info.iface = m_bind.c_str();
	info.protocols = m_Protocols;
	info.gid = -1;
	info.uid = -1;
	info.options = LWS_SERVER_OPTION_VALIDATE_UTF8;
	info.user = this;
	m_Ctx = lws_create_context(&info);
	if (!m_Ctx)
	{
		std::cerr << "Monitor: cannot start the HTTP server on " << m_bind << " port " << m_httpPort << std::endl;
		return;
	}

	// stats to the WebSocket clients every 200 ms: lws_cancel_service is the one
	// lws call allowed from another thread
	std::thread timer([this] {
		while (m_running)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
			m_PushDue = true;
			lws_cancel_service(m_Ctx);
		}
	});
	while (m_running)
		lws_service(m_Ctx, 0);
	timer.join();
	lws_context_destroy(m_Ctx);
	m_Ctx = nullptr;
	m_Sessions.clear();
}

////////////////////////////////////////////////////////////////////////////////
// Plain TCP stats socket (for tcdmon / scripting)

void CMonitorServer::StatsThread()
{
	int listenFd = socket(AF_INET, SOCK_STREAM, 0);
	if (listenFd < 0)
	{
		std::cerr << "Monitor: stats socket failed: " << strerror(errno) << std::endl;
		return;
	}

	int opt = 1;
	setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	// Non-blocking listen socket
	int flags = fcntl(listenFd, F_GETFL, 0);
	fcntl(listenFd, F_SETFL, flags | O_NONBLOCK);

	struct sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(m_statsPort);
	inet_pton(AF_INET, m_bind.c_str(), &addr.sin_addr);

	if (bind(listenFd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
	{
		std::cerr << "Monitor: stats bind failed on port " << m_statsPort << ": " << strerror(errno) << std::endl;
		close(listenFd);
		return;
	}

	listen(listenFd, 4);

	// Track connected stats clients; with a MonitorPassword one from another host
	// than 127.0.0.1 first gets "AUTH" and must answer "AUTH <password>" within 5 s
	std::vector<int> clients;
	struct SWaiting { int fd; std::string peer, line; std::chrono::steady_clock::time_point deadline; };
	std::vector<SWaiting> waiting;
	std::unordered_map<std::string, SFails> fails;

	while (m_running)
	{
		// Accept new connections (non-blocking)
		struct sockaddr_in clientAddr{};
		socklen_t addrLen = sizeof(clientAddr);
		int clientFd = accept(listenFd, (struct sockaddr *)&clientAddr, &addrLen);
		if (clientFd >= 0)
		{
			// Set non-blocking
			int fl = fcntl(clientFd, F_GETFL, 0);
			fcntl(clientFd, F_SETFL, fl | O_NONBLOCK);
			char peer[INET_ADDRSTRLEN] = "";
			inet_ntop(AF_INET, &clientAddr.sin_addr, peer, sizeof(peer));
			if (m_Password.empty() || 0 == strncmp(peer, "127.", 4))
				clients.push_back(clientFd);
			else
			{
				const auto f = fails.find(peer);
				if (f != fails.end() && f->second.until > std::chrono::steady_clock::now())
					close(clientFd);	// too many failed logins from there just now
				else if (waiting.size() >= AUTH_WAITING_MAX)
					close(clientFd);	// enough in their check already
				else
				{
					send(clientFd, "AUTH\n", 5, MSG_NOSIGNAL);
					waiting.push_back({ clientFd, peer, std::string(), std::chrono::steady_clock::now() + std::chrono::seconds(5) });
				}
			}
		}

		// the logins of the waiting clients
		for (auto w = waiting.begin(); w != waiting.end(); )
		{
			char b[256];
			ssize_t n;
			while (w->line.find('\n') == std::string::npos && (n = recv(w->fd, b, sizeof(b), MSG_DONTWAIT)) > 0 && w->line.size() < 1024)
				w->line.append(b, size_t(n));
			const auto nl = w->line.find('\n');
			if (nl == std::string::npos && std::chrono::steady_clock::now() < w->deadline && w->line.size() < 1024)
			{
				++w;
				continue;
			}
			std::string given = nl == std::string::npos ? std::string() : w->line.substr(0, nl);
			if (!given.empty() && given.back() == '\r')
				given.pop_back();
			if (given.compare(0, 5, "AUTH ") == 0 && SameSecret(given.substr(5), m_Password))
			{
				send(w->fd, "OK\n", 3, MSG_NOSIGNAL);
				clients.push_back(w->fd);
				fails.erase(w->peer);
			}
			else
			{
				send(w->fd, "DENIED\n", 7, MSG_NOSIGNAL);
				close(w->fd);
				auto &f = fails[w->peer];
				const auto now = std::chrono::steady_clock::now();
				if (now - f.last > std::chrono::seconds(LOGIN_FORGET_S))
					f.count = 0;	// the earlier failed logins are long ago
				f.last = now;
				if (++f.count >= LOGIN_TRIES)
				{
					f.count = 0;
					f.until = now + std::chrono::seconds(LOGIN_PAUSE_S);
					std::cerr << "Monitor: " << LOGIN_TRIES << " failed tcdmon logins from " << w->peer << ", pausing them for " << LOGIN_PAUSE_S << " s" << std::endl;
				}
			}
			w = waiting.erase(w);
		}

		if (!clients.empty())
		{
			// Push stats JSON to all connected clients
			std::string json = BuildStatsJson() + "\n";
			auto it = clients.begin();
			while (it != clients.end())
			{
				ssize_t n = send(*it, json.c_str(), json.size(), MSG_NOSIGNAL);
				if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
				{
					close(*it);
					it = clients.erase(it);
				}
				else
					++it;
			}

			// Drain any input from clients (commands)
			char buf[512];
			for (int fd : clients)
				while (recv(fd, buf, sizeof(buf), MSG_DONTWAIT) > 0) {}
		}

		// 200ms push interval
		usleep(200000);
	}

	for (int fd : clients)
		close(fd);
	for (auto &w : waiting)
		close(w.fd);
	close(listenFd);
}

////////////////////////////////////////////////////////////////////////////////
// Save config to ini file

bool CMonitorServer::SaveConfig(const std::string &ini_path)
{
	// Read existing ini, update gain/AGC values, write back
	std::ifstream in(ini_path);
	if (!in.is_open()) return false;

	// keys of AGC digital and AGC analog: replaced where they stand, else written
	// after AGCMaxGainDown (or at the end), so an ini without them gets them
	struct SKey { const char *key; float value; bool present; bool written; };
	bool bitstreamPresent = false;
	std::vector<SKey> profile = {
		{ "AGCDigitalNoiseGate", g_Stats.config.agc_digital_noisegate.load(), false, false },
		{ "AGCDigitalMaxGainUp", g_Stats.config.agc_digital_maxgain_up.load(), false, false },
		{ "AGCDigitalGateFloor", g_Stats.config.agc_digital_gatefloor.load(), false, false },
		{ "AGCAnalogNoiseGate", g_Stats.config.agc_analog_noisegate.load(), false, false },
		{ "AGCAnalogMaxGainUp", g_Stats.config.agc_analog_maxgain_up.load(), false, false },
		{ "AGCAnalogGateFloor", g_Stats.config.agc_analog_gatefloor.load(), false, false },
		{ "AGCAnalogNoiseSuppress", g_Stats.config.agc_analog_noisesuppress.load(), false, false },
	};
	{
		std::string l;
		while (std::getline(in, l))
		{
			if (l.empty() || l[0] == '#') continue;
			std::string k = l.substr(0, l.find('='));
			while (!k.empty() && k.back() == ' ') k.pop_back();
			for (auto &a : profile)
				if (k == a.key) a.present = true;
			if (k == "OutputGainDMRBitstream") bitstreamPresent = true;
		}
		in.clear();
		in.seekg(0);
	}

	std::ostringstream out;
	std::string line;
	// the vocoder choices: a line of tcd.ini is rewritten when there is a choice
	// (a device's codec: any), and appended when there was none and there are two;
	// the module view is replaced where it stands, else written when it is not auto
	bool codecWritten[kNumChoices] = {};
	auto codecLine = [](int k) { return g_Stats.codec_options[k][g_Stats.config.codec_choice[k].load()]; };
	bool viewWritten = false, poolWritten = false;
	const int pool = g_Stats.config.device_pool.load();
	static const char *views[] = { "auto", "cards", "compact" };
	const int view = std::clamp(g_Stats.config.monitor_view.load(), 0, 2);
	while (std::getline(in, line))
	{
		// Check if this line matches a configurable key
		std::string trimmed = line;
		// skip comments
		if (!trimmed.empty() && trimmed[0] != '#')
		{
			auto eq = trimmed.find('=');
			if (eq != std::string::npos)
			{
				std::string key = trimmed.substr(0, eq);
				// trim key
				while (!key.empty() && key.back() == ' ') key.pop_back();

				if (key == "DStarGainIn")       { out << key << " = " << g_Stats.config.gain_dstar_in.load() << "\n"; continue; }
				if (key == "DStarGainOut")      { out << key << " = " << g_Stats.config.gain_dstar_out.load() << "\n"; continue; }
				if (key == "DmrGainIn")         { out << key << " = " << g_Stats.config.gain_dmr_in.load() << "\n"; continue; }
				if (key == "DmrGainOut")        { out << key << " = " << g_Stats.config.gain_dmr_out.load() << "\n"; continue; }
				if (key == "DmrReencodeGain")   { out << key << " = " << g_Stats.config.gain_dmr_reencode.load() << "\n"; continue; }
				if (key == "OutputGainDStar")   { out << key << " = " << g_Stats.config.outgain_dstar.load() << "\n"; continue; }
				if (key == "OutputGainDMR")
				{
					out << key << " = " << g_Stats.config.outgain_dmr.load() << "\n";
					if (!bitstreamPresent)
						out << "OutputGainDMRBitstream = " << g_Stats.config.outgain_dmr_bitstream.load() << "\n";
					continue;
				}
				if (key == "OutputGainDMRBitstream") { out << key << " = " << g_Stats.config.outgain_dmr_bitstream.load() << "\n"; continue; }
				if (key == "OutputGainUSRP")   { out << key << " = " << g_Stats.config.outgain_usrp.load() << "\n"; continue; }
				if (key == "OutputGainIMBE")   { out << key << " = " << g_Stats.config.outgain_imbe.load() << "\n"; continue; }
				if (key == "OutputGainM17")    { out << key << " = " << g_Stats.config.outgain_m17.load() << "\n"; continue; }
				if (key == "DmrGainMode")      { int m = g_Stats.config.dmr_gain_mode.load(); out << key << " = " << (m==2 ? "reencode" : m==1 ? "ambe" : "off") << "\n"; continue; }
				if (key == "AmbeGainDb")       { out << key << " = " << g_Stats.config.ambe_gain_db.load() << "\n"; continue; }
				if (key == "MonitorView") { out << key << " = " << views[view] << "\n"; viewWritten = true; continue; }
				// without devices at this start the setting is not in use: the line stays
				if (key == "DeviceChannels" && g_Stats.pool_available.load()) { out << key << " = " << (pool ? "pool" : "fixed") << "\n"; poolWritten = true; continue; }
				const int k = ChoiceOfIni(key);
				if (k >= 0 && g_Stats.codec_options[k].size() > (g_CodecChoices[k].own ? 1u : 0u))
				{
					out << key << " = " << codecLine(k) << "\n";
					codecWritten[k] = true;
					continue;
				}
				if (key == "AGC")               { out << key << " = " << (g_Stats.config.agc_enabled.load() ? "true" : "false") << "\n"; continue; }
				if (key == "AGCTarget")         { out << key << " = " << g_Stats.config.agc_target.load() << "\n"; continue; }
				if (key == "AGCAttack")         { out << key << " = " << g_Stats.config.agc_attack.load() << "\n"; continue; }
				if (key == "AGCRelease")        { out << key << " = " << g_Stats.config.agc_release.load() << "\n"; continue; }
				if (key == "AGCMaxGainDown")
				{
					out << key << " = " << g_Stats.config.agc_maxgain_down.load() << "\n";
					// the per-profile settings follow it when the ini doesn't have them yet
					for (auto &a : profile)
						if (!a.present && !a.written) { out << a.key << " = " << a.value << "\n"; a.written = true; }
					continue;
				}
				bool done = false;
				for (auto &a : profile)
					if (key == a.key) { out << key << " = " << a.value << "\n"; a.written = true; done = true; }
				if (done) continue;
				}
		}
		out << line << "\n";
	}
	in.close();
	for (auto &a : profile)
		if (!a.written) out << a.key << " = " << a.value << "\n";
	for (int k = 0; k < kNumChoices; k++)
		if (!codecWritten[k] && g_Stats.codec_options[k].size() > 1)
			out << g_CodecChoices[k].ini << " = " << codecLine(k) << "\n";
	if (!viewWritten && view != 0)
		out << "MonitorView = " << views[view] << "\n";
	if (!poolWritten && pool)
		out << "DeviceChannels = pool\n";

	std::ofstream of(ini_path);
	if (!of.is_open()) return false;
	of << out.str();
	return true;
}
