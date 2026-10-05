//
// shim.cpp - BlueJ2534: SAE J2534-1 v04.04 PassThru shim that talks to an
// OBDX Pro FT over Bluetooth SPP (RFCOMM) instead of USB, using the ELM AT
// command set plus the OBDX extended DX command set.
//
// Proof of concept targeting the OBDX Pro FT:
//   - CAN / ISO15765 first-class: TX via "DXSD <4-byte ID> <data>" (header
//     inline, no ATSH juggling), ISO-TP segmentation/flow-control handled by
//     the tool (ATCAF1 + ATFCSH/ATFCSD/ATFCSM1 from the J2534 FC filter).
//   - DXPT1 passthrough mode streams received frames at any time, so a
//     background pump feeds PassThruReadMsgs asynchronously (no "send 1,
//     receive 1" ELM limitation).
//   - J1850 VPW supported (ATSP2); 125 kbaud CAN maps to MS-CAN (ATSPE,
//     Ford pins 3/11). No K-line on the FT, so ISO9141/ISO14230 are refused.
//   - PassThruSetProgrammingVoltage drives the Ford FEPS pin via DXFEPS1/0.
//
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <deque>
#include <map>

#include "j2534.h"
#include "transport.h"
#include "elm327.h"
#include "log.h"

// =========================================================================
// Globals / configuration
// =========================================================================

static HINSTANCE g_hInst = NULL;
static char g_dllDir[MAX_PATH] = {0};

struct Config {
    std::string mode;      // "bt" or "com"
    std::string btAddress; // optional AA:BB:CC:DD:EE:FF
    std::string btName;    // discovery name substring
    std::string comPort;   // COMx
    int baud;
    bool log;
};
static Config g_cfg;

static char g_lastError[256] = "No error";

static void setLastError(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    _vsnprintf(g_lastError, sizeof(g_lastError) - 1, fmt, args);
    va_end(args);
    g_lastError[sizeof(g_lastError) - 1] = 0;
    Log("ERROR: %s", g_lastError);
}

static void loadConfig()
{
    char ini[MAX_PATH];
    _snprintf(ini, sizeof(ini), "%s\\BlueJ2534.ini", g_dllDir);
    ini[MAX_PATH - 1] = 0;

    char buf[256];
    GetPrivateProfileStringA("connection", "Mode", "bt", buf, sizeof(buf), ini);
    g_cfg.mode = buf;
    GetPrivateProfileStringA("connection", "BtAddress", "", buf, sizeof(buf), ini);
    g_cfg.btAddress = buf;
    GetPrivateProfileStringA("connection", "BtName", "OBDII", buf, sizeof(buf), ini);
    g_cfg.btName = buf;
    GetPrivateProfileStringA("connection", "ComPort", "COM5", buf, sizeof(buf), ini);
    g_cfg.comPort = buf;
    g_cfg.baud = GetPrivateProfileIntA("connection", "Baud", 38400, ini);
    g_cfg.log = GetPrivateProfileIntA("options", "Log", 1, ini) != 0;
}

static unsigned long nowMicros()
{
    static LARGE_INTEGER freq = {0};
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (unsigned long)((c.QuadPart * 1000000) / freq.QuadPart);
}

// =========================================================================
// Hex helpers
// =========================================================================

