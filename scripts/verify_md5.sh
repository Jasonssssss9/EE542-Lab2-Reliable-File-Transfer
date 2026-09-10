#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "Usage: $0 <original_file> <received_file>" >&2
    exit 2
fi

original_file=$1
received_file=$2

if [[ ! -f "$original_file" ]]; then
    echo "Original file does not exist: $original_file" >&2
    exit 1
fi

if [[ ! -f "$received_file" ]]; then
    echo "Received file does not exist: $received_file" >&2
    exit 1
fi

original_hash=$(md5sum -- "$original_file" | awk '{print $1}')
received_hash=$(md5sum -- "$received_file" | awk '{print $1}')

echo "Original file: $original_file"
echo "Original MD5:  $original_hash"
echo "Received file: $received_file"
echo "Received MD5:  $received_hash"

if [[ "$original_hash" != "$received_hash" ]]; then
    echo "MD5 FAIL" >&2
    exit 1
fi

echo "MD5 PASS"
