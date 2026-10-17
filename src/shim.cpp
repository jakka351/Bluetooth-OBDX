//
// shim.cpp - BlueJ2534: SAE J2534-1 v04.04 PassThru shim that talks to an
// OBDX Pro FT over Bluetooth SPP (RFCOMM) using the OBDX DVI binary protocol.
//
// Why DVI (not ELM text): the OBDX shares one serial link for bus frames and
// command responses. In ELM string mode they collide on a busy bus - an "OK"
// is lost among streamed frames or merged onto one. DVI frames every message
// with a length and checksum, so unsolicited bus frames (0x08/0x09) and
// command acks (0x20/0x41/0x44) are always cleanly separable. Comms stays
// enabled while we send; no stop/resume stream dance is needed.
//
// The OBDX Pro FT is CAN-only (HS-CAN, MS-CAN, ISO15765 - no K-line, no VPW;
// the official FT J2534 entry declares J1850VPW=0).
//
// Locking: g_dev.lock serializes ALL adapter I/O (transact + poll) and all
// shared channel state. g_chan.rxLock guards only the RX queue. Order:
// g_dev.lock then rxLock.
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
#include "dvi.h"
#include "log.h"

// =========================================================================
// Globals / configuration
// =========================================================================

static char g_dllDir[MAX_PATH] = {0};
static HINSTANCE g_hInst = NULL;
static bool g_modulePinned = false;

struct Config {
    std::string mode;      // "bt" or "com"
    std::string btAddress; // optional AA:BB:CC:DD:EE:FF
    std::string btName;    // discovery name substring
    std::string comPort;   // COMx
    int baud;
    bool log;
};
static Config g_cfg;
static bool g_cfgLoaded = false;

static SRWLOCK g_errLock = SRWLOCK_INIT;
static char g_lastError[256] = "No error";

static void setLastError(const char* fmt, ...)
{
    char buf[256];
    va_list args;
    va_start(args, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, args);
    va_end(args);
    buf[sizeof(buf) - 1] = 0;

    AcquireSRWLockExclusive(&g_errLock);
    strcpy(g_lastError, buf);
    ReleaseSRWLockExclusive(&g_errLock);
    Log("ERROR: %s", buf);
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
    GetPrivateProfileStringA("connection", "BtName", "OBDX", buf, sizeof(buf), ini);
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
    LONGLONG secs = c.QuadPart / freq.QuadPart;
    LONGLONG rem  = c.QuadPart % freq.QuadPart;
    return (unsigned long)(secs * 1000000 + (rem * 1000000) / freq.QuadPart);
}

static unsigned long clampTimeout(unsigned long ms)
{
    return ms > 0x7FFFFFF0ul ? 0x7FFFFFF0ul : ms;
}

// =========================================================================
// Device & channel state
// =========================================================================

struct Filter {
    bool          used;
    unsigned long type;            // PASS / BLOCK / FLOW_CONTROL
    unsigned long maskId;
    unsigned long patternId;
    unsigned long flowControlId;   // tester TX id (FLOW_CONTROL only)
};

struct PeriodicMsg {
    bool          used;
    PASSTHRU_MSG  msg;
    unsigned long intervalMs;
    DWORD         nextDue;
};

struct IsoTpRx {
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
    unsigned long pins;         // J1962_PINS: 0x060E HS-CAN, 0x030B MS-CAN
    bool          pinsExplicit; // app set J1962_PINS itself
    unsigned long flags;
    unsigned long baud;
    unsigned long loopback;
    unsigned long bs, stmin;    // flow-control block size / STmin

    Filter        filters[MAX_FILTERS];
    PeriodicMsg   periodic[MAX_PERIODIC];

    std::deque<PASSTHRU_MSG> rxQueue;
    CRITICAL_SECTION          rxLock;
    HANDLE                    rxEvent;
    bool                      rxOverflowed;

    std::map<unsigned long, IsoTpRx> isotp;
};

struct Device {
    bool             open;
    Dvi              dvi;
    CRITICAL_SECTION lock;
    HANDLE           workerThread;
    HANDLE           workerStop;
};

static Device  g_dev;
static Channel g_chan;
static bool    g_syncInit = false;

#define DEVICE_ID   1
#define CHANNEL_ID  1

// =========================================================================
// RX queue
// =========================================================================

