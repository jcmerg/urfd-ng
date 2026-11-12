//  Copyright © 2019 Jean-Luc Deltombe (LX3JL). All rights reserved.

// urfd -- The universal reflector
// Copyright © 2021 Thomas A. Early N7TAE
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

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <queue>

#include "WiresXCmd.h"

class CWiresxCmdQueue
{
public:
	// constructor
	CWiresxCmdQueue() {}

	// destructor
	~CWiresxCmdQueue() {}

	// lock
	void Lock()                 { m_Mutex.lock(); }
	void Unlock()               { m_Mutex.unlock(); }

	// pass thru
	CWiresxCmd front()        { return queue.front(); }
	void pop()                { queue.pop(); }
	// with the queue locked; wakes the handler's Wait()
	void push(CWiresxCmd cmd) { queue.push(cmd); m_Cv.notify_all(); }
	bool empty() const        { return queue.empty(); }

	// the handler's wait, with the queue not locked: until a command is pushed
	// (push wakes it, called with the queue locked, so it cannot come between the
	// check here and the wait) or the first one is delay seconds old, at once
	// after Stop()
	void Wait(double delay)
	{
		std::unique_lock<std::mutex> lock(m_Mutex);
		if (m_Stop)
			return;
		if (queue.empty())
			m_Cv.wait(lock);
		else if (const double age = queue.front().GetTime().time(); age < delay)
			m_Cv.wait_for(lock, std::chrono::duration<double>(delay - age));
	}
	// the handler runs (Init): Wait() waits again
	void Start(void)
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_Stop = false;
	}
	// ends the handler's wait for good (Close)
	void Stop(void)
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_Stop = true;
		m_Cv.notify_all();
	}

protected:
	// status
	std::mutex  m_Mutex;
	std::condition_variable m_Cv;
	bool m_Stop = false;
	std::queue<CWiresxCmd> queue;
};
