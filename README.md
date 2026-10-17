<a href="https://testerpresent.com.au/"><img width="27%" height="27%" align="right" alt="" src="https://github.com/user-attachments/assets/dfcd8128-6270-4a49-bb2a-666cbd9f946d"></a>
<img width="10%" height="10%" align="right" alt="image" src="https://github.com/user-attachments/assets/6fa1ccab-6974-4186-9e89-b52530085d47" />

# Bluetooth OBDX J2534

Proof-of-concept **SAE J2534-1 (v04.04) PassThru shim** that enumerates as a
normal J2534 device in 32-bit diagnostic software, but routes all
communication over **Bluetooth SPP (RFCOMM)** to an **OBDX Pro FT** using the
OBDX **DVI binary protocol** instead of a USB cable.

```
32-bit app (FORScan/FJDS/...)            BlueJ2534.dll (this project)
 ┌───────────────────────┐   PassThru*   ┌──────────────────────────────┐
 │  J2534 API  (04.04)   ├──────────────►│ J2534 state: channels,       │
 └───────────────────────┘               │ filters, periodic msgs,      │
                                         │ ISO-TP, RX queue             │
                                         │        │                     │
                                         │  DVI framing (len+checksum)  │
                                         │        │                     │
                                         │  Bluetooth RFCOMM socket     │
                                         │  (or BT virtual COM port)    │
                                         └───────────┬──────────────────┘
                                                     ▼  SPP
                                          OBDX Pro FT ──► HS-CAN / MS-CAN
```

**Why DVI, not ELM text:** the adapter shares one serial link for bus frames
and command responses. In ELM string mode they collide on a busy bus — an
"OK" is lost among streamed frames, or merged onto one (`...00 0OK`), so every
filter/config command fails. DVI frames each message with a length and
checksum, so unsolicited bus frames (`0x08`/`0x09`) and command acks
(`0x20`/`0x41`/`0x44`) are always cleanly separable. Comms stays enabled while
sending — no stop/resume-stream dance — and DVI exposes ≥28 real hardware
filter slots with per-filter flow IDs (no single-receive-address contention).

The OBDX Pro FT is **CAN-only** (HS-CAN, MS-CAN, ISO15765 — no K-line, no VPW;
the official FT J2534 entry declares `J1850VPW=0`).

## Layout

| Path | Purpose |
|---|---|
| `src/j2534.h` | J2534 v04.04 API definitions (incl. J2534-2 `_PS` protocols, `J1962_PINS`) |
| `src/shim.cpp` | PassThru exports, channel/filter/periodic state, ISO-TP handling, RX pump |
| `src/dvi.*` | OBDX DVI binary protocol: framing, checksum, transact + frame parsing |
| `src/transport.*` | Bluetooth RFCOMM (Winsock, with inquiry-by-name) + COM port transports |
| `src/BlueJ2534.def` | Clean undecorated 32-bit `__stdcall` export names |
| `manager/` | The all-in-one VCI Manager app (install, configure, test, log) + embedded logo |
| `build.bat` | Builds `bin/BlueJ2534.dll` (MSVC, 32-bit) |
| `build_manager.bat` | Builds `bin/BlueManager.exe` |
| `install.ps1` / `uninstall.ps1` | Script alternative to the app's Setup tab |
| `BlueJ2534.ini` | Connection settings (lives next to the DLL) |

## Build

```bash
cmd /c build.bat
```

```bash
cmd /c build_manager.bat
```

## Manager app (install + configure + connect + log, one app)

`bin\BlueManager.exe` — a standard-Windows-theme tool laid out like a
Bosch-style device manager: a header (title text left, Tester Present logo
right), tabs, a device explorer list, an LED connection-status indicator, and
**Connect / Disconnect**. It is x86 so it can load the 32-bit DLL in-process,
and self-elevates via UAC so its Setup tab can write the registry. Tabs:

- **Explorer** — the device in a list (with the Bluetooth logo as its icon),
  an engineering-style status LED (red/amber/green) + state text, and
  **Connect / Disconnect**. Connect opens the device over DVI, reads firmware
  + battery, and holds the session open until you disconnect.
- **Properties** — firmware, DLL/API version, battery, library path.
- **Connection** — BT vs COM; the paired-device picker reads your real
  Windows-paired devices from the registry and **filters to likely OBD
  adapters** by default ("Show all" reveals everything); address/name, COM
  port, serial baud, debug log.
- **Setup** — install location + **Install / Uninstall**: copies
  `BlueJ2534.dll`, writes `BlueJ2534.ini`, and registers
  `HKLM\SOFTWARE\WOW6432Node\PassThruSupport.04.04\BlueJ2534`.
- **Log** — tail `BlueJ2534.log` with auto-refresh, clear, open-folder.

Script alternative for registration only: `install.ps1` / `uninstall.ps1`
(run as admin).

## Protocols

| J2534 protocol | Mapping on the OBDX Pro FT (DVI `0x31`/`0x34`) |
|---|---|
| `CAN` (5), `ISO15765` (6) | HS-CAN (proto `0x02`); baud code via `0x34 0x15` |
| `CAN_PS` (0x8004), `ISO15765_PS` (0x8005) | Pin-select: HS-CAN pins 6/14, or **MS-CAN pins 3/11** (proto `0x04`) |
| `J1850VPW`, `ISO9141` / `ISO14230`, PWM, SCI | **Not available** — the FT is CAN-only |

