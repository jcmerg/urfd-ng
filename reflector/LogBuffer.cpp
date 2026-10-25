// LogBuffer -- Thread-safe circular log buffer for admin interface

#include <unistd.h>
#include <ctime>
#include <cstdio>
#include "LogBuffer.h"

CLogBuffer g_LogBuffer;

CLogBuffer::CLogBuffer()
	: m_Head(0)
	, m_Count(0)
	, m_OutBuf(*this, STDOUT_FILENO)
	, m_ErrBuf(*this, STDERR_FILENO)
	, m_OrigOut(nullptr)
	, m_OrigErr(nullptr)
{
	m_Lines.resize(LOG_BUFFER_SIZE);
}

CLogBuffer::~CLogBuffer()
{
	Uninstall();
}

void CLogBuffer::Install(void)
{
	m_OrigOut = std::cout.rdbuf(&m_OutBuf);
	m_OrigErr = std::cerr.rdbuf(&m_ErrBuf);
}

void CLogBuffer::Uninstall(void)
{
	if (m_OrigOut)
	{
		std::cout.rdbuf(m_OrigOut);
		m_OrigOut = nullptr;
	}
	if (m_OrigErr)
	{
		std::cerr.rdbuf(m_OrigErr);
		m_OrigErr = nullptr;
	}
}

// the line this thread is writing to the stream, up to its '\n'; what a thread
// leaves without a '\n' still reaches the console when it ends (straight to the
// descriptor: the log buffer may be gone by then)
namespace
{
	struct SPending
	{
		std::string out, err;
		~SPending()
		{
			if (!out.empty() && ::write(STDOUT_FILENO, out.data(), out.size()) < 0) {}
			if (!err.empty() && ::write(STDERR_FILENO, err.data(), err.size()) < 0) {}
		}
	};
}

std::string &CLogBuffer::CLineBuf::Pending(void)
{
	thread_local SPending pending;
	return (m_Fd == STDERR_FILENO) ? pending.err : pending.out;
}


int CLogBuffer::CLineBuf::overflow(int c)
{
	if (c != EOF)
	{
		char ch = (char)c;
		xsputn(&ch, 1);
	}
	return c;
}

std::streamsize CLogBuffer::CLineBuf::xsputn(const char *s, std::streamsize n)
{
	std::string &line = Pending();
	for (std::streamsize i = 0; i < n; i++)
	{
		line += s[i];
		if (s[i] == '\n')
		{
			m_Log.WriteLine(m_Fd, line);
			line.clear();
		}
	}
	return n;
}

void CLogBuffer::WriteLine(int fd, const std::string &line)
{
	std::lock_guard<std::mutex> lock(m_Mutex);

	// Write directly to the fd (bypasses rdbuf, works with supervisord),
	// nothing to do if it is gone
	if (::write(fd, line.data(), line.size()) < 0) {}

	// Prepend timestamp to stored line
	char ts[20];
	time_t now = time(nullptr);
	struct tm t;
	localtime_r(&now, &t);
	snprintf(ts, sizeof(ts), "%02d:%02d:%02d ", t.tm_hour, t.tm_min, t.tm_sec);

	m_Lines[m_Head] = std::string(ts) + line.substr(0, line.size() - 1);
	m_Head = (m_Head + 1) % LOG_BUFFER_SIZE;
	if (m_Count < LOG_BUFFER_SIZE)
		m_Count++;
}

std::vector<std::string> CLogBuffer::GetLines(int count) const
{
	std::lock_guard<std::mutex> lock(m_Mutex);
	if (count > m_Count)
		count = m_Count;

	std::vector<std::string> result;
	result.reserve(count);

	// Start from oldest requested line
	int start = (m_Head - count + LOG_BUFFER_SIZE) % LOG_BUFFER_SIZE;
	for (int i = 0; i < count; i++)
	{
		result.push_back(m_Lines[(start + i) % LOG_BUFFER_SIZE]);
	}

	return result;
}
