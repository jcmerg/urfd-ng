// HyTalkPnas -- the message layer of Hytera's HyTalk PoC protocol
//
// Copyright (C) 2026
// Licensed under the GNU General Public License v3 or later

#include "HyTalkPnas.h"

#include <openssl/evp.h>
#include <algorithm>
#include <cstring>
#include <cctype>
#include <sstream>
#include <fstream>
#include <cstdio>

#define PNAS_HEADER_SIZE 15

////////////////////////////////////////////////////////////////////////////////////////
// decoding

bool CPnasMsg::ParseIes(const uint8_t *data, size_t len, std::vector<SPnasIe> &out)
{
	size_t pos = 0;
	while (pos + 4 <= len)
	{
		SPnasIe ie;
		ie.iei = (uint16_t)((data[pos] << 8) | data[pos + 1]);
		size_t l = (size_t)((data[pos + 2] << 8) | data[pos + 3]);
		pos += 4;
		if (pos + l > len)
			return false;
		ie.value.assign(data + pos, data + pos + l);
		pos += l;
		out.push_back(std::move(ie));
	}
	return pos == len;
}

bool CPnasMsg::Parse(const uint8_t *data, size_t len)
{
	if (len < PNAS_HEADER_SIZE || (data[0] & 0xF0) != 0x50 || data[10] != 0x00 || data[11] != 0xC0)
		return false;
	seq  = (uint32_t)((data[1] << 24) | (data[2] << 16) | (data[3] << 8) | data[4]);
	tid  = (uint32_t)((data[5] << 24) | (data[6] << 16) | (data[7] << 8) | data[8]);
	type = data[9];
	// the length is the whole message, header included
	size_t total = (size_t)((data[12] << 16) | (data[13] << 8) | data[14]);
	if (total < PNAS_HEADER_SIZE || total > len)
		return false;
	ies.clear();
	return ParseIes(data + PNAS_HEADER_SIZE, total - PNAS_HEADER_SIZE, ies);
}

const SPnasIe *CPnasMsg::Find(uint16_t iei) const
{
	for (const auto &ie : ies)
		if (ie.iei == iei)
			return &ie;
	return nullptr;
}

std::string CPnasMsg::GetString(uint16_t iei) const
{
	const SPnasIe *ie = Find(iei);
	if (!ie)
		return std::string();
	std::string s(ie->value.begin(), ie->value.end());
	auto nul = s.find('\0');
	if (nul != std::string::npos)
		s.resize(nul);
	return s;
}

uint32_t CPnasMsg::GetUint(uint16_t iei, uint32_t def) const
{
	const SPnasIe *ie = Find(iei);
	if (!ie || ie->value.empty() || ie->value.size() > 4)
		return def;
	uint32_t v = 0;
	for (uint8_t b : ie->value)
		v = (v << 8) | b;
	return v;
}

////////////////////////////////////////////////////////////////////////////////////////
// encoding

CPnasWriter::CPnasWriter(uint8_t type, uint32_t tid) : m_Type(type), m_Tid(tid)
{
}

CPnasWriter &CPnasWriter::AddBytes(uint16_t iei, const uint8_t *data, size_t len)
{
	m_Body.push_back(iei >> 8);
	m_Body.push_back(iei & 0xFF);
	m_Body.push_back((len >> 8) & 0xFF);
	m_Body.push_back(len & 0xFF);
	if (len)
		m_Body.insert(m_Body.end(), data, data + len);
	return *this;
}

CPnasWriter &CPnasWriter::AddUint8(uint16_t iei, uint8_t v)
{
	return AddBytes(iei, &v, 1);
}

CPnasWriter &CPnasWriter::AddUint16(uint16_t iei, uint16_t v)
{
	const uint8_t b[2] = { uint8_t(v >> 8), uint8_t(v) };
	return AddBytes(iei, b, 2);
}

CPnasWriter &CPnasWriter::AddUint32(uint16_t iei, uint32_t v)
{
	const uint8_t b[4] = { uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v) };
	return AddBytes(iei, b, 4);
}

CPnasWriter &CPnasWriter::AddString(uint16_t iei, const std::string &s, size_t maxlen)
{
	return AddBytes(iei, (const uint8_t *)s.data(), std::min(s.size(), maxlen));
}

CPnasWriter &CPnasWriter::AddIpv4(uint16_t iei, uint32_t addr_be)
{
	const uint8_t *a = (const uint8_t *)&addr_be;
	const uint8_t b[5] = { 1, a[0], a[1], a[2], a[3] };
	return AddBytes(iei, b, 5);
}

