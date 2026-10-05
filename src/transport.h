//
// transport.h - byte transport abstraction (Bluetooth RFCOMM or COM port)
//
#pragma once

#include <string>

class Transport {
public:
    virtual ~Transport() {}
    virtual bool open(std::string& err) = 0;
    virtual void close() = 0;
    virtual bool write(const void* data, int len) = 0;
    // Returns number of bytes read (0 on timeout), -1 on error/disconnect.
    virtual int read(void* buf, int maxlen, unsigned timeoutMs) = 0;
    virtual bool isOpen() const = 0;
};

// addr: "AA:BB:CC:DD:EE:FF" (optional). name: device-name substring used for
// discovery when addr is empty (e.g. "OBDII", "ELM327", "Vgate").
Transport* createBtRfcommTransport(const std::string& addr, const std::string& name);

// port: "COM5" etc. (an SPP virtual COM port bound to the paired ELM327).
Transport* createComTransport(const std::string& port, int baud);
