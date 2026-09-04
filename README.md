# EE542-Lab2-Reliable-File-Transfer

EE542 Lab 2 project for a fast, reliable file-transfer protocol over IP. This is a small
three-day C++ lab, and GNU Make is the only build system.

## Layout

- `include/`: Shared protocol, reliability, pacing, file I/O, and common definitions.
- `src/`: Client, server, and four focused implementation files.
- `scripts/`: Network setup, verification, MD5, and benchmark-matrix commands.
- `tests/`: A compact protocol and window-state test executable.
- `docs/`: Protocol design and report notes.
- `data/`: Local test input/output files; generated contents are ignored by Git.

## Build

```bash
make
make test
```

This creates `bin/client`, `bin/server`, and `bin/protocol_test`.

## Stage 1 Usage

Start the receiver first:

```bash
./bin/server --port 9000 --output data/received.bin --mtu 1500
```

Then start the sender:

```bash
./bin/client --host <server-ip> --port 9000 \
    --file data/data.bin --mtu 1500 --rate 95
```

Verify the result externally:

```bash
./scripts/verify_md5.sh data/data.bin data/received.bin
```

Both endpoints must use the same MTU. `--rate` is the fixed total DATA sending rate in
Mbps; it is not selected automatically.

## Current Limitations

Stage 1 is intended only for a zero-loss network. It has cumulative ACKs and a fixed
sliding window, but no DATA retransmission, SACK bitmap, fast retransmit, RTO, RTT
estimation, CRC32C, or internal MD5. Those belong to later stages.
