# EE542-Lab2-Reliable-File-Transfer

EE542 Lab 2 project for a fast, reliable file-transfer protocol over IP. This is a small
three-day C++ lab, and GNU Make is the only build system.

## Layout

- `include/`: Shared protocol, reliability, pacing, file I/O, and common definitions.
- `src/`: Client, server, and four focused implementation files.
- `scripts/`: Local network setup, verification, transfer, and MD5 helpers.
- `tests/`: A compact protocol and window-state test executable.
- `docs/`: Protocol design and report notes.
- `data/`: Local test input/output files; generated contents are ignored by Git.

## Build

```bash
make
make test
```

This creates `bin/client`, `bin/server`, and `bin/protocol_test`.

## Usage

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

## Experiment Scripts

The experiment scripts are intentionally local-only. Run Client commands on the Client
VM, Server commands on the Server VM, and router commands from the Linux shell on the
VyOS VM. The scripts do not use SSH or coordinate processes across machines.

Client means Sender, and Server means Receiver throughout the project. The topology is:

```text
Client/Sender 192.168.20.100
        |
VyOS eth2 192.168.20.1
VyOS eth1 192.168.10.1
        |
Server/Receiver 192.168.10.100
```

The mandatory cases are:

| Case | Expected RTT | Router netem on each egress | Client/Server rate | Router rate |
| --- | --- | --- | --- | --- |
| 1 | about 10 ms | 5 ms delay, 1% loss | 100 Mbps | 100 Mbps |
| 2 | about 200 ms | 100 ms delay, 20% loss | 100 Mbps | 100 Mbps |
| 3 | about 200 ms | 100 ms delay, no configured loss | 100 Mbps | 80 Mbps |

Run every required case with both MTU 1500 and MTU 9001. Each machine must be configured
independently with:

```bash
sudo ./scripts/setup_case.sh <client|server|router> <1|2|3> <1500|9001>
```

For example, configure Case 3 with MTU 1500 on the Client VM:

```bash
sudo ./scripts/setup_case.sh client 3 1500
```

On the Server VM:

```bash
sudo ./scripts/setup_case.sh server 3 1500
```

On the VyOS VM:

```bash
sudo ./scripts/setup_case.sh router 3 1500
```

Inspect the resulting configuration without changing it:

```bash
# Client VM
./scripts/verify_network.sh client 3 1500

# Server VM
./scripts/verify_network.sh server 3 1500

# VyOS VM
./scripts/verify_network.sh router 3 1500
```

Always run the Server/Receiver first. On the Server VM:

```bash
./scripts/run_transfer.sh server 1500 data/received_1g.bin \
    results/case3_mtu1500_server.log
```

Then run the Client/Sender on the Client VM:

```bash
./scripts/run_transfer.sh client 1500 85 data/data_1g.bin \
    results/case3_mtu1500_rate85_client.log
```

`run_transfer.sh` runs one local endpoint only. Its complete syntax is:

```bash
./scripts/run_transfer.sh client <mtu> <rate_mbps> <input_file> [output_log]
./scripts/run_transfer.sh server <mtu> <output_file> [output_log]
```

When a log path is supplied, output is displayed and saved with `tee`.

For an MTU 9001 Case 3 experiment, configure each machine locally:

```bash
# Client VM
sudo ./scripts/setup_case.sh client 3 9001

# Server VM
sudo ./scripts/setup_case.sh server 3 9001

# VyOS VM
sudo ./scripts/setup_case.sh router 3 9001
```

Then start the Server/Receiver:

```bash
./scripts/run_transfer.sh server 9001 data/received_1g.bin
```

Finally, start the Client/Sender:

```bash
./scripts/run_transfer.sh client 9001 100 data/data_1g.bin
```

`verify_md5.sh` compares two files available on the same local VM:

```bash
./scripts/verify_md5.sh data/data_1g.bin data/received_1g.bin
```

Because the original and received files normally reside on separate VMs, run `md5sum`
separately on the Client and Server and compare the displayed hashes manually unless both
files have been placed on one VM.

## Stage 2 Reliability

The receiver returns 8192-bit SACK snapshots during DATA transfer. The sender combines
cumulative ACK and SACK information, retransmits only missing chunks, uses a
three-later-packet fast retransmit threshold, and falls back to a fixed 500 ms RTO. New and
retransmitted DATA share the same Pacer and `--rate` budget. RTT estimation, adaptive
RTO/rate control, automatic bandwidth estimation, CRC32C, and internal MD5 are not
implemented; these are later optimization or optional features.
