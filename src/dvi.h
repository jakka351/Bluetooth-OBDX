//
// dvi.h - OBDX DVI (Direct Vehicle Interface) binary protocol layer
//
// The OBDX Pro FT's native byte protocol (OBDX Pro FT Reference Guide v2 §3).
// Every message is  CMD  LEN[.LEN]  DATA...  CHK  where CHK = ~(sum of all
// preceding bytes) & 0xFF. Because every message is length-delimited and
// checksummed, unsolicited bus frames (0x08/0x09) and command acknowledgements
// (0x20/0x41/0x44/0x32/0x4A) never collide on the wire - unlike the ELM text
// mode, where an "OK" and a streamed frame share one line channel.
//
#pragma once

#include <string>
#include <vector>
#include "transport.h"

// DVI command / response bytes used by this shim.
#define DVI_RX_NORMAL     0x08  // unsolicited: received network frame
#define DVI_RX_LARGE      0x09  // unsolicited: received frame, 2-byte length
#define DVI_TX_NORMAL     0x10  // send network frame            -> ack 0x20
#define DVI_TX_LARGE      0x11  // send network frame, 2-byte len -> ack 0x20
#define DVI_TX_ACK        0x20
#define DVI_INFO          0x22  // scantool info                 -> resp 0x32
#define DVI_INFO_RESP     0x32
#define DVI_SETTINGS      0x24  // command format/settings       -> resp 0x34
#define DVI_PROTO         0x31  // set protocol / comms          -> resp 0x41
#define DVI_PROTO_RESP    0x41
#define DVI_CAN           0x34  // CAN settings (filters, baud)  -> resp 0x44
#define DVI_CAN_RESP      0x44
#define DVI_ADC           0x3A  // ADC / FEPS                    -> resp 0x4A
#define DVI_ADC_RESP      0x4A
#define DVI_ERROR         0x7F  // error frame: data = [origCmd, errCode, ...]

// A parsed DVI message: command byte + payload (length & checksum stripped).
struct DviMsg {
    unsigned char cmd;
    std::vector<unsigned char> data;
};

class Dvi {
public:
    Dvi();
    ~Dvi();

    // Open the transport, take the adapter from its power-on ELM text mode
    // into DVI byte mode, and confirm with an info query.
    bool open(Transport* t, std::string& err);
    void close();
    bool isOpen() const;
    bool linkDown() const { return m_dead; }
    std::string firmware() const { return m_firmware; }

    // Build and send one framed command (adds the length byte(s) + checksum).
    bool send(unsigned char cmd, const unsigned char* data, size_t len, bool large = false);

    // Send a command and read framed replies until one matches wantCmd, or a
    // 0x7F error naming the sent cmd arrives, or timeout. Network frames
    // (0x08/0x09) seen meanwhile are appended to *net (never dropped).
    // Returns true when resp is filled (resp.cmd is wantCmd or DVI_ERROR).
    bool transact(unsigned char cmd, const unsigned char* data, size_t len,
                  unsigned char wantCmd, DviMsg& resp,
                  unsigned timeoutMs, std::vector<DviMsg>* net, bool large = false);

    // Drain unsolicited traffic for up to maxMs; append 0x08/0x09 to net.
    void poll(std::vector<DviMsg>& net, unsigned maxMs);

    void flush();   // drop any buffered partial frame

private:
    int  readChunk(unsigned timeoutMs);    // transport -> m_buf; -1 on death
    bool extractMsg(DviMsg& out);          // one frame from m_buf (resyncs)
    bool switchToDvi(std::string& err);    // ELM preamble + DXDP1
    std::string elmCommand(const std::string& cmd, unsigned timeoutMs);

    Transport*                 m_t;
    std::string                m_firmware;
    std::vector<unsigned char> m_buf;
    bool                       m_dead;
};

unsigned char dviChecksum(const unsigned char* bytes, size_t n);
