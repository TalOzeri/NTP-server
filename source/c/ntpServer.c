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


#define CLOSED_SOCKET    (-1)

static int g_serverfd = CLOSED_SOCKET; // Global socket descriptor, closed by handler

/**
 * @brief Prints an error message and terminates the program.
 *
 * Calls perror(msg) to display a system error message,
 * then exits the program with EXIT_FAILURE.
 * Validates that the message pointer is not NULL.
 *
 * @param msg A string describing the error context.
 *
 * @note This function does not return.
 */
void error(char *msg) {
    CHECK_NULL(msg);
    perror(msg);
    exit(EXIT_FAILURE);
}

void handle_sigint(int sig) {
    IGNORE_UNUSED_VARIABLES(sig);
    printf("\n[+] Caught SIGINT. Shutting down server...\n");
    if (g_serverfd != CLOSED_SOCKET) {
        close(g_serverfd);
        g_serverfd = CLOSED_SOCKET;
        printf("[+] Socket closed.\n");
    }

    exit(0);
}

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

/**
* @brief Get the current system time and convert it to NTP format.
*
* This function retrieves the current system time using gettimeofday().
* It converts the time to NTP format:
* - NTP seconds: seconds since 1900 (NTP epoch)
* - NTP fraction: fractional part of the second, as 32-bit fixed point
*
* The NTP fraction is calculated by converting microseconds to a
* 32-bit fixed-point fraction: (microseconds / 1,000,000) * 2^32.
*
* @param tv Pointer to timeval to store the current system time.
* @param ntp_seconds Pointer to store the NTP seconds.
* @param ntp_fraction Pointer to store the NTP fractional seconds.
*
* @return true on success, false on failure.
*/
bool get_time(struct timeval *tv, uint32_t *ntp_seconds, uint32_t *ntp_fraction) {
    CHECK_NULL(tv);
    CHECK_NULL(ntp_seconds);
    CHECK_NULL(ntp_fraction);

    if (gettimeofday(tv, NULL) == FAILURE) {
        perror("gettimeofday failed");
        return false;
    }

    *ntp_seconds = tv->tv_sec + NTP_TIMESTAMP_DELTA;

    double fraction = (double)(tv->tv_usec) / USEC_IN_SEC;
    *ntp_fraction = (uint32_t)(fraction * NTP_FRAC_SCALE);

    return true;
}



/**
 * @brief Initialize an NTP response packet with base server values.
 *
 * Fills the provided NTP packet with standard values for a server response,
 * such as flags, stratum, poll interval, precision, and fixed root delay/dispersion.
 *
 * @param response Pointer to ntp_packet_t structure to fill.
 */
void create_base_ntp_response(ntp_packet_t *response) {

    CHECK_NULL(response);

    memset(response, 0, sizeof(ntp_packet_t));

    response->flags.raw = LI_VN_MODE_SERVER;
    response->stratum        = STRATUM;
    response->poll           = DEFAULT_POLL_INTERVAL;
    response->precision      = PRECISION_MS;
    response->rootDelay      = htonl(1 << 16); // 1.0 in NTP short format
    response->rootDispersion = htonl(1 << 16); // 1.0 in NTP short format
}

/**
 * @brief Process an incoming NTP request and send a response.
 *
 * Validates the request (leap indicator, version, mode, stratum).
 * Prepares the response packet timestamps and sends it to client.
 *
 * @param response Pointer to the NTP response packet to send.
 * @param request Pointer to the received NTP request packet.
 * @param client_addr Pointer to client's sockaddr_in structure.
 *
 * @return true if response sent, false if request ignored or error sending.
 */
