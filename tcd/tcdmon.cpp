// tcdmon — ncurses monitoring tool for tcd transcoder
// Connects to tcd stats socket (TCP), displays live audio levels,
// AGC state, device status. Works over SSH.
//
// Usage: tcdmon [host] [port]
//        Default: 127.0.0.1 8081

#include <ncurses.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <map>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <fstream>
#include <algorithm>
#include <climits>
#include <clocale>

// Minimal JSON value extraction (no library dependency)
static std::string json_str(const std::string &json, const std::string &key)
{
	std::string search = "\"" + key + "\":\"";
	auto pos = json.find(search);
	if (pos == std::string::npos) return "";
	pos += search.size();
	auto end = json.find('"', pos);
	if (end == std::string::npos) return "";
	return json.substr(pos, end - pos);
}

static double json_num(const std::string &json, const std::string &key)
{
	std::string search = "\"" + key + "\":";
	auto pos = json.find(search);
	if (pos == std::string::npos) return 0.0;
	pos += search.size();
	return strtod(json.c_str() + pos, nullptr);
}

static bool json_bool(const std::string &json, const std::string &key)
{
	std::string search = "\"" + key + "\":";
	auto pos = json.find(search);
	if (pos == std::string::npos) return false;
	pos += search.size();
	return json[pos] == 't';
}

static int json_int(const std::string &json, const std::string &key)
{
	return (int)json_num(json, key);
}

// numbers as the terminal's locale writes them (LANG, LC_NUMERIC): 134.853 and
// -12,3 in German, 134,853 and -12.3 in English, ungrouped in the C locale.
// Read once by numeric_locale(); LC_NUMERIC itself stays "C", so the JSON
// numbers still parse with their '.'
static std::string g_thousands_sep;
static std::string g_grouping;
static char g_decimal_point = '.';

static void numeric_locale()
{
	if (setlocale(LC_NUMERIC, ""))
	{
		const struct lconv *lc = localeconv();
		g_thousands_sep = lc->thousands_sep ? lc->thousands_sep : "";
		g_grouping = lc->grouping ? lc->grouping : "";
		if (lc->decimal_point && lc->decimal_point[0] && !lc->decimal_point[1])
			g_decimal_point = lc->decimal_point[0];
	}
	setlocale(LC_NUMERIC, "C");
	// a separator of more than one byte (the no-break spaces of French or
	// Russian) as a plain space: this ncurses terminal shows single bytes
	if (g_thousands_sep.size() > 1)
		g_thousands_sep = " ";
}

// a count with its thousands grouped by the locale
static std::string thousands(long long v)
{
	std::string s = std::to_string(v < 0 ? -v : v);
	if (!g_thousands_sep.empty() && !g_grouping.empty())
	{
		size_t g = 0;
		int size = g_grouping[0];
		for (long i = long(s.size()) - size; size > 0 && size != CHAR_MAX && i > 0; i -= size)
		{
			s.insert(size_t(i), g_thousands_sep);
			if (g + 1 < g_grouping.size())
				size = g_grouping[++g];
		}
	}
	return v < 0 ? "-" + s : s;
}
#define TS(v) thousands(v).c_str()

// a printf of a number with a fraction, its decimal point as the locale has it
static std::string fraction(const char *format, double v)
{
	char buf[32];
	snprintf(buf, sizeof(buf), format, v);
	std::string s = buf;
	if (g_decimal_point != '.')
		std::replace(s.begin(), s.end(), '.', g_decimal_point);
	return s;
}

// Extract a JSON sub-object by key
static std::string json_obj(const std::string &json, const std::string &key)
{
	std::string search = "\"" + key + "\":{";
	auto pos = json.find(search);
	if (pos == std::string::npos) return "";
	pos = json.find('{', pos);
	int depth = 0;
	size_t start = pos;
	for (size_t i = pos; i < json.size(); i++)
	{
		if (json[i] == '{') depth++;
		else if (json[i] == '}') { depth--; if (depth == 0) return json.substr(start, i - start + 1); }
	}
	return "";
}

