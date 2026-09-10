#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat >&2 <<EOF
Usage:
  Client: $0 client <mtu> <rate_mbps> <input_file> [output_log]
  Server: $0 server <mtu> <output_file> [output_log]
EOF
}

if [[ $# -lt 1 ]]; then
    usage
    exit 2
fi

role=$1

case "$role" in
    client)
        if [[ $# -lt 4 || $# -gt 5 ]]; then
            usage
            exit 2
        fi
        mtu=$2
        rate_mbps=$3
        input_file=$4
        output_log=${5:-}

        if [[ ! -f "$input_file" ]]; then
            echo "Client input file does not exist: $input_file" >&2
            exit 1
        fi

        command=(
            ./bin/client
            --host 192.168.10.100
            --port 9000
            --file "$input_file"
            --mtu "$mtu"
            --rate "$rate_mbps"
        )
        ;;
    server)
        if [[ $# -lt 3 || $# -gt 4 ]]; then
            usage
            exit 2
        fi
        mtu=$2
        output_file=$3
        output_log=${4:-}

        mkdir -p -- "$(dirname -- "$output_file")"
        command=(
            ./bin/server
            --port 9000
            --output "$output_file"
            --mtu "$mtu"
        )
        ;;
    *)
        usage
        exit 2
        ;;
esac

case "$mtu" in
    1500|9001) ;;
    *)
        echo "MTU must be 1500 or 9001." >&2
        usage
        exit 2
        ;;
esac

if [[ ! -x "${command[0]}" ]]; then
    echo "Missing executable ${command[0]}; run 'make' first." >&2
    exit 1
fi

if [[ -n "$output_log" ]]; then
    mkdir -p -- "$(dirname -- "$output_log")"
fi

printf 'Running:'
printf ' %q' "${command[@]}"
printf '\n'

if [[ -n "$output_log" ]]; then
    "${command[@]}" 2>&1 | tee -- "$output_log"
else
    "${command[@]}"
fi