static void enqueueMsg(const PASSTHRU_MSG& m)
{
    EnterCriticalSection(&g_chan.rxLock);
    if (g_chan.rxQueue.size() >= RX_QUEUE_MAX) {
        g_chan.rxQueue.pop_front();
        if (!g_chan.rxOverflowed) {
            g_chan.rxOverflowed = true;
            Log("RX queue overflow - dropping oldest messages");
        }
    } else if (g_chan.rxQueue.size() < RX_QUEUE_MAX / 2) {
        g_chan.rxOverflowed = false;
    }
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
// Filters (device lock held)
// =========================================================================

static bool is29Bit()
{
    return (g_chan.flags & CAN_29BIT_ID) != 0;
}

// J2534: nothing is received until a PASS or FLOW_CONTROL filter matches;
// BLOCK filters carve exceptions. The DVI hardware filters mirror this, but
// we also enforce it in software (zero filters => receive nothing).
static bool filterAcceptCan(unsigned long id)
{
    for (int i = 0; i < MAX_FILTERS; i++) {
        const Filter& f = g_chan.filters[i];
        if (f.used && f.type == BLOCK_FILTER &&
            (id & f.maskId) == (f.patternId & f.maskId))
            return false;
    }
    for (int i = 0; i < MAX_FILTERS; i++) {
        const Filter& f = g_chan.filters[i];
        if (f.used && f.type != BLOCK_FILTER &&
            (id & f.maskId) == (f.patternId & f.maskId))
            return true;
    }
    return false;
}

// =========================================================================
// Received-frame routing (device lock held)
// =========================================================================

struct RawFrame {
    unsigned long id;
    std::vector<unsigned char> data;
};

static void isotpFeed(const RawFrame& f);

// Turn a DVI 0x08/0x09 network frame (data = [id0 id1 id2 id3, payload...])
// into J2534 messages via the filter + ISO-TP path.
static void routeNetFrame(const DviMsg& m)
{
    if (!g_chan.open || m.data.size() < 5)
        return;
    unsigned long id = ((unsigned long)m.data[0] << 24) | ((unsigned long)m.data[1] << 16) |
                       ((unsigned long)m.data[2] << 8) | m.data[3];
    if (!filterAcceptCan(id))
        return;

    RawFrame f;
    f.id = id;
    f.data.assign(m.data.begin() + 4, m.data.end());

    if (g_chan.protocol == CAN) {
        size_t n = f.data.size() > 8 ? 8 : f.data.size();
        PASSTHRU_MSG msg;
        makeMsg(msg, g_chan.appProtocol, 0, id, f.data.data(), (unsigned long)n, is29Bit());
        enqueueMsg(msg);
    } else {
        isotpFeed(f);
    }
}

// ISO15765 receive. With auto frame processing the tool assembles multi-frame
// responses; single frames keep their PCI. A raw FF/CF fallback is retained.
static void isotpFeed(const RawFrame& f)
{
    size_t n = f.data.size();
    if (n < 1)
        return;
    const unsigned char* d = f.data.data();
    unsigned char pci = d[0] >> 4;

    switch (pci) {
    case 0x0: {
        size_t len = d[0] & 0x0F;
        if (len < 1 || len > n - 1)
            return;
        PASSTHRU_MSG m;
        makeMsg(m, g_chan.appProtocol, 0, f.id, d + 1, (unsigned long)len, is29Bit());
        enqueueMsg(m);
        g_chan.isotp.erase(f.id);
        break;
    }
    case 0x1: {
        if (n < 2)
            return;
        unsigned long total = ((unsigned long)(d[0] & 0x0F) << 8) | d[1];
        if (total < 8)
            return;
        PASSTHRU_MSG ind;
        makeMsg(ind, g_chan.appProtocol, ISO15765_FIRST_FRAME, f.id, NULL, 0, is29Bit());
        enqueueMsg(ind);
        if (n - 2 >= total) {
            PASSTHRU_MSG m;
            makeMsg(m, g_chan.appProtocol, 0, f.id, d + 2, total, is29Bit());
            enqueueMsg(m);
            g_chan.isotp.erase(f.id);
        } else {
            IsoTpRx& st = g_chan.isotp[f.id];
            st.expectedLen = total;
            st.data.assign(d + 2, d + n);
            st.nextSeq = 1;
            st.lastActivity = GetTickCount();
        }
        break;
    }
    case 0x2: {
        std::map<unsigned long, IsoTpRx>::iterator it = g_chan.isotp.find(f.id);
        if (it == g_chan.isotp.end())
            return;
        IsoTpRx& st = it->second;
        if ((d[0] & 0x0F) != (st.nextSeq & 0x0F)) {
            g_chan.isotp.erase(it);
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
            g_chan.isotp.erase(it);
        }
        break;
    }
    case 0x3:
        break;   // flow control for our TX - tool handles it
    default: {
        PASSTHRU_MSG m;
        makeMsg(m, g_chan.appProtocol, 0, f.id, d, (unsigned long)n, is29Bit());
        enqueueMsg(m);
        break;
    }
    }
}

static void isotpSweepStale()
{
    DWORD now = GetTickCount();
    std::map<unsigned long, IsoTpRx>::iterator it = g_chan.isotp.begin();
    while (it != g_chan.isotp.end()) {
        if ((long)(now - it->second.lastActivity) > 1500) {
            Log("ISO-TP: dropping stalled reassembly on %lX", it->first);
            g_chan.isotp.erase(it++);
        } else {
            ++it;
        }
    }
}

// =========================================================================
// DVI command helpers (device lock held)
// =========================================================================

// Send a DVI command, route any network frames that arrive to the RX queue,
// and classify the ack. out (optional) receives the response message.
static long dviCmd(unsigned char cmd, const unsigned char* data, size_t len,
                   unsigned char wantCmd, DviMsg* out = NULL,
                   unsigned timeoutMs = 2000, bool large = false)
{
    if (g_dev.dvi.linkDown())
        return ERR_DEVICE_NOT_CONNECTED;

    std::vector<DviMsg> net;
    DviMsg resp;
    bool ok = g_dev.dvi.transact(cmd, data, len, wantCmd, resp, timeoutMs, &net, large);
    for (size_t i = 0; i < net.size(); i++)
        routeNetFrame(net[i]);
    if (!ok)
        return g_dev.dvi.linkDown() ? ERR_DEVICE_NOT_CONNECTED : ERR_TIMEOUT;
    if (out)
        *out = resp;
    if (resp.cmd == DVI_ERROR) {
        setLastError("Adapter rejected DVI cmd 0x%02X (err %u)", cmd,
                     resp.data.size() > 1 ? resp.data[1] : 0);
        return ERR_FAILED;
    }
    return STATUS_NOERROR;
}

// Map (base protocol, pins, baud) to the DVI protocol byte + baud-rate code.
// 29-bit vs 11-bit is carried per-filter, so it does not constrain this.
static bool dviProtocolCodes(unsigned long protocol, unsigned long pins, unsigned long baud,
                             unsigned char* proto, unsigned char* baudCode, long* err)
{
    *err = STATUS_NOERROR;
    if (protocol != CAN && protocol != ISO15765) {
        *err = ERR_INVALID_PROTOCOL_ID;
        return false;
    }
    if (pins == 0x030B) {               // MS-CAN pins 3/11 (Ford), 125k fixed
        *proto = 0x04;
        *baudCode = 4;
        return true;
    }
    *proto = 0x02;                      // HS-CAN pins 6/14
    switch (baud) {
    case 500000: *baudCode = 6; break;
    case 250000: *baudCode = 5; break;
    case 125000: *baudCode = 4; break;
    case 50000:  *baudCode = 1; break;
    default:     *err = ERR_INVALID_BAUDRATE; return false;
    }
    return true;
}

// Program the DVI hardware filters from g_chan.filters. The FT has >=28
// hardware filter slots, so each J2534 filter (incl. per-filter flow ID)
// maps 1:1 - no receive-address contention like the ELM ATCRA path.
static bool reprogramFilters()
{
    bool ok = true;
    for (int i = 0; i < MAX_FILTERS; i++) {
        const Filter& f = g_chan.filters[i];
        unsigned char nn = is29Bit() ? 0x01 : 0x00;
        if (!f.used) {
            // Disable slot i (Filter Enable Status sub-command 0x05).
            unsigned char d[4] = { 0x05, (unsigned char)i, nn, 0x00 };
            dviCmd(DVI_CAN, d, 4, DVI_CAN_RESP);    // best-effort
            continue;
        }
        unsigned char xx = (f.type == PASS_FILTER)  ? 0x00 :
                           (f.type == BLOCK_FILTER)  ? 0x02 : 0x01;   // flow=01
        // Entire filter (sub 0x00): 00 MM NN XX ZZ id[4] mask[4] flow[4]
        unsigned char d[17];
        d[0] = 0x00;
        d[1] = (unsigned char)i;
        d[2] = nn;
        d[3] = xx;
        d[4] = 0x01;                                // enabled
        d[5]  = (unsigned char)(f.patternId >> 24); d[6]  = (unsigned char)(f.patternId >> 16);
        d[7]  = (unsigned char)(f.patternId >> 8);  d[8]  = (unsigned char)(f.patternId);
        d[9]  = (unsigned char)(f.maskId >> 24);    d[10] = (unsigned char)(f.maskId >> 16);
        d[11] = (unsigned char)(f.maskId >> 8);     d[12] = (unsigned char)(f.maskId);
        d[13] = (unsigned char)(f.flowControlId >> 24); d[14] = (unsigned char)(f.flowControlId >> 16);
        d[15] = (unsigned char)(f.flowControlId >> 8);  d[16] = (unsigned char)(f.flowControlId);
        if (dviCmd(DVI_CAN, d, 17, DVI_CAN_RESP) != STATUS_NOERROR)
            ok = false;
    }
    return ok;
}

// Push the flow-control block-size / STmin to the tool (DVI FC frame data).
static void dviSetFcData()
{
    unsigned char d[4] = { 0x09, 0x30, (unsigned char)(g_chan.bs & 0xFF),
                           (unsigned char)(g_chan.stmin & 0xFF) };
    dviCmd(DVI_CAN, d, 4, DVI_CAN_RESP);
}

// Enable/disable network communication (00=off, 01=on read+write).
static long dviSetComms(unsigned char on)
{
    unsigned char d[2] = { 0x02, on };
    return dviCmd(DVI_PROTO, d, 2, DVI_PROTO_RESP);
}

// Program protocol, baud, formatting, filters and comms for the channel.
static long programBus()
{
    unsigned char proto, baudCode;
    long errCode;
    if (!dviProtocolCodes(g_chan.protocol, g_chan.pins, g_chan.baud, &proto, &baudCode, &errCode))
        return errCode;

    dviSetComms(0x00);                              // quiet while reconfiguring

    unsigned char d[2];
    d[0] = 0x01; d[1] = proto;                      // set OBD protocol
    if (dviCmd(DVI_PROTO, d, 2, DVI_PROTO_RESP) != STATUS_NOERROR) {
        setLastError("Tool rejected set-protocol (0x%02X)", proto);
        return ERR_FAILED;
    }

    // Guarantee a 0x20 ack on every write (independent of prior adapter
    // state). 0x24 sub 0x01 = network write responses; response cmd is 0x34.
    unsigned char wr[2] = { 0x01, 0x01 };
    dviCmd(DVI_SETTINGS, wr, 2, 0x34);

    unsigned char b[2] = { 0x15, baudCode };        // CAN baud rate
    dviCmd(DVI_CAN, b, 2, DVI_CAN_RESP);

    unsigned char on = (g_chan.protocol == ISO15765) ? 0x01 : 0x00;
    unsigned char fp[2] = { 0x0B, on };             // auto frame processing
    dviCmd(DVI_CAN, fp, 2, DVI_CAN_RESP);
    unsigned char af[2] = { 0x0F, on };             // auto formatting (length byte)
    dviCmd(DVI_CAN, af, 2, DVI_CAN_RESP);

    if (g_chan.protocol == ISO15765)
        dviSetFcData();

    reprogramFilters();

    if (dviSetComms(0x01) != STATUS_NOERROR) {      // enable read+write last
        setLastError("Tool rejected enable-comms");
        return ERR_FAILED;
    }
    return STATUS_NOERROR;
}

// =========================================================================
// Transmit (device lock held)
// =========================================================================

static void loopbackEcho(const PASSTHRU_MSG& msg)
{
    if (!g_chan.loopback)
        return;
    PASSTHRU_MSG echo = msg;
    echo.RxStatus = TX_MSG_TYPE | (is29Bit() ? CAN_29BIT_ID : 0);
    echo.Timestamp = nowMicros();
    enqueueMsg(echo);
}

static unsigned txWaitMs(unsigned long appTimeout, unsigned long extraMs)
{
    unsigned long t = appTimeout ? appTimeout : 2000;
    if (t < 250)   t = 250;
    if (t > 10000) t = 10000;
    return (unsigned)(t + extraMs);
}

static long validateMsg(const PASSTHRU_MSG& m)
{
    if (m.ProtocolID != g_chan.appProtocol)
        return ERR_MSG_PROTOCOL_ID;
    if (m.DataSize < 4 || m.DataSize > sizeof(m.Data))
        return ERR_INVALID_MSG;
    if (g_chan.protocol == CAN && m.DataSize > 12)
        return ERR_INVALID_MSG;
    if (g_chan.protocol == ISO15765 && m.DataSize > 4099)
        return ERR_INVALID_MSG;
    return STATUS_NOERROR;
}

static long transmitMsg(const PASSTHRU_MSG& msg, unsigned long timeoutMs)
{
    if (g_dev.dvi.linkDown())
        return ERR_DEVICE_NOT_CONNECTED;

    unsigned long plen = msg.DataSize - 4;

    // ISO15765 multi-frame needs a flow-control filter owning this tester ID
    // (J2534 04.04: ERR_NO_FLOW_CONTROL). Single frames (incl. 7DF) do not.
    if (g_chan.protocol == ISO15765 && plen > 7) {
        unsigned long id = ((unsigned long)msg.Data[0] << 24) | ((unsigned long)msg.Data[1] << 16) |
                           ((unsigned long)msg.Data[2] << 8) | msg.Data[3];
        unsigned long idMask = is29Bit() ? 0x1FFFFFFF : 0x7FF;
        bool haveFc = false;
        for (int i = 0; i < MAX_FILTERS && !haveFc; i++) {
            const Filter& f = g_chan.filters[i];
            haveFc = f.used && f.type == FLOW_CONTROL_FILTER &&
                     (f.flowControlId & idMask) == (id & idMask);
        }
        if (!haveFc) {
            setLastError("Multi-frame write to %lX without a flow-control filter",
                         id & idMask);
            return ERR_NO_FLOW_CONTROL;
        }
    }

    // DVI send frame: [id0 id1 id2 id3][payload...]. The tool adds the CAN
    // length byte and (ISO15765) segments + handles flow control.
    std::vector<unsigned char> buf;
    buf.reserve(msg.DataSize);
    for (unsigned long i = 0; i < msg.DataSize; i++)
        buf.push_back(msg.Data[i]);
    bool large = buf.size() > 255;
    unsigned long extra = (plen > 7) ? (plen / 7) * 10 + 500 : 0;

    DviMsg ack;
    long rc = dviCmd(large ? DVI_TX_LARGE : DVI_TX_NORMAL, buf.data(), buf.size(),
                     DVI_TX_ACK, &ack, txWaitMs(timeoutMs, extra), large);
    if (rc != STATUS_NOERROR)
        return rc;

    loopbackEcho(msg);
    return STATUS_NOERROR;
}

// =========================================================================
// Worker thread: periodic messages + async RX pump
// =========================================================================

static DWORD WINAPI workerThreadProc(LPVOID)
{
    for (;;) {
        if (WaitForSingleObject(g_dev.workerStop, 20) == WAIT_OBJECT_0)
            return 0;

        EnterCriticalSection(&g_dev.lock);
        if (g_chan.open && g_dev.open && !g_dev.dvi.linkDown()) {
            DWORD now = GetTickCount();
            for (int i = 0; i < MAX_PERIODIC; i++) {
                PeriodicMsg& p = g_chan.periodic[i];
                if (!p.used || (long)(now - p.nextDue) < 0)
                    continue;
                p.nextDue = now + p.intervalMs;
                transmitMsg(p.msg, 1000);
            }

            std::vector<DviMsg> net;
            g_dev.dvi.poll(net, 40);
            for (size_t i = 0; i < net.size(); i++)
                routeNetFrame(net[i]);

            isotpSweepStale();
        }
        LeaveCriticalSection(&g_dev.lock);
    }
}

// =========================================================================
// J2534 API
// =========================================================================

long J2534_API PassThruOpen(void* pName, unsigned long* pDeviceID)
{
    if (!g_syncInit)
        return ERR_FAILED;
    if (!pDeviceID) {
        setLastError("PassThruOpen: NULL pDeviceID");
        return ERR_NULL_PARAMETER;
    }

    EnterCriticalSection(&g_dev.lock);
    if (!g_cfgLoaded) {
        loadConfig();
        LogInit(g_dllDir, g_cfg.log);
        Log("BlueJ2534 0.3.0 (DVI) (mode=%s, bt='%s'/'%s', com=%s)",
            g_cfg.mode.c_str(), g_cfg.btAddress.c_str(),
            g_cfg.btName.c_str(), g_cfg.comPort.c_str());
        g_cfgLoaded = true;
    }
    Log("PassThruOpen(name=%s)", pName ? (const char*)pName : "(null)");

    if (g_dev.open) {
        LeaveCriticalSection(&g_dev.lock);
        setLastError("PassThruOpen: device already open");
        return ERR_DEVICE_IN_USE;
    }

    Transport* t;
    if (_stricmp(g_cfg.mode.c_str(), "com") == 0)
        t = createComTransport(g_cfg.comPort, g_cfg.baud);
    else
        t = createBtRfcommTransport(g_cfg.btAddress, g_cfg.btName);

    std::string err;
    if (!g_dev.dvi.open(t, err)) {
        g_dev.dvi.close();
        LeaveCriticalSection(&g_dev.lock);
        setLastError("PassThruOpen: %s", err.c_str());
        return ERR_DEVICE_NOT_CONNECTED;
    }

    if (g_dev.workerThread) {
        if (WaitForSingleObject(g_dev.workerThread, 5000) != WAIT_OBJECT_0) {
            g_dev.dvi.close();
            LeaveCriticalSection(&g_dev.lock);
            setLastError("PassThruOpen: previous session is still shutting down");
            return ERR_FAILED;
        }
        CloseHandle(g_dev.workerThread);
        g_dev.workerThread = NULL;
    }

    if (!g_modulePinned && g_hInst) {
        HMODULE self = NULL;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_PIN |
                           GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           (LPCSTR)&loadConfig, &self);
        g_modulePinned = true;
    }

    g_dev.open = true;
    ResetEvent(g_dev.workerStop);
    g_dev.workerThread = CreateThread(NULL, 0, workerThreadProc, NULL, 0, NULL);
    if (!g_dev.workerThread) {
        g_dev.open = false;
        g_dev.dvi.close();
        LeaveCriticalSection(&g_dev.lock);
        setLastError("PassThruOpen: failed to start the worker thread");
        return ERR_FAILED;
    }
    LeaveCriticalSection(&g_dev.lock);

    *pDeviceID = DEVICE_ID;
    Log("PassThruOpen: OK, firmware '%s'", g_dev.dvi.firmware().c_str());
    return STATUS_NOERROR;
}