29-bit CAN is carried per-filter (DVI frame-type byte), so it works on any
CAN speed — unlike the ELM path, which was 11-bit-only on MS/USER CAN.

### Bus switching (HS ↔ MS CAN)

Three ways to land on MS-CAN:

1. Connect `CAN_PS`/`ISO15765_PS` and `SET_CONFIG J1962_PINS (0x8001)`:
   `0x060E` = HS-CAN pins 6/14, `0x030B` = MS-CAN pins 3/11. Switching on an
   open channel reprograms the bus (DVI `0x31 0x02 0x01`) and re-applies the
   baud, formatting and filters; pins/baud commit only on success.
2. Connect with `BaudRate = 125000` (treated as MS-CAN, the usual Ford case).
3. `SET_CONFIG DATA_RATE 125000` on an open channel (also lands on MS-CAN
   unless the app pinned `J1962_PINS=0x060E`). Unsupported bauds are rejected.

### ISO15765 specifics

- TX uses DVI `0x10`/`0x11` with the 4-byte ID inline; the tool adds the CAN
  length byte, segments multi-frame messages and answers flow control
  (auto-formatting + auto frame processing on). FC block-size/STmin come from
  `SET_CONFIG` and are pushed via DVI `0x34 0x09`.
- The ECU response returns as a `0x08`/`0x09` network frame and is routed to
  the RX queue; multi-frame writes require a matching **FLOW_CONTROL filter**
  (else `ERR_NO_FLOW_CONTROL`).
- FirstFrame indications (`RxStatus = ISO15765_FIRST_FRAME`) are queued per
  J2534 04.04; a raw FF/CF reassembly fallback is kept.

### Async receive

With network comms enabled the tool streams matching frames as `0x08`/`0x09`
messages at any time; a worker thread polls them into the RX queue, so
`PassThruReadMsgs` behaves like a real J2534 device. Because DVI is framed,
sending a command never disturbs this stream.

### Ford FEPS

`PassThruSetProgrammingVoltage` (pin 13) maps to DVI `0x3A` FEPS relay +
voltage control; `VOLTAGE_OFF (0xFFFFFFFF)` turns both off.

## Configuration (`BlueJ2534.ini`)

```ini
[connection]
Mode=bt                   ; bt = RFCOMM direct, com = BT virtual COM port
BtAddress=0C:8B:95:4C:AF:56 ; blank = discover by name (slower, can miss)
BtName=OBDX Pro FT        ; name substring for discovery
ComPort=COM5              ; used when Mode=com
Baud=38400
[options]
Log=1                     ; BlueJ2534.log next to the DLL (or %LOCALAPPDATA%)
```

Setting `BtAddress` is strongly preferred — name discovery often returns the
adapter with an empty friendly name, or fails while the radio is busy. Use the
Manager's paired-device picker to fill it in.

## Behavior notes (v0.3, DVI)

- At open the adapter is switched from its power-on ELM text mode into DVI
  byte mode (`DXDP1`), confirmed by a `0x22` info query; at close it is
  switched back to ELM so other apps find it in its default state.
- J2534 filter semantics: **nothing is received until a PASS or FLOW_CONTROL
  filter is set** (enforced in software and mirrored into the DVI hardware
  filters). Each J2534 filter maps to its own DVI slot with its own flow ID —
  no receive-address contention.
- `PassThruReadMsgs` returns `ERR_TIMEOUT` on partial reads at timeout per
  04.04; blocked readers are woken by Disconnect/Close and by a dead link
  (`ERR_DEVICE_NOT_CONNECTED`). Write failures map to J2534 errors, never
  silent success.
- If the install folder is not writable (Program Files), the debug log falls
  back to `%LOCALAPPDATA%\BlueJ2534\BlueJ2534.log`.

## POC limitations

- One device, one channel at a time (`ERR_CHANNEL_IN_USE` otherwise).
- Periodic messages are timed in the shim (Bluetooth latency applies), fine
  for TesterPresent-style keep-alives, not for ms-accurate scheduling.
- Writes wait for the `0x20` ack round-trip even with `Timeout=0` (bounded to
  the app timeout, 250 ms–10 s).
- Bluetooth SPP adds tens of ms of round-trip latency — ECU *flashing* over
  this path is possible for small payloads but slow; diagnostics, DTC work,
  DID reads and routine control are the realistic use cases.
- K-line / VPW / PWM init IOCTLs return `ERR_NOT_SUPPORTED` (CAN-only FT).
- The DVI checksum/framing is verified against every worked example in the
  reference guide, but the full path is **not yet confirmed against the
  hardware** — the first live session is the real test. The Manager's
  **Connect** button and `BlueJ2534.log` are built to make that quick.

## Quick sanity test

Pair the OBDX Pro FT, pick it on the Manager's Connection tab (or set its
address in `BlueJ2534.ini`), then press **Connect** on the Explorer tab — it
opens the device over DVI, turns the status LED green, and shows the firmware
and battery voltage. `BlueJ2534.log` records every DVI frame exchanged.
