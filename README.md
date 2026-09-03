# EE542-Lab2-Reliable-File-Transfer

EE542 Lab 2 project for a fast, reliable file-transfer protocol over IP.


## Layout

- `include/`: Four shared headers for protocol, reliability, file I/O, and common helpers.
- `src/`: Sender, receiver, and three focused implementation files.
- `scripts/`: Reusable network setup, verification, MD5, and benchmark-matrix commands.
- `tests/`: A protocol test executable.
- `docs/`: The protocol design and report notes.
- `data/`: Local test input/output files; generated contents are ignored by Git.

## Build

`Makefile` will define sender, receiver, test, and cleanup targets once implementation starts.
