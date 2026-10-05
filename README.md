# Bluetooth OBDX 

Proof-of-concept **SAE J2534-1 (v04.04) PassThru shim** that enumerates as a
normal J2534 device in 32-bit diagnostic software, but routes all
communication over **Bluetooth SPP (RFCOMM)** to an **OBDX Pro FT**
(ELM-compatible AT command set + OBDX extended DX command set) instead of a
USB cable.

```
32-bit app (FORScan/PCMtec...)            BlueJ2534.dll (this project)
 ┌───────────────────────┐   PassThru*   ┌──────────────────────────────┐
 │  J2534 API  (04.04)   ├──────────────►│ J2534 state: channels,       │
 └───────────────────────┘               │ filters, periodic msgs,      │
                                         │ ISO-TP, RX queue             │
                                         │        │                     │
                                         │  AT / DX command layer       │
                                         │        │                     │
                                         │  Bluetooth RFCOMM socket     │
                                         │  (or BT virtual COM port)    │
                                         └───────────┬──────────────────┘
                                                     ▼  SPP
                                          OBDX Pro FT ──► HS/MS CAN, VPW
```

## Layout

| Path | Purpose |
|---|---|
| `src/j2534.h` | J2534 v04.04 API definitions (incl. J2534-2 `_PS` protocols, `J1962_PINS`) |
| `src/shim.cpp` | PassThru exports, channel/filter/periodic state, ISO-TP handling, RX pump |
| `src/elm327.*` | AT/DX command layer (works with OBDX Pro FT; AT subset works on generic ELM327) |
| `src/transport.*` | Bluetooth RFCOMM (Winsock, with inquiry-by-name) + COM port transports |
| `src/BlueJ2534.def` | Clean undecorated 32-bit `__stdcall` export names |
| `installer/` | WinForms setup GUI (.NET Framework 4.8.1) |
| `build.bat` | Builds `bin/BlueJ2534.dll` (MSVC, 32-bit) |
| `build_installer.bat` | Builds `bin/BlueJ2534Setup.exe` |
| `install.ps1` / `uninstall.ps1` | Script alternative to the GUI installer |
| `BlueJ2534.ini` | Connection settings (lives next to the DLL) |

## Build

```bash
cmd /c build.bat
```

```bash
cmd /c build_installer.bat
```

## Install

Run `bin\BlueJ2534Setup.exe` (elevates via UAC). It:

- copies `BlueJ2534.dll` to the chosen folder,
- writes `BlueJ2534.ini` from the GUI settings (paired Bluetooth devices are
  listed straight from the registry; or pick a virtual COM port),
- registers `HKLM\SOFTWARE\WOW6432Node\PassThruSupport.04.04\BlueJ2534`
  so every 32-bit J2534 application can see the device.

Script alternative: `install.ps1` / `uninstall.ps1` (run as admin).

## Protocols

| J2534 protocol | Mapping on the OBDX Pro FT |
|---|---|
| `CAN` (5), `ISO15765` (6) | HS-CAN; `ATSP 6/7/8/9` by baud + 29-bit flag |
| `CAN_PS` (0x8004), `ISO15765_PS` (0x8005) | Pin-select: HS-CAN pins 6/14 or **MS-CAN pins 3/11** (`ATSP E`) |
| `J1850VPW` (1) | `ATSP 2`, header via `ATSH`, CRC handled by the tool |
| `ISO9141` / `ISO14230` | **Not available** — the FT has no K-line |

### Bus switching (HS ↔ MS CAN)

Three ways to land on MS-CAN:

1. Connect `CAN_PS`/`ISO15765_PS` and `SET_CONFIG J1962_PINS (0x8037)`:
   `0x060E` = HS-CAN pins 6/14, `0x030B` = MS-CAN pins 3/11. Switching on an
   open channel reprograms the bus and re-applies filters/flow control.
2. Connect with `BaudRate = 125000` (MS-CAN is 125 kbaud on Fords).
3. `SET_CONFIG DATA_RATE 125000` on an open channel.

### ISO15765 specifics

- TX uses `DXSD <4-byte ID> <payload>` — full header inline, no `ATSH` state.
- The tool does ISO-TP itself (`ATCAF1`): length byte, segmentation and flow
  control. The FC header/data come from the J2534 **FLOW_CONTROL filter**
  (`ATFCSH` + `ATFCSD 30 BS STmin` + `ATFCSM1`).
- Assembled multi-frame responses are parsed from the `1X LL …` form; raw
  frame fallback (FF/CF reassembly in the shim) is kept for CAF0/clone use.
- FirstFrame indications (`RxStatus = ISO15765_FIRST_FRAME`) are queued per
  J2534 04.04.

### Async receive

`DXPT1` (OBDX passthrough mode) streams any frame matching the filters at any
time; a worker thread pumps them into the RX queue, so `PassThruReadMsgs`
with a timeout behaves like a real J2534 device rather than ELM-style
"send one, receive one".

### Ford FEPS

`PassThruSetProgrammingVoltage` maps to `DXFEPS1/0` (18 V programming voltage
on pin 13), `VOLTAGE_OFF (0xFFFFFFFF)` turns it off.

## Configuration (`BlueJ2534.ini`)

```ini
[connection]
Mode=bt            ; bt = RFCOMM direct, com = BT virtual COM port
BtAddress=         ; AA:BB:CC:DD:EE:FF, blank = discover by name (~10 s)
BtName=OBDX        ; name substring for discovery
ComPort=COM5       ; used when Mode=com
Baud=38400
[options]
Log=1              ; BlueJ2534.log next to the DLL
```

## POC limitations

- One device, one channel at a time (`ERR_CHANNEL_IN_USE` otherwise).
- One hardware flow-control pair (the tool has a single FC header slot);
  additional PASS/BLOCK filters are applied in software over an open mask.
- Periodic messages are timed in the shim (Bluetooth latency applies), fine
  for TesterPresent-style keep-alives, not for ms-accurate scheduling.
- Bluetooth SPP adds tens of ms of round-trip latency — ECU *flashing* over
  this path is possible for small payloads but slow; diagnostics, DTC work,
  DID reads and routine control are the realistic use cases.
- `FIVE_BAUD_INIT`/`FAST_INIT` return `ERR_NOT_SUPPORTED` (no K-line on FT).
- Built and export-verified; **not yet tested against hardware** — the
  `BlueJ2534.log` output is designed to make the first live session easy to
  debug.

## Quick sanity test (no app needed)

Pair the OBDX Pro FT, then from a 32-bit test harness call:
`PassThruOpen` → `PassThruConnect(ISO15765, CAN_29BIT_ID off, 500000)` →
`PassThruStartMsgFilter(FLOW_CONTROL, mask 7FF, pattern 7E8, fc 7E0)` →
`PassThruWriteMsgs(7E0 # 3E 00)` → `PassThruReadMsgs`.
`BlueJ2534.log` shows every AT/DX exchange.
