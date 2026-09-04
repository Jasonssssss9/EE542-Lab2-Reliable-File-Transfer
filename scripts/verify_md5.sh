#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "Usage: $0 <original-file> <received-file>" >&2
    exit 2
fi

original_hash=$(md5sum -- "$1" | awk '{print $1}')
received_hash=$(md5sum -- "$2" | awk '{print $1}')

echo "original: $original_hash  $1"
echo "received: $received_hash  $2"

if [[ "$original_hash" != "$received_hash" ]]; then
    echo "MD5 mismatch" >&2
    exit 1
fi

echo "MD5 match"