std::vector<uint8_t> CPnasWriter::Finish(uint32_t seq) const
{
	const size_t total = PNAS_HEADER_SIZE + m_Body.size();
	std::vector<uint8_t> msg;
	msg.reserve(total);
	msg.push_back(0x50);
	for (int s = 24; s >= 0; s -= 8) msg.push_back((seq >> s) & 0xFF);
	for (int s = 24; s >= 0; s -= 8) msg.push_back((m_Tid >> s) & 0xFF);
	msg.push_back(m_Type);
	msg.push_back(0x00);
	msg.push_back(0xC0);
	msg.push_back((total >> 16) & 0xFF);
	msg.push_back((total >> 8) & 0xFF);
	msg.push_back(total & 0xFF);
	msg.insert(msg.end(), m_Body.begin(), m_Body.end());
	return msg;
}

////////////////////////////////////////////////////////////////////////////////////////
// Package_Data and the static keys

// The HyTalk app encrypts its signalling with keys built into its protocol
// library: the DES/RC4 key (8 bytes), then the AES-128, -192 and -256 keys
// (16, 24 and 32 bytes), 80 bytes in all. urfd does not ship them; the
// operator puts them into the file [HyTalk] KeyFile names (docs/HYTALK.md).
#define PNAS_KEY_BLOB_SIZE 80
static uint8_t s_KeyBlob[PNAS_KEY_BLOB_SIZE];
static bool s_HaveKeys = false;

bool PnasPackage::LoadKeys(const std::string &path, std::string &err)
{
	std::ifstream in(path);
	if (!in.is_open())
	{
		err = "cannot open " + path;
		return false;
	}
	// hex bytes, "a5 3c ..." or "0xa5, 0x3c, ...", '#' starts a comment
	std::vector<uint8_t> bytes;
	std::string line;
	while (std::getline(in, line))
	{
		line = line.substr(0, line.find('#'));
		for (char &c : line)
			if (c == ',' || c == '{' || c == '}' || c == ';')
				c = ' ';
		std::istringstream ss(line);
		std::string tok;
		while (ss >> tok)
		{
			if (tok.size() > 2 && tok[0] == '0' && (tok[1] == 'x' || tok[1] == 'X'))
				tok = tok.substr(2);
			if (tok.size() != 2 || !std::isxdigit((unsigned char)tok[0]) || !std::isxdigit((unsigned char)tok[1]))
			{
				err = path + ": '" + tok + "' is not a hex byte";
				return false;
			}
			bytes.push_back((uint8_t)std::stoul(tok, nullptr, 16));
		}
	}
	if (bytes.size() != PNAS_KEY_BLOB_SIZE)
	{
		err = path + " has " + std::to_string(bytes.size()) + " bytes, " + std::to_string(PNAS_KEY_BLOB_SIZE) + " expected";
		return false;
	}
	memcpy(s_KeyBlob, bytes.data(), PNAS_KEY_BLOB_SIZE);
	s_HaveKeys = true;
	return true;
}

bool PnasPackage::HaveKeys(void)
{
	return s_HaveKeys;
}

// the key the client picks: by key id for AES, the 8 bytes (24 for 3DES) at the start otherwise
static const uint8_t *PackageKey(uint8_t keyId)
{
	switch (keyId)
	{
	case 1:  return s_KeyBlob + 8;
	case 2:  return s_KeyBlob + 24;
	case 3:  return s_KeyBlob + 48;
	default: return s_KeyBlob;
	}
}

static const EVP_CIPHER *PackageCipher(uint8_t encType)
{
	switch (encType)
	{
	case 1:  return EVP_aes_128_cbc();
	case 2:  return EVP_aes_192_cbc();
	case 3:  return EVP_aes_256_cbc();
	case 4:  return EVP_des_cbc();
	case 5:  return EVP_des_ede3_cbc();
	case 6:  return EVP_rc4();
	default: return nullptr;
	}
}

// zero IV, PKCS#7 padding for the block ciphers (OpenSSL's default), none for RC4
static bool PackageCrypt(uint8_t encType, uint8_t keyId, bool encrypt, const uint8_t *in, size_t len, std::vector<uint8_t> &out)
{
	if (!s_HaveKeys)
		return false;	// no key file: encrypted units cannot be read or made
	const EVP_CIPHER *cipher = PackageCipher(encType);
	if (!cipher)
		return false;
	EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
	if (!ctx)
		return false;
	const uint8_t iv[16] = {};
	bool ok = EVP_CipherInit_ex(ctx, cipher, nullptr, nullptr, nullptr, encrypt ? 1 : 0) == 1;
	if (ok && encType == 6)
		ok = EVP_CIPHER_CTX_set_key_length(ctx, 8) == 1;
	if (ok)
		ok = EVP_CipherInit_ex(ctx, nullptr, nullptr, PackageKey(keyId), iv, encrypt ? 1 : 0) == 1;
	int n1 = 0, n2 = 0;
	if (ok)
	{
		out.resize(len + 32);
		ok = EVP_CipherUpdate(ctx, out.data(), &n1, in, (int)len) == 1
		  && EVP_CipherFinal_ex(ctx, out.data() + n1, &n2) == 1;
	}
	EVP_CIPHER_CTX_free(ctx);
	if (!ok)
		return false;
	out.resize(n1 + n2);
	return true;
}

