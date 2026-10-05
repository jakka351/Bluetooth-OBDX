//
// transport.cpp - Bluetooth RFCOMM (Winsock) and COM port transports
//
#define _CRT_SECURE_NO_WARNINGS
#include <winsock2.h>
#include <ws2bth.h>
#include <windows.h>
#include <stdio.h>
#include <string>

#include "transport.h"
#include "log.h"

#pragma comment(lib, "ws2_32.lib")

// Serial Port Profile service class UUID 00001101-0000-1000-8000-00805F9B34FB
static const GUID SPP_SERVICE_UUID =
    { 0x00001101, 0x0000, 0x1000, { 0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB } };

// -------------------------------------------------------------------------
// Bluetooth RFCOMM transport
// -------------------------------------------------------------------------

static bool parseBtAddress(const std::string& s, ULONGLONG& out)
{
    unsigned int b[6];
    if (sscanf(s.c_str(), "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6 &&
        sscanf(s.c_str(), "%2x%2x%2x%2x%2x%2x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6)
        return false;
    out = 0;
    for (int i = 0; i < 6; i++)
        out = (out << 8) | (ULONGLONG)(b[i] & 0xFF);
    return true;
}

// Inquiry: find a classic BT device whose friendly name contains `nameSub`
// (case-insensitive). Returns true and fills addr on success.
static bool discoverByName(const std::string& nameSub, ULONGLONG& addr, std::string& err)
{
    WSAQUERYSETW qs;
    memset(&qs, 0, sizeof(qs));
    qs.dwSize = sizeof(qs);
    qs.dwNameSpace = NS_BTH;

    HANDLE hLookup = NULL;
    DWORD flags = LUP_CONTAINERS | LUP_RETURN_NAME | LUP_RETURN_ADDR | LUP_FLUSHCACHE;
    if (WSALookupServiceBeginW(&qs, flags, &hLookup) != 0) {
        err = "Bluetooth inquiry failed to start (no Bluetooth radio?), WSA error " +
              std::to_string(WSAGetLastError());
        return false;
    }

    std::wstring wanted;
    for (size_t i = 0; i < nameSub.size(); i++)
        wanted += (wchar_t)towlower((unsigned char)nameSub[i]);

    bool found = false;
    char buf[8192];
    for (;;) {
        DWORD len = sizeof(buf);
        WSAQUERYSETW* res = (WSAQUERYSETW*)buf;
        if (WSALookupServiceNextW(hLookup, LUP_RETURN_NAME | LUP_RETURN_ADDR, &len, res) != 0)
            break;

        std::wstring name = res->lpszServiceInstanceName ? res->lpszServiceInstanceName : L"";
        std::wstring lower;
        for (size_t i = 0; i < name.size(); i++)
            lower += (wchar_t)towlower(name[i]);

        ULONGLONG a = 0;
        if (res->dwNumberOfCsAddrs > 0) {
            SOCKADDR_BTH* sab = (SOCKADDR_BTH*)res->lpcsaBuffer[0].RemoteAddr.lpSockaddr;
            if (sab)
                a = sab->btAddr;
        }
        Log("BT inquiry: found '%ls' addr %012llX", name.c_str(), a);

        if (!found && a != 0 && !wanted.empty() && lower.find(wanted) != std::wstring::npos) {
            addr = a;
            found = true;
            // keep enumerating so the log shows everything in range
        }
    }
    WSALookupServiceEnd(hLookup);

    if (!found)
        err = "No Bluetooth device matching '" + nameSub + "' found in range";
    return found;
}

class BtRfcommTransport : public Transport {
    std::string m_addrStr;
    std::string m_name;
    SOCKET m_sock;
    bool m_wsaInit;
public:
    BtRfcommTransport(const std::string& addr, const std::string& name)
        : m_addrStr(addr), m_name(name), m_sock(INVALID_SOCKET), m_wsaInit(false) {}

    ~BtRfcommTransport() { close(); }

    bool open(std::string& err)
    {
        WSADATA wsd;
        if (WSAStartup(MAKEWORD(2, 2), &wsd) != 0) {
            err = "WSAStartup failed";
            return false;
        }
        m_wsaInit = true;

        ULONGLONG btAddr = 0;
        if (!m_addrStr.empty()) {
            if (!parseBtAddress(m_addrStr, btAddr)) {
                err = "Invalid Bluetooth address '" + m_addrStr + "' (expected AA:BB:CC:DD:EE:FF)";
                return false;
            }
        } else {
            Log("BT: no address configured, running inquiry for name '%s' (takes ~10s)",
                m_name.c_str());
            if (!discoverByName(m_name.empty() ? "OBD" : m_name, btAddr, err))
                return false;
        }

        Log("BT: connecting RFCOMM to %012llX (SPP)", btAddr);
        m_sock = socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
        if (m_sock == INVALID_SOCKET) {
            err = "Failed to create Bluetooth socket, WSA error " + std::to_string(WSAGetLastError());
            return false;
        }

        SOCKADDR_BTH sab;
        memset(&sab, 0, sizeof(sab));
        sab.addressFamily = AF_BTH;
        sab.btAddr = btAddr;
        sab.serviceClassId = SPP_SERVICE_UUID;
        sab.port = BT_PORT_ANY;

        if (connect(m_sock, (SOCKADDR*)&sab, sizeof(sab)) != 0) {
            err = "Bluetooth RFCOMM connect failed, WSA error " + std::to_string(WSAGetLastError()) +
                  " (is the ELM327 powered and paired?)";
            closesocket(m_sock);
            m_sock = INVALID_SOCKET;
            return false;
        }

        Log("BT: connected");
        return true;
    }

    void close()
    {
        if (m_sock != INVALID_SOCKET) {
            closesocket(m_sock);
            m_sock = INVALID_SOCKET;
        }
        if (m_wsaInit) {
            WSACleanup();
            m_wsaInit = false;
        }
    }

    bool isOpen() const { return m_sock != INVALID_SOCKET; }

    bool write(const void* data, int len)
    {
        if (m_sock == INVALID_SOCKET)
            return false;
        const char* p = (const char*)data;
        while (len > 0) {
            int n = send(m_sock, p, len, 0);
            if (n <= 0)
                return false;
            p += n;
            len -= n;
        }
        return true;
    }

    int read(void* buf, int maxlen, unsigned timeoutMs)
    {
        if (m_sock == INVALID_SOCKET)
            return -1;

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(m_sock, &rfds);
        timeval tv;
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;

        int sel = select(0, &rfds, NULL, NULL, &tv);
        if (sel == 0)
            return 0;       // timeout
        if (sel < 0)
            return -1;

        int n = recv(m_sock, (char*)buf, maxlen, 0);
        if (n <= 0)
            return -1;      // closed or error
        return n;
    }
};

Transport* createBtRfcommTransport(const std::string& addr, const std::string& name)
{
    return new BtRfcommTransport(addr, name);
}

// -------------------------------------------------------------------------
// COM port transport (SPP virtual COM port fallback)
// -------------------------------------------------------------------------

class ComTransport : public Transport {
    std::string m_port;
    int m_baud;
    HANDLE m_h;
public:
    ComTransport(const std::string& port, int baud)
        : m_port(port), m_baud(baud), m_h(INVALID_HANDLE_VALUE) {}

    ~ComTransport() { close(); }

    bool open(std::string& err)
    {
        std::string path = "\\\\.\\" + m_port;
        m_h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL,
                          OPEN_EXISTING, 0, NULL);
        if (m_h == INVALID_HANDLE_VALUE) {
            err = "Failed to open " + m_port + ", error " + std::to_string(GetLastError());
            return false;
        }

        DCB dcb;
        memset(&dcb, 0, sizeof(dcb));
        dcb.DCBlength = sizeof(dcb);
        if (GetCommState(m_h, &dcb)) {
            dcb.BaudRate = m_baud > 0 ? m_baud : 38400;
            dcb.ByteSize = 8;
            dcb.Parity   = NOPARITY;
            dcb.StopBits = ONESTOPBIT;
            dcb.fBinary  = TRUE;
            dcb.fOutxCtsFlow = FALSE;
            dcb.fOutxDsrFlow = FALSE;
            dcb.fDtrControl  = DTR_CONTROL_ENABLE;
            dcb.fRtsControl  = RTS_CONTROL_ENABLE;
            SetCommState(m_h, &dcb);   // BT virtual COM ports often ignore this; non-fatal
        }

        // Timeouts are controlled per-read via COMMTIMEOUTS in read().
        Log("COM: opened %s", m_port.c_str());
        return true;
    }

    void close()
    {
        if (m_h != INVALID_HANDLE_VALUE) {
            CloseHandle(m_h);
            m_h = INVALID_HANDLE_VALUE;
        }
    }

    bool isOpen() const { return m_h != INVALID_HANDLE_VALUE; }

    bool write(const void* data, int len)
    {
        if (m_h == INVALID_HANDLE_VALUE)
            return false;
        DWORD written = 0;
        if (!WriteFile(m_h, data, (DWORD)len, &written, NULL))
            return false;
        return written == (DWORD)len;
    }

    int read(void* buf, int maxlen, unsigned timeoutMs)
    {
        if (m_h == INVALID_HANDLE_VALUE)
            return -1;

        COMMTIMEOUTS ct;
        memset(&ct, 0, sizeof(ct));
        ct.ReadIntervalTimeout = 20;             // return once bytes stop flowing
        ct.ReadTotalTimeoutConstant = timeoutMs;
        SetCommTimeouts(m_h, &ct);

        DWORD got = 0;
        if (!ReadFile(m_h, buf, (DWORD)maxlen, &got, NULL))
            return -1;
        return (int)got;
    }
};

Transport* createComTransport(const std::string& port, int baud)
{
    return new ComTransport(port, baud);
}