// Extract JSON array
static std::string json_arr(const std::string &json, const std::string &key)
{
	std::string search = "\"" + key + "\":[";
	auto pos = json.find(search);
	if (pos == std::string::npos) return "[]";
	pos = json.find('[', pos);
	int depth = 0;
	size_t start = pos;
	for (size_t i = pos; i < json.size(); i++)
	{
		if (json[i] == '[') depth++;
		else if (json[i] == ']') { depth--; if (depth == 0) return json.substr(start, i - start + 1); }
	}
	return "[]";
}

// Split array into objects
static std::vector<std::string> json_arr_items(const std::string &arr)
{
	std::vector<std::string> items;
	int depth = 0;
	size_t start = 0;
	for (size_t i = 0; i < arr.size(); i++)
	{
		if (arr[i] == '{') { if (depth == 0) start = i; depth++; }
		else if (arr[i] == '}') { depth--; if (depth == 0) items.push_back(arr.substr(start, i - start + 1)); }
	}
	return items;
}

// Extract module keys from {"modules":{"F":{...},"S":{...}}}
static std::map<char, std::string> json_modules(const std::string &json)
{
	std::map<char, std::string> result;
	std::string mods = json_obj(json, "modules");
	if (mods.empty()) return result;

	// Find each single-letter key
	size_t pos = 0;
	while ((pos = mods.find("\"", pos)) != std::string::npos)
	{
		pos++;
		if (pos + 2 < mods.size() && mods[pos + 1] == '"' && mods[pos] >= 'A' && mods[pos] <= 'Z')
		{
			char mod = mods[pos];
			auto brace = mods.find('{', pos);
			if (brace == std::string::npos) break;
			int depth = 0;
			for (size_t i = brace; i < mods.size(); i++)
			{
				if (mods[i] == '{') depth++;
				else if (mods[i] == '}')
				{
					depth--;
					if (depth == 0)
					{
						result[mod] = mods.substr(brace, i - brace + 1);
						pos = i + 1;
						break;
					}
				}
			}
		}
	}
	return result;
}

struct ModuleData
{
	std::string codec_in;
	std::string direct;     // the targets coded from the stream's frames directly (tcd 0.20.0)
	double rms_in, peak_in, rms_out, peak_out;
	double agc_gain, idle;
	bool agc_gate;
	int pkts_in, pkts_out;
	bool standby, linked;   // StandbyModules: a standby module has a link only while no other tcd has it
};

struct DeviceData
{
	std::string serial, type, role;
	int buf_depth, errors;
	bool online;
	std::vector<std::string> ch_modules;  // active module per channel
};

static volatile sig_atomic_t g_resize = 0;
static void handle_winch(int) { g_resize = 1; }

// Draw a horizontal VU bar
static void draw_vu(int y, int x, int width, const char *label, double rms, double peak)
{
	// Scale: -60 dBFS = empty, 0 dBFS = full
	int bar_w = width - 14;  // label(8) + value(6)
	if (bar_w < 10) bar_w = 10;

	double frac = (rms + 60.0) / 60.0;
	if (frac < 0) frac = 0;
	if (frac > 1) frac = 1;
	int filled = (int)(frac * bar_w);

	double pfrac = (peak + 60.0) / 60.0;
	if (pfrac < 0) pfrac = 0;
	if (pfrac > 1) pfrac = 1;
	int peak_pos = (int)(pfrac * bar_w);

	mvprintw(y, x, "%-7s ", label);

	for (int i = 0; i < bar_w; i++)
	{
		if (i == peak_pos)
		{
			attron(COLOR_PAIR(4) | A_BOLD);
			addch('|');
			attroff(COLOR_PAIR(4) | A_BOLD);
		}
		else if (i < filled)
		{
			// Green < -18, Yellow < -6, Red >= -6
			double db_at = -60.0 + (double)i / bar_w * 60.0;
			if (db_at >= -6) attron(COLOR_PAIR(3));
			else if (db_at >= -18) attron(COLOR_PAIR(2));
			else attron(COLOR_PAIR(1));
			addch('#');
			attroff(COLOR_PAIR(1) | COLOR_PAIR(2) | COLOR_PAIR(3));
		}
		else
		{
			attron(COLOR_PAIR(5));
			addch('-');
			attroff(COLOR_PAIR(5));
		}
	}

	char val[16];
	if (rms > -99) snprintf(val, sizeof(val), "%s", fraction("%5.1f", rms).c_str());
	else snprintf(val, sizeof(val), " -inf");
	mvprintw(y, x + 8 + bar_w, "%s", val);
}

