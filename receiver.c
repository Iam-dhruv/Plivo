/* BASELINE RECEIVER (C) — naive on purpose. Rewrite it (C, C++, Go, or Rust).
 *
 * Ports (all 127.0.0.1):
 *   bind 47002  <- media from your sender, via the hostile relay
 *   send 47020  -> harness player. MUST be: 4-byte big-endian seq +
 *                  160-byte payload. Frame i counts only if it arrives
 *                  BEFORE its deadline t0 + DELAY_MS + i*20ms.
 *   send 47003  -> feedback to your sender, via the relay (optional)
 *
 * This baseline forwards whatever arrives straight to the player: lost
 * frames stay lost, late frames stay late, duplicates are re-sent
 * harmlessly. All yours to fix — jitter buffer, reordering, recovery.
 *
 * Env vars available: T0, DURATION_S, DELAY_MS. Harness kills the process
 * at run end; a forever-loop is fine.
 */
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <stdint.h>
#include <errno.h>

#define TYPE_DATA 0
#define TYPE_PARITY 1
#define TYPE_RETRANSMIT 2
#define TYPE_FEEDBACK 3
#define JITTER_SIZE 4096
#define FEC_GROUPS 1024

typedef struct __attribute__((packed)) {
    uint32_t seq;
    uint32_t send_ms;
    uint8_t  type;
    uint8_t  group_size;
    uint16_t group_base;
    uint8_t  payload[160];
} WirePacket;

typedef struct {
    uint8_t present;
    uint8_t payload[160];
} JitterSlot;

typedef struct {
    uint8_t count;
    uint8_t parity_present;
    uint8_t data_xor[160];
    uint8_t parity_payload[160];
    uint8_t group_size;
} FECState;

JitterSlot buffer[JITTER_SIZE];
FECState fec_state[FEC_GROUPS];
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
uint32_t max_seq_seen = 0;

int fb_fd;
struct sockaddr_in relay_fb;

// Function to trigger a NACK
void send_nack(uint32_t seq) {
    WirePacket nack;
    memset(&nack, 0, sizeof(nack));
    nack.type = TYPE_FEEDBACK;
    uint32_t net_seq = htonl(seq);
    memcpy(nack.payload, &net_seq, 4);
    sendto(fb_fd, &nack, sizeof(nack), 0, (struct sockaddr *)&relay_fb, sizeof(relay_fb));
}

// Thread 2: The exact-time playout thread
void* playout_thread(void* arg) {
    int out_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in player = {0};
    player.sin_family = AF_INET;
    player.sin_port = htons(47020);
    player.sin_addr.s_addr = inet_addr("127.0.0.1");

    // Robust parsing for T0 (handling potential OCR typos in the doc)
    const char *t0_env = getenv("T0");
    if (!t0_env) t0_env = getenv("TO");
    double t0 = t0_env ? atof(t0_env) : 0.0;
    
    int delay_ms = getenv("DELAY_MS") ? atoi(getenv("DELAY_MS")) : 60;

    uint32_t i = 0;
    struct timespec ts;
    
    for (;;) {
        // Absolute deadline calculation
        double deadline = t0 + (delay_ms / 1000.0) + (i * 0.020);
        
        // CRITICAL FIX: Wake up 4ms early so the packet traverses the local network 
        // stack and arrives at port 47020 *before* the harness evaluates it.
        double target_time = deadline - 0.004; 
        
        ts.tv_sec = (time_t)target_time;
        ts.tv_nsec = (long)((target_time - ts.tv_sec) * 1e9);

        // Sleep until the early target tick
        int ret = clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &ts, NULL);
        if (ret != 0 && ret != EINTR) {
            // If the deadline was deeply in the past (e.g. startup lag), 
            // clock_nanosleep returns an error, meaning we need to catch up immediately.
        }

        pthread_mutex_lock(&lock);
        JitterSlot *slot = &buffer[i % JITTER_SIZE];
        if (slot->present) {
            unsigned char out_buf[164];
            uint32_t net_seq = htonl(i);
            memcpy(out_buf, &net_seq, 4);
            memcpy(out_buf + 4, slot->payload, 160);
            sendto(out_fd, out_buf, 164, 0, (struct sockaddr *)&player, sizeof(player));
        } 
        // Reset for the next cycle loop around
        memset(slot, 0, sizeof(JitterSlot));
        pthread_mutex_unlock(&lock);

        i++;
    }
    return NULL;
}

