/*
 *   Copyright (c) 2023 by Thomas A. Early N7TAE
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "CurlGet.h"

// curl_global_init() and curl_global_cleanup() are explicitly not thread safe.
// Doing them per instance meant every lookup thread tore down libcurl's global
// state (resolver, TLS) while sibling threads were inside curl_easy_perform(),
// which shows up as spurious transfer timeouts. Initialise exactly once instead,
// and leave cleanup to an explicit call once all threads have been joined.
static std::once_flag s_CurlInitOnce;

CCurlGet::CCurlGet()
{
	std::call_once(s_CurlInitOnce, []() { curl_global_init(CURL_GLOBAL_ALL); });
}

CCurlGet::~CCurlGet()
{
}

void CCurlGet::GlobalCleanup()
{
	curl_global_cleanup();
}

// callback function writes data to a std::ostream
size_t CCurlGet::data_write(void* buf, size_t size, size_t nmemb, void* userp)
{
	if(userp)
	{
		std::ostream& os = *static_cast<std::ostream*>(userp);
		std::streamsize len = size * nmemb;
		if(os.write(static_cast<char*>(buf), len))
			return len;
	}

	return 0;
}

// progress callback: a non-zero return aborts the transfer
int CCurlGet::xfer_info(void *clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
	return static_cast<const std::atomic<bool>*>(clientp)->load() ? 0 : 1;
}

// options both requests use; NOSIGNAL because several threads use libcurl.
// A transfer that stays below 1 KB/s for 30 s is given up, so a long total
// timeout only lets a slow but moving download finish.
CURLcode CCurlGet::Prepare(CURL *curl, const std::string &url, std::stringstream &ss, long timeout, long connecttimeout, long ipresolve)
{
	CURLcode code;
	if(CURLE_OK == (code = curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &data_write))
	&& CURLE_OK == (code = curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L))
	&& CURLE_OK == (code = curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L))
	&& CURLE_OK == (code = curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L))
	&& CURLE_OK == (code = curl_easy_setopt(curl, CURLOPT_FILE, &ss))
	&& CURLE_OK == (code = curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout))
	&& CURLE_OK == (code = curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, connecttimeout))
	&& CURLE_OK == (code = curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L))
	&& CURLE_OK == (code = curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L))
	&& CURLE_OK == (code = curl_easy_setopt(curl, CURLOPT_IPRESOLVE, ipresolve)))
		code = curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	return code;
}

CURLcode CCurlGet::GetURL(const std::string &url, std::stringstream &ss, long timeout, long connecttimeout, long ipresolve, const std::atomic<bool> *running)
{
	CURLcode code(CURLE_FAILED_INIT);
	CURL* curl = curl_easy_init();

	if(curl)
	{
		// a 4xx/5xx answer is a failed download (its body is no database), so the next source is tried
		if(CURLE_OK == (code = Prepare(curl, url, ss, timeout, connecttimeout, ipresolve))
		&& CURLE_OK == (code = curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L))
		&& (nullptr == running
			|| (CURLE_OK == (code = curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &xfer_info))
			&& CURLE_OK == (code = curl_easy_setopt(curl, CURLOPT_XFERINFODATA, const_cast<std::atomic<bool>*>(running)))
			&& CURLE_OK == (code = curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L)))))
		{
			code = curl_easy_perform(curl);
		}
		curl_easy_cleanup(curl);
	}
	if (code != CURLE_OK)
	{
		std::cout << "WARNING: was not able retrieve data at '" << url << "'\nCurl returned: " << code << std::endl;
	}
	return code;
}

CURLcode CCurlGet::PostForm(const std::string &url, const std::string &field, const std::string &value, std::stringstream &ss, long &http, long timeout)
{
	CURLcode code(CURLE_FAILED_INIT);
	http = 0;
	CURL* curl = curl_easy_init();

	if(curl)
	{
		char *esc = curl_easy_escape(curl, value.c_str(), int(value.size()));
		const std::string body = field + "=" + (esc ? esc : "");
		curl_free(esc);
		// a redirect must not turn the POST into a GET that drops the body
		if(CURLE_OK == (code = Prepare(curl, url, ss, timeout, timeout))
		&& CURLE_OK == (code = curl_easy_setopt(curl, CURLOPT_POSTREDIR, long(CURL_REDIR_POST_ALL)))
		&& CURLE_OK == (code = curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str()))
		&& CURLE_OK == (code = curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, long(body.size()))))
		{
			code = curl_easy_perform(curl);
			curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
		}
		curl_easy_cleanup(curl);
	}
	return code;
}
