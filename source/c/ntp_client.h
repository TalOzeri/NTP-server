#ifndef NTP_CLIENT_H
#define NTP_CLIENT_H

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

// Constants
#define NTP_TIMESTAMP_DELTA 2208988800ull
#define NTP_PORT_NUMBER        (123)
#define LI_VN_MODE_CLIENT      (0x1b)

extern const char* HOST_NAME;  // NTP server hostname

// Union representing the first byte of an NTP packet (LI | VN | Mode).
// This allows safe access to the flags both as a full byte and as individual bitfields.
// The NTP specification defines the first byte as:
// Bits 0-1: Leap Indicator (LI)
// Bits 2-4: Version Number (VN)
// Bits 5-7: Mode
// However, C does not define bitfield ordering, so we check system endianness.
typedef union {
    struct {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        uint8_t mode : 3;             // Bits 0-2: Mode (e.g., client = 3, server = 4)
        uint8_t version_number : 3;   // Bits 3-5: NTP version (3 or 4 are common)
        uint8_t leap_indicator : 2;   // Bits 6-7: Leap Indicator (0 = no warning, 3 = unsynced)
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        uint8_t leap_indicator : 2;   // Bits 0-1 (most significant)
        uint8_t version_number : 3;   // Bits 2-4
        uint8_t mode : 3;             // Bits 5-7 (least significant)
#else
#   error "Unknown byte order. Cannot safely define bitfields."
#endif
    } bits;

    uint8_t raw; // Raw access to the full byte (for sending/receiving over network)
} ntp_flags_t;


// Full NTP packet structure (48 bytes total).
// This layout matches the standard NTPv3/4 packet format used in the protocol.
typedef struct __attribute__((packed)) ntp_packet_t {
    ntp_flags_t flags;        // First byte: Leap, Version, Mode
    uint8_t  stratum;         // Stratum level
    uint8_t  poll;            // Max interval between messages
    uint8_t  precision;       // Clock precision

    uint32_t rootDelay;       // Total round trip delay time
    uint32_t rootDispersion;  // Max error from primary source
    uint32_t refId;           // Reference clock ID

    uint32_t refTm_s;         // Reference timestamp (seconds)
    uint32_t refTm_f;         // Reference timestamp (fraction)

    uint32_t origTm_s;        // Originate timestamp (seconds)
    uint32_t origTm_f;        // Originate timestamp (fraction)

    uint32_t rxTm_s;          // Receive timestamp (seconds)
    uint32_t rxTm_f;          // Receive timestamp (fraction)

    uint32_t txTm_s;          // Transmit timestamp (seconds)
    uint32_t txTm_f;          // Transmit timestamp (fraction)
} ntp_packet_t; // Total: 384 bits (48 bytes)

// Function declarations
void error(char* msg);
int run_ntp_client(void);

#endif // NTP_CLIENT_H
