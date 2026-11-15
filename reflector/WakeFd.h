// urfd -- The universal reflector
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

#pragma once

#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/eventfd.h>
#endif

// a descriptor a thread polls and other threads make readable to end its wait:
// an eventfd on Linux, a pipe elsewhere (tcd, which builds this too, also runs
// on macOS). Non-blocking and close-on-exec; a signal is never lost, the reader
// clears all that came at once. Without a descriptor (Open failed) Fd() is -1,
// which poll() leaves out: the waiter has only its timeout then
class CWakeFd
{
public:
	CWakeFd() {}
	~CWakeFd() { Close(); }
	CWakeFd(const CWakeFd &) = delete;
	CWakeFd &operator=(const CWakeFd &) = delete;

	// false and errno set when it failed
	bool Open(void)
	{
		Close();
#ifdef __linux__
		m_Rd = m_Wr = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
		return m_Rd >= 0;
#else
		int w[2];
		if (pipe(w))
			return false;
		for (int fd : w)
		{
			const int fl = fcntl(fd, F_GETFL);
			if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) || fcntl(fd, F_SETFD, FD_CLOEXEC))
			{
				const int err = errno;
				close(w[0]);
				close(w[1]);
				errno = err;
				return false;
			}
		}
		m_Rd = w[0];
		m_Wr = w[1];
		return true;
#endif
	}

	void Close(void)
	{
		if (m_Wr >= 0 && m_Wr != m_Rd)
			close(m_Wr);
		if (m_Rd >= 0)
			close(m_Rd);
		m_Rd = m_Wr = -1;
	}

	int Fd(void) const { return m_Rd; }

	void Signal(void) const
	{
		const uint64_t one = 1;	// an eventfd takes 8 bytes, a pipe any; a full pipe wakes as well
		if (m_Wr >= 0 && write(m_Wr, &one, sizeof(one)) < 0) {}
	}

	void Clear(void) const
	{
		uint64_t buf[8];
		if (m_Rd < 0)
			return;
#ifdef __linux__
		if (read(m_Rd, buf, sizeof(buf)) < 0) {}	// an eventfd gives its whole count at once
#else
		while (read(m_Rd, buf, sizeof(buf)) > 0) {}
#endif
	}

private:
	int m_Rd = -1;
	int m_Wr = -1;
};
