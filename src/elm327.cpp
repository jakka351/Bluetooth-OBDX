//
// elm327.cpp - ELM327 AT command layer
//
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <string>
#include <vector>

#include "elm327.h"
#include "log.h"

Elm327::Elm327() : m_t(NULL) {}

Elm327::~Elm327() { close(); }

bool Elm327::isOpen() const { return m_t && m_t->isOpen(); }

void Elm327::close()
{
    if (m_t) {
        delete m_t;
        m_t = NULL;
    }
}

bool Elm327::open(Transport* t, std::string& err)
{
    close();
    m_t = t;
    if (!m_t->open(err))
        return false;
    return chipInit(err);
}

std::string Elm327::command(const std::string& cmd, unsigned timeoutMs, bool& ok)
{
    ok = false;
    if (!m_t || !m_t->isOpen())
        return "";

    Log("ELM >> %s", cmd.c_str());

    std::string tx = cmd + "\r";
    if (!m_t->write(tx.data(), (int)tx.size())) {
        Log("ELM: transport write failed");
        return "";
    }

    std::string resp;
    DWORD deadline = GetTickCount() + timeoutMs;
    char buf[512];
    for (;;) {
        DWORD now = GetTickCount();
        if ((long)(deadline - now) <= 0)
            break;
        int n = m_t->read(buf, sizeof(buf), deadline - now);
        if (n < 0) {
            Log("ELM: transport read failed/closed");
            return resp;
        }
        if (n == 0)
            break;  // timeout
        resp.append(buf, n);
        if (resp.find('>') != std::string::npos) {
            ok = true;
            break;
        }
    }

    // Strip prompt and NULs
    std::string clean;
    for (size_t i = 0; i < resp.size(); i++) {
        char c = resp[i];
        if (c != '>' && c != '\0')
            clean += c;
    }

    if (LogEnabled()) {
        std::string printable;
        for (size_t i = 0; i < clean.size(); i++)
            printable += (clean[i] == '\r' || clean[i] == '\n') ? '|' : clean[i];
        Log("ELM << %s%s", printable.c_str(), ok ? "" : " [NO PROMPT]");
    }
    return clean;
}

bool Elm327::commandOk(const std::string& cmd, unsigned timeoutMs)
{
    bool ok = false;
    std::string r = command(cmd, timeoutMs, ok);
    return ok && r.find("OK") != std::string::npos;
}

int Elm327::rawRead(void* buf, int maxlen, unsigned timeoutMs)
{
    if (!m_t || !m_t->isOpen())
        return -1;
    return m_t->read(buf, maxlen, timeoutMs);
}

void Elm327::splitLines(const std::string& resp, std::vector<std::string>& lines)
{
    lines.clear();
    std::string cur;
    for (size_t i = 0; i <= resp.size(); i++) {
        char c = (i < resp.size()) ? resp[i] : '\r';
        if (c == '\r' || c == '\n') {
            // trim
            size_t a = 0, b = cur.size();
            while (a < b && (cur[a] == ' ' || cur[a] == '\t')) a++;
            while (b > a && (cur[b-1] == ' ' || cur[b-1] == '\t')) b--;
            std::string line = cur.substr(a, b - a);
            if (!line.empty())
                lines.push_back(line);
            cur.clear();
        } else {
            cur += c;
        }
    }
}

bool Elm327::chipInit(std::string& err)
{
    bool ok = false;

    // Some adapters are mid-print when we connect; a bare CR flushes that.
    command("", 300, ok);

    // ATZ - full reset. Response ends with ident string but some clones
    // don't print the prompt promptly, so tolerate a missing prompt here.
    std::string r = command("ATZ", 4000, ok);
    if (r.empty()) {
        // Retry once - BT adapters sometimes swallow the first write.
        r = command("ATZ", 4000, ok);
        if (r.empty()) {
            err = "No response from ELM327 (ATZ)";
            return false;
        }
    }

    // Echo off first so subsequent parsing is clean.
    if (!commandOk("ATE0")) {
        // Echo might still have been on for this one; try once more.
        if (!commandOk("ATE0")) {
            err = "Device did not accept ATE0";
            return false;
        }
    }

    // Identify. OBDX tools answer AT@1 with e.g. "OBDX Pro FT"; generic
    // ELM327s answer with their ident string too.
    std::string id = command("AT@1", 2000, ok);
    std::vector<std::string> lines;
    splitLines(id, lines);
    for (size_t i = 0; i < lines.size(); i++) {
        if (lines[i] != "OK" && lines[i] != "?") {
            m_firmware = lines[i];
            break;
        }
    }
    if (m_firmware.empty()) {
        std::string ati = command("ATI", 2000, ok);
        splitLines(ati, lines);
        if (!lines.empty())
            m_firmware = lines[0];
    }
    Log("Device ident: '%s'", m_firmware.c_str());

    commandOk("ATL0");   // linefeeds off
    commandOk("ATS1");   // spaces on between bytes (we strip them anyway)
    commandOk("ATH1");   // headers on - we need CAN IDs / VPW headers
    commandOk("ATAT1");  // adaptive timing
    commandOk("ATST32"); // response wait: 0x32 * 4 = 200ms

    return true;
}