// the length of a padded block cipher text for plain len
static size_t PackageAlign(uint8_t encType, size_t len)
{
	if (encType >= 1 && encType <= 3)
		return (len / 16 + 1) * 16;
	if (encType == 4 || encType == 5)
		return (len / 8 + 1) * 8;
	return len;
}

int PnasPackage::Take(std::vector<uint8_t> &stream, std::vector<uint8_t> &msg, uint8_t &encType, uint8_t &keyId)
{
	if (stream.empty())
		return 0;
	const uint8_t options = stream[0];
	if (options > 1)
		return -1;
	const size_t start = 1 + 4 * options;
	if (stream.size() < start + PNAS_HEADER_SIZE)
		return 0;

	if (options == 0)
	{
		encType = 0;
		keyId = 0;
		const uint8_t *p = stream.data() + 1;
		const size_t total = (size_t)((p[12] << 16) | (p[13] << 8) | p[14]);
		if (total < PNAS_HEADER_SIZE || total > 0x10000)
			return -1;
		if (stream.size() < 1 + total)
			return 0;
		msg.assign(p, p + total);
		stream.erase(stream.begin(), stream.begin() + 1 + total);
		return 1;
	}

	// encrypted: the stream position is known only after decrypting the first block,
	// so the plain header gives the length once the whole message has arrived
	encType = stream[3] & 0x7F;
	keyId = stream[4];
	if (!PackageCipher(encType))
		return -1;
	const size_t block = (encType <= 3) ? 16 : ((encType <= 5) ? 8 : 1);
	if (block > 1 && stream.size() < start + 16)
		return 0;
	// the header is in the first 16 (or two times 8) bytes: decrypt them alone (CBC, zero IV)
	std::vector<uint8_t> head;
	size_t probe = (block > 1) ? 16 : PNAS_HEADER_SIZE;
	{
		const EVP_CIPHER *cipher = PackageCipher(encType);
		EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
		const uint8_t iv[16] = {};
		int n = 0;
		head.resize(probe + 32);
		bool ok = ctx && EVP_CipherInit_ex(ctx, cipher, nullptr, nullptr, nullptr, 0) == 1;
		if (ok && encType == 6)
			ok = EVP_CIPHER_CTX_set_key_length(ctx, 8) == 1;
		if (ok)
			ok = EVP_CipherInit_ex(ctx, nullptr, nullptr, PackageKey(keyId), iv, 0) == 1;
		if (ok)
			EVP_CIPHER_CTX_set_padding(ctx, 0);
		ok = ok && EVP_CipherUpdate(ctx, head.data(), &n, stream.data() + start, (int)probe) == 1;
		if (ctx)
			EVP_CIPHER_CTX_free(ctx);
		if (!ok || n < PNAS_HEADER_SIZE)
			return -1;
	}
	const size_t total = (size_t)((head[12] << 16) | (head[13] << 8) | head[14]);
	if (head[0] != 0x50 || total < PNAS_HEADER_SIZE || total > 0x10000)
		return -1;
	const size_t clen = PackageAlign(encType, total);
	if (stream.size() < start + clen)
		return 0;
	if (!PackageCrypt(encType, keyId, false, stream.data() + start, clen, msg))
		return -1;
	stream.erase(stream.begin(), stream.begin() + start + clen);
	return 1;
}

bool PnasPackage::Wrap(const std::vector<uint8_t> &msg, uint8_t encType, uint8_t keyId, std::vector<uint8_t> &out)
{
	out.clear();
	if (encType == 0)
	{
		out.push_back(0x00);
		out.insert(out.end(), msg.begin(), msg.end());
		return true;
	}
	std::vector<uint8_t> enc;
	if (!PackageCrypt(encType, keyId, true, msg.data(), msg.size(), enc))
		return false;
	out = { 0x01, 0x00, 0x00, uint8_t(encType | 0x80), keyId };
	out.insert(out.end(), enc.begin(), enc.end());
	return true;
}

////////////////////////////////////////////////////////////////////////////////////////
// digest

std::string PnasMd5Hex(const std::string &s)
{
	uint8_t d[16];
	unsigned int dlen = 0;
	EVP_Digest(s.data(), s.size(), d, &dlen, EVP_md5(), nullptr);
	char hex[33];
	for (int i = 0; i < 16; i++)
		snprintf(hex + 2 * i, 3, "%02x", d[i]);
	return std::string(hex, 32);
}

std::string PnasDigestHa1(const std::string &ha1, const std::string &nonce)
{
	return PnasMd5Hex(ha1 + ":" + nonce + ":" + PnasMd5Hex("REGISTER:"));
}

std::string PnasDigest(const std::string &user, const std::string &realm, const std::string &password, const std::string &nonce)
{
	return PnasDigestHa1(PnasMd5Hex(user + ":" + realm + ":" + password), nonce);
}
