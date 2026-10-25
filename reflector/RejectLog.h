#pragma once

// RejectLog -- keeps scanners and misconfigured clients from flooding the log.
// Of the rejections of one kind from one address, the first each hour is logged;
// the others are only counted and summed up in one line once that hour is over.
//
// Copyright (C) 2026
// Licensed under the GNU General Public License v3 or later

#include <string>

// true when this rejection is to be logged; kind names it in the summary line,
// e.g. "MMDVM: failed logins", addr is the address without the port
bool RejectLog(const char *kind, const std::string &addr);
