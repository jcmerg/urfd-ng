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

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iterator>
#include <unordered_map>

// what a protocol keeps per stream id while it lasts (a YSF position, a VW radio
// id). Each transmission has its own id, so entries pile up: once there are many,
// those of the streams neither open nor closed lately go (live, the protocol's
// CProtocol::StreamIdLive)
template <typename T>
class CPerStream
{
public:
	explicit CPerStream(std::function<bool(uint16_t)> live) : m_Live(std::move(live)) {}

	T &Of(uint16_t streamid)
	{
		// once the table doubled since the last time, not with every new id
		if ( m_Map.size() >= m_PruneAt && m_Map.find(streamid) == m_Map.end() )
		{
			for ( auto it = m_Map.begin(); it != m_Map.end(); )
				it = m_Live(it->first) ? std::next(it) : m_Map.erase(it);
			m_PruneAt = std::max<size_t>(64, 2 * m_Map.size());
		}
		return m_Map[streamid];
	}
	const T *Find(uint16_t streamid) const
	{
		const auto it = m_Map.find(streamid);
		return it == m_Map.end() ? nullptr : &it->second;
	}
	void Erase(uint16_t streamid) { m_Map.erase(streamid); }

private:
	std::function<bool(uint16_t)> m_Live;
	size_t m_PruneAt = 64;
	std::unordered_map<uint16_t, T> m_Map;
};
