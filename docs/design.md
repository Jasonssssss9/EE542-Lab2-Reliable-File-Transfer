# Stage 1 Design

Stage 1 is the smallest zero-loss FRFT transfer. The client is the sender and the server
is the receiver. Both use one event loop and direct POSIX UDP socket calls; there are no
worker threads or socket abstractions.

## Flow

```text
Client                                      Server
START metadata ---------------------------> create and mmap output
       <----------------------------------- START_ACK
fixed window -> Pacer -> DATA(seq) --------> write at seq * chunk_size
       <----------------------------------- cumulative ACK
COMPLETE ---------------------------------> flush output
       <----------------------------------- COMPLETE_ACK + statistics
```

The 24-byte FRFT header and each control payload are serialized field by field in network
byte order. DATA payload size is `MTU - 20 - 8 - 24`, giving 1448 bytes for MTU 1500 and
8949 bytes for MTU 9001.

`ReceiverTracker` stores one byte per chunk, permits sequence-based out-of-order writes,
ignores duplicate DATA, and advances the cumulative ACK across the contiguous received
prefix. `SenderWindow` permits only `window_chunks` unacknowledged DATA packets.

One fixed-rate `Pacer` uses absolute monotonic deadlines. Every DATA packet selected by the
sender passes through it. Stage 1 has no retransmission path yet, but Stage 2 retransmissions
must use this same Pacer instance.

The server sends a cumulative ACK after every valid DATA packet. Its ACK uses the final
16-byte ACK prefix with `bitmap_bits = 0` and without `FLAG_SACK`.

START and COMPLETE control messages retry up to ten times at one-second intervals. DATA is
not retransmitted in Stage 1, so this version is deliberately limited to zero-loss testing.
File correctness is checked externally with `scripts/verify_md5.sh`.