// Draw signal flow line
static void draw_flow(int y, int x, int /*width*/, const ModuleData &m, const std::string &cfg_json)
{
	if (m.codec_in == "none" || m.idle > 2.0)
	{
		attron(COLOR_PAIR(5));
		mvprintw(y, x, "%s", !m.standby ? "idle" : m.linked ? "idle (standby, taken over)" : "standby: another transcoder has this module");
		attroff(COLOR_PAIR(5));
		return;
	}

	int col = x;

	// Source codec
	attron(COLOR_PAIR(1) | A_BOLD);
	mvprintw(y, col, "%s", m.codec_in.c_str());
	attroff(COLOR_PAIR(1) | A_BOLD);
	col += m.codec_in.size();

	attron(COLOR_PAIR(5));
	mvprintw(y, col, " > ");
	attroff(COLOR_PAIR(5));
	col += 3;

	// Pre-AGC level
	attron(COLOR_PAIR(2));
	char buf[32];
	snprintf(buf, sizeof(buf), "PCM %.0fdB", m.rms_in);
	mvprintw(y, col, "%s", buf);
	attroff(COLOR_PAIR(2));
	col += strlen(buf);

	attron(COLOR_PAIR(5));
	mvprintw(y, col, " > ");
	attroff(COLOR_PAIR(5));
	col += 3;

	// AGC
	bool agc_on = json_bool(cfg_json, "agc_enabled");
	if (agc_on)
	{
		if (m.agc_gate)
		{
			attron(COLOR_PAIR(3) | A_BOLD);
			mvprintw(y, col, "GATE");
			attroff(COLOR_PAIR(3) | A_BOLD);
			col += 4;
		}
		else
		{
			attron(COLOR_PAIR(6) | A_BOLD);
			snprintf(buf, sizeof(buf), "AGC %sdB", fraction("%+.1f", m.agc_gain).c_str());
			mvprintw(y, col, "%s", buf);
			attroff(COLOR_PAIR(6) | A_BOLD);
			col += strlen(buf);
		}
	}
	else
	{
		attron(COLOR_PAIR(5));
		mvprintw(y, col, "no AGC");
		attroff(COLOR_PAIR(5));
		col += 6;
	}

	attron(COLOR_PAIR(5));
	mvprintw(y, col, " > ");
	attroff(COLOR_PAIR(5));
	col += 3;

	// Post-AGC level
	attron(COLOR_PAIR(2));
	snprintf(buf, sizeof(buf), "PCM %.0fdB", m.rms_out);
	mvprintw(y, col, "%s", buf);
	attroff(COLOR_PAIR(2));
	col += strlen(buf);

	attron(COLOR_PAIR(5));
	mvprintw(y, col, " > ");
	attroff(COLOR_PAIR(5));
	col += 3;

	// Target codecs
	attron(COLOR_PAIR(3) | A_BOLD);
	std::string targets;
	// a target coded from the stream's frames directly gets a '*'
	auto mark = [&](const char *name) { return std::string(name) + (m.direct.find(name) != std::string::npos ? "* " : " "); };
	if (m.codec_in.find("Star") == std::string::npos) targets += mark("D-Star");
	if (m.codec_in.find("DMR") == std::string::npos) targets += mark("DMR/YSF/NXDN");
	if (m.codec_in.find("Codec2") == std::string::npos) targets += "M17 ";
	if (m.codec_in.find("P25") == std::string::npos) targets += mark("P25");
	if (m.codec_in != "USRP") targets += "USRP ";
	if (m.codec_in != "SVX") targets += "SVX";
	mvprintw(y, col, "%s", targets.c_str());
	attroff(COLOR_PAIR(3) | A_BOLD);
}

