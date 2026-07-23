/* BASELINE RECEIVER (C) — naive on purpose. Rewrite it (C, C++, Go, or Rust).
 *
 * Ports (all 127.0.0.1):
 *   bind 47002  <- media from your sender, via the hostile relay
 *   send 47020  -> harness player. MUST be: 4-byte big-endian seq +
 *                  160-byte payload. Frame i counts only if it arrives
 *                  BEFORE its deadline t0 + DELAY_MS + i*20ms.
 *   send 47003  -> feedback to your sender, via the relay (optional)
 *
 * This version forwards received frames immediately and reconstructs a
 * single missing frame per FEC group when parity arrives.
 */
#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define TYPE_DATA 0
#define TYPE_PARITY 1
#define SEQ_RING 4096
#define FEC_GROUPS 2048
#define MAX_FEC_GROUP 8

typedef struct __attribute__((packed)) {
    uint32_t seq;
    uint32_t send_ms;
    uint8_t type;
    uint8_t group_size;
    uint16_t group_base;
    uint8_t payload[160];
} WirePacket;

typedef struct {
    uint32_t seq;
    uint8_t present;
    uint8_t delivered;
    uint8_t payload[160];
} FrameSlot;

typedef struct {
    uint32_t group_base;
    uint8_t valid;
    uint8_t parity_present;
    uint8_t group_size;
    uint8_t present_mask;
    uint8_t delivered_mask;
    uint8_t payloads[MAX_FEC_GROUP][160];
    uint8_t parity_payload[160];
} FECState;

static FrameSlot buffer[SEQ_RING];
static FECState fec_state[FEC_GROUPS];

static void send_frame(int out_fd, const struct sockaddr_in *player,
                       uint32_t seq, const uint8_t payload[160]) {
    unsigned char out_buf[164];
    uint32_t net_seq = htonl(seq);
    memcpy(out_buf, &net_seq, 4);
    memcpy(out_buf + 4, payload, 160);
    sendto(out_fd, out_buf, sizeof(out_buf), 0,
           (const struct sockaddr *)player, sizeof(*player));
}

static void reset_group(FECState *fs, uint32_t group_base, uint8_t group_size) {
    memset(fs, 0, sizeof(*fs));
    fs->group_base = group_base;
    fs->group_size = group_size;
    fs->valid = 1;
}

static void deliver_slot(int out_fd, const struct sockaddr_in *player,
                         uint32_t seq, const uint8_t payload[160]) {
    FrameSlot *slot = &buffer[seq % SEQ_RING];
    if (slot->delivered && slot->seq == seq) {
        return;
    }
    slot->seq = seq;
    slot->present = 1;
    slot->delivered = 1;
    memcpy(slot->payload, payload, 160);
    send_frame(out_fd, player, seq, payload);
}

static void try_reconstruct(int out_fd, const struct sockaddr_in *player,
                            FECState *fs) {
    if (!fs->valid || !fs->parity_present || fs->group_size == 0) {
        return;
    }

    uint8_t missing_index = 255;
    uint8_t present_count = 0;
    for (uint8_t i = 0; i < fs->group_size; i++) {
        uint8_t mask = (uint8_t)(1u << i);
        if (fs->present_mask & mask) {
            present_count++;
        } else if (missing_index == 255) {
            missing_index = i;
        } else {
            return;
        }
    }

    if (missing_index == 255 || present_count != (uint8_t)(fs->group_size - 1)) {
        return;
    }

    uint8_t recovered[160];
    memcpy(recovered, fs->parity_payload, 160);
    for (uint8_t i = 0; i < fs->group_size; i++) {
        if (i == missing_index) {
            continue;
        }
        for (int j = 0; j < 160; j++) {
            recovered[j] ^= fs->payloads[i][j];
        }
    }

    uint32_t seq = fs->group_base + missing_index;
    uint8_t mask = (uint8_t)(1u << missing_index);
    if (fs->delivered_mask & mask) {
        return;
    }
    fs->delivered_mask |= mask;
    deliver_slot(out_fd, player, seq, recovered);
}

int main(void) {
    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in in_addr = {0};
    in_addr.sin_family = AF_INET;
    in_addr.sin_port = htons(47002);
    in_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    bind(in_fd, (struct sockaddr *)&in_addr, sizeof(in_addr));

    int out_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in player = {0};
    player.sin_family = AF_INET;
    player.sin_port = htons(47020);
    player.sin_addr.s_addr = inet_addr("127.0.0.1");

    memset(buffer, 0, sizeof(buffer));
    memset(fec_state, 0, sizeof(fec_state));

    WirePacket pkt;
    for (;;) {
        memset(&pkt, 0, sizeof(pkt));
        ssize_t n = recvfrom(in_fd, &pkt, sizeof(pkt), 0, NULL, NULL);
        if (n < (ssize_t)sizeof(WirePacket)) {
            continue;
        }

        uint32_t seq = ntohl(pkt.seq);
        uint32_t group_base = (uint32_t)ntohs(pkt.group_base);
        uint8_t group_size = pkt.group_size;
        if (group_size == 0 || group_size > MAX_FEC_GROUP) {
            continue;
        }

        int group_idx = (group_base / group_size) % FEC_GROUPS;
        FECState *fs = &fec_state[group_idx];
        if (!fs->valid || fs->group_base != group_base || fs->group_size != group_size) {
            reset_group(fs, group_base, group_size);
        }

        if (pkt.type == TYPE_DATA) {
            if (seq >= group_base) {
                uint8_t index = (uint8_t)(seq - group_base);
                if (index < group_size) {
                    uint8_t mask = (uint8_t)(1u << index);
                    if (!(fs->present_mask & mask)) {
                        fs->present_mask |= mask;
                        memcpy(fs->payloads[index], pkt.payload, 160);
                        deliver_slot(out_fd, &player, seq, pkt.payload);
                        fs->delivered_mask |= mask;
                        try_reconstruct(out_fd, &player, fs);
                    }
                }
            }
        } else if (pkt.type == TYPE_PARITY) {
            fs->parity_present = 1;
            memcpy(fs->parity_payload, pkt.payload, 160);
            try_reconstruct(out_fd, &player, fs);
        }

        if (fs->present_mask == (uint8_t)((1u << group_size) - 1)) {
            fs->valid = 0;
        }
    }

    return 0;
}