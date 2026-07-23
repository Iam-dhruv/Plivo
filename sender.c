/* BASELINE SENDER (C) — naive on purpose. Rewrite it (C, C++, Go, or Rust).
 *
 * Ports (all 127.0.0.1):
 *   bind 47010  <- harness source delivers frame i here at t0 + i*20ms
 *                  (format: 4-byte big-endian seq + 160-byte payload)
 *   send 47001  -> relay uplink toward the receiver (YOUR wire format)
 *   bind 47004  <- feedback from your receiver, via the relay (optional)
 *
 * This baseline forwards each frame once, unchanged, and ignores feedback.
 * No redundancy, no retransmission. It cannot pass. That is the point.
 *
 * Env vars available if you want them: T0 (epoch seconds, float),
 * DURATION_S, DELAY_MS. The harness kills this process when the run ends,
 * so a forever-loop is fine.
 *
 * build: make        run: python3 run.py --delay_ms 60
 */
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <poll.h>
#include <stdint.h>

#define FEC_N 5
#define HISTORY_SIZE 1024

#define TYPE_DATA 0
#define TYPE_PARITY 1
#define TYPE_RETRANSMIT 2
#define TYPE_FEEDBACK 3

typedef struct __attribute__((packed)) {
    uint32_t seq;
    uint32_t send_ms;
    uint8_t  type;
    uint8_t  group_size;
    uint16_t group_base;
    uint8_t  payload[160];
} WirePacket;

WirePacket history[HISTORY_SIZE];
uint8_t parity_buf[160];

uint32_t get_time_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (tv.tv_sec * 1000) + (tv.tv_usec / 1000);
}

int main(void) {
    // 1. Harness Input Socket (47010)
    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in in_addr = {0};
    in_addr.sin_family = AF_INET;
    in_addr.sin_port = htons(47010);
    in_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    bind(in_fd, (struct sockaddr *)&in_addr, sizeof(in_addr));

    // 2. Feedback Input Socket (47004)
    int fb_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in fb_addr = {0};
    fb_addr.sin_family = AF_INET;
    fb_addr.sin_port = htons(47004);
    fb_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    bind(fb_fd, (struct sockaddr *)&fb_addr, sizeof(fb_addr));

    // 3. Relay Output Socket (47001)
    int out_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in relay = {0};
    relay.sin_family = AF_INET;
    relay.sin_port = htons(47001);
    relay.sin_addr.s_addr = inet_addr("127.0.0.1");

    struct pollfd fds[2];
    fds[0].fd = in_fd; fds[0].events = POLLIN;
    fds[1].fd = fb_fd; fds[1].events = POLLIN;

    unsigned char buf[2048];
    memset(parity_buf, 0, sizeof(parity_buf));

    for (;;) {
        if (poll(fds, 2, -1) < 0) continue;

        // Handle new data from harness
        if (fds[0].revents & POLLIN) {
            ssize_t n = recvfrom(in_fd, buf, sizeof(buf), 0, NULL, NULL);
            if (n >= 164) { 
                uint32_t net_seq;
                memcpy(&net_seq, buf, 4);
                uint32_t seq = ntohl(net_seq);
                
                WirePacket pkt;
                pkt.seq = htonl(seq);
                pkt.send_ms = htonl(get_time_ms());
                pkt.type = TYPE_DATA;
                pkt.group_size = FEC_N;
                pkt.group_base = htons((uint16_t)(seq - (seq % FEC_N)));
                memcpy(pkt.payload, buf + 4, 160);

                // Save to ring buffer
                history[seq % HISTORY_SIZE] = pkt;

                // Send DATA
                sendto(out_fd, &pkt, sizeof(pkt), 0, (struct sockaddr *)&relay, sizeof(relay));

                // Update XOR Parity
                for (int i = 0; i < 160; i++) {
                    parity_buf[i] ^= pkt.payload[i];
                }

                // If end of FEC group, send PARITY packet
                if (seq % FEC_N == FEC_N - 1) {
                    WirePacket parity_pkt;
                    parity_pkt.seq = htonl(seq); 
                    parity_pkt.send_ms = htonl(get_time_ms());
                    parity_pkt.type = TYPE_PARITY;
                    parity_pkt.group_size = FEC_N;
                    parity_pkt.group_base = htons((uint16_t)(seq - (seq % FEC_N)));
                    memcpy(parity_pkt.payload, parity_buf, 160);

                    sendto(out_fd, &parity_pkt, sizeof(parity_pkt), 0, (struct sockaddr *)&relay, sizeof(relay));
                    memset(parity_buf, 0, sizeof(parity_buf)); // Reset for next group
                }
            }
        }

        // Handle feedback (NACKs) from receiver
        if (fds[1].revents & POLLIN) {
            WirePacket fb_pkt;
            ssize_t n = recvfrom(fb_fd, &fb_pkt, sizeof(fb_pkt), 0, NULL, NULL);
            if (n == sizeof(WirePacket) && fb_pkt.type == TYPE_FEEDBACK) {
                // The payload contains the sequence number that is missing (first 4 bytes)
                uint32_t missing_seq;
                memcpy(&missing_seq, fb_pkt.payload, 4);
                missing_seq = ntohl(missing_seq);
                
                // Validate it's still in the buffer
                WirePacket *cached = &history[missing_seq % HISTORY_SIZE];
                if (ntohl(cached->seq) == missing_seq && cached->type == TYPE_DATA) {
                    cached->type = TYPE_RETRANSMIT;
                    cached->send_ms = htonl(get_time_ms());
                    sendto(out_fd, cached, sizeof(*cached), 0, (struct sockaddr *)&relay, sizeof(relay));
                    cached->type = TYPE_DATA; // reset back
                }
            }
        }
    }
    return 0;
}