long J2534_API PassThruClose(unsigned long DeviceID)
{
    Log("PassThruClose(%lu)", DeviceID);
    if (DeviceID != DEVICE_ID || !g_dev.open)
        return ERR_INVALID_DEVICE_ID;

    if (g_chan.open)
        PassThruDisconnect(CHANNEL_ID);

    SetEvent(g_dev.workerStop);
    SetEvent(g_chan.rxEvent);
    if (g_dev.workerThread) {
        if (WaitForSingleObject(g_dev.workerThread, 10000) == WAIT_OBJECT_0) {
            CloseHandle(g_dev.workerThread);
            g_dev.workerThread = NULL;
        } else {
            Log("PassThruClose: worker did not exit in 10s (link stalled?)");
        }
    }

    EnterCriticalSection(&g_dev.lock);
    if (g_dev.open && !g_dev.dvi.linkDown()) {
        unsigned char d[2] = { 0x06, 0x00 };        // switch back to ELM mode
        dviCmd(DVI_PROTO, d, 2, DVI_PROTO_RESP);
    }
    g_dev.dvi.close();
    g_dev.open = false;
    LeaveCriticalSection(&g_dev.lock);
    return STATUS_NOERROR;
}

long J2534_API PassThruConnect(unsigned long DeviceID, unsigned long ProtocolID,
                               unsigned long Flags, unsigned long BaudRate,
                               unsigned long* pChannelID)
{
    Log("PassThruConnect(dev=%lu proto=0x%lX flags=0x%lX baud=%lu)",
        DeviceID, ProtocolID, Flags, BaudRate);

    if (!pChannelID)
        return ERR_NULL_PARAMETER;
    if (DeviceID != DEVICE_ID || !g_dev.open)
        return ERR_INVALID_DEVICE_ID;

    unsigned long base = ProtocolID;
    switch (ProtocolID) {
    case CAN_PS:      base = CAN;      break;
    case ISO15765_PS: base = ISO15765; break;
    case CAN:
    case ISO15765:    break;
    default:
        setLastError("PassThruConnect: protocol 0x%lX not supported "
                     "(OBDX Pro FT: CAN / ISO15765 [_PS] only)", ProtocolID);
        return ERR_INVALID_PROTOCOL_ID;
    }
    if (Flags & CAN_ID_BOTH)
        Log("PassThruConnect: CAN_ID_BOTH not supported; parsing as %s",
            (Flags & CAN_29BIT_ID) ? "29-bit" : "11-bit");

    EnterCriticalSection(&g_dev.lock);
    if (g_chan.open) {
        LeaveCriticalSection(&g_dev.lock);
        setLastError("PassThruConnect: channel already in use (POC supports one channel)");
        return ERR_CHANNEL_IN_USE;
    }

    g_chan.protocol = base;
    g_chan.appProtocol = ProtocolID;
    g_chan.pins = (BaudRate == 125000) ? 0x030B : 0x060E;
    g_chan.pinsExplicit = false;
    g_chan.flags = Flags;
    g_chan.baud = BaudRate ? BaudRate : 500000;
    g_chan.loopback = 0;
    g_chan.bs = 0;
    g_chan.stmin = 0;
    memset(g_chan.filters, 0, sizeof(g_chan.filters));
    memset(g_chan.periodic, 0, sizeof(g_chan.periodic));
    g_chan.isotp.clear();

    long rc = programBus();
    if (rc != STATUS_NOERROR) {
        LeaveCriticalSection(&g_dev.lock);
        if (rc == ERR_INVALID_BAUDRATE)
            setLastError("PassThruConnect: unsupported baud rate %lu", BaudRate);
        return rc;
    }

    EnterCriticalSection(&g_chan.rxLock);
    g_chan.rxQueue.clear();
    g_chan.rxOverflowed = false;
    LeaveCriticalSection(&g_chan.rxLock);

    g_chan.open = true;
    LeaveCriticalSection(&g_dev.lock);

    *pChannelID = CHANNEL_ID;
    return STATUS_NOERROR;
}

