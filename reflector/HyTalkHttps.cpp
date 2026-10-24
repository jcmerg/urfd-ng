// HyTalkHttps -- HTTP and HTTPS for HyTalk radios, see HyTalkHttps.h
//
// Copyright (C) 2026
// Licensed under the GNU General Public License v3 or later

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <iostream>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "HyTalkHttps.h"

#define HTTPS_TIMEOUT     10      // seconds a connection may take for its request
#define HTTPS_MAX_REQUEST 65536

// an EC P-256 key and a certificate for it, self-signed, valid for 10 years
bool CHyTalkHttps::MakeContext(void)
{
	m_Ctx = SSL_CTX_new(TLS_server_method());
	if (!m_Ctx)
		return false;
	SSL_CTX_set_min_proto_version(m_Ctx, TLS1_2_VERSION);

	EVP_PKEY *key = EVP_EC_gen("P-256");
	X509 *cert = X509_new();
	bool ok = key && cert;
	if (ok)
	{
		X509_set_version(cert, 2);
		uint32_t serial = 0;
		RAND_bytes((unsigned char *)&serial, sizeof(serial));
		ASN1_INTEGER_set(X509_get_serialNumber(cert), long(serial & 0x7fffffff));
		X509_gmtime_adj(X509_getm_notBefore(cert), -24L * 3600);
		X509_gmtime_adj(X509_getm_notAfter(cert), 10L * 365 * 24 * 3600);
		X509_set_pubkey(cert, key);
		X509_NAME *name = X509_get_subject_name(cert);
		X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char *)"urfd HyTalk", -1, -1, 0);
		X509_set_issuer_name(cert, name);
		ok = X509_sign(cert, key, EVP_sha256()) > 0
		  && SSL_CTX_use_certificate(m_Ctx, cert) == 1
		  && SSL_CTX_use_PrivateKey(m_Ctx, key) == 1;
	}
	X509_free(cert);
	EVP_PKEY_free(key);
	return ok;
}

bool CHyTalkHttps::Open(const CIp &ip, Handler handler, const char *what)
{
	m_Handler = handler;
	if (!MakeContext())
	{
		std::cerr << "HyTalk: can't make the TLS certificate for the " << what << std::endl;
		return false;
	}
	m_Fd = socket(AF_INET, SOCK_STREAM, 0);
	if (m_Fd < 0)
		return false;
	int one = 1;
	setsockopt(m_Fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (bind(m_Fd, ip.GetCPointer(), ip.GetSize()) != 0 || listen(m_Fd, 8) != 0)
	{
		std::cerr << "HyTalk: can't listen on the " << what << " port TCP " << ip.GetPort() << std::endl;
		close(m_Fd);
		m_Fd = -1;
		return false;
	}
	fcntl(m_Fd, F_SETFL, O_NONBLOCK);
	return true;
}

void CHyTalkHttps::Drop(SConn &c)
{
	if (c.ssl)
	{
		SSL_shutdown(c.ssl);
		SSL_free(c.ssl);
		c.ssl = nullptr;
	}
	if (c.fd >= 0)
		close(c.fd);
	c.fd = -1;
}

void CHyTalkHttps::Close(void)
{
	for (auto &c : m_Conns)
		Drop(c);
	m_Conns.clear();
	if (m_Fd >= 0)
		close(m_Fd);
	m_Fd = -1;
	if (m_Ctx)
		SSL_CTX_free(m_Ctx);
	m_Ctx = nullptr;
}

bool CHyTalkHttps::ReadRequest(SConn &c)
{
	char buf[2048];
	for (;;)
	{
		int n;
		if (c.ssl)
		{
			n = SSL_read(c.ssl, buf, sizeof(buf));
			if (n <= 0)
			{
				const int err = SSL_get_error(c.ssl, n);
				return err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE;
			}
		}
		else
		{
			n = int(recv(c.fd, buf, sizeof(buf), 0));
			if (n == 0)
				return false;
			if (n < 0)
				return errno == EAGAIN || errno == EWOULDBLOCK;
		}
		c.in.append(buf, size_t(n));
		if (c.in.size() > HTTPS_MAX_REQUEST)
			return false;
	}
}

bool CHyTalkHttps::Flush(SConn &c)
{
	while (c.outPos < c.out.size())
	{
		const char *p = c.out.data() + c.outPos;
		const int len = int(c.out.size() - c.outPos);
		int n;
		if (c.ssl)
		{
			n = SSL_write(c.ssl, p, len);
			if (n <= 0)
			{
				const int err = SSL_get_error(c.ssl, n);
				if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
					return false;
				c.outPos = c.out.size();  // broken, give up
				return true;
			}
		}
		else
		{
			n = int(send(c.fd, p, size_t(len), MSG_NOSIGNAL));
			if (n < 0)
			{
				if (errno == EAGAIN || errno == EWOULDBLOCK)
					return false;
				c.outPos = c.out.size();
				return true;
			}
		}
		c.outPos += size_t(n);
	}
	return true;
}

void CHyTalkHttps::Task(void)
{
	if (m_Fd >= 0)
	{
		struct sockaddr_in sa;
		socklen_t salen = sizeof(sa);
		int fd;
		while ((fd = accept(m_Fd, (struct sockaddr *)&sa, &salen)) >= 0)
		{
			fcntl(fd, F_SETFL, O_NONBLOCK);
			SConn c;
			c.fd = fd;
			char a[INET_ADDRSTRLEN] = "";
			inet_ntop(AF_INET, &sa.sin_addr, a, sizeof(a));
			c.addr = a;
			m_Conns.push_back(c);
			salen = sizeof(sa);
		}
	}

	for (auto it = m_Conns.begin(); it != m_Conns.end(); )
	{
		SConn &c = *it;
		bool closeIt = false;

		// a TLS client hello starts with the handshake record type 0x16
		if (!c.decided)
		{
			unsigned char first;
			const ssize_t n = recv(c.fd, &first, 1, MSG_PEEK);
			if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK))
				closeIt = true;
			else if (n == 1)
			{
				c.decided = true;
				if (first == 0x16)
				{
					c.ssl = SSL_new(m_Ctx);
					if (!c.ssl || SSL_set_fd(c.ssl, c.fd) != 1)
						closeIt = true;
					else
						SSL_set_accept_state(c.ssl);
				}
			}
		}

		if (!closeIt && c.decided && c.out.empty())
		{
			if (!ReadRequest(c))
				closeIt = true;
			// a request is complete with its headers and the body its Content-Length announces
			const auto end = c.in.find("\r\n\r\n");
			if (!closeIt && end != std::string::npos)
			{
				size_t want = 0;
				auto cl = c.in.find("Content-Length:");
				if (cl == std::string::npos)
					cl = c.in.find("content-length:");
				if (cl != std::string::npos && cl < end)
					want = size_t(atoi(c.in.c_str() + cl + 15));
				if (c.in.size() >= end + 4 + want)
				{
					const std::string line = c.in.substr(0, c.in.find("\r\n"));
					const std::string body = c.in.substr(end + 4, want);
					c.out = m_Handler(line, body, c.addr);
				}
			}
		}

		if (!closeIt && !c.out.empty() && Flush(c))
			closeIt = true;

		if (closeIt || c.since.time() > HTTPS_TIMEOUT)
		{
			Drop(c);
			it = m_Conns.erase(it);
		}
		else
			++it;
	}
}
