//
// dvi.cpp - OBDX DVI binary protocol layer
//
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <string>
#include <vector>

#include "dvi.h"
#include "log.h"

static unsigned clampMs(unsigned ms)
{
    return ms > 0x7FFFFFF0u ? 0x7FFFFFF0u : ms;
}

unsigned char dviChecksum(const unsigned char* bytes, size_t n)
{
    unsigned sum = 0;
    for (size_t i = 0; i < n; i++)
        sum += bytes[i];
    return (unsigned char)(~sum & 0xFF);
}

Dvi::Dvi() : m_t(NULL), m_dead(false) {}
Dvi::~Dvi() { close(); }

bool Dvi::isOpen() const { return m_t && m_t->isOpen() && !m_dead; }

void Dvi::close()
{
    if (m_t) {
        delete m_t;
        m_t = NULL;
    }
    m_buf.clear();
    m_dead = false;
}

void Dvi::flush()
{
    m_buf.clear();
}

bool Dvi::open(Transport* t, std::string& err)
{
    close();
    m_t = t;
    if (!m_t->open(err))
        return false;
    return switchToDvi(err);
}

int Dvi::readChunk(unsigned timeoutMs)
{
    if (!m_t || !m_t->isOpen() || m_dead)
        return -1;
    unsigned char buf[1024];
    int n = m_t->read(buf, sizeof(buf), clampMs(timeoutMs));
    if (n < 0) {
        Log("DVI: transport read failed - marking link down");
        m_dead = true;
        return -1;
    }
    for (int i = 0; i < n; i++)
        m_buf.push_back(buf[i]);
    if (m_buf.size() > 65536) {       // runaway guard (max large frame ~12KB)
        Log("DVI: receive buffer runaway (%u bytes) - flushed", (unsigned)m_buf.size());
        m_buf.clear();
    }
    return n;
}

// Extract one valid framed message from m_buf. On a bad checksum or an
// implausible length, drop one byte and keep scanning (framing recovery).
// Returns false when more bytes are needed.
bool Dvi::extractMsg(DviMsg& out)
{
    while (m_buf.size() >= 2) {
        unsigned char cmd = m_buf[0];
        size_t hdr, len;
        bool large = (cmd == DVI_RX_LARGE || cmd == DVI_TX_LARGE);
        if (large) {
            if (m_buf.size() < 3)
                return false;                       // need the 2nd length byte
            len = ((size_t)m_buf[1] << 8) | m_buf[2];
            hdr = 3;
        } else {
            len = m_buf[1];
            hdr = 2;
        }
        if (len > 12288) {                          // implausible - resync
            m_buf.erase(m_buf.begin());
            continue;
        }
        size_t total = hdr + len + 1;               // + checksum
        if (m_buf.size() < total)
            return false;                           // need more bytes
        unsigned char ck = dviChecksum(&m_buf[0], total - 1);
        if (ck != m_buf[total - 1]) {               // bad frame - resync
            m_buf.erase(m_buf.begin());
            continue;
        }
        out.cmd = cmd;
        out.data.assign(m_buf.begin() + hdr, m_buf.begin() + hdr + len);
        m_buf.erase(m_buf.begin(), m_buf.begin() + total);
        return true;
    }
    return false;
}

bool Dvi::send(unsigned char cmd, const unsigned char* data, size_t len, bool large)
{
    if (!m_t || !m_t->isOpen() || m_dead)
        return false;

    std::vector<unsigned char> f;
    f.reserve(len + 4);
    f.push_back(cmd);
    if (large) {
        f.push_back((unsigned char)((len >> 8) & 0xFF));
        f.push_back((unsigned char)(len & 0xFF));
    } else {
        f.push_back((unsigned char)(len & 0xFF));
    }
    for (size_t i = 0; i < len; i++)
        f.push_back(data[i]);
    f.push_back(dviChecksum(f.data(), f.size()));

    if (LogEnabled()) {
        std::string hex;
        static const char* d = "0123456789ABCDEF";
        for (size_t i = 0; i < f.size() && i < 24; i++) {
            hex += d[f[i] >> 4]; hex += d[f[i] & 0xF]; hex += ' ';
        }
        Log("DVI >> %s%s", hex.c_str(), f.size() > 24 ? "..." : "");
    }

    if (!m_t->write(f.data(), (int)f.size())) {
        Log("DVI: transport write failed - marking link down");
        m_dead = true;
        return false;
    }
    return true;
}