long J2534_API PassThruDisconnect(unsigned long ChannelID)
{
    Log("PassThruDisconnect(%lu)", ChannelID);
    if (ChannelID != CHANNEL_ID || !g_chan.open)
        return ERR_INVALID_CHANNEL_ID;

    EnterCriticalSection(&g_dev.lock);
    if (g_dev.open && !g_dev.dvi.linkDown())
        dviSetComms(0x00);          // stop network comms
    g_chan.open = false;
    g_dev.dvi.flush();
    g_chan.isotp.clear();
    LeaveCriticalSection(&g_dev.lock);

    EnterCriticalSection(&g_chan.rxLock);
    g_chan.rxQueue.clear();
    LeaveCriticalSection(&g_chan.rxLock);
    SetEvent(g_chan.rxEvent);
    return STATUS_NOERROR;
}

long J2534_API PassThruReadMsgs(unsigned long ChannelID, PASSTHRU_MSG* pMsg,
                                unsigned long* pNumMsgs, unsigned long Timeout)
{
    if (!pMsg || !pNumMsgs)
        return ERR_NULL_PARAMETER;
    if (ChannelID != CHANNEL_ID || !g_chan.open) {
        if (pNumMsgs) *pNumMsgs = 0;
        return ERR_INVALID_CHANNEL_ID;
    }

    unsigned long want = *pNumMsgs;
    unsigned long got = 0;
    Timeout = clampTimeout(Timeout);
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
        if (!g_chan.open) {
            *pNumMsgs = got;
            return got ? STATUS_NOERROR : ERR_INVALID_CHANNEL_ID;
        }
        if (g_dev.dvi.linkDown())
            break;
        long remain = (long)(deadline - GetTickCount());
        if (remain <= 0)
            break;
        WaitForSingleObject(g_chan.rxEvent, remain > 100 ? 100 : remain);
    }

    *pNumMsgs = got;
    if (got >= want)
        return STATUS_NOERROR;
    if (got == 0 && g_dev.dvi.linkDown())
        return ERR_DEVICE_NOT_CONNECTED;
    if (Timeout == 0)
        return got ? STATUS_NOERROR : ERR_BUFFER_EMPTY;
    return ERR_TIMEOUT;
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
    Timeout = clampTimeout(Timeout);

    for (unsigned long i = 0; i < count; i++) {
        PASSTHRU_MSG& m = pMsg[i];
        rc = validateMsg(m);
        if (rc != STATUS_NOERROR)
            break;

        EnterCriticalSection(&g_dev.lock);
        if (!g_dev.open)
            rc = ERR_DEVICE_NOT_CONNECTED;
        else if (!g_chan.open)
            rc = ERR_INVALID_CHANNEL_ID;
        else
            rc = transmitMsg(m, Timeout);
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

    long rc = validateMsg(*pMsg);
    if (rc != STATUS_NOERROR)
        return rc;

    EnterCriticalSection(&g_dev.lock);
    for (int i = 0; i < MAX_PERIODIC; i++) {
        if (!g_chan.periodic[i].used) {
            g_chan.periodic[i].msg = *pMsg;
            g_chan.periodic[i].intervalMs = TimeInterval;
            g_chan.periodic[i].nextDue = GetTickCount();
            g_chan.periodic[i].used = true;
            LeaveCriticalSection(&g_dev.lock);
            *pMsgID = i + 1;
            Log("StartPeriodicMsg: id=%d interval=%lums", i + 1, TimeInterval);
            return STATUS_NOERROR;
        }
    }
    LeaveCriticalSection(&g_dev.lock);
    return ERR_EXCEEDED_LIMIT;
}

