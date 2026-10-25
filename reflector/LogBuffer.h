#pragma once

// LogBuffer -- Thread-safe circular log buffer for admin interface
// Captures urfd console output for live log viewing.
//
// std::cout and std::cerr go through a line buffer per thread: a line reaches
// the console (and this buffer) only whole, at its '\n', so lines that threads
// write at the same time no longer run into each other.

#include <string>
#include <vector>
#include <mutex>
#include <sstream>
#include <iostream>

#define LOG_BUFFER_SIZE 200

class CLogBuffer
{
public:
	CLogBuffer();
	~CLogBuffer();

	// Install: redirect std::cout and std::cerr through line buffers
	void Install(void);
	void Uninstall(void);

	// Get last N lines
	std::vector<std::string> GetLines(int count) const;

	// a whole line, '\n' included: to the file descriptor and into the buffer
	void WriteLine(int fd, const std::string &line);

private:
	// a stream's output, collected per thread up to the end of each line
	class CLineBuf : public std::streambuf
	{
	public:
		CLineBuf(CLogBuffer &log, int fd) : m_Log(log), m_Fd(fd) {}
	protected:
		int overflow(int c) override;
		std::streamsize xsputn(const char *s, std::streamsize n) override;
	private:
		std::string &Pending(void);
		CLogBuffer &m_Log;
		int m_Fd;
	};

	mutable std::mutex m_Mutex;
	std::vector<std::string> m_Lines;
	int m_Head;          // next write position
	int m_Count;         // number of valid lines

	CLineBuf m_OutBuf, m_ErrBuf;
	std::streambuf *m_OrigOut;  // original cout buffer
	std::streambuf *m_OrigErr;  // original cerr buffer
};

extern CLogBuffer g_LogBuffer;
