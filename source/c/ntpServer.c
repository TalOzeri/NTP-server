#include "ntp_server.h"

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

    exit(EXIT_SUCCESS);
}

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
 * @brief Process an NTP client request and prepare a response.
 *
 * Validates the incoming NTP packet and client address.
 * If the request is valid, fills the response packet and sends it.
 *
 * Return values:
 * - HANDLE_REQUEST_SUCCESS -> Request processed successfully, response sent.
 * - HANDLE_REQUEST_IGNORE  -> Request ignored (invalid packet: bad mode, version, stratum, etc.).
 * - HANDLE_REQUEST_ERROR   -> System-level failure (e.g., sendto failed).
 *
 * @param response Pointer to NTP packet to fill as response.
 * @param request Pointer to received NTP packet.
 * @param client_addr Pointer to client address structure.
 *
 * @return int Status code as described above.
 */
int handle_request(ntp_packet_t *response, ntp_packet_t *request, struct sockaddr_in *client_addr) {

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
        return HANDLE_REQUEST_ERROR;
    }

    char client_ip[INET_ADDRSTRLEN];

    int sent_bytes;

    if (flags.bits.leap_indicator == UNSYNCHRONIZED_LEAP_INDICATOR) {
        fprintf(stderr, "Leap Indicator unsynchronized, ignoring packet\n");
        return HANDLE_REQUEST_IGNORE;
    }

    if (flags.bits.version_number < MIN_NTP_VERSION_NUM || flags.bits.version_number > MAX_NTP_VERSION_NUM) {
        fprintf(stderr, "Unsupported NTP version %d\n", flags.bits.version_number);
        return HANDLE_REQUEST_IGNORE;
    }

    if (flags.bits.mode != NTP_CLIENT_MODE) {
        fprintf(stderr, "Packet mode is not client mode\n");
        return HANDLE_REQUEST_IGNORE;
    }

    if (request->stratum > MAX_STRATUM) {
        fprintf(stderr, "ERROR: The stratum in the request is not supported\n");
        return HANDLE_REQUEST_IGNORE;
    }


    response->rxTm_s = htonl(ntp_seconds);
    response->rxTm_f = htonl(ntp_fraction);

    // Print client info
    if (inet_ntop(AF_INET, &(client_addr->sin_addr), client_ip, INET_ADDRSTRLEN) == NULL) {
        perror("inet_ntop failed");
        strncpy(client_ip, UNKNOWN_IP_STR, sizeof(client_ip));
        client_ip[sizeof(client_ip) - NULL_TERMINATOR_OFFSET] = '\0';
    }
    printf("Request from %s\n", client_ip);

    // Copy client timestamps
    response->origTm_s = request->origTm_s;
    response->origTm_f = request->origTm_f;

    // Set reference timestamp to current time (since we're not syncing with upstream)
    response->refTm_s = response->rxTm_s;
    response->refTm_f = response->rxTm_f;

    // Set transmit time
    if (!get_time(&tv, &ntp_seconds, &ntp_fraction)) {
        perror("sendto failed");
        return HANDLE_REQUEST_ERROR;
    }
    response->txTm_s = htonl(ntp_seconds);
    response->txTm_f = htonl(ntp_fraction);

    // Send response
    sent_bytes = sendto(g_serverfd, (char *)response, sizeof(ntp_packet_t), 0,
                   (struct sockaddr *)client_addr, sizeof(*client_addr));

    if (sent_bytes == SOCKET_ERROR) {
        fprintf(stderr, "ERROR: Sending packet to the client\n");
        return HANDLE_REQUEST_ERROR;
    }

    return HANDLE_REQUEST_SUCCESS;
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


    // Reuse address
    int optval = 1;

        socklen_t len = sizeof(client_addr);

    int received_bytes, status;

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(NTP_PORT_NUMBER);

    // Create UDP socket
    g_serverfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_serverfd == SOCKET_ERROR)
        error("Error creating socket");

    // Bind address to socket
    if (bind(g_serverfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == SOCKET_ERROR)
        error("Could not bind to address");


    if (setsockopt(g_serverfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) == SOCKET_ERROR)
        error("setsockopt failed");


    printf("[+] NTP server is running on port %d. Press Ctrl+C to stop.\n", NTP_PORT_NUMBER);

    while (true) {
        ntp_packet_t request;
        ntp_packet_t response;
        create_base_ntp_response(&response);

        received_bytes = recvfrom(g_serverfd, (char *)&request, sizeof(ntp_packet_t), 0,
                         (struct sockaddr *)&client_addr, &len);

        if (received_bytes == SOCKET_ERROR) {
            if (errno == EINTR) {
                // Interrupted by signal, just retry immediately
                continue;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // No data available right now, could optionally sleep or just continue
                fprintf(stderr, "Warning: No data available (EAGAIN/EWOULDBLOCK), retrying...\n");
                continue;
            } else {
                // Fatal or unexpected error
                perror("recvfrom failed");
                exit(EXIT_FAILURE);
            }
        }

        if (received_bytes != sizeof(ntp_packet_t)) {
            fprintf(stderr, "Invalid NTP packet size: got %d, expected %lu\n",
                    received_bytes, sizeof(ntp_packet_t));
            continue;
        }

        status = handle_request(&response, &request, &client_addr);
        if (status == HANDLE_REQUEST_IGNORE) {
            continue; // Ignore bad client packet
        } else if (status == HANDLE_REQUEST_ERROR) {
            fprintf(stderr, "Fatal error during request handling. Exiting.\n");
            exit(EXIT_FAILURE);
        }
    }
    
    return EXIT_SUCCESS;
}