static bool isHexDigit(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

static int hexVal(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static std::string toHex(const unsigned char* data, unsigned long len)
{
    static const char* digits = "0123456789ABCDEF";
    std::string s;
    s.reserve(len * 2);
    for (unsigned long i = 0; i < len; i++) {
        s += digits[data[i] >> 4];
        s += digits[data[i] & 0xF];
    }
    return s;
}

// =========================================================================
// Device & channel state
// =========================================================================

struct Filter {
    bool          used;
    unsigned long type;            // PASS_FILTER / BLOCK_FILTER / FLOW_CONTROL_FILTER
    unsigned long maskId;
    unsigned long patternId;
    unsigned long flowControlId;   // tester TX id (FLOW_CONTROL only)
    unsigned long flags;
};

struct PeriodicMsg {
    bool          used;
    PASSTHRU_MSG  msg;
    unsigned long intervalMs;
    DWORD         nextDue;
};

struct IsoTpRx {
    bool          active;
    unsigned long expectedLen;
    unsigned char nextSeq;
    std::vector<unsigned char> data;
    DWORD         lastActivity;
};

#define MAX_FILTERS   10
#define MAX_PERIODIC  10
#define RX_QUEUE_MAX  2048

struct Channel {
    bool          open;
    unsigned long protocol;     // base protocol (CAN_PS -> CAN, etc.)
    unsigned long appProtocol;  // protocol ID the application connected with
    unsigned long pins;         // J1962_PINS: 0x060E = HS-CAN, 0x030B = MS-CAN
    unsigned long flags;
    unsigned long baud;
    unsigned long loopback;
    unsigned long bs, stmin;       // our FC parameters (advertised via ATFCSD)

    Filter        filters[MAX_FILTERS];
    PeriodicMsg   periodic[MAX_PERIODIC];

    std::deque<PASSTHRU_MSG> rxQueue;
    CRITICAL_SECTION          rxLock;
    HANDLE                    rxEvent;

    std::map<unsigned long, IsoTpRx> isotp;

    // ELM programming cache - avoid resending AT commands needlessly
    long          curTxHeader;     // -1 = unknown
    long          curRxAddr;       // ATCRA, -1 = none
    long          curFcHeader;     // ATFCSH, -1 = none
};

struct Device {
    bool             open;
    Elm327           elm;
    CRITICAL_SECTION lock;          // serializes all ELM transactions
    HANDLE           periodicThread;
    HANDLE           periodicStop;
};

static Device  g_dev;
static Channel g_chan;
static bool    g_initDone = false;

#define DEVICE_ID   1
#define CHANNEL_ID  1

static void ensureInit()
{
    if (!g_initDone) {
        InitializeCriticalSection(&g_dev.lock);
        InitializeCriticalSection(&g_chan.rxLock);
        g_chan.rxEvent = CreateEventA(NULL, FALSE, FALSE, NULL);
        g_dev.periodicStop = CreateEventA(NULL, TRUE, FALSE, NULL);
        g_initDone = true;
    }
}

// =========================================================================
// RX queue
// =========================================================================

static void enqueueMsg(const PASSTHRU_MSG& m)
{
    EnterCriticalSection(&g_chan.rxLock);
    if (g_chan.rxQueue.size() < RX_QUEUE_MAX)
        g_chan.rxQueue.push_back(m);
    LeaveCriticalSection(&g_chan.rxLock);
    SetEvent(g_chan.rxEvent);
}

static void makeMsg(PASSTHRU_MSG& m, unsigned long protocol, unsigned long rxStatus,
                    unsigned long id, const unsigned char* payload, unsigned long payloadLen,
                    bool is29)
{
    memset(&m, 0, sizeof(m));
    m.ProtocolID = protocol;
    m.RxStatus = rxStatus | (is29 ? CAN_29BIT_ID : 0);
    m.Timestamp = nowMicros();
    m.Data[0] = (unsigned char)(id >> 24);
    m.Data[1] = (unsigned char)(id >> 16);
    m.Data[2] = (unsigned char)(id >> 8);
    m.Data[3] = (unsigned char)(id);
    if (payload && payloadLen) {
        if (payloadLen > sizeof(m.Data) - 4)
            payloadLen = sizeof(m.Data) - 4;
        memcpy(m.Data + 4, payload, payloadLen);
    }
    m.DataSize = 4 + payloadLen;
    m.ExtraDataIndex = m.DataSize;
}

// =========================================================================
// Filters
// =========================================================================

// Returns true if a frame/message with this CAN id should be delivered.
static bool filterAccept(unsigned long id)
{
    bool anyPass = false;
    for (int i = 0; i < MAX_FILTERS; i++) {
        const Filter& f = g_chan.filters[i];
        if (!f.used)
            continue;
        if (f.type == BLOCK_FILTER) {
            if ((id & f.maskId) == (f.patternId & f.maskId))
                return false;
        } else { // PASS or FLOW_CONTROL
            anyPass = true;
        }
    }
    if (!anyPass)
        return true;    // lenient POC: nothing configured -> pass everything

    for (int i = 0; i < MAX_FILTERS; i++) {
        const Filter& f = g_chan.filters[i];
        if (!f.used || f.type == BLOCK_FILTER)
            continue;
        if ((id & f.maskId) == (f.patternId & f.maskId))
            return true;
    }
    return false;
}

// Find the flow-control filter matching an RX id -> our TX id for FC frames.
static Filter* findFlowFilterByRx(unsigned long rxId)
{
    for (int i = 0; i < MAX_FILTERS; i++) {
        Filter& f = g_chan.filters[i];
        if (f.used && f.type == FLOW_CONTROL_FILTER &&
            (rxId & f.maskId) == (f.patternId & f.maskId))
            return &f;
    }
    return NULL;
}

// =========================================================================
// ELM programming helpers (call with device lock held)
// =========================================================================

static bool is29Bit()
{
    return (g_chan.flags & CAN_29BIT_ID) != 0;
}

static bool elmSetRxAddr(unsigned long id)
{
    if (g_chan.curRxAddr == (long)id)
        return true;
    char cmd[48];
    if (is29Bit())
        _snprintf(cmd, sizeof(cmd), "ATCRA %02lX %02lX %02lX %02lX",
                  (id >> 24) & 0x1F, (id >> 16) & 0xFF, (id >> 8) & 0xFF, id & 0xFF);
    else
        _snprintf(cmd, sizeof(cmd), "ATCRA %03lX", id & 0x7FF);
    bool ok = g_dev.elm.commandOk(cmd);
    if (ok)
        g_chan.curRxAddr = (long)id;
    return ok;
}

static void elmClearRxAddr()
{
    if (g_chan.curRxAddr == -1)
        return;
    g_dev.elm.commandOk("ATCRA");
    g_chan.curRxAddr = -1;
}

// Open the hardware filter completely (mask = 0 -> every frame passes);
// software filters in the shim then decide what the app actually sees.
static void elmOpenMask()
{
    if (is29Bit())
        g_dev.elm.commandOk("ATCM 00 00 00 00");
    else
        g_dev.elm.commandOk("ATCM 000");
    g_chan.curRxAddr = -1;
}

// Program the tool to auto-answer First Frames with our Flow Control frame
// (header = tester ID from the J2534 FC filter, data = 30 BS STmin).
static bool elmSetFlowControl(unsigned long txId)
{
    if (g_chan.curFcHeader == (long)txId)
        return true;
    char cmd[48];
    bool ok;
    if (is29Bit())
        _snprintf(cmd, sizeof(cmd), "ATFCSH %02lX %02lX %02lX %02lX",
                  (txId >> 24) & 0x1F, (txId >> 16) & 0xFF,
                  (txId >> 8) & 0xFF, txId & 0xFF);
    else
        _snprintf(cmd, sizeof(cmd), "ATFCSH %03lX", txId & 0x7FF);
    ok = g_dev.elm.commandOk(cmd);
    _snprintf(cmd, sizeof(cmd), "ATFCSD 30 %02lX %02lX",
              g_chan.bs & 0xFF, g_chan.stmin & 0xFF);
    ok = g_dev.elm.commandOk(cmd) && ok;
    ok = g_dev.elm.commandOk("ATFCSM1") && ok;
    if (ok)
        g_chan.curFcHeader = (long)txId;
    return ok;
}

// Map (base protocol, pins, baud, 29-bit flag) to an OBDX/ELM protocol
// number. OBDX Pro FT: 2=VPW, 6/7=CAN 500k 11/29bit, 8/9=CAN 250k 11/29bit,
// B=USER CAN 125k, C=USER CAN 50k, E=MS-CAN 125k (Ford, pins 3/11 - the
// tool switches the transceiver pins itself on ATSPE).
// The FT has no K-line, so ISO9141/ISO14230 are rejected.
static const char* elmProtocolFor(unsigned long protocol, unsigned long pins,
                                  unsigned long baud, bool f29, std::string& warn)
{
    switch (protocol) {
    case J1850VPW: return "2";
    case CAN:
    case ISO15765:
        if (pins == 0x030B) return "E";    // MS-CAN pins 3/11 (125k fixed)
        if (baud == 500000) return f29 ? "7" : "6";
        if (baud == 250000) return f29 ? "9" : "8";
        if (baud == 125000) return "E";    // MS-CAN by baud rate
        if (baud == 50000)  return "C";
        warn = "Unsupported CAN baud rate, defaulting to 500k";
        return f29 ? "7" : "6";
    }
    return NULL;
}

// Reprogram the tool for the channel's current bus (protocol number, CAN
// auto-formatting, filters, flow control, passthrough mode). Used on
// connect and again on a J1962_PINS bus switch. Device lock held.
static bool programBus(std::string& err)
{
    std::string warn;
    const char* ep = elmProtocolFor(g_chan.protocol, g_chan.pins, g_chan.baud,
                                    is29Bit(), warn);
    if (!ep) {
        err = "protocol not supported by OBDX Pro FT";
        return false;
    }
    if (!warn.empty())
        Log("programBus: %s", warn.c_str());

    if (!g_dev.elm.commandOk(std::string("ATSP") + ep)) {
        err = "tool rejected ATSP";
        return false;
    }

    if (g_chan.protocol == CAN || g_chan.protocol == ISO15765) {
        // Raw CAN: no auto formatting. ISO15765: tool adds length byte,
        // segments multi-frame TX and handles flow control (CAF1).
        g_dev.elm.commandOk(g_chan.protocol == CAN ? "ATCAF0" : "ATCAF1");

        // Changing protocol can reset filter state: start from a fully
        // open hardware filter, then re-apply any flow-control filter.
        g_dev.elm.commandOk("ATAR");
        g_chan.curRxAddr = -1;
        g_chan.curFcHeader = -1;
        elmOpenMask();
        for (int i = 0; i < MAX_FILTERS; i++) {
            Filter& f = g_chan.filters[i];
            if (f.used && f.type == FLOW_CONTROL_FILTER) {
                elmSetRxAddr(f.patternId);
                elmSetFlowControl(f.flowControlId);
                break;  // tool has a single FC header slot
            }
        }
    }

    // Passthrough mode: stream received frames at any time.
    g_dev.elm.commandOk("DXPT1");
    return true;
}

// =========================================================================
// RX parsing: ELM response lines -> frames -> ISO-TP / queue
// =========================================================================

struct RawFrame {
    unsigned long id;
    std::vector<unsigned char> data;    // can exceed 8 bytes: the OBDX
                                        // delivers assembled ISO-TP messages
                                        // as a single line in CAF1 mode
};

// Parse one response line (headers on) into a CAN frame / assembled message.
static bool parseCanLine(const std::string& line, RawFrame& f)
{
    std::string hex;
    for (size_t i = 0; i < line.size(); i++) {
        char c = line[i];
        if (c == ' ')
            continue;
        if (!isHexDigit(c))
            return false;   // "NO DATA", "CAN ERROR", "SEARCHING...", etc.
        hex += c;
    }

    size_t idLen = is29Bit() ? 8 : 3;
    if (hex.size() < idLen + 2)
        return false;

    f.id = 0;
    for (size_t i = 0; i < idLen; i++)
        f.id = (f.id << 4) | hexVal(hex[i]);

    size_t dataChars = hex.size() - idLen;
    if (dataChars % 2)
        dataChars--;        // tolerate a stray trailing nibble
    size_t len = dataChars / 2;
    f.data.clear();
    f.data.reserve(len);
    for (size_t i = 0; i < len; i++)
        f.data.push_back((unsigned char)((hexVal(hex[idLen + i*2]) << 4) |
                                          hexVal(hex[idLen + i*2 + 1])));
    return !f.data.empty();
}

static void isotpFeed(const RawFrame& f);

// Digest all lines from an ELM response (frames received from the bus).
static void processResponse(const std::string& resp)
{
    std::vector<std::string> lines;
    Elm327::splitLines(resp, lines);
    for (size_t i = 0; i < lines.size(); i++) {
        const std::string& line = lines[i];
        if (line == "OK" || line == "?" || line.find("SEARCHING") != std::string::npos)
            continue;
        if (line.find("NO DATA") != std::string::npos ||
            line.find("ERROR") != std::string::npos ||
            line.find("STOPPED") != std::string::npos ||
            line.find("UNABLE") != std::string::npos ||
            line.find("BUFFER FULL") != std::string::npos) {
            Log("ELM status line: %s", line.c_str());
            continue;
        }

        RawFrame f;
        if (!parseCanLine(line, f)) {
            Log("Unparsed line: %s", line.c_str());
            continue;
        }

        if (!filterAccept(f.id))
            continue;

        if (g_chan.protocol == CAN) {
            size_t n = f.data.size() > 8 ? 8 : f.data.size();
            PASSTHRU_MSG m;
            makeMsg(m, g_chan.appProtocol, 0, f.id, f.data.data(), (unsigned long)n, is29Bit());
            enqueueMsg(m);
        } else if (g_chan.protocol == ISO15765) {
            isotpFeed(f);
        }
    }
}

// ISO15765 receive path.
//
// With ATCAF1 the OBDX assembles multi-frame responses and delivers them as
// one line: "7E8 10 0C 11 22 ... CC" (First-Frame PCI retained, full payload
// follows). Single frames arrive as "7E8 06 41 00 ..." (SF PCI retained).
// If automatic processing is off (or a generic ELM327 in CAF0 is used), raw
// FF/CF frames arrive instead, so a manual reassembly fallback is kept.
static void isotpFeed(const RawFrame& f)
{
    size_t n = f.data.size();
    if (n < 1)
        return;
    const unsigned char* d = f.data.data();
    unsigned char pci = d[0] >> 4;

    IsoTpRx& st = g_chan.isotp[f.id];

    switch (pci) {
    case 0x0: { // Single Frame
        size_t len = d[0] & 0x0F;
        if (len < 1 || len > n - 1)
            return;
        PASSTHRU_MSG m;
        makeMsg(m, g_chan.appProtocol, 0, f.id, d + 1, (unsigned long)len, is29Bit());
        enqueueMsg(m);
        st.active = false;
        break;
    }
    case 0x1: { // First Frame - assembled message or start of raw reassembly
        if (n < 2)
            return;
        unsigned long total = ((unsigned long)(d[0] & 0x0F) << 8) | d[1];
        if (total < 8)
            return;

        // FirstFrame indication: 4-byte ID, no data (J2534 04.04).
        PASSTHRU_MSG ind;
        makeMsg(ind, g_chan.appProtocol, ISO15765_FIRST_FRAME, f.id, NULL, 0, is29Bit());
        enqueueMsg(ind);

        if (n - 2 >= total) {
            // Tool already assembled the full message on one line.
            PASSTHRU_MSG m;
            makeMsg(m, g_chan.appProtocol, 0, f.id, d + 2, total, is29Bit());
            enqueueMsg(m);
            st.active = false;
        } else {
            // Raw First Frame (6 payload bytes) - reassemble manually.
            st.expectedLen = total;
            st.data.assign(d + 2, d + n);
            st.nextSeq = 1;
            st.active = true;
            st.lastActivity = GetTickCount();
        }
        break;
    }
    case 0x2: { // Consecutive Frame (raw reassembly fallback)
        if (!st.active)
            return;
        if ((d[0] & 0x0F) != (st.nextSeq & 0x0F)) {
            Log("ISO-TP: sequence error on %03lX (got %X want %X)",
                f.id, d[0] & 0x0F, st.nextSeq & 0x0F);
            st.active = false;
            return;
        }
        st.nextSeq++;
        st.lastActivity = GetTickCount();
        for (size_t i = 1; i < n && st.data.size() < st.expectedLen; i++)
            st.data.push_back(d[i]);
        if (st.data.size() >= st.expectedLen) {
            PASSTHRU_MSG m;
            makeMsg(m, g_chan.appProtocol, 0, f.id, st.data.data(), st.expectedLen, is29Bit());
            enqueueMsg(m);
            st.active = false;
        }
        break;
    }
    case 0x3:   // Flow Control for our transmissions - the tool handles these
        break;
    default: {
        // First byte doesn't look like a PCI: the tool delivered the payload
        // with formatting already stripped - pass it through whole.
        PASSTHRU_MSG m;
        makeMsg(m, g_chan.appProtocol, 0, f.id, d, (unsigned long)n, is29Bit());
        enqueueMsg(m);
        break;
    }
    }
}

// =========================================================================
// Transmit (call with device lock held)
// =========================================================================

// Build "DXSD 00 00 07 E0 <data...>" - full frame with 4-byte CAN ID inline
// (OBDX DX Send Data; no ATSH state to manage, no auto-filter side effects).
static std::string buildDxSd(const PASSTHRU_MSG& msg)
{
    std::string cmd = "DXSD ";
    cmd += toHex(msg.Data, 4);
    cmd += toHex(msg.Data + 4, msg.DataSize - 4);
    return cmd;
}

static void loopbackEcho(const PASSTHRU_MSG& msg)
{
    if (!g_chan.loopback)
        return;
    PASSTHRU_MSG echo = msg;
    echo.RxStatus = TX_MSG_TYPE | (is29Bit() ? CAN_29BIT_ID : 0);
    echo.Timestamp = nowMicros();
    enqueueMsg(echo);
}

static long transmitCanRaw(const PASSTHRU_MSG& msg)
{
    // Raw CAN channel: ATCAF0 was set at connect, frame goes out as given.
    if (msg.DataSize - 4 > 8)
        return ERR_INVALID_MSG;

    bool ok;
    std::string resp = g_dev.elm.command(buildDxSd(msg), 3000, ok);
    processResponse(resp);
    if (!ok)
        return ERR_TIMEOUT;

    loopbackEcho(msg);
    return STATUS_NOERROR;
}

static long transmitIso15765(const PASSTHRU_MSG& msg, unsigned long timeoutMs)
{
    unsigned long len = msg.DataSize - 4;
    if (len > 4095)
        return ERR_INVALID_MSG;

    // ATCAF1 is active on ISO15765 channels: the OBDX adds the length byte,
    // segments multi-frame messages and handles flow control itself (using
    // the FC header/data programmed from the J2534 flow-control filter).
    // Give long transfers more time: BT round trip + segmented TX on the bus.
    unsigned waitMs = 3000 + (len / 7) * 10;
    if (timeoutMs > waitMs)
        waitMs = timeoutMs;

    bool ok;
    std::string resp = g_dev.elm.command(buildDxSd(msg), waitMs, ok);
    processResponse(resp);
    if (!ok)
        return ERR_TIMEOUT;

    loopbackEcho(msg);
    return STATUS_NOERROR;
}

static long transmitVpw(const PASSTHRU_MSG& msg)
{
    // J1850 VPW: J2534 message bytes = priority, target, source, then data.
    // Set the 3 header bytes via ATSH, send the rest; the tool handles the
    // CRC. Responses come back as complete frames (headers on).
    if (msg.DataSize < 4)
        return ERR_INVALID_MSG;

    char cmd[32];
    _snprintf(cmd, sizeof(cmd), "ATSH %02X %02X %02X",
              msg.Data[0], msg.Data[1], msg.Data[2]);
    if ((long)((msg.Data[0] << 16) | (msg.Data[1] << 8) | msg.Data[2]) != g_chan.curTxHeader) {
        if (!g_dev.elm.commandOk(cmd))
            return ERR_FAILED;
        g_chan.curTxHeader = (msg.Data[0] << 16) | (msg.Data[1] << 8) | msg.Data[2];
    }

    bool ok;
    std::string resp = g_dev.elm.command(
        toHex(msg.Data + 3, msg.DataSize - 3), 6000, ok);

    // Response lines are complete VPW frames (3 header bytes + data).
    std::vector<std::string> lines;
    Elm327::splitLines(resp, lines);
    for (size_t i = 0; i < lines.size(); i++) {
        std::string hex;
        bool valid = true;
        for (size_t j = 0; j < lines[i].size() && valid; j++) {
            if (lines[i][j] == ' ')
                continue;
            if (!isHexDigit(lines[i][j]))
                valid = false;
            else
                hex += lines[i][j];
        }
        if (!valid || hex.size() < 2)
            continue;
        PASSTHRU_MSG m;
        memset(&m, 0, sizeof(m));
        m.ProtocolID = g_chan.appProtocol;
        m.Timestamp = nowMicros();
        m.DataSize = (unsigned long)(hex.size() / 2);
        if (m.DataSize > sizeof(m.Data))
            m.DataSize = sizeof(m.Data);
        for (unsigned long j = 0; j < m.DataSize; j++)
            m.Data[j] = (unsigned char)((hexVal(hex[j*2]) << 4) | hexVal(hex[j*2+1]));
        m.ExtraDataIndex = m.DataSize;  // CRC already stripped by the tool
        enqueueMsg(m);
    }
    return ok ? STATUS_NOERROR : ERR_TIMEOUT;
}

static long transmitMsg(const PASSTHRU_MSG& msg, unsigned long timeoutMs)
{
    switch (g_chan.protocol) {
    case CAN:       return transmitCanRaw(msg);
    case ISO15765:  return transmitIso15765(msg, timeoutMs);
    case J1850VPW:  return transmitVpw(msg);
    }
    return ERR_INVALID_PROTOCOL_ID;
}

// =========================================================================
// Worker thread: periodic messages + async RX pump
// =========================================================================

// In DXPT1 passthrough mode the OBDX streams any frame matching the filters
// at any time - not just as a response to a transmit. This pump drains them
// into the RX queue while no command transaction is in flight.
static std::string g_pumpBuf;

static void pumpIncoming()   // device lock held
{
    char buf[1024];
    int n = g_dev.elm.rawRead(buf, sizeof(buf), 10);
    while (n > 0) {
        for (int i = 0; i < n; i++) {
            char c = buf[i];
            if (c != '>' && c != '\0')
                g_pumpBuf += c;
        }
        n = g_dev.elm.rawRead(buf, sizeof(buf), 10);
    }

    size_t pos;
    while ((pos = g_pumpBuf.find_first_of("\r\n")) != std::string::npos) {
        std::string line = g_pumpBuf.substr(0, pos);
        g_pumpBuf.erase(0, pos + 1);
        if (!line.empty() && g_chan.open)
            processResponse(line + "\r");
    }
    if (g_pumpBuf.size() > 8192)
        g_pumpBuf.clear();      // runaway partial line - drop it
}

static DWORD WINAPI workerThreadProc(LPVOID)
{
    for (;;) {
        if (WaitForSingleObject(g_dev.periodicStop, 20) == WAIT_OBJECT_0)
            return 0;
        if (!g_chan.open || !g_dev.open)
            continue;

        DWORD now = GetTickCount();
        for (int i = 0; i < MAX_PERIODIC; i++) {
            PeriodicMsg& p = g_chan.periodic[i];
            if (!p.used || (long)(now - p.nextDue) < 0)
                continue;
            p.nextDue = now + p.intervalMs;
            EnterCriticalSection(&g_dev.lock);
            if (g_chan.open && g_dev.open)
                transmitMsg(p.msg, 1000);
            LeaveCriticalSection(&g_dev.lock);
        }

        if (TryEnterCriticalSection(&g_dev.lock)) {
            if (g_chan.open && g_dev.open)
                pumpIncoming();
            LeaveCriticalSection(&g_dev.lock);
        }
    }
}

// =========================================================================
// J2534 API
// =========================================================================

long J2534_API PassThruOpen(void* pName, unsigned long* pDeviceID)
{
    ensureInit();
    Log("PassThruOpen(name=%s)", pName ? (const char*)pName : "(null)");

    if (!pDeviceID) {
        setLastError("PassThruOpen: NULL pDeviceID");
        return ERR_NULL_PARAMETER;
    }
    if (g_dev.open) {
        setLastError("PassThruOpen: device already open");
        return ERR_DEVICE_IN_USE;
    }

    Transport* t;
    if (_stricmp(g_cfg.mode.c_str(), "com") == 0)
        t = createComTransport(g_cfg.comPort, g_cfg.baud);
    else
        t = createBtRfcommTransport(g_cfg.btAddress, g_cfg.btName);

    std::string err;
    EnterCriticalSection(&g_dev.lock);
    bool ok = g_dev.elm.open(t, err);
    if (!ok) {
        g_dev.elm.close();
        LeaveCriticalSection(&g_dev.lock);
        setLastError("PassThruOpen: %s", err.c_str());
        return ERR_DEVICE_NOT_CONNECTED;
    }

    g_dev.open = true;
    ResetEvent(g_dev.periodicStop);
    g_dev.periodicThread = CreateThread(NULL, 0, workerThreadProc, NULL, 0, NULL);
    LeaveCriticalSection(&g_dev.lock);

    *pDeviceID = DEVICE_ID;
    Log("PassThruOpen: OK, firmware '%s'", g_dev.elm.firmware().c_str());
    return STATUS_NOERROR;
}

long J2534_API PassThruClose(unsigned long DeviceID)
{
    Log("PassThruClose(%lu)", DeviceID);
    if (DeviceID != DEVICE_ID || !g_dev.open)
        return ERR_INVALID_DEVICE_ID;

    if (g_chan.open)
        PassThruDisconnect(CHANNEL_ID);

    SetEvent(g_dev.periodicStop);
    if (g_dev.periodicThread) {
        WaitForSingleObject(g_dev.periodicThread, 2000);
        CloseHandle(g_dev.periodicThread);
        g_dev.periodicThread = NULL;
    }

    EnterCriticalSection(&g_dev.lock);
    g_dev.elm.close();
    g_dev.open = false;
    LeaveCriticalSection(&g_dev.lock);
    return STATUS_NOERROR;
}

long J2534_API PassThruConnect(unsigned long DeviceID, unsigned long ProtocolID,
                               unsigned long Flags, unsigned long BaudRate,
                               unsigned long* pChannelID)
{
    Log("PassThruConnect(dev=%lu proto=%lu flags=0x%lX baud=%lu)",
        DeviceID, ProtocolID, Flags, BaudRate);

    if (!pChannelID)
        return ERR_NULL_PARAMETER;
    if (DeviceID != DEVICE_ID || !g_dev.open)
        return ERR_INVALID_DEVICE_ID;
    if (g_chan.open) {
        setLastError("PassThruConnect: channel already in use (POC supports one channel)");
        return ERR_CHANNEL_IN_USE;
    }

    // J2534-2 pin-select protocols map onto the base protocol; the physical
    // bus is chosen by J1962_PINS (HS-CAN 6/14 default, MS-CAN 3/11).
    unsigned long base = ProtocolID;
    switch (ProtocolID) {
    case CAN_PS:      base = CAN;      break;
    case ISO15765_PS: base = ISO15765; break;
    case J1850VPW_PS: base = J1850VPW; break;
    case CAN:
    case ISO15765:
    case J1850VPW:    break;
    default:
        setLastError("PassThruConnect: protocol 0x%lX not supported "
                     "(OBDX Pro FT: CAN/ISO15765[_PS]/J1850VPW)", ProtocolID);
        return ERR_INVALID_PROTOCOL_ID;
    }

    g_chan.protocol = base;
    g_chan.appProtocol = ProtocolID;
    g_chan.pins = (BaudRate == 125000) ? 0x030B : 0x060E;
    g_chan.flags = Flags;
    g_chan.baud = BaudRate;
    g_chan.loopback = 0;
    g_chan.bs = 0;
    g_chan.stmin = 0;
    g_chan.curTxHeader = -1;
    g_chan.curRxAddr = -1;
    g_chan.curFcHeader = -1;
    memset(g_chan.filters, 0, sizeof(g_chan.filters));
    memset(g_chan.periodic, 0, sizeof(g_chan.periodic));
    g_chan.isotp.clear();

    std::string err;
    EnterCriticalSection(&g_dev.lock);
    bool ok = programBus(err);
    LeaveCriticalSection(&g_dev.lock);
    if (!ok) {
        setLastError("PassThruConnect: %s", err.c_str());
        return ERR_FAILED;
    }

    EnterCriticalSection(&g_chan.rxLock);
    g_chan.rxQueue.clear();
    LeaveCriticalSection(&g_chan.rxLock);
    g_chan.open = true;

    *pChannelID = CHANNEL_ID;
    return STATUS_NOERROR;
}

long J2534_API PassThruDisconnect(unsigned long ChannelID)
{
    Log("PassThruDisconnect(%lu)", ChannelID);
    if (ChannelID != CHANNEL_ID || !g_chan.open)
        return ERR_INVALID_CHANNEL_ID;

    g_chan.open = false;
    EnterCriticalSection(&g_dev.lock);
    if (g_dev.open) {
        g_dev.elm.commandOk("DXPT0");   // leave passthrough streaming mode
        g_dev.elm.commandOk("ATFCSM0");
        g_dev.elm.commandOk("ATAR");    // reset filters/masks
    }
    LeaveCriticalSection(&g_dev.lock);

    EnterCriticalSection(&g_chan.rxLock);
    g_chan.rxQueue.clear();
    LeaveCriticalSection(&g_chan.rxLock);
    return STATUS_NOERROR;
}

long J2534_API PassThruReadMsgs(unsigned long ChannelID, PASSTHRU_MSG* pMsg,
                                unsigned long* pNumMsgs, unsigned long Timeout)
{
    if (!pMsg || !pNumMsgs)
        return ERR_NULL_PARAMETER;
    if (ChannelID != CHANNEL_ID || !g_chan.open) {
        *pNumMsgs = 0;
        return ERR_INVALID_CHANNEL_ID;
    }

    unsigned long want = *pNumMsgs;
    unsigned long got = 0;
    DWORD deadline = GetTickCount() + Timeout;

    for (;;) {
        EnterCriticalSection(&g_chan.rxLock);
        while (got < want && !g_chan.rxQueue.empty()) {
            pMsg[got++] = g_chan.rxQueue.front();
            g_chan.rxQueue.pop_front();
        }
        LeaveCriticalSection(&g_chan.rxLock);

        if (got >= want || Timeout == 0)
            break;
        long remain = (long)(deadline - GetTickCount());
        if (remain <= 0)
            break;
        WaitForSingleObject(g_chan.rxEvent, remain);
    }

    *pNumMsgs = got;
    if (got == 0)
        return Timeout == 0 ? ERR_BUFFER_EMPTY : ERR_TIMEOUT;
    return STATUS_NOERROR;
}

long J2534_API PassThruWriteMsgs(unsigned long ChannelID, PASSTHRU_MSG* pMsg,
                                 unsigned long* pNumMsgs, unsigned long Timeout)
{
    if (!pMsg || !pNumMsgs)
        return ERR_NULL_PARAMETER;
    if (ChannelID != CHANNEL_ID || !g_chan.open) {
        if (pNumMsgs) *pNumMsgs = 0;
        return ERR_INVALID_CHANNEL_ID;
    }

    unsigned long count = *pNumMsgs;
    unsigned long sent = 0;
    long rc = STATUS_NOERROR;

    for (unsigned long i = 0; i < count; i++) {
        PASSTHRU_MSG& m = pMsg[i];
        if (m.ProtocolID != g_chan.appProtocol) {
            rc = ERR_MSG_PROTOCOL_ID;
            break;
        }
        if (m.DataSize < 4 || m.DataSize > sizeof(m.Data)) {
            rc = ERR_INVALID_MSG;
            break;
        }
        Log("PassThruWriteMsgs: %lu bytes [%s]", m.DataSize,
            toHex(m.Data, m.DataSize > 16 ? 16 : m.DataSize).c_str());

        EnterCriticalSection(&g_dev.lock);
        rc = g_dev.open ? transmitMsg(m, Timeout ? Timeout : 3000)
                        : ERR_DEVICE_NOT_CONNECTED;
        LeaveCriticalSection(&g_dev.lock);
        if (rc != STATUS_NOERROR)
            break;
        sent++;
    }

    *pNumMsgs = sent;
    return rc;
}

long J2534_API PassThruStartPeriodicMsg(unsigned long ChannelID, PASSTHRU_MSG* pMsg,
                                        unsigned long* pMsgID, unsigned long TimeInterval)
{
    if (!pMsg || !pMsgID)
        return ERR_NULL_PARAMETER;
    if (ChannelID != CHANNEL_ID || !g_chan.open)
        return ERR_INVALID_CHANNEL_ID;
    if (TimeInterval < 5 || TimeInterval > 65535)
        return ERR_INVALID_TIME_INTERVAL;

    for (int i = 0; i < MAX_PERIODIC; i++) {
        if (!g_chan.periodic[i].used) {
            g_chan.periodic[i].msg = *pMsg;
            g_chan.periodic[i].intervalMs = TimeInterval;
            g_chan.periodic[i].nextDue = GetTickCount();
            g_chan.periodic[i].used = true;
            *pMsgID = i + 1;
            Log("StartPeriodicMsg: id=%d interval=%lums", i + 1, TimeInterval);
            return STATUS_NOERROR;
        }
    }
    return ERR_EXCEEDED_LIMIT;
}

long J2534_API PassThruStopPeriodicMsg(unsigned long ChannelID, unsigned long MsgID)
{
    if (ChannelID != CHANNEL_ID || !g_chan.open)
        return ERR_INVALID_CHANNEL_ID;
    if (MsgID < 1 || MsgID > MAX_PERIODIC || !g_chan.periodic[MsgID - 1].used)
        return ERR_INVALID_MSG_ID;
    g_chan.periodic[MsgID - 1].used = false;
    return STATUS_NOERROR;
}

long J2534_API PassThruStartMsgFilter(unsigned long ChannelID, unsigned long FilterType,
                                      PASSTHRU_MSG* pMaskMsg, PASSTHRU_MSG* pPatternMsg,
                                      PASSTHRU_MSG* pFlowControlMsg, unsigned long* pFilterID)
{
    if (!pFilterID || !pMaskMsg || !pPatternMsg)
        return ERR_NULL_PARAMETER;
    if (ChannelID != CHANNEL_ID || !g_chan.open)
        return ERR_INVALID_CHANNEL_ID;

    if (FilterType == FLOW_CONTROL_FILTER) {
        if (g_chan.protocol != ISO15765)
            return ERR_MSG_PROTOCOL_ID;
        if (!pFlowControlMsg)
            return ERR_NULL_PARAMETER;
    } else if (FilterType != PASS_FILTER && FilterType != BLOCK_FILTER) {
        return ERR_INVALID_MSG;
    } else if (pFlowControlMsg) {
        return ERR_INVALID_MSG;     // only FLOW_CONTROL takes a third message
    }

    if (pMaskMsg->DataSize < 4 || pPatternMsg->DataSize < 4)
        return ERR_INVALID_MSG;

    int slot = -1;
    for (int i = 0; i < MAX_FILTERS; i++) {
        if (!g_chan.filters[i].used) { slot = i; break; }
    }
    if (slot < 0)
        return ERR_EXCEEDED_LIMIT;

    Filter& f = g_chan.filters[slot];
    f.type = FilterType;
    f.maskId    = ((unsigned long)pMaskMsg->Data[0] << 24) | (pMaskMsg->Data[1] << 16) |
                  (pMaskMsg->Data[2] << 8) | pMaskMsg->Data[3];
    f.patternId = ((unsigned long)pPatternMsg->Data[0] << 24) | (pPatternMsg->Data[1] << 16) |
                  (pPatternMsg->Data[2] << 8) | pPatternMsg->Data[3];
    f.flowControlId = 0;
    f.flags = pPatternMsg->TxFlags;

    if (FilterType == FLOW_CONTROL_FILTER) {
        f.flowControlId = ((unsigned long)pFlowControlMsg->Data[0] << 24) |
                          (pFlowControlMsg->Data[1] << 16) |
                          (pFlowControlMsg->Data[2] << 8) | pFlowControlMsg->Data[3];

        // Program the ELM: receive only the pattern ID, auto-answer First
        // Frames with a flow control frame from our tester ID.
        EnterCriticalSection(&g_dev.lock);
        bool ok = elmSetRxAddr(f.patternId);
        ok = elmSetFlowControl(f.flowControlId) && ok;
        LeaveCriticalSection(&g_dev.lock);
        if (!ok) {
            setLastError("StartMsgFilter: ELM327 rejected filter programming");
            return ERR_FAILED;
        }
    }

    f.used = true;
    *pFilterID = slot + 1;
    Log("StartMsgFilter: id=%d type=%lu mask=%08lX pattern=%08lX fc=%08lX",
        slot + 1, FilterType, f.maskId, f.patternId, f.flowControlId);
    return STATUS_NOERROR;
}

long J2534_API PassThruStopMsgFilter(unsigned long ChannelID, unsigned long FilterID)
{
    if (ChannelID != CHANNEL_ID || !g_chan.open)
        return ERR_INVALID_CHANNEL_ID;
    if (FilterID < 1 || FilterID > MAX_FILTERS || !g_chan.filters[FilterID - 1].used)
        return ERR_INVALID_FILTER_ID;

    Filter& f = g_chan.filters[FilterID - 1];
    if (f.type == FLOW_CONTROL_FILTER) {
        EnterCriticalSection(&g_dev.lock);
        if (g_dev.open) {
            elmClearRxAddr();
            g_dev.elm.commandOk("ATFCSM0");
            g_chan.curFcHeader = -1;
        }
        LeaveCriticalSection(&g_dev.lock);
    }
    f.used = false;
    return STATUS_NOERROR;
}

long J2534_API PassThruSetProgrammingVoltage(unsigned long DeviceID,
                                             unsigned long PinNumber,
                                             unsigned long Voltage)
{
    Log("PassThruSetProgrammingVoltage(pin=%lu, mV=%lu) - not supported on ELM327",
        PinNumber, Voltage);
    if (DeviceID != DEVICE_ID || !g_dev.open)
        return ERR_INVALID_DEVICE_ID;
    setLastError("Programming voltage is not supported by ELM327 hardware");
    return ERR_NOT_SUPPORTED;
}

long J2534_API PassThruReadVersion(unsigned long DeviceID, char* pFirmwareVersion,
                                   char* pDllVersion, char* pApiVersion)
{
    if (!pFirmwareVersion || !pDllVersion || !pApiVersion)
        return ERR_NULL_PARAMETER;
    if (DeviceID != DEVICE_ID || !g_dev.open)
        return ERR_INVALID_DEVICE_ID;

    std::string fw = g_dev.elm.firmware();
    if (fw.empty())
        fw = "ELM327 (unknown)";
    strncpy(pFirmwareVersion, fw.c_str(), 79);
    pFirmwareVersion[79] = 0;
    strcpy(pDllVersion, "BlueJ2534 0.1.0");
    strcpy(pApiVersion, "04.04");
    return STATUS_NOERROR;
}

long J2534_API PassThruGetLastError(char* pErrorDescription)
{
    if (!pErrorDescription)
        return ERR_NULL_PARAMETER;
    strncpy(pErrorDescription, g_lastError, 79);
    pErrorDescription[79] = 0;
    return STATUS_NOERROR;
}

long J2534_API PassThruIoctl(unsigned long ChannelID, unsigned long IoctlID,
                             void* pInput, void* pOutput)
{
    Log("PassThruIoctl(chan=%lu, ioctl=0x%lX)", ChannelID, IoctlID);

    switch (IoctlID) {
    case GET_CONFIG: {
        SCONFIG_LIST* list = (SCONFIG_LIST*)pInput;
        if (!list || !list->ConfigPtr)
            return ERR_NULL_PARAMETER;
        for (unsigned long i = 0; i < list->NumOfParams; i++) {
            SCONFIG& c = list->ConfigPtr[i];
            switch (c.Parameter) {
            case DATA_RATE:      c.Value = g_chan.baud;    break;
            case LOOPBACK:       c.Value = g_chan.loopback; break;
            case ISO15765_BS:    c.Value = g_chan.bs;      break;
            case ISO15765_STMIN: c.Value = g_chan.stmin;   break;
            case J1962_PINS:     c.Value = g_chan.pins;    break;
            case BS_TX:
            case STMIN_TX:       c.Value = 0xFFFF;         break; // use FC from ECU
            default:             c.Value = 0;              break;
            }
        }
        return STATUS_NOERROR;
    }
    case SET_CONFIG: {
        SCONFIG_LIST* list = (SCONFIG_LIST*)pInput;
        if (!list || !list->ConfigPtr)
            return ERR_NULL_PARAMETER;
        for (unsigned long i = 0; i < list->NumOfParams; i++) {
            SCONFIG& c = list->ConfigPtr[i];
            switch (c.Parameter) {
            case DATA_RATE: {
                // Live baud change = bus reprogram (e.g. 500k <-> 125k).
                if (g_chan.baud != c.Value) {
                    g_chan.baud = c.Value;
                    if (g_chan.open && g_dev.open &&
                        (g_chan.protocol == CAN || g_chan.protocol == ISO15765)) {
                        std::string err;
                        EnterCriticalSection(&g_dev.lock);
                        bool ok = programBus(err);
                        LeaveCriticalSection(&g_dev.lock);
                        if (!ok) {
                            setLastError("SET_CONFIG DATA_RATE: %s", err.c_str());
                            return ERR_FAILED;
                        }
                    }
                }
                break;
            }
            case LOOPBACK:       g_chan.loopback = c.Value ? 1 : 0; break;
            case ISO15765_BS:    g_chan.bs = c.Value;      g_chan.curFcHeader = -1; break;
            case ISO15765_STMIN: g_chan.stmin = c.Value;   g_chan.curFcHeader = -1; break;
            case J1962_PINS: {
                // Bus switch: 0x060E (or 0) = HS-CAN pins 6/14,
                // 0x030B = MS-CAN pins 3/11 (OBDX protocol E).
                unsigned long pins = c.Value ? c.Value : 0x060E;
                if (pins != 0x060E && pins != 0x030B) {
                    setLastError("SET_CONFIG J1962_PINS: unsupported pin pair 0x%04lX "
                                 "(0x060E=HS-CAN, 0x030B=MS-CAN)", pins);
                    return ERR_INVALID_IOCTL_VALUE;
                }
                if (pins != g_chan.pins) {
                    g_chan.pins = pins;
                    if (g_chan.open && g_dev.open) {
                        std::string err;
                        EnterCriticalSection(&g_dev.lock);
                        bool ok = programBus(err);
                        LeaveCriticalSection(&g_dev.lock);
                        if (!ok) {
                            setLastError("SET_CONFIG J1962_PINS: %s", err.c_str());
                            return ERR_FAILED;
                        }
                        Log("Bus switched to %s",
                            pins == 0x030B ? "MS-CAN (pins 3/11)" : "HS-CAN (pins 6/14)");
                    }
                }
                break;
            }
            default:
                Log("SET_CONFIG: parameter 0x%lX=%lu accepted (no-op)",
                    c.Parameter, c.Value);
                break;
            }
        }
        return STATUS_NOERROR;
    }
    case READ_VBATT: {
        if (!pOutput)
            return ERR_NULL_PARAMETER;
        if (!g_dev.open)
            return ERR_INVALID_DEVICE_ID;
        EnterCriticalSection(&g_dev.lock);
        bool ok;
        std::string r = g_dev.elm.command("ATRV", 2000, ok);
        LeaveCriticalSection(&g_dev.lock);
        float volts = 0.0f;
        if (sscanf(r.c_str(), " %f", &volts) != 1) {
            // response may have leading junk; scan for a digit
            for (size_t i = 0; i < r.size(); i++) {
                if (r[i] >= '0' && r[i] <= '9') {
                    sscanf(r.c_str() + i, "%f", &volts);
                    break;
                }
            }
        }
        *(unsigned long*)pOutput = (unsigned long)(volts * 1000.0f);
        return STATUS_NOERROR;
    }
    case CLEAR_RX_BUFFER:
        if (ChannelID != CHANNEL_ID || !g_chan.open)
            return ERR_INVALID_CHANNEL_ID;
        EnterCriticalSection(&g_chan.rxLock);
        g_chan.rxQueue.clear();
        LeaveCriticalSection(&g_chan.rxLock);
        g_chan.isotp.clear();
        return STATUS_NOERROR;
    case CLEAR_TX_BUFFER:
        return STATUS_NOERROR;  // we transmit synchronously; nothing queued
    case CLEAR_PERIODIC_MSGS:
        if (ChannelID != CHANNEL_ID || !g_chan.open)
            return ERR_INVALID_CHANNEL_ID;
        memset(g_chan.periodic, 0, sizeof(g_chan.periodic));
        return STATUS_NOERROR;
    case CLEAR_MSG_FILTERS:
        if (ChannelID != CHANNEL_ID || !g_chan.open)
            return ERR_INVALID_CHANNEL_ID;
        memset(g_chan.filters, 0, sizeof(g_chan.filters));
        EnterCriticalSection(&g_dev.lock);
        if (g_dev.open) {
            elmClearRxAddr();
            g_dev.elm.commandOk("ATFCSM0");
            g_chan.curFcHeader = -1;
        }
        LeaveCriticalSection(&g_dev.lock);
        return STATUS_NOERROR;
    case FAST_INIT:
    case FIVE_BAUD_INIT:
        setLastError("K-line init not available: the OBDX Pro FT has no "
                     "ISO9141/ISO14230 support");
        return ERR_NOT_SUPPORTED;
    case READ_PROG_VOLTAGE:
        if (!pOutput)
            return ERR_NULL_PARAMETER;
        *(unsigned long*)pOutput = 0;
        return STATUS_NOERROR;
    }

    setLastError("PassThruIoctl: unsupported IOCTL 0x%lX", IoctlID);
    return ERR_INVALID_IOCTL_ID;
}

// =========================================================================
// DllMain
// =========================================================================

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_hInst = hInst;
        GetModuleFileNameA(hInst, g_dllDir, MAX_PATH);
        char* slash = strrchr(g_dllDir, '\\');
        if (slash)
            *slash = 0;
        loadConfig();
        LogInit(g_dllDir, g_cfg.log);
        Log("BlueJ2534 loaded (mode=%s, bt='%s'/'%s', com=%s)",
            g_cfg.mode.c_str(), g_cfg.btAddress.c_str(),
            g_cfg.btName.c_str(), g_cfg.comPort.c_str());
        DisableThreadLibraryCalls(hInst);
    } else if (reason == DLL_PROCESS_DETACH) {
        LogShutdown();
    }
    return TRUE;
}
