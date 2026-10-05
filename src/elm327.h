//
// elm327.h - OBDX Pro FT / ELM327 AT+DX command layer
//
// Targets the OBDX Pro FT (ELM-compatible AT command set plus the OBDX
// extended DX command set, per "OBDX Pro FT Reference Guide v2"), but the
// AT subset used also works on generic ELM327 adapters.
//
#pragma once

#include <string>
#include <vector>
#include "transport.h"

class Elm327 {
public:
    Elm327();
    ~Elm327();

    // Takes ownership of transport.
    bool open(Transport* t, std::string& err);
    void close();
    bool isOpen() const;

    // Send a command (AT or hex data), read everything until the '>' prompt
    // (or timeout). Returns the raw response with the prompt stripped.
    // ok=false on transport failure / timeout waiting for the prompt.
    std::string command(const std::string& cmd, unsigned timeoutMs, bool& ok);

    // command() + check the response contains "OK".
    bool commandOk(const std::string& cmd, unsigned timeoutMs = 2000);

    // Raw transport read (for the async RX pump in DXPT1 passthrough mode).
    // Returns bytes read (0 on timeout), -1 on error.
    int rawRead(void* buf, int maxlen, unsigned timeoutMs);

    // Split a raw response into trimmed non-empty lines, dropping echo.
    static void splitLines(const std::string& resp, std::vector<std::string>& lines);

    std::string firmware() const { return m_firmware; }

private:
    bool chipInit(std::string& err);

    Transport* m_t;
    std::string m_firmware;
};
