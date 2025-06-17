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


#define NTP_PORT_NUMBER (123)
#define MAX_CONNECTIONS (3)
#define NTP_TIMESTAMP_DELTA (2208988800ull)
#define STRATUM (2)
#define LI_VN_MODE_SERVER (0x1c)
#define LI_VN_MODE_CLIENT (0x1b)
#define DEFAULT_POLL_INTERVAL (6)
#define PRECISION_MS (-6)
#define MAX_STRATUM (15)

#define LI(request)   (uint8_t) ((request->li_vn_mode & 0xC0) >> 6) // (li   & 11 000 000) >> 6
#define VN(request)   (uint8_t) ((request->li_vn_mode & 0x38) >> 3) // (vn   & 00 111 000) >> 3
#define MODE(request) (uint8_t) ((request->li_vn_mode & 0x07) >> 0) // (mode & 00 000 111) >> 0

int serverfd = -1; // Global var - for closing the socket from the handler

void error( char* msg )
{
    perror( msg ); // Print the error message to stderr.

    exit(EXIT_FAILURE); // Quit the process.
}

void handle_sigint(int sig) {
    (void)sig; // No need for this sig - this is the convention for the sig handlers.
    printf("\n[+] Caught SIGINT. Shutting down server...\n");
    if (serverfd != -1) {
        close(serverfd);
        printf("[+] Socket closed.\n");
    }
    exit(0);
}


typedef struct
{

    uint8_t li_vn_mode;      // Eight bits. li, vn, and mode.
    // li.   Two bits.   Leap indicator.
    // vn.   Three bits. Version number of the protocol.
    // mode. Three bits. Client will pick mode 3 for client.

    uint8_t stratum;         // Eight bits. Stratum level of the local clock.
    uint8_t poll;            // Eight bits. Maximum interval between successive messages.
    uint8_t precision;       // Eight bits. Precision of the local clock.

    uint32_t rootDelay;      // 32 bits. Total round trip delay time.
    uint32_t rootDispersion; // 32 bits. Max error aloud from primary clock source.
    uint32_t refId;          // 32 bits. Reference clock identifier.

    uint32_t refTm_s;        // 32 bits. Reference time-stamp seconds.
    uint32_t refTm_f;        // 32 bits. Reference time-stamp fraction of a second.

    uint32_t origTm_s;       // 32 bits. Originate time-stamp seconds.
    uint32_t origTm_f;       // 32 bits. Originate time-stamp fraction of a second.

    uint32_t rxTm_s;         // 32 bits. Received time-stamp seconds.
    uint32_t rxTm_f;         // 32 bits. Received time-stamp fraction of a second.

    uint32_t txTm_s;         // 32 bits and the most important field the client cares about. Transmit time-stamp seconds.
    uint32_t txTm_f;         // 32 bits. Transmit time-stamp fraction of a second.

} ntp_packet;              // Total: 384 bits or 48 bytes.

void get_time(struct timeval *tv, uint32_t *ntp_seconds, uint32_t *ntp_fraction){
    // Get the time
    gettimeofday(tv, NULL);

    // Convert the time to NTP format
    *ntp_seconds = tv->tv_sec + NTP_TIMESTAMP_DELTA;
    *ntp_fraction = (uint32_t)((tv->tv_usec / 1e6) * (1LL << 32));
}

void create_base_ntp_response(ntp_packet *response){

    // Create and zero out the packet. All 48 bytes worth.



    memset( response, 0, sizeof( ntp_packet ) );

    // Set the first byte's bits to 00,011,100 for li = 0, vn = 3, and mode = 4 (server). The rest will be left set to zero.

    response->li_vn_mode = LI_VN_MODE_SERVER; // Represents 27 in base 10 or 00011100 in base 2.

    response->stratum = STRATUM; // Set stratum.

    response->poll = DEFAULT_POLL_INTERVAL; // 2^6 = 64 seconds (typical value)

    response->precision = PRECISION_MS; // Set precision - -6 equals 15ms.

    response->rootDelay = htonl(1 << 16); // Set root delay - 1 second.

    response->rootDispersion = htonl(1 << 16); // Set root dispersion - 1 second.
}

// return 1 if we should skip (invalid / already handled), 0 if OK to send response
int handle_request(ntp_packet *response, ntp_packet *request, struct sockaddr_in *client_addr) {
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


    if (!(request->stratum <= MAX_STRATUM)) {
        fprintf(stderr, "ERROR: The stratum in the request is not supported\n");
        return 1;
    }

    // Initiate imeval and ntp_seconds / fraction
    struct timeval tv;
    uint32_t ntp_seconds, ntp_fraction;

    // Save receive timestamp
    get_time(&tv, &ntp_seconds, &ntp_fraction);
    response->rxTm_s = htonl(ntp_seconds);
    response->rxTm_f = htonl(ntp_fraction);



    // Log about the request
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(client_addr->sin_addr), client_ip, INET_ADDRSTRLEN);
    printf("Request from %s\n", client_ip);



    // Save the client's transmit time (originate time)
    response->origTm_s = request->origTm_s;
    response->origTm_f = request->origTm_f;

    // Set Reference timestamp (the time when the server's clock was last updated from a more accurate source).
    // Since the server is not syncing with an upstream server i will set it as follow:
    response->refTm_s = response->rxTm_s;
    response->refTm_f = response->rxTm_f;


    // Save Transmit timestamp
    get_time(&tv, &ntp_seconds, &ntp_fraction);
    response->txTm_s = htonl(ntp_seconds);
    response->txTm_f = htonl(ntp_fraction);


    // Send the packet back to the client
    int n = sendto(serverfd, (char *)response, sizeof(ntp_packet), 0, (struct sockaddr*)client_addr, sizeof(*client_addr));

    if ( n < 0 ){
        fprintf(stderr, "ERROR: Sending packet to the client\n");
        return 1;
    }

    return 0;
}

int main(){

    signal(SIGINT, handle_sigint);

    struct sockaddr_in server_addr, client_addr;
    // Fill the server_addr with zeros.
    memset(&server_addr, 0, sizeof(server_addr));

    ntp_packet response;
    create_base_ntp_response(&response);
    
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(NTP_PORT_NUMBER);

    // Create a udp socket
    serverfd = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
    if (serverfd < 0)
        error("Error creating socket");

    // Bind the server address to the socket descriptor
    if (bind(serverfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
        error("Could not bind to address");

    // Add sockopt - reuse address
    int optval = 1;
    if (setsockopt(serverfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0)
        error("setsockopt failed");

    ntp_packet request;
    socklen_t len = sizeof(client_addr);

    printf("[+] NTP server is running on port %d. Press Ctrl+C to stop.\n", NTP_PORT_NUMBER);

    while (1) {

        int n = recvfrom(serverfd, (char*) &request, sizeof(ntp_packet), 0, (struct sockaddr*)&client_addr, &len);
        // Validations
        if (n < 0)
            error("ERROR: Reading from socket");

        if (n != sizeof(ntp_packet)) {
            fprintf(stderr, "Invalid NTP packet size: got %d, expected %lu\n",
                    n, sizeof(ntp_packet));
            continue;
        }

        // Send the packet to the handler - if 1 is returned than coninute (Caused by validation)
        if (handle_request(&response, &request, &client_addr))
            continue;

    }

}