long J2534_API PassThruStopPeriodicMsg(unsigned long ChannelID, unsigned long MsgID)
{
    if (ChannelID != CHANNEL_ID || !g_chan.open)
        return ERR_INVALID_CHANNEL_ID;
    if (MsgID < 1 || MsgID > MAX_PERIODIC)
        return ERR_INVALID_MSG_ID;

    EnterCriticalSection(&g_dev.lock);
    bool was = g_chan.periodic[MsgID - 1].used;
    g_chan.periodic[MsgID - 1].used = false;
    LeaveCriticalSection(&g_dev.lock);
    return was ? STATUS_NOERROR : ERR_INVALID_MSG_ID;
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
        if (pMaskMsg->DataSize != 4 || pPatternMsg->DataSize != 4 ||
            pFlowControlMsg->DataSize != 4)
            return ERR_INVALID_MSG;
    } else if (FilterType != PASS_FILTER && FilterType != BLOCK_FILTER) {
        return ERR_INVALID_MSG;
    } else if (pFlowControlMsg) {
        return ERR_INVALID_MSG;
    } else if (pMaskMsg->DataSize != 4 || pPatternMsg->DataSize != 4) {
        return ERR_INVALID_MSG;
    }

    EnterCriticalSection(&g_dev.lock);
    int slot = -1;
    for (int i = 0; i < MAX_FILTERS; i++) {
        if (!g_chan.filters[i].used) { slot = i; break; }
    }
    if (slot < 0) {
        LeaveCriticalSection(&g_dev.lock);
        return ERR_EXCEEDED_LIMIT;
    }

    Filter& f = g_chan.filters[slot];
    memset(&f, 0, sizeof(f));
    f.type = FilterType;
    f.maskId    = ((unsigned long)pMaskMsg->Data[0] << 24) | ((unsigned long)pMaskMsg->Data[1] << 16) |
                  ((unsigned long)pMaskMsg->Data[2] << 8) | pMaskMsg->Data[3];
    f.patternId = ((unsigned long)pPatternMsg->Data[0] << 24) | ((unsigned long)pPatternMsg->Data[1] << 16) |
                  ((unsigned long)pPatternMsg->Data[2] << 8) | pPatternMsg->Data[3];
    if (FilterType == FLOW_CONTROL_FILTER)
        f.flowControlId = ((unsigned long)pFlowControlMsg->Data[0] << 24) |
                          ((unsigned long)pFlowControlMsg->Data[1] << 16) |
                          ((unsigned long)pFlowControlMsg->Data[2] << 8) | pFlowControlMsg->Data[3];
    f.used = true;

    // Program this one DVI filter slot (comms stays on - framing is clean).
    unsigned char nn = is29Bit() ? 0x01 : 0x00;
    unsigned char xx = (f.type == PASS_FILTER) ? 0x00 : (f.type == BLOCK_FILTER) ? 0x02 : 0x01;
    unsigned char d[17];
    d[0] = 0x00; d[1] = (unsigned char)slot; d[2] = nn; d[3] = xx; d[4] = 0x01;
    d[5]  = (unsigned char)(f.patternId >> 24); d[6]  = (unsigned char)(f.patternId >> 16);
    d[7]  = (unsigned char)(f.patternId >> 8);  d[8]  = (unsigned char)(f.patternId);
    d[9]  = (unsigned char)(f.maskId >> 24);    d[10] = (unsigned char)(f.maskId >> 16);
    d[11] = (unsigned char)(f.maskId >> 8);     d[12] = (unsigned char)(f.maskId);
    d[13] = (unsigned char)(f.flowControlId >> 24); d[14] = (unsigned char)(f.flowControlId >> 16);
    d[15] = (unsigned char)(f.flowControlId >> 8);  d[16] = (unsigned char)(f.flowControlId);
    long rc = dviCmd(DVI_CAN, d, 17, DVI_CAN_RESP);
    LeaveCriticalSection(&g_dev.lock);

    if (rc != STATUS_NOERROR) {
        EnterCriticalSection(&g_dev.lock);
        f.used = false;
        LeaveCriticalSection(&g_dev.lock);
        setLastError("StartMsgFilter: tool rejected filter programming");
        return rc;
    }

    *pFilterID = slot + 1;
    Log("StartMsgFilter: id=%d type=%lu mask=%08lX pattern=%08lX fc=%08lX",
        slot + 1, FilterType, f.maskId, f.patternId, f.flowControlId);
    return STATUS_NOERROR;
}

