# Stage 2 Design

The client is the sender and the server is the receiver. Both use one event loop and direct
POSIX UDP socket calls; there are no worker threads or socket abstractions.

## Flow

```text
Client                                      Server
START metadata ---------------------------> create and mmap output
       <----------------------------------- START_ACK
fixed window -> Pacer -> DATA(seq) --------> write at seq * chunk_size
       <----------------------------------- cumulative ACK + SACK bitmap
selective retransmit -> same Pacer --------> ignore duplicates safely
COMPLETE ---------------------------------> flush output
       <----------------------------------- COMPLETE_ACK + statistics
```

The 24-byte FRFT header and each control payload are serialized field by field in network
byte order. DATA payload size is `MTU - 20 - 8 - 24`, giving 1448 bytes for MTU 1500 and
8949 bytes for MTU 9001.

`ReceiverTracker` stores one byte per chunk, permits sequence-based out-of-order writes,
ignores duplicate DATA, advances the cumulative ACK across the contiguous received prefix,
and generates SACK directly from that receive state.

Each ACK contains the existing 16-byte prefix plus an 8192-bit bitmap based at
`cumulative_ack`. Bit `i` represents chunk `bitmap_base + i`; this implementation stores
bit zero in the least significant bit of the first byte.

`SenderWindow` tracks UNSENT, IN_FLIGHT, and ACKED state, last send time, and whether a
retransmission is pending. It marks cumulative and non-contiguous SACKed chunks monotonically.
A missing in-flight chunk is queued after three later chunks are ACKed. A fixed 500 ms RTO
recovers tail losses and repeated loss. Only the selected missing chunk is retransmitted.

One fixed-rate `Pacer` uses absolute monotonic deadlines. New and retransmitted DATA selected
by reliability pass through the same Pacer and share the configured `--rate` budget.

START and COMPLETE control messages retry up to ten times at one-second intervals. The
server refreshes its three-second TIME_WAIT when it answers a duplicate COMPLETE. File
correctness is checked externally with `scripts/verify_md5.sh`.

The server currently sends one SACK after every valid DATA packet. Periodic ACK scheduling,
RTT-based RTO, adaptive rate control, and other performance tuning remain Stage 3 work.
