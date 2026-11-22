#pragma once

//  Copyright © 2015 Jean-Luc Deltombe (LX3JL). All rights reserved.

// tcd - a hybrid transcoder using DVSI hardware and Codec2 software
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

#include <deque>
#include <mutex>
#include <condition_variable>
#include <memory>

#include "TranscoderPacket.h"

// a blocking queue between threads: pop waits for an item; after Shutdown pop
// gives up at once, and what is left or pushed after is dropped
template <typename T>
class CBlockingQueue
{
public:
	// the queue's size after it, 0 when it is shut down (dropped)
	std::size_t push(T item)
	{
		std::size_t size;
		{
			std::lock_guard<std::mutex> lock(mx);
			if (stop)
				return 0;
			q.push_back(std::move(item));
			size = q.size();
		}
		cv.notify_one();
		return size;
	}

	// false when shut down
	bool pop(T &item)
	{
		std::unique_lock<std::mutex> lock(mx);
		cv.wait(lock, [this] { return stop || !q.empty(); });
		if (stop)
		{
			q.clear();
			return false;
		}
		item = std::move(q.front());
		q.pop_front();
		return true;
	}

	// without waiting: false when it is empty
	bool try_pop(T &item)
	{
		std::lock_guard<std::mutex> lock(mx);
		if (q.empty())
			return false;
		item = std::move(q.front());
		q.pop_front();
		return true;
	}

	bool IsEmpty()
	{
		std::lock_guard<std::mutex> lock(mx);
		return q.empty();
	}

	void Shutdown()
	{
		{
			std::lock_guard<std::mutex> lock(mx);
			stop = true;
		}
		cv.notify_all();
	}

private:
	std::mutex mx;
	std::condition_variable cv;
	std::deque<T> q;
	bool stop = false;
};

// for holding CTranscoder packets while the vocoders are working their magic
class CPacketQueue : public CBlockingQueue<std::shared_ptr<CTranscoderPacket>>
{
public:
	// nullptr when shut down
	std::shared_ptr<CTranscoderPacket> pop()
	{
		std::shared_ptr<CTranscoderPacket> packet;
		CBlockingQueue::pop(packet);
		return packet;
	}
	// without waiting: nullptr when it is empty
	std::shared_ptr<CTranscoderPacket> try_pop()
	{
		std::shared_ptr<CTranscoderPacket> packet;
		CBlockingQueue::try_pop(packet);
		return packet;
	}
};
