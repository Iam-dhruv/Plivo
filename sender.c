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
#include <stdint.h>

#define FEC_N 4
#define TYPE_DATA 0
#define TYPE_PARITY 1

typedef struct __attribute__((packed)) {
    uint32_t seq;
    uint32_t send_ms;
    uint8_t  type;
    uint8_t  group_size;
    uint16_t group_base;
    uint8_t  payload[160];
} WirePacket;

uint8_t parity_buf[160];

uint32_t get_time_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (tv.tv_sec * 1000) + (tv.tv_usec / 1000);
}

int main(void) {
    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in in_addr = {0};
    in_addr.sin_family = AF_INET;
    in_addr.sin_port = htons(47010);
    in_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    bind(in_fd, (struct sockaddr *)&in_addr, sizeof(in_addr));

    int out_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in relay = {0};
    relay.sin_family = AF_INET;
    relay.sin_port = htons(47001);
    relay.sin_addr.s_addr = inet_addr("127.0.0.1");

    unsigned char buf[2048];
    memset(parity_buf, 0, sizeof(parity_buf));

    for (;;) {
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

            sendto(out_fd, &pkt, sizeof(pkt), 0, (struct sockaddr *)&relay, sizeof(relay));

            if ((seq % FEC_N) < 2) {
                sendto(out_fd, &pkt, sizeof(pkt), 0, (struct sockaddr *)&relay, sizeof(relay));
            }

            // Update XOR Parity
            for (int i = 0; i < 160; i++) {
                parity_buf[i] ^= pkt.payload[i];
            }

            // Emit PARITY packet immediately at the end of the small group
            if (seq % FEC_N == FEC_N - 1) {
                WirePacket parity_pkt;
                parity_pkt.seq = htonl(seq); 
                parity_pkt.send_ms = htonl(get_time_ms());
                parity_pkt.type = TYPE_PARITY;
                parity_pkt.group_size = FEC_N;
                parity_pkt.group_base = htons((uint16_t)(seq - (seq % FEC_N)));
                memcpy(parity_pkt.payload, parity_buf, 160);

                sendto(out_fd, &parity_pkt, sizeof(parity_pkt), 0, (struct sockaddr *)&relay, sizeof(relay));
                memset(parity_buf, 0, sizeof(parity_buf)); 
            }
        }
    }
    return 0;
}