long J2534_API PassThruStopMsgFilter(unsigned long ChannelID, unsigned long FilterID)
{
    if (ChannelID != CHANNEL_ID || !g_chan.open)
        return ERR_INVALID_CHANNEL_ID;
    if (FilterID < 1 || FilterID > MAX_FILTERS)
        return ERR_INVALID_FILTER_ID;

    EnterCriticalSection(&g_dev.lock);
    Filter& f = g_chan.filters[FilterID - 1];
    if (!f.used) {
        LeaveCriticalSection(&g_dev.lock);
        return ERR_INVALID_FILTER_ID;
    }
    f.used = false;
    unsigned char nn = is29Bit() ? 0x01 : 0x00;
    unsigned char d[4] = { 0x05, (unsigned char)(FilterID - 1), nn, 0x00 };   // disable slot
    if (g_dev.open && !g_dev.dvi.linkDown())
        dviCmd(DVI_CAN, d, 4, DVI_CAN_RESP);
    LeaveCriticalSection(&g_dev.lock);
    return STATUS_NOERROR;
}

long J2534_API PassThruSetProgrammingVoltage(unsigned long DeviceID,
                                             unsigned long PinNumber,
                                             unsigned long Voltage)
{
    Log("PassThruSetProgrammingVoltage(pin=%lu, mV=0x%lX)", PinNumber, Voltage);
    if (DeviceID != DEVICE_ID || !g_dev.open)
        return ERR_INVALID_DEVICE_ID;
    if (PinNumber != 13) {
        setLastError("Only pin 13 (Ford FEPS) is supported on the OBDX Pro FT");
        return ERR_PIN_INVALID;
    }
    if (Voltage == 0xFFFFFFFE) {
        setLastError("SHORT_TO_GROUND is not supported on the FEPS pin");
        return ERR_NOT_SUPPORTED;
    }

    unsigned char relay = (Voltage == 0xFFFFFFFF) ? 0x00 : 0x01;   // VOLTAGE_OFF
    if (relay && (Voltage < 15000 || Voltage > 20000))
        Log("FEPS voltage is fixed ~18V on the FT (requested %lumV)", Voltage);

    EnterCriticalSection(&g_dev.lock);
    unsigned char r[2] = { 0x02, relay };     // FEPS relay
    unsigned char v[2] = { 0x03, relay };     // FEPS voltage regulator
    long rc = dviCmd(DVI_ADC, r, 2, DVI_ADC_RESP);
    if (rc == STATUS_NOERROR)
        rc = dviCmd(DVI_ADC, v, 2, DVI_ADC_RESP);
    LeaveCriticalSection(&g_dev.lock);

    if (rc != STATUS_NOERROR)
        setLastError("Tool rejected the FEPS (DVI 0x3A) command");
    return rc;
}

