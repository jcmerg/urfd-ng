// StatsLogger — CSV logging of per-frame AGC/level stats

#include <iostream>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <cctype>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "StatsLogger.h"

CStatsLogger g_StatsLog;

bool CStatsLogger::Start()
{
	if (!m_enabled)
		return true;

	// Create directory if needed
	struct stat st{};
	if (stat(m_dir.c_str(), &st) != 0)
	{
		if (mkdir(m_dir.c_str(), 0755) != 0)
		{
			std::cerr << "StatsLogger: cannot create " << m_dir << ": " << strerror(errno) << std::endl;
			m_enabled = false;
			return false;
		}
	}

	m_running = true;
	m_cleanupFuture = std::async(std::launch::async, &CStatsLogger::CleanupThread, this);

	std::cout << "StatsLogger: logging to " << m_dir << " (retain " << m_retainHours << "h)" << std::endl;
	return true;
}

void CStatsLogger::Stop()
{
	// the audio threads may still log while the controller stops after us: no new files
	m_enabled = false;
	m_running = false;
	if (m_cleanupFuture.valid())
		m_cleanupFuture.get();

	// Close all open files
	std::lock_guard<std::mutex> lock(m_mutex);
	for (auto &[key, sl] : m_streams)
	{
		if (sl.file.is_open())
			sl.file.close();
	}
	m_streams.clear();
}

void CStatsLogger::LogFrame(char module, uint16_t streamId, const char *codec,
                            float rms_in, float peak_in, float rms_out, float peak_out,
                            float agc_gain_db, bool gate)
{
	if (!m_enabled)
		return;

	uint32_t key = makeKey(module, streamId);

	std::lock_guard<std::mutex> lock(m_mutex);

	auto it = m_streams.find(key);
	if (it == m_streams.end())
	{
		// Open new file for this stream
		auto now = std::chrono::system_clock::now();
		auto t = std::chrono::system_clock::to_time_t(now);
		struct tm tm{};
		localtime_r(&t, &tm);

		char filename[128];
		snprintf(filename, sizeof(filename), "%c_%04d%02d%02d_%02d%02d%02d_%u.csv",
			module, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
			tm.tm_hour, tm.tm_min, tm.tm_sec, streamId);

		std::string path = m_dir + "/" + filename;

		StreamLog sl;
		sl.path = path;
		sl.file.open(path, std::ios::out);
		if (!sl.file.is_open())
		{
			std::cerr << "StatsLogger: cannot open " << path << std::endl;
			return;
		}

		// CSV header
		sl.file << "ms,module,codec,rms_in,peak_in,rms_out,peak_out,agc_gain,gate\n";
		sl.frame_count = 0;

		auto [ins, _] = m_streams.emplace(key, std::move(sl));
		it = ins;
	}

	auto &sl = it->second;

	// Timestamp: ms since epoch
	auto now = std::chrono::steady_clock::now();
	sl.last = now;
	auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

	char line[256];
	snprintf(line, sizeof(line), "%lld,%c,%s,%.1f,%.1f,%.1f,%.1f,%.1f,%d\n",
		(long long)ms, module, codec, rms_in, peak_in, rms_out, peak_out, agc_gain_db, gate ? 1 : 0);
	sl.file << line;
	sl.frame_count++;

	// Flush every 50 frames (~1s) to keep I/O reasonable
	if (sl.frame_count % 50 == 0)
		sl.file.flush();
}

void CStatsLogger::CleanupThread()
{
	unsigned seconds = 0;
	while (m_running)
	{
		usleep(1000000);
		if (!m_running)
			break;
		seconds++;
		if (0 == seconds % 10)
			CloseIdle();
		if (0 == seconds % 600)	// every 10 minutes
			DeleteOld();
	}
}

// a stream's file once no frame came for 10 s: the stream is over (the codec
// paths of a stream all log under its key, so no single one knows its end)
void CStatsLogger::CloseIdle()
{
	const auto cutoff = std::chrono::steady_clock::now() - std::chrono::seconds(10);
	std::lock_guard<std::mutex> lock(m_mutex);
	for (auto it = m_streams.begin(); it != m_streams.end(); )
	{
		if (it->second.last < cutoff)
		{
			it->second.file.close();
			it = m_streams.erase(it);
		}
		else
			++it;
	}
}

// the names LogFrame gives: <module>_YYYYMMDD_HHMMSS_<streamid>.csv
bool CStatsLogger::IsOurs(const char *name)
{
	const size_t len = strlen(name);
	if (len < 22 || !isupper((unsigned char)name[0]) || name[1] != '_' || name[10] != '_' || name[17] != '_' || strcmp(name + len - 4, ".csv"))
		return false;
	for (size_t i = 2; i < len - 4; i++)
		if (i != 10 && i != 17 && !isdigit((unsigned char)name[i]))
			return false;
	return true;
}

// Delete our files older than the retain period, nothing else in the directory
void CStatsLogger::DeleteOld()
{
	auto cutoff = std::chrono::system_clock::now() - std::chrono::hours(m_retainHours);
	auto cutoff_t = std::chrono::system_clock::to_time_t(cutoff);

	DIR *dir = opendir(m_dir.c_str());
	if (!dir)
		return;

	struct dirent *entry;
	while ((entry = readdir(dir)) != nullptr)
	{
		if (!IsOurs(entry->d_name))
			continue;

		std::string path = m_dir + "/" + entry->d_name;
		struct stat st{};
		if (stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_mtime < cutoff_t)
			unlink(path.c_str());
	}
	closedir(dir);
}
