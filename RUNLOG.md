### RUNLOG.md

---

**Experiment 1: Baseline Port & Initial Protocol**

* **Profile:** A
* **Delay (ms):** 100
* **Miss %:** 100
* **Overhead:** 1.44x
* **What changed:** Replaced the naive baseline with a custom wire format. Implemented Forward Error Correction (FEC) using XOR parity with a group size of N=5. Added an Automatic Repeat reQuest (ARQ) feedback loop to send NACKs for missing frames. Playout was handled by a dedicated thread waking up on an absolute system clock (`clock_nanosleep`).
* **Why:** To address the flaky network's packet loss. FEC provides proactive, zero-latency recovery by spending bandwidth. The ARQ loop provides a reactive safety net for unrecoverable losses. The absolute clock was implemented to ensure frames hit the strict `t0 + i*20ms` deadline.

---

**Experiment 2: Playout Guard Time Fix**

*  **Profile:** A
* **Delay (ms):** 100
* **Miss %:** 1
* **Overhead:** 1.45x
* **What changed:** Subtracted 4ms from the absolute deadline calculation (`target_time = deadline - 0.004`). Added memory safety (`memset`) to the receiver loop and robust environment variable parsing.
* **Why:** The initial exact-time scheduling resulted in 100% deadline misses. Waking the thread exactly at the deadline failed to account for local OS thread scheduling delays and local network stack traversal. Waking 4ms early ensured the packet arrived in the harness buffer safely before the strict judgment time.

---

**Experiment 3: Aggressive FEC & ARQ Removal**

* **Profile:** A
* **Delay (ms):** 60
* **Miss %:** 10
* **Overhead:** 1.61x
* **What changed:** Shrunk the FEC group size from N=5 to N=2. Completely stripped out the feedback (NACK) loop and the `poll()` logic on the sender. Reduced the playout guard time from 4ms to 2ms.
* **Why:** To reduce the total delay, the system could no longer wait for the round-trip time required by a NACK. Shrinking N=2 traded higher bandwidth (50% overhead) for much faster proactive recovery, allowing the system to operate smoothly at a lower delay threshold.

---

**Experiment 4: Interleaved Parity (Failed & Reverted)**

* **Profile:** B
* **Delay (ms):** 100
* **Miss %:** 3.40
* **Overhead:** 1.43x
* **What changed:** Modified the sender to delay parity packets by two full groups (80ms later). Expanded the receiver's stale-state cleanup window to wait for these delayed packets.
* **Why:** Attempted to combat burst packet drops, which were swallowing both the data and the immediately sent parity packets.
* **Note:** This was quickly reverted because the delayed parity arrived well after the tight playout deadlines, rendering the intact recovery data useless for a real-time system.

---

**Experiment 5: Zero-Jitter Receiver & Hybrid 1.75x Redundancy**

* **Profile:** B
* **Delay (ms):** 95
* **Miss %:** .93
* **Overhead:** 1.88x
* **What changed:**
* **Receiver:** Completely removed the strict `playout_thread` and absolute timer. Arriving packets and reconstructed frames are now delivered instantly to the harness player.
* **Sender:** Shifted FEC group size to N=4. Added a strict duplication rule to immediately double-send the first two frames of every group (`seq % 4 < 2`).


* **Why:**
* **Receiver:** Pushing packets immediately eliminates all local OS scheduler jitter, relying on the harness to buffer early arrivals.
* **Sender:** Frames at the beginning of a group have the tightest relative deadlines. Instantly duplicating them ensures they survive drops without waiting for the block's parity packet. This hybrid strategy perfectly maximizes the allowed bandwidth budget at ~1.75x (7 packets sent for every 4 frames generated).