// MonitorStatsPort of the tcd.ini at path, 0 without one
static int StatsPortOf(const char *path)
{
	std::ifstream ini(path);
	std::string line;
	while (std::getline(ini, line))
	{
		line = line.substr(0, line.find('#'));
		const auto eq = line.find('=');
		if (eq == std::string::npos)
			continue;
		std::string key = line.substr(0, eq);
		key.erase(key.find_last_not_of(" \t") + 1);
		key.erase(0, key.find_first_not_of(" \t"));
		if (key == "MonitorStatsPort")
			return atoi(line.c_str() + eq + 1);
	}
	return 0;
}

int main(int argc, char *argv[])
{
	numeric_locale();
	const char *host = "127.0.0.1";
	int port = 8081;
	// without a port: the one of this machine's tcd.ini ($TCD_INI, the Docker
	// image's, a native install's), else 8081
	for (const char *ini : { (const char *)getenv("TCD_INI"), "/usr/local/etc/tcd/tcd.ini", "/usr/local/etc/tcd.ini" })
	{
		const int p = ini ? StatsPortOf(ini) : 0;
		if (p > 0 && p < 65536)
		{
			port = p;
			break;
		}
	}
	// tcd's MonitorPassword, asked by tcd from other hosts than 127.0.0.1
	std::string password = getenv("TCD_MONITOR_PASSWORD") ? getenv("TCD_MONITOR_PASSWORD") : "";

	// tcdmon [-p password] [host [port]]
	int pos = 0;
	for (int i = 1; i < argc; i++)
	{
		if (0 == strcmp(argv[i], "-p") && i + 1 < argc)
			password = argv[++i];
		else if (0 == strcmp(argv[i], "-h") || 0 == strcmp(argv[i], "--help"))
		{
			printf("usage: %s [-p password] [host [port]]   (default 127.0.0.1 and the MonitorStatsPort of $TCD_INI, /usr/local/etc/tcd/tcd.ini or /usr/local/etc/tcd.ini, else 8081; without -p the password from TCD_MONITOR_PASSWORD or a prompt, -p shows it in the process list)\n", argv[0]);
			return 0;
		}
		else if (pos == 0) { host = argv[i]; pos++; }
		else if (pos == 1) { port = atoi(argv[i]); pos++; }
	}

	// Connect to tcd stats socket
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) { perror("socket"); return 1; }

	struct sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	if (inet_pton(AF_INET, host, &addr.sin_addr) != 1)
	{
		struct hostent *he = gethostbyname(host);
		if (!he) { fprintf(stderr, "Cannot resolve %s\n", host); return 1; }
		memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
	}

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
	{
		fprintf(stderr, "Cannot connect to %s:%d: %s\n", host, port, strerror(errno));
		return 1;
	}

	// tcd asks for its MonitorPassword with a line "AUTH", else it starts with the stats
	std::string first;
	{
		struct pollfd pfd = { fd, POLLIN, 0 };
		char c;
		while (poll(&pfd, 1, 3000) > 0 && recv(fd, &c, 1, 0) == 1 && c != '\n' && first.size() < 65536)
			first += c;
	}
	if (first == "AUTH")
	{
		if (password.empty())
		{
			const char *p = getpass("tcd monitor password: ");
			password = p ? p : "";
		}
		const std::string line = "AUTH " + password + "\n";
		send(fd, line.c_str(), line.size(), 0);
		std::string answer;
		struct pollfd pfd = { fd, POLLIN, 0 };
		char c;
		while (poll(&pfd, 1, 5000) > 0 && recv(fd, &c, 1, 0) == 1 && c != '\n' && answer.size() < 64)
			answer += c;
		if (answer != "OK")
		{
			fprintf(stderr, "tcd refused the password\n");
			return 1;
		}
		first.clear();
	}

	// Non-blocking reads
	int flags = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);

	// Setup ncurses
	signal(SIGWINCH, handle_winch);
	initscr();
	cbreak();
	noecho();
	nodelay(stdscr, TRUE);
	curs_set(0);
	keypad(stdscr, TRUE);

	if (has_colors())
	{
		start_color();
		use_default_colors();
		init_pair(1, COLOR_GREEN, -1);
		init_pair(2, COLOR_YELLOW, -1);
		init_pair(3, COLOR_RED, -1);
		init_pair(4, COLOR_WHITE, -1);
		// dark gray where the terminal has 16 colours; with 8 (docker exec, TERM=xterm)
		// colour 8 does not exist and the pair would draw black on black
		init_pair(5, COLORS > 8 ? 8 : COLOR_WHITE, -1);
		init_pair(6, COLOR_MAGENTA, -1);
		init_pair(7, COLOR_CYAN, -1);
	}

	std::string linebuf;
	std::string last_json = first;	// the first stats line, read during the login check

	while (true)
	{
		// Check for keypress
		int ch = getch();
		if (ch == 'q' || ch == 'Q') break;

		if (g_resize)
		{
			g_resize = 0;
			endwin();
			refresh();
		}

		// Read data from stats socket
		char buf[8192];
		ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
		if (n > 0)
		{
			buf[n] = '\0';
			linebuf += buf;

			// Process complete lines
			size_t nlpos;
			while ((nlpos = linebuf.find('\n')) != std::string::npos)
			{
				last_json = linebuf.substr(0, nlpos);
				linebuf.erase(0, nlpos + 1);
			}
		}
		else if (n == 0)
		{
			// Connection closed
			break;
		}

		if (last_json.empty())
		{
			usleep(50000);
			continue;
		}

		// Parse and render
		auto modules = json_modules(last_json);
		auto dev_arr = json_arr(last_json, "devices");
		auto dev_items = json_arr_items(dev_arr);
		auto ref_json = json_obj(last_json, "reflector");
		auto cfg_json = json_obj(last_json, "config");

		int rows, cols;
		getmaxyx(stdscr, rows, cols);

		erase();

		// Header
		attron(COLOR_PAIR(7) | A_BOLD);
		{
			std::string ver = json_str(last_json, "version");
			mvprintw(0, 0, "TCD Transcoder Monitor%s%s", ver.empty() ? "" : " ", ver.c_str());
		}
		attroff(COLOR_PAIR(7) | A_BOLD);

		// Reflector status
		bool ref_conn = json_bool(ref_json, "connected");
		int ref_rx = json_int(ref_json, "pkts_rx");
		int ref_tx = json_int(ref_json, "pkts_tx");
		{
			char counts[48];
			const int n = snprintf(counts, sizeof(counts), "RX:%s TX:%s", TS(ref_rx), TS(ref_tx));
			const char *state = ref_conn ? "Reflector: CONNECTED" : "Reflector: DISCONNECTED";
			const int x = cols - n - 1;
			attron(ref_conn ? COLOR_PAIR(1) : (COLOR_PAIR(3) | A_BOLD));
			mvprintw(0, std::max(0, x - int(strlen(state)) - 2), "%s", state);
			attroff(COLOR_PAIR(1) | COLOR_PAIR(3) | A_BOLD);
			mvprintw(0, std::max(0, x), "%s", counts);
		}

		int row = 2;

		// Modules: a card for each module on the air, as the web dashboard's
		// compact view; the others in one line with their state
		std::string idle_mods, standby_mods, more_mods;
		for (auto &[mod, mj] : modules)
		{
			ModuleData m;
			m.codec_in = json_str(mj, "codec_in");
			m.direct = json_str(mj, "direct");
			m.rms_in = json_num(mj, "rms_in");
			m.peak_in = json_num(mj, "peak_in");
			m.rms_out = json_num(mj, "rms_out");
			m.peak_out = json_num(mj, "peak_out");
			m.agc_gain = json_num(mj, "agc_gain");
			m.agc_gate = json_bool(mj, "agc_gate");
			m.idle = json_num(mj, "idle");
			m.pkts_in = json_int(mj, "pkts_in");
			m.pkts_out = json_int(mj, "pkts_out");
			m.standby = json_bool(mj, "standby");
			m.linked = json_bool(mj, "linked");

			bool active = (m.codec_in != "none" && m.idle < 2.0);
			if (!active)
			{
				// standby: another transcoder has the module (taken over ones are ours, idle)
				(m.standby && !m.linked ? standby_mods : idle_mods) += mod;
				continue;
			}
			if (row >= rows - 8)
			{
				more_mods += mod;	// no room for its card: named below
				continue;
			}

			// Module header
			attron(COLOR_PAIR(7) | A_BOLD);
			mvprintw(row, 0, "Module %c", mod);
			attroff(COLOR_PAIR(7) | A_BOLD);
			attron(COLOR_PAIR(2));
			printw("  [%s]", m.codec_in.c_str());
			attroff(COLOR_PAIR(2));
			printw("  pkts: %s/%s", TS(m.pkts_in), TS(m.pkts_out));
			row++;

			// Signal flow
			draw_flow(row, 2, cols - 4, m, cfg_json);
			row++;

			// VU meters
			draw_vu(row, 2, cols - 4, "Pre", m.rms_in, m.peak_in);
			row++;
			draw_vu(row, 2, cols - 4, "Post", m.rms_out, m.peak_out);
			row++;

			row++;  // spacing
		}
		if (!more_mods.empty())
		{
			attron(COLOR_PAIR(7));
			mvprintw(row++, 0, "Also on the air (no room for their cards): %s", more_mods.c_str());
			attroff(COLOR_PAIR(7));
		}
		if (!idle_mods.empty() || !standby_mods.empty())
		{
			attron(COLOR_PAIR(5));
			mvprintw(row, 0, "%s", modules.size() == idle_mods.size() + standby_mods.size() ? "No module on the air" : "Not on the air");
			if (!idle_mods.empty())
				printw("  idle: %s", idle_mods.c_str());
			if (!standby_mods.empty())
				printw("  standby (another transcoder has them): %s", standby_mods.c_str());
			attroff(COLOR_PAIR(5));
			row += 2;
		}

		// Hardware: the free AMBE channels of each codec, fixed or pool, then the devices
		{
			auto hw = json_obj(last_json, "hw_channels");
			auto ds = json_obj(hw, "dstar"), dm = json_obj(hw, "dmr");
			attron(COLOR_PAIR(7) | A_BOLD);
			mvprintw(row, 0, "Hardware");
			attroff(COLOR_PAIR(7) | A_BOLD);
			if (!dev_items.empty())
				printw("  AMBE channels: D-Star %d of %d in use, DMR %d of %d in use, %s",
					json_int(ds, "busy"), json_int(ds, "total"), json_int(dm, "busy"), json_int(dm, "total"),
					json_int(cfg_json, "device_pool") ? "pool (any module)" : "fixed (one per module)");
			else
				printw("  no AMBE device");
		}
		row++;

		for (auto &dj : dev_items)
		{
			if (row >= rows - 1)
				break;
			std::string serial = json_str(dj, "serial");
			std::string type = json_str(dj, "type");
			std::string role = json_str(dj, "role");
			bool online = json_bool(dj, "online");
			int bufd = json_int(dj, "buf_depth");
			int errs = json_int(dj, "errors");

			// Parse active channels
			auto ch_arr = json_arr(dj, "channels");
			auto ch_items = json_arr_items(ch_arr);
			std::string active_mods;
			int active_count = 0;
			for (auto &cj : ch_items)
			{
				std::string m = json_str(cj, "module");
				if (!m.empty()) { active_count++; if (!active_mods.empty()) active_mods += ","; active_mods += m; }
			}
			int total_ch = (type == "DV3003") ? 3 : 1;

			// phased out from the monitor: going (no new streams) or out (closed)
			const int phase = json_int(dj, "phase");
			if (phase == 2) attron(COLOR_PAIR(5));
			else if (online && active_count > 0) attron(COLOR_PAIR(7) | A_BOLD);
			else if (online) attron(COLOR_PAIR(1));
			else attron(COLOR_PAIR(3));
			mvprintw(row, 2, "%s", phase == 2 ? "--" : phase == 1 ? "<-" : online ? (active_count > 0 ? ">>" : "OK") : "!!");
			attroff(COLOR_PAIR(1) | COLOR_PAIR(3) | COLOR_PAIR(5) | COLOR_PAIR(7) | A_BOLD);

			printw("  %s (%s) [%s]  slots:%d/%d  buf:%d",
				serial.c_str(), type.c_str(), role.c_str(), active_count, total_ch, bufd);
			if (!active_mods.empty())
			{
				attron(COLOR_PAIR(7));
				printw("  %s", active_mods.c_str());
				attroff(COLOR_PAIR(7));
			}
			// answers lost on the serial line, a silent frame instead
			if (errs > 0) { attron(COLOR_PAIR(3)); printw("  lost:%s", TS(errs)); attroff(COLOR_PAIR(3)); }
			// its frames by their FEC, like the software rows: decoded with corrected
			// errors / uncorrectable (repeated), encoded that fail their FEC
			{
				const int dec = json_int(dj, "decodes"), enc = json_int(dj, "encodes");
				const int fec = json_int(dj, "error_frames"), unc = json_int(dj, "uncorrectable"), bad = json_int(dj, "bad_out");
				attron(COLOR_PAIR(5));
				printw("  dec:%s enc:%s", TS(dec), TS(enc));
				attroff(COLOR_PAIR(5));
				if (fec || unc || bad)
				{
					attron(COLOR_PAIR(3));
					printw("  fec:%s unc:%s bad:%s", TS(fec), TS(unc), TS(bad));
					attroff(COLOR_PAIR(3));
				}
			}
			if (phase) printw("  %s", phase == 2 ? "phased out" : "phasing out");
			row++;
		}

		// Software: the codecs of codec plugins and tcd's own vocoders, as many streams as the CPU takes
		attron(COLOR_PAIR(7) | A_BOLD);
		mvprintw(row, 0, "Software");
		attroff(COLOR_PAIR(7) | A_BOLD);
		row++;
		for (const auto &sw_json : json_arr_items(json_arr(last_json, "swvocoders")))
		{
			if (row >= rows - 1)
				break;
			std::string name = json_str(sw_json, "name");
			std::string codec = json_str(sw_json, "codec");
			std::string active_mod = json_str(sw_json, "active_module");
			bool active = !active_mod.empty();
			bool builtin = json_bool(sw_json, "builtin");
			bool reencode = json_bool(sw_json, "reencode_active");
			int cached = json_int(sw_json, "cached_streams");
			int enc = json_int(sw_json, "encodes");
			int dec = json_int(sw_json, "decodes");
			int re = json_int(sw_json, "reencodes");
			int direct = json_int(sw_json, "transcodes");

			if (active) attron(COLOR_PAIR(7) | A_BOLD);
			else attron(COLOR_PAIR(1));
			mvprintw(row, 2, "%s", active ? ">>" : "OK");
			attroff(COLOR_PAIR(1) | COLOR_PAIR(7) | A_BOLD);

			if (builtin)
				printw("  %s (built in) [%s]", name.c_str(), codec.c_str());
			else
				printw("  %s (codec plugin) [%s]  active:%d", name.c_str(), codec.c_str(), cached);
			if (codec == "dmr")
				printw("  re-enc:%s", reencode ? "ON" : "off");
			if (active)
			{
				attron(COLOR_PAIR(7));
				printw("  %s", active_mod.c_str());
				attroff(COLOR_PAIR(7));
			}
			attron(COLOR_PAIR(5));
			printw("  dec:%s enc:%s", TS(dec), TS(enc));
			if (codec == "dmr")
				printw(" re:%s", TS(re));
			if (direct)
				printw(" direct:%s", TS(direct));
			attroff(COLOR_PAIR(5));
			// how the decoded frames came out (plugins of codec API 3 report it)
			{
				const int fec = json_int(sw_json, "error_frames"), rep = json_int(sw_json, "repeats"), mute = json_int(sw_json, "mutes");
				if (fec || rep || mute)
				{
					attron(COLOR_PAIR(3));
					printw("  fec:%s rep:%s mute:%s", TS(fec), TS(rep), TS(mute));
					attroff(COLOR_PAIR(3));
				}
			}
			row++;
		}

		// who codes AMBE (and P25, M17) for new streams, when it is not the default
		{
			auto codecs = json_obj(last_json, "codecs");
			std::string ds = json_str(json_obj(codecs, "dstar"), "use"), dm = json_str(json_obj(codecs, "dmr"), "use");
			std::string p2 = json_str(json_obj(codecs, "p25"), "use"), m7 = json_str(json_obj(codecs, "m17"), "use");
			const bool p25 = !p2.empty() && p2 != "op25";
			const bool m17 = !m7.empty() && m7 != "codec2";
			if (row < rows - 1 && !codecs.empty() && (ds != "dvsi" || dm != "dvsi" || p25 || m17))
			{
				attron(COLOR_PAIR(5));
				mvprintw(row++, 2, "Vocoder for new streams: D-Star %s, DMR %s%s%s%s%s", ds.empty() ? "-" : ds.c_str(), dm.empty() ? "-" : dm.c_str(),
				         p25 ? ", IMBE (P25, YSF VW) " : "", p25 ? p2.c_str() : "", m17 ? ", Codec2 (M17) " : "", m17 ? m7.c_str() : "");
				attroff(COLOR_PAIR(5));
			}
		}

		// AGC config summary, where there is room
		row++;
		if (row + 3 < rows) {
		bool agc_on = json_bool(cfg_json, "agc_enabled");
		attron(COLOR_PAIR(5));
		mvprintw(row, 0, "AGC: %s", agc_on ? "ON" : "OFF");
		if (agc_on)
		{
			printw("  Target:%.0f  Attack:%.0f  Release:%.0f  Down:-%.0f",
				json_num(cfg_json, "agc_target"), json_num(cfg_json, "agc_attack"),
				json_num(cfg_json, "agc_release"), json_num(cfg_json, "agc_maxgain_down"));
			row++;
			mvprintw(row, 0, "     digital: Gate:%.0f Up:+%.0f Floor:%.0f   analog: Gate:%.0f Up:+%.0f Floor:%.0f NS:%.0f",
				json_num(cfg_json, "agc_digital_noisegate"), json_num(cfg_json, "agc_digital_maxgain_up"),
				json_num(cfg_json, "agc_digital_gatefloor"),
				json_num(cfg_json, "agc_analog_noisegate"), json_num(cfg_json, "agc_analog_maxgain_up"),
				json_num(cfg_json, "agc_analog_gatefloor"), json_num(cfg_json, "agc_analog_noisesuppress"));
		}
		attroff(COLOR_PAIR(5));
		row++;
		attron(COLOR_PAIR(5));
		mvprintw(row, 0, "Out: DSt:%+.0f M17:%+.0f DMR:%+.0f P25:%+.0f USRP:%+.0f",
			json_num(cfg_json, "outgain_dstar"), json_num(cfg_json, "outgain_m17"),
			json_num(cfg_json, "outgain_dmr"),
			json_num(cfg_json, "outgain_imbe"), json_num(cfg_json, "outgain_usrp"));
		attroff(COLOR_PAIR(5));
		}

		row++;
		attron(COLOR_PAIR(5));
		mvprintw(rows - 1, 0, "q=quit  |  %s:%d", host, port);
		attroff(COLOR_PAIR(5));

		refresh();
		usleep(100000);  // 100ms refresh
	}

	endwin();
	close(fd);
	return 0;
}