bool Dvi::transact(unsigned char cmd, const unsigned char* data, size_t len,
                   unsigned char wantCmd, DviMsg& resp,
                   unsigned timeoutMs, std::vector<DviMsg>* net, bool large)
{
    if (!send(cmd, data, len, large))
        return false;

    DWORD deadline = GetTickCount() + clampMs(timeoutMs);
    for (;;) {
        DviMsg m;
        while (extractMsg(m)) {
            if (m.cmd == DVI_RX_NORMAL || m.cmd == DVI_RX_LARGE) {
                if (net) net->push_back(m);
                continue;
            }
            if (m.cmd == wantCmd) {
                resp = m;
                return true;
            }
            if (m.cmd == DVI_ERROR) {
                // data = [origCmd, errCode, ...]; is it the failure of ours?
                if (!m.data.empty() && m.data[0] == cmd) {
                    resp = m;
                    Log("DVI: error 0x%02X for cmd 0x%02X",
                        m.data.size() > 1 ? m.data[1] : 0, cmd);
                    return true;
                }
                // 0x7F for 0xFF = a bus read error; ignore and keep waiting.
                continue;
            }
            // Some other ack (stale) - ignore and keep waiting.
        }
        long remain = (long)(deadline - GetTickCount());
        if (remain <= 0)
            return false;
        int n = readChunk(remain > 50 ? 50 : (unsigned)remain);
        if (n < 0)
            return false;
    }
}

void Dvi::poll(std::vector<DviMsg>& net, unsigned maxMs)
{
    if (!m_t || !m_t->isOpen() || m_dead)
        return;
    DWORD deadline = GetTickCount() + clampMs(maxMs ? maxMs : 1);
    for (;;) {
        DviMsg m;
        while (extractMsg(m)) {
            if (m.cmd == DVI_RX_NORMAL || m.cmd == DVI_RX_LARGE)
                net.push_back(m);
            // command acks have no meaning here; drop them
        }
        long remain = (long)(deadline - GetTickCount());
        if (remain <= 0)
            break;
        int n = readChunk(remain > 20 ? 20 : (unsigned)remain);
        if (n <= 0)
            break;
    }
}

// --- ELM text preamble -> switch to DVI -----------------------------------

std::string Dvi::elmCommand(const std::string& cmd, unsigned timeoutMs)
{
    std::string tx = cmd + "\r";
    if (!m_t->write(tx.data(), (int)tx.size()))
        return "";
    std::string resp;
    DWORD deadline = GetTickCount() + clampMs(timeoutMs);
    char buf[256];
    for (;;) {
        long remain = (long)(deadline - GetTickCount());
        if (remain <= 0)
            break;
        int n = m_t->read(buf, sizeof(buf), remain > 200 ? 200 : (unsigned)remain);
        if (n < 0)
            return resp;
        if (n == 0)
            continue;
        resp.append(buf, n);
        if (resp.find('>') != std::string::npos)
            break;
    }
    return resp;
}

bool Dvi::switchToDvi(std::string& err)
{
    // The adapter powers up in ELM text mode. Reset, echo off, grab the
    // ident string, then DXDP1 switches to the DVI byte protocol.
    elmCommand("", 300);
    std::string z = elmCommand("ATZ", 4000);
    if (z.empty())
        z = elmCommand("ATZ", 4000);
    if (z.empty()) {
        err = "No response from the adapter (ATZ)";
        return false;
    }
    elmCommand("ATE0", 2000);
    std::string id = elmCommand("AT@1", 2000);
    // keep the first non-prompt, non-OK line as the ident
    {
        std::string cur;
        for (size_t i = 0; i <= id.size(); i++) {
            char c = (i < id.size()) ? id[i] : '\r';
            if (c == '\r' || c == '\n' || c == '>') {
                // trim
                size_t a = 0, b = cur.size();
                while (a < b && (cur[a] == ' ' || cur[a] == '\t')) a++;
                while (b > a && (cur[b-1] == ' ' || cur[b-1] == '\t')) b--;
                std::string line = cur.substr(a, b - a);
                if (!line.empty() && line != "OK" && line != "?") {
                    m_firmware = line;
                    break;
                }
                cur.clear();
            } else {
                cur += c;
            }
        }
    }
    Log("Device ident (ELM): '%s'", m_firmware.c_str());

    elmCommand("DXDP1", 3000);      // -> OK, now in DVI byte mode
    Sleep(50);
    m_buf.clear();                  // discard any trailing text / prompt

    // Confirm DVI is live: request firmware version (0x22 0x01).
    unsigned char sub = 0x01;
    DviMsg resp;
    std::vector<DviMsg> net;
    if (!transact(DVI_INFO, &sub, 1, DVI_INFO_RESP, resp, 2000, &net)) {
        // One more attempt in case DXDP1 was swallowed.
        m_buf.clear();
        elmCommand("DXDP1", 2000);
        Sleep(50);
        m_buf.clear();
        if (!transact(DVI_INFO, &sub, 1, DVI_INFO_RESP, resp, 2000, &net)) {
            err = "Adapter did not enter DVI mode (no 0x32 info response)";
            return false;
        }
    }
    // resp.data = [subcmd, XX, VV, SS, TT] = firmware vX.X.X.X
    if (resp.cmd == DVI_INFO_RESP && resp.data.size() >= 5) {
        char fw[48];
        _snprintf(fw, sizeof(fw), "OBDX Pro FT fw v%u.%u.%u.%u",
                  resp.data[1], resp.data[2], resp.data[3], resp.data[4]);
        fw[sizeof(fw) - 1] = 0;
        if (m_firmware.empty())
            m_firmware = fw;
        Log("DVI confirmed: %s", fw);
    }
    return true;
}
