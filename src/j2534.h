//
// j2534.h - SAE J2534-1 v04.04 PassThru API definitions
//
// BlueJ2534 - Bluetooth ELM327 J2534 PassThru shim (proof of concept)
//
#pragma once

#include <windows.h>

#define J2534_API __stdcall

// -------------------------------------------------------------------------
// Protocol IDs
// -------------------------------------------------------------------------
#define J1850VPW        1
#define J1850PWM        2
#define ISO9141         3
#define ISO14230        4
#define CAN             5
#define ISO15765        6
#define SCI_A_ENGINE    7
#define SCI_A_TRANS     8
#define SCI_B_ENGINE    9
#define SCI_B_TRANS     10

// J2534-2 pin-selectable protocols (physical bus chosen via J1962_PINS)
#define J1850VPW_PS     0x00008000
#define J1850PWM_PS     0x00008001
#define ISO9141_PS      0x00008002
#define ISO14230_PS     0x00008003
#define CAN_PS          0x00008004
#define ISO15765_PS     0x00008005
#define SW_CAN_PS       0x00008008

// -------------------------------------------------------------------------
// Return / error codes
// -------------------------------------------------------------------------
#define STATUS_NOERROR              0x00
#define ERR_NOT_SUPPORTED           0x01
#define ERR_INVALID_CHANNEL_ID      0x02
#define ERR_INVALID_PROTOCOL_ID     0x03
#define ERR_NULL_PARAMETER          0x04
#define ERR_INVALID_IOCTL_VALUE     0x05
#define ERR_INVALID_FLAGS           0x06
#define ERR_FAILED                  0x07
#define ERR_DEVICE_NOT_CONNECTED    0x08
#define ERR_TIMEOUT                 0x09
#define ERR_INVALID_MSG             0x0A
#define ERR_INVALID_TIME_INTERVAL   0x0B
#define ERR_EXCEEDED_LIMIT          0x0C
#define ERR_INVALID_MSG_ID          0x0D
#define ERR_DEVICE_IN_USE           0x0E
#define ERR_INVALID_IOCTL_ID        0x0F
#define ERR_BUFFER_EMPTY            0x10
#define ERR_BUFFER_FULL             0x11
#define ERR_BUFFER_OVERFLOW         0x12
#define ERR_PIN_INVALID             0x13
#define ERR_CHANNEL_IN_USE          0x14
#define ERR_MSG_PROTOCOL_ID         0x15
#define ERR_INVALID_FILTER_ID       0x16
#define ERR_NO_FLOW_CONTROL         0x17
#define ERR_NOT_UNIQUE              0x18
#define ERR_INVALID_BAUDRATE        0x19
#define ERR_INVALID_DEVICE_ID       0x1A

// -------------------------------------------------------------------------
// Connect flags
// -------------------------------------------------------------------------
#define CAN_29BIT_ID            0x00000100
#define ISO9141_NO_CHECKSUM     0x00000200
#define CAN_ID_BOTH             0x00000800
#define ISO9141_K_LINE_ONLY     0x00001000

// -------------------------------------------------------------------------
// TxFlags
// -------------------------------------------------------------------------
#define ISO15765_FRAME_PAD      0x00000040
#define ISO15765_ADDR_TYPE      0x00000080
// CAN_29BIT_ID shared with connect flags (0x00000100)
#define WAIT_P3_MIN_ONLY        0x00000200
#define SCI_MODE                0x00400000
#define SCI_TX_VOLTAGE          0x00800000

// -------------------------------------------------------------------------
// RxStatus
// -------------------------------------------------------------------------
#define TX_MSG_TYPE             0x00000001
#define START_OF_MESSAGE        0x00000002
#define ISO15765_FIRST_FRAME    0x00000002
#define RX_BREAK                0x00000004
#define TX_INDICATION           0x00000008
#define ISO15765_PADDING_ERROR  0x00000010
#define ISO15765_ADDR_TYPE_RX   0x00000080
// CAN_29BIT_ID shared (0x00000100)

// -------------------------------------------------------------------------
// Filter types
// -------------------------------------------------------------------------
#define PASS_FILTER             0x00000001
#define BLOCK_FILTER            0x00000002
#define FLOW_CONTROL_FILTER     0x00000003

// -------------------------------------------------------------------------
// IOCTL IDs
// -------------------------------------------------------------------------
#define GET_CONFIG                          0x01
#define SET_CONFIG                          0x02
#define READ_VBATT                          0x03
#define FIVE_BAUD_INIT                      0x04
#define FAST_INIT                           0x05
#define CLEAR_TX_BUFFER                     0x07
#define CLEAR_RX_BUFFER                     0x08
#define CLEAR_PERIODIC_MSGS                 0x09
#define CLEAR_MSG_FILTERS                   0x0A
#define CLEAR_FUNCT_MSG_LOOKUP_TABLE        0x0B
#define ADD_TO_FUNCT_MSG_LOOKUP_TABLE       0x0C
#define DELETE_FROM_FUNCT_MSG_LOOKUP_TABLE  0x0D
#define READ_PROG_VOLTAGE                   0x0E

