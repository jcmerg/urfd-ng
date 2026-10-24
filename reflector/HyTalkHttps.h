#pragma once

// HyTalkHttps -- a small HTTP server for HyTalk radios that speaks TLS or
// plain HTTP, whichever the client starts with. The radios use HTTPS for
// their user profile (on the PNAS port) and for the presence server (the
// group list); they check neither certificate nor host name, so a
// self-signed one made at start-up will do. TLS 1.2 is needed, the radio
// offers nothing newer.
//
// Copyright (C) 2026
// Licensed under the GNU General Public License v3 or later

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "IP.h"
#include "Timer.h"

typedef struct ssl_st SSL;
typedef struct ssl_ctx_st SSL_CTX;

class CHyTalkHttps
{
public:
	// the whole HTTP response to a request line and body from addr
	using Handler = std::function<std::string(const std::string &requestLine, const std::string &body, const std::string &addr)>;

	~CHyTalkHttps() { Close(); }

	// listen on TCP ip; what names the service in error messages
	bool Open(const CIp &ip, Handler handler, const char *what);
	void Close(void);
	// accepts, reads and answers without blocking
	void Task(void);

private:
	struct SConn
	{
		int fd = -1;
		SSL *ssl = nullptr;
		bool decided = false;         // TLS or plain HTTP known
		std::string addr;
		std::string in, out;
		size_t outPos = 0;
		CTimer since;
	};
	bool MakeContext(void);
	bool ReadRequest(SConn &c);       // false: closed or broken
	bool Flush(SConn &c);             // true: all of out sent
	void Drop(SConn &c);

	int m_Fd = -1;
	SSL_CTX *m_Ctx = nullptr;
	Handler m_Handler;
	std::vector<SConn> m_Conns;
};