long J2534_API PassThruReadVersion(unsigned long DeviceID, char* pFirmwareVersion,
                                   char* pDllVersion, char* pApiVersion)
{
    if (!pFirmwareVersion || !pDllVersion || !pApiVersion)
        return ERR_NULL_PARAMETER;
    if (DeviceID != DEVICE_ID || !g_dev.open)
        return ERR_INVALID_DEVICE_ID;

    std::string fw = g_dev.dvi.firmware();
    if (fw.empty())
        fw = "OBDX Pro FT (unknown)";
    strncpy(pFirmwareVersion, fw.c_str(), 79);
    pFirmwareVersion[79] = 0;
    strcpy(pDllVersion, "BlueJ2534 0.3.0 DVI");
    strcpy(pApiVersion, "04.04");
    return STATUS_NOERROR;
}

long J2534_API PassThruGetLastError(char* pErrorDescription)
{
    if (!pErrorDescription)
        return ERR_NULL_PARAMETER;
    AcquireSRWLockShared(&g_errLock);
    strncpy(pErrorDescription, g_lastError, 79);
    ReleaseSRWLockShared(&g_errLock);
    pErrorDescription[79] = 0;
    return STATUS_NOERROR;
}

long J2534_API PassThruIoctl(unsigned long ChannelID, unsigned long IoctlID,
                             void* pInput, void* pOutput)
{
    Log("PassThruIoctl(id=%lu, ioctl=0x%lX)", ChannelID, IoctlID);

    switch (IoctlID) {
    case GET_CONFIG: {
        if (ChannelID != CHANNEL_ID || !g_chan.open)
            return ERR_INVALID_CHANNEL_ID;
        SCONFIG_LIST* list = (SCONFIG_LIST*)pInput;
        if (!list || !list->ConfigPtr)
            return ERR_NULL_PARAMETER;
        EnterCriticalSection(&g_dev.lock);
        for (unsigned long i = 0; i < list->NumOfParams; i++) {
            SCONFIG& c = list->ConfigPtr[i];
            switch (c.Parameter) {
            case DATA_RATE:      c.Value = g_chan.baud;    break;
            case LOOPBACK:       c.Value = g_chan.loopback; break;
            case ISO15765_BS:    c.Value = g_chan.bs;      break;
            case ISO15765_STMIN: c.Value = g_chan.stmin;   break;
            case J1962_PINS:     c.Value = g_chan.pins;    break;
            case BS_TX:
            case STMIN_TX:       c.Value = 0xFFFF;         break;
            default:
                Log("GET_CONFIG: parameter 0x%lX unknown, returning 0", c.Parameter);
                c.Value = 0;
                break;
            }
        }
        LeaveCriticalSection(&g_dev.lock);
        return STATUS_NOERROR;
    }
    case SET_CONFIG: {
        if (ChannelID != CHANNEL_ID || !g_chan.open)
            return ERR_INVALID_CHANNEL_ID;
        SCONFIG_LIST* list = (SCONFIG_LIST*)pInput;
        if (!list || !list->ConfigPtr)
            return ERR_NULL_PARAMETER;

        long rc = STATUS_NOERROR;
        EnterCriticalSection(&g_dev.lock);
        for (unsigned long i = 0; i < list->NumOfParams && rc == STATUS_NOERROR; i++) {
            SCONFIG& c = list->ConfigPtr[i];
            switch (c.Parameter) {
            case DATA_RATE: {
                if (g_chan.baud == c.Value)
                    break;
                unsigned long newPins = g_chan.pins;
                if (!g_chan.pinsExplicit)
                    newPins = (c.Value == 125000) ? 0x030B : 0x060E;
                unsigned char p, b; long e;
                if (!dviProtocolCodes(g_chan.protocol, newPins, c.Value, &p, &b, &e)) {
                    setLastError("SET_CONFIG DATA_RATE: unsupported rate %lu", c.Value);
                    rc = ERR_INVALID_IOCTL_VALUE;
                    break;
                }
                unsigned long ob = g_chan.baud, op = g_chan.pins;
                g_chan.baud = c.Value; g_chan.pins = newPins;
                rc = programBus();
                if (rc != STATUS_NOERROR) { g_chan.baud = ob; g_chan.pins = op; }
                break;
            }
            case LOOPBACK:
                g_chan.loopback = c.Value ? 1 : 0;
                break;
            case ISO15765_BS:
            case ISO15765_STMIN:
                if (c.Parameter == ISO15765_BS) g_chan.bs = c.Value & 0xFF;
                else                            g_chan.stmin = c.Value & 0xFF;
                if (g_chan.protocol == ISO15765)
                    dviSetFcData();
                break;
            case J1962_PINS: {
                unsigned long pins = c.Value ? c.Value : 0x060E;
                if (pins != 0x060E && pins != 0x030B) {
                    setLastError("SET_CONFIG J1962_PINS: unsupported pin pair 0x%04lX", pins);
                    rc = ERR_INVALID_IOCTL_VALUE;
                    break;
                }
                g_chan.pinsExplicit = true;
                if (pins == g_chan.pins)
                    break;
                unsigned long ob = g_chan.baud, op = g_chan.pins;
                g_chan.pins = pins;
                if (pins == 0x030B) g_chan.baud = 125000;
                rc = programBus();
                if (rc != STATUS_NOERROR) { g_chan.baud = ob; g_chan.pins = op; }
                else Log("Bus switched to %s", pins == 0x030B ? "MS-CAN (3/11)" : "HS-CAN (6/14)");
                break;
            }
            default:
                Log("SET_CONFIG: parameter 0x%lX=%lu accepted (no-op)", c.Parameter, c.Value);
                break;
            }
        }
        LeaveCriticalSection(&g_dev.lock);
        return rc;
    }
    case READ_VBATT: {
        if (ChannelID != DEVICE_ID || !g_dev.open)
            return ERR_INVALID_DEVICE_ID;
        if (!pOutput)
            return ERR_NULL_PARAMETER;
        EnterCriticalSection(&g_dev.lock);
        unsigned char q[2] = { 0x00, 0xFE };    // single ADC, channel FE = mV
        DviMsg resp;
        long rc = dviCmd(DVI_ADC, q, 2, DVI_ADC_RESP, &resp);
        LeaveCriticalSection(&g_dev.lock);
        if (rc != STATUS_NOERROR)
            return rc;
        // resp.data = [sub, channel, NN, MM] -> mV = (NN<<8)|MM
        if (resp.data.size() >= 4) {
            unsigned long mv = ((unsigned long)resp.data[2] << 8) | resp.data[3];
            *(unsigned long*)pOutput = mv;
            return STATUS_NOERROR;
        }
        setLastError("READ_VBATT: short ADC response");
        return ERR_FAILED;
    }
    case CLEAR_RX_BUFFER:
        if (ChannelID != CHANNEL_ID || !g_chan.open)
            return ERR_INVALID_CHANNEL_ID;
        EnterCriticalSection(&g_dev.lock);
        g_chan.isotp.clear();
        LeaveCriticalSection(&g_dev.lock);
        EnterCriticalSection(&g_chan.rxLock);
        g_chan.rxQueue.clear();
        LeaveCriticalSection(&g_chan.rxLock);
        return STATUS_NOERROR;
    case CLEAR_TX_BUFFER:
        if (ChannelID != CHANNEL_ID || !g_chan.open)
            return ERR_INVALID_CHANNEL_ID;
        return STATUS_NOERROR;
    case CLEAR_PERIODIC_MSGS:
        if (ChannelID != CHANNEL_ID || !g_chan.open)
            return ERR_INVALID_CHANNEL_ID;
        EnterCriticalSection(&g_dev.lock);
        memset(g_chan.periodic, 0, sizeof(g_chan.periodic));
        LeaveCriticalSection(&g_dev.lock);
        return STATUS_NOERROR;
    case CLEAR_MSG_FILTERS:
        if (ChannelID != CHANNEL_ID || !g_chan.open)
            return ERR_INVALID_CHANNEL_ID;
        EnterCriticalSection(&g_dev.lock);
        memset(g_chan.filters, 0, sizeof(g_chan.filters));
        if (g_dev.open && !g_dev.dvi.linkDown())
            reprogramFilters();
        LeaveCriticalSection(&g_dev.lock);
        return STATUS_NOERROR;
    case FAST_INIT:
    case FIVE_BAUD_INIT:
        setLastError("K-line init not available on the CAN-only OBDX Pro FT");
        return ERR_NOT_SUPPORTED;
    case READ_PROG_VOLTAGE:
        if (ChannelID != DEVICE_ID || !g_dev.open)
            return ERR_INVALID_DEVICE_ID;
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

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID lpvReserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_hInst = hInst;
        GetModuleFileNameA(hInst, g_dllDir, MAX_PATH);
        char* slash = strrchr(g_dllDir, '\\');
        if (slash)
            *slash = 0;
        InitializeCriticalSection(&g_dev.lock);
        InitializeCriticalSection(&g_chan.rxLock);
        g_chan.rxEvent = CreateEventA(NULL, FALSE, FALSE, NULL);
        g_dev.workerStop = CreateEventA(NULL, TRUE, FALSE, NULL);
        g_syncInit = (g_chan.rxEvent && g_dev.workerStop);
        DisableThreadLibraryCalls(hInst);
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_dev.workerThread && lpvReserved == NULL) {
            SetEvent(g_dev.workerStop);     // module is pinned; leak sync objs
            return TRUE;
        }
        if (lpvReserved == NULL && g_syncInit && !g_dev.workerThread) {
            DeleteCriticalSection(&g_dev.lock);
            DeleteCriticalSection(&g_chan.rxLock);
            if (g_chan.rxEvent)   CloseHandle(g_chan.rxEvent);
            if (g_dev.workerStop) CloseHandle(g_dev.workerStop);
        }
        LogShutdown();
    }
    return TRUE;
}