// -------------------------------------------------------------------------
// Configuration parameters (SCONFIG.Parameter)
// -------------------------------------------------------------------------
#define DATA_RATE           0x01
#define LOOPBACK            0x03
#define NODE_ADDRESS        0x04
#define NETWORK_LINE        0x05
#define P1_MIN              0x06
#define P1_MAX              0x07
#define P2_MIN              0x08
#define P2_MAX              0x09
#define P3_MIN              0x0A
#define P3_MAX              0x0B
#define P4_MIN              0x0C
#define P4_MAX              0x0D
#define W0                  0x19
#define W1                  0x0E
#define W2                  0x0F
#define W3                  0x10
#define W4                  0x11
#define W5                  0x12
#define TIDLE               0x13
#define TINIL               0x14
#define TWUP                0x15
#define PARITY              0x16
#define BIT_SAMPLE_POINT    0x17
#define SYNC_JUMP_WIDTH     0x18
#define T1_MAX              0x1A
#define T2_MAX              0x1B
#define T4_MAX              0x1C
#define T5_MAX              0x1D
#define ISO15765_BS         0x1E
#define ISO15765_STMIN      0x1F
#define DATA_BITS           0x20
#define FIVE_BAUD_MOD       0x21
#define BS_TX               0x22
#define STMIN_TX            0x23
#define T3_MAX              0x24
#define ISO15765_WFT_MAX    0x25

// J2534-2 config parameters
// J1962_PINS value: (pinA << 8) | pinB. 0x060E = HS-CAN pins 6/14,
// 0x030B = MS-CAN pins 3/11 (Ford), 0x0000 = default for the protocol.
#define J1962_PINS          0x8037

// -------------------------------------------------------------------------
// Structures
// -------------------------------------------------------------------------
#pragma pack(push, 1)

typedef struct {
    unsigned long ProtocolID;
    unsigned long RxStatus;
    unsigned long TxFlags;
    unsigned long Timestamp;        // microseconds
    unsigned long DataSize;
    unsigned long ExtraDataIndex;
    unsigned char Data[4128];
} PASSTHRU_MSG;

typedef struct {
    unsigned long Parameter;
    unsigned long Value;
} SCONFIG;

typedef struct {
    unsigned long NumOfParams;
    SCONFIG*      ConfigPtr;
} SCONFIG_LIST;

typedef struct {
    unsigned long  NumOfBytes;
    unsigned char* BytePtr;
} SBYTE_ARRAY;

#pragma pack(pop)

// -------------------------------------------------------------------------
// API prototypes (exported)
// -------------------------------------------------------------------------
extern "C" {
long J2534_API PassThruOpen(void* pName, unsigned long* pDeviceID);
long J2534_API PassThruClose(unsigned long DeviceID);
long J2534_API PassThruConnect(unsigned long DeviceID, unsigned long ProtocolID,
                               unsigned long Flags, unsigned long BaudRate,
                               unsigned long* pChannelID);
long J2534_API PassThruDisconnect(unsigned long ChannelID);
long J2534_API PassThruReadMsgs(unsigned long ChannelID, PASSTHRU_MSG* pMsg,
                                unsigned long* pNumMsgs, unsigned long Timeout);
long J2534_API PassThruWriteMsgs(unsigned long ChannelID, PASSTHRU_MSG* pMsg,
                                 unsigned long* pNumMsgs, unsigned long Timeout);
long J2534_API PassThruStartPeriodicMsg(unsigned long ChannelID, PASSTHRU_MSG* pMsg,
                                        unsigned long* pMsgID, unsigned long TimeInterval);
long J2534_API PassThruStopPeriodicMsg(unsigned long ChannelID, unsigned long MsgID);
long J2534_API PassThruStartMsgFilter(unsigned long ChannelID, unsigned long FilterType,
                                      PASSTHRU_MSG* pMaskMsg, PASSTHRU_MSG* pPatternMsg,
                                      PASSTHRU_MSG* pFlowControlMsg, unsigned long* pFilterID);
long J2534_API PassThruStopMsgFilter(unsigned long ChannelID, unsigned long FilterID);
long J2534_API PassThruSetProgrammingVoltage(unsigned long DeviceID, unsigned long PinNumber,
                                             unsigned long Voltage);
long J2534_API PassThruReadVersion(unsigned long DeviceID, char* pFirmwareVersion,
                                   char* pDllVersion, char* pApiVersion);
long J2534_API PassThruGetLastError(char* pErrorDescription);
long J2534_API PassThruIoctl(unsigned long ChannelID, unsigned long IoctlID,
                             void* pInput, void* pOutput);
}