bool handle_request(ntp_packet_t *response, ntp_packet_t *request, struct sockaddr_in *client_addr) {

    CHECK_NULL(response);
    CHECK_NULL(request);
    CHECK_NULL(client_addr);

    // Initialize variables.
    ntp_flags_t flags;
    memset(&flags, 0, sizeof(ntp_flags_t));
    memcpy(&flags, &request->flags, sizeof(ntp_flags_t));

    // Timestamp receive time
    struct timeval tv;
    uint32_t ntp_seconds, ntp_fraction;
    if (!get_time(&tv, &ntp_seconds, &ntp_fraction)) {
        fprintf(stderr, "ERROR: Could not get receive timestamp\n");
        return false;
    }

    char client_ip[INET_ADDRSTRLEN];

    int n;

    if (flags.bits.leap_indicator == UNSYNCHRONIZED_LEAP_INDICATOR) {
        fprintf(stderr, "Leap Indicator unsynchronized, ignoring packet\n");
        return false;
    }

    if (flags.bits.version_number < MIN_NTP_VERSION_NUM || flags.bits.version_number > MAX_NTP_VERSION_NUM) {
        fprintf(stderr, "Unsupported NTP version %d\n", flags.bits.version_number);
        return false;
    }

    if (flags.bits.mode != NTP_CLIENT_MODE) {
        fprintf(stderr, "Packet mode is not client mode\n");
        return false;
    }

    if (request->stratum > MAX_STRATUM) {
        fprintf(stderr, "ERROR: The stratum in the request is not supported\n");
        return false;
    }


    response->rxTm_s = htonl(ntp_seconds);
    response->rxTm_f = htonl(ntp_fraction);

    // Print client info
    inet_ntop(AF_INET, &(client_addr->sin_addr), client_ip, INET_ADDRSTRLEN);
    printf("Request from %s\n", client_ip);

    // Copy client timestamps
    response->origTm_s = request->origTm_s;
    response->origTm_f = request->origTm_f;

    // Set reference timestamp to current time (since we're not syncing with upstream)
    response->refTm_s = response->rxTm_s;
    response->refTm_f = response->rxTm_f;

    // Set transmit time
    if (!get_time(&tv, &ntp_seconds, &ntp_fraction)) {
        fprintf(stderr, "ERROR: Could not get transmit timestamp\n");
        return false;
    }
    response->txTm_s = htonl(ntp_seconds);
    response->txTm_f = htonl(ntp_fraction);

    // Send response
    n = sendto(g_serverfd, (char *)response, sizeof(ntp_packet_t), 0,
                   (struct sockaddr *)client_addr, sizeof(*client_addr));

    if (n < 0) {
        fprintf(stderr, "ERROR: Sending packet to the client\n");
        return false;
    }

    return true;
}


/**
 * @brief Main entry point for the NTP server.
 *
 * Sets up the UDP socket, binds it to NTP port, and listens for incoming requests.
 * For each valid request, prepares and sends an NTP response.
 *
 * @return 0 on clean exit (normally unreachable).
 */
int main() {
    signal(SIGINT, handle_sigint);

    struct sockaddr_in server_addr, client_addr;
    memset(&server_addr, 0, sizeof(server_addr));

    ntp_packet_t response;
    create_base_ntp_response(&response);

    // Reuse address
    int optval = 1;

    ntp_packet_t request;
    socklen_t len = sizeof(client_addr);

    int n;

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(NTP_PORT_NUMBER);

    // Create UDP socket
    g_serverfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_serverfd < 0)
        error("Error creating socket");

    // Bind address to socket
    if (bind(g_serverfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
        error("Could not bind to address");


    if (setsockopt(g_serverfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0)
        error("setsockopt failed");


    printf("[+] NTP server is running on port %d. Press Ctrl+C to stop.\n", NTP_PORT_NUMBER);

    while (1) {
        n = recvfrom(g_serverfd, (char *)&request, sizeof(ntp_packet_t), 0,
                         (struct sockaddr *)&client_addr, &len);

        if (n < 0)
            error("ERROR: Reading from socket");

        if (n != sizeof(ntp_packet_t)) {
            fprintf(stderr, "Invalid NTP packet size: got %d, expected %lu\n",
                    n, sizeof(ntp_packet_t));
            continue;
        }

        handle_request(&response, &request, &client_addr);
    }
    
    return 0;
}
