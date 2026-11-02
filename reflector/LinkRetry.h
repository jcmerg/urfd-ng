#pragma once

// LinkRetry -- how long a client protocol waits before it tries to link again
//
// Copyright (C) 2026
// Licensed under the GNU General Public License v3 or later

// DCSClient, DExtraClient and DPlusClient: 5 s, then twice as long each time up to
// 60 s, and 5 s again once linked (ircDDBGateway: 1 s doubling to 60 s). A reflector
// that is down or busy with itself is not asked every 5 s for good.
inline unsigned LinkRetrySeconds(unsigned tries)
{
	return (tries >= 4) ? 60U : (5U << tries);
}