int main(void) {
    // Media Input (47002)
    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in in_addr = {0};
    in_addr.sin_family = AF_INET;
    in_addr.sin_port = htons(47002);
    in_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    bind(in_fd, (struct sockaddr *)&in_addr, sizeof(in_addr));

    // Feedback Output (47003)
    fb_fd = socket(AF_INET, SOCK_DGRAM, 0);
    relay_fb.sin_family = AF_INET;
    relay_fb.sin_port = htons(47003);
    relay_fb.sin_addr.s_addr = inet_addr("127.0.0.1");

    memset(buffer, 0, sizeof(buffer));
    memset(fec_state, 0, sizeof(fec_state));

    pthread_t p_thread;
    pthread_create(&p_thread, NULL, playout_thread, NULL);

    WirePacket pkt;
    for (;;) {
        memset(&pkt, 0, sizeof(pkt));
        ssize_t n = recvfrom(in_fd, &pkt, sizeof(pkt), 0, NULL, NULL);
        if (n < (ssize_t)sizeof(WirePacket)) continue;

        uint32_t seq = ntohl(pkt.seq);
        uint16_t group_base = ntohs(pkt.group_base);
        uint8_t n_fec = pkt.group_size;
        int group_idx = (group_base / n_fec) % FEC_GROUPS;

        pthread_mutex_lock(&lock);
        
        // Track sequence gaps and fire NACKs instantly
        if (seq > max_seq_seen && seq < max_seq_seen + 50) {
            for (uint32_t missing = max_seq_seen + 1; missing < seq; missing++) {
                if (!buffer[missing % JITTER_SIZE].present) {
                    send_nack(missing);
                }
            }
            max_seq_seen = seq;
        } else if (seq > max_seq_seen) {
            max_seq_seen = seq;
        }

        if (pkt.type == TYPE_DATA || pkt.type == TYPE_RETRANSMIT) {
            if (!buffer[seq % JITTER_SIZE].present) {
                buffer[seq % JITTER_SIZE].present = 1;
                memcpy(buffer[seq % JITTER_SIZE].payload, pkt.payload, 160);

                fec_state[group_idx].count++;
                fec_state[group_idx].group_size = n_fec;
                for (int j = 0; j < 160; j++) {
                    fec_state[group_idx].data_xor[j] ^= pkt.payload[j];
                }
            }
        } else if (pkt.type == TYPE_PARITY) {
            fec_state[group_idx].parity_present = 1;
            fec_state[group_idx].group_size = n_fec;
            memcpy(fec_state[group_idx].parity_payload, pkt.payload, 160);
        }

        // Attempt FEC recovery for the group
        FECState *fs = &fec_state[group_idx];
        if (fs->parity_present && fs->count == fs->group_size - 1) {
            // Find the single missing seq in this block
            for (uint32_t s = group_base; s < group_base + fs->group_size; s++) {
                if (!buffer[s % JITTER_SIZE].present) {
                    buffer[s % JITTER_SIZE].present = 1;
                    for (int j = 0; j < 160; j++) {
                        buffer[s % JITTER_SIZE].payload[j] = fs->data_xor[j] ^ fs->parity_payload[j];
                    }
                    fs->count++; // Prevent duplicate recovery
                    break;
                }
            }
        }

        // Cleanup stale FEC state (crude ring approach)
        if (seq % n_fec == 0) {
             int stale_idx = ((group_base / n_fec) - 2) % FEC_GROUPS;
             if (stale_idx >= 0) {
                 memset(&fec_state[stale_idx], 0, sizeof(FECState));
             }
        }

        pthread_mutex_unlock(&lock);
    }
    return 0;
}