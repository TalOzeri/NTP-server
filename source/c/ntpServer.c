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

#define NTP_PORT_NUMBER        (123)
#define MAX_CONNECTIONS        (3)
#define NTP_TIMESTAMP_DELTA    (2208988800ull)
#define STRATUM                (2)
#define LI_VN_MODE_SERVER      (0x1c)
#define LI_VN_MODE_CLIENT      (0x1b)
#define DEFAULT_POLL_INTERVAL  (6)
#define PRECISION_MS           (-6)
#define MAX_STRATUM            (15)

#define LI(request)    ((uint8_t)((request->li_vn_mode & 0xC0) >> 6)) // Leap Indicator (2 bits)
#define VN(request)    ((uint8_t)((request->li_vn_mode & 0x38) >> 3)) // Version Number (3 bits)
#define MODE(request)  ((uint8_t)((request->li_vn_mode & 0x07) >> 0)) // Mode (3 bits)

static int g_serverfd = -1; // Global socket descriptor, closed by handler

void error(char *msg) {
    CHECK_NULL(msg);
    perror(msg);
    exit(EXIT_FAILURE);
}

void handle_sigint(int sig) {
    (void)sig;
    printf("\n[+] Caught SIGINT. Shutting down server...\n");
    if (g_serverfd != -1) {
        close(g_serverfd);
        printf("[+] Socket closed.\n");
    }
    exit(0);
}

typedef struct {
    uint8_t  li_vn_mode;      // Leap Indicator (2 bits), Version (3 bits), Mode (3 bits)
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
} ntp_packet; // Total: 384 bits (48 bytes)

void get_time(struct timeval *tv, uint32_t *ntp_seconds, uint32_t *ntp_fraction) {

    CHECK_NULL(tv);
    CHECK_NULL(ntp_seconds);
    CHECK_NULL(ntp_fraction);

    gettimeofday(tv, NULL);
    *ntp_seconds = tv->tv_sec + NTP_TIMESTAMP_DELTA;
    *ntp_fraction = (uint32_t)((tv->tv_usec / 1e6) * (1LL << 32));
}

void create_base_ntp_response(ntp_packet *response) {

    CHECK_NULL(response);

    memset(response, 0, sizeof(ntp_packet));

    response->li_vn_mode     = LI_VN_MODE_SERVER;
    response->stratum        = STRATUM;
    response->poll           = DEFAULT_POLL_INTERVAL;
    response->precision      = PRECISION_MS;
    response->rootDelay      = htonl(1 << 16); // 1.0 in NTP short format
    response->rootDispersion = htonl(1 << 16); // 1.0 in NTP short format
}

// Return 1 if packet should be ignored, 0 if response should be sent
int handle_request(ntp_packet *response, ntp_packet *request, struct sockaddr_in *client_addr) {

    CHECK_NULL(response);
    CHECK_NULL(request);
    CHECK_NULL(client_addr);

    if (LI(request) == 3) {
        fprintf(stderr, "Leap Indicator unsynchronized, ignoring packet\n");
        return 1;
    }

    if (VN(request) < 1 || VN(request) > 4) {
        fprintf(stderr, "Unsupported NTP version %d\n", VN(request));
        return 1;
    }

    if (MODE(request) != 3) {
        fprintf(stderr, "Packet mode is not client mode\n");
        return 1;
    }

    if (request->stratum > MAX_STRATUM) {
        fprintf(stderr, "ERROR: The stratum in the request is not supported\n");
        return 1;
    }

    // Timestamp receive time
    struct timeval tv;
    uint32_t ntp_seconds, ntp_fraction;
    get_time(&tv, &ntp_seconds, &ntp_fraction);

    response->rxTm_s = htonl(ntp_seconds);
    response->rxTm_f = htonl(ntp_fraction);

    // Print client info
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(client_addr->sin_addr), client_ip, INET_ADDRSTRLEN);
    printf("Request from %s\n", client_ip);

    // Copy client timestamps
    response->origTm_s = request->origTm_s;
    response->origTm_f = request->origTm_f;

    // Set reference timestamp to current time (since we're not syncing with upstream)
    response->refTm_s = response->rxTm_s;
    response->refTm_f = response->rxTm_f;

    // Set transmit time
    get_time(&tv, &ntp_seconds, &ntp_fraction);
    response->txTm_s = htonl(ntp_seconds);
    response->txTm_f = htonl(ntp_fraction);

    // Send response
    int n = sendto(g_serverfd, (char *)response, sizeof(ntp_packet), 0,
                   (struct sockaddr *)client_addr, sizeof(*client_addr));

    if (n < 0) {
        fprintf(stderr, "ERROR: Sending packet to the client\n");
        return 1;
    }

    return 0;
}

int main() {
    signal(SIGINT, handle_sigint);

    struct sockaddr_in server_addr, client_addr;
    memset(&server_addr, 0, sizeof(server_addr));

    ntp_packet response;
    create_base_ntp_response(&response);

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

    // Reuse address
    int optval = 1;
    if (setsockopt(g_serverfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0)
        error("setsockopt failed");

    ntp_packet request;
    socklen_t len = sizeof(client_addr);

    printf("[+] NTP server is running on port %d. Press Ctrl+C to stop.\n", NTP_PORT_NUMBER);

    while (1) {
        int n = recvfrom(g_serverfd, (char *)&request, sizeof(ntp_packet), 0,
                         (struct sockaddr *)&client_addr, &len);

        if (n < 0)
            error("ERROR: Reading from socket");

        if (n != sizeof(ntp_packet)) {
            fprintf(stderr, "Invalid NTP packet size: got %d, expected %lu\n",
                    n, sizeof(ntp_packet));
            continue;
        }

        if (handle_request(&response, &request, &client_addr))
            continue;
    }
    
    return 0;
}
