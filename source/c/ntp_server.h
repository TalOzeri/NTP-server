#ifndef NTP_SERVER_H
#define NTP_SERVER_H

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <signal.h>
#include <stdbool.h>
#include <errno.h>

/* Macros */
/*
 * CHECK_NULL(ptr)
 * ----------------
 * Terminates the program if 'ptr' is NULL.
 * Prints an error message with the pointer name.
 *
 * Usage: CHECK_NULL(my_ptr);
 *
 * Note: Use only when NULL is fatal. Acts as a single statement.
 */
#define CHECK_NULL(ptr)                             \
    do {                                             \
        if ((ptr) == NULL) {                         \
            fprintf(stderr, "Pointer %s is NULL\n", #ptr); \
            exit(EXIT_FAILURE);                      \
        }                                            \
    } while (false)

#define IGNORE_UNUSED_VARIABLES(var) (void)(var)

#define NTP_PORT_NUMBER        (123)
#define MAX_CONNECTIONS        (3)
#define NTP_TIMESTAMP_DELTA    (2208988800ull)
#define STRATUM                (2)
#define LI_VN_MODE_SERVER      (0x1c)
#define LI_VN_MODE_CLIENT      (0x1b)
#define DEFAULT_POLL_INTERVAL  (6)
#define PRECISION_MS           (-6)
#define MAX_STRATUM            (15)
#define UNSYNCHRONIZED_LEAP_INDICATOR         (3)
#define MIN_NTP_VERSION_NUM     (1)
#define MAX_NTP_VERSION_NUM     (4)
#define NTP_CLIENT_MODE         (3)
#define FAILURE                 (-1)

#define USEC_IN_SEC (1000000UL)
#define NTP_FRAC_SCALE (1LL << 32)

#define UNKNOWN_IP_STR "UNKNOWN"
#define NULL_TERMINATOR_OFFSET (1)

#define HANDLE_REQUEST_SUCCESS (0)
#define HANDLE_REQUEST_IGNORE  (1)
#define HANDLE_REQUEST_ERROR  (-1)

#define SOCKET_ERROR (-1)
#define CLOSED_SOCKET (-1)

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


/* Function declarations */
void error(char *msg);
void handle_sigint(int sig);
bool get_time(struct timeval *tv, uint32_t *ntp_seconds, uint32_t *ntp_fraction);
void create_base_ntp_response(ntp_packet_t *response);
int handle_request(ntp_packet_t *response, ntp_packet_t *request, struct sockaddr_in *client_addr);

#endif // NTP_SERVER_H
