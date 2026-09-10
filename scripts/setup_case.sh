#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat >&2 <<EOF
Usage: sudo $0 <role> <case> <mtu>
  role: client | server | router
  case: 1 | 2 | 3
  mtu:  1500 | 9001
EOF
}

if [[ $# -ne 3 ]]; then
    usage
    exit 2
fi

role=$1
test_case=$2
mtu=$3

case "$role" in
    client|server|router) ;;
    *) usage; exit 2 ;;
esac

case "$test_case" in
    1|2|3) ;;
    *) usage; exit 2 ;;
esac

case "$mtu" in
    1500|9001) ;;
    *) usage; exit 2 ;;
esac

if [[ $EUID -ne 0 ]]; then
    echo "This script must be run with sudo or as root." >&2
    exit 1
fi

configure_endpoint() {
    local iface=$1

    ip link set dev "$iface" mtu "$mtu"
    tc qdisc del dev "$iface" root 2>/dev/null || true
    tc qdisc add dev "$iface" root handle 1: \
        tbf rate 100mbit latency 0.001ms burst 9015
}

configure_router_interface() {
    local iface=$1
    local rate=$2
    local delay=$3
    local loss=$4

    ip link set dev "$iface" mtu "$mtu"
    tc qdisc del dev "$iface" root 2>/dev/null || true
    tc qdisc add dev "$iface" root handle 1: \
        tbf rate "$rate" latency 0.001ms burst 9015
    if [[ -n "$loss" ]]; then
        tc qdisc add dev "$iface" parent 1:1 handle 10: \
            netem delay "$delay" loss "$loss"
    else
        tc qdisc add dev "$iface" parent 1:1 handle 10: \
            netem delay "$delay"
    fi
}

show_interface() {
    local iface=$1

    echo
    echo "Interface: $iface"
    ip link show dev "$iface"
    tc -s qdisc show dev "$iface"
}

if [[ "$role" == "client" || "$role" == "server" ]]; then
    configure_endpoint ens33
    echo "Configured $role for Case $test_case: ens33, MTU $mtu, rate 100 Mbps"
    show_interface ens33
    exit 0
fi

case "$test_case" in
    1)
        router_rate=100mbit
        router_delay=5ms
        router_loss=1%
        ;;
    2)
        router_rate=100mbit
        router_delay=100ms
        router_loss=20%
        ;;
    3)
        router_rate=80mbit
        router_delay=100ms
        router_loss=
        ;;
esac

configure_router_interface eth1 "$router_rate" "$router_delay" "$router_loss"
configure_router_interface eth2 "$router_rate" "$router_delay" "$router_loss"

loss_summary=${router_loss:-none}
echo "Configured router for Case $test_case: MTU $mtu, rate $router_rate, delay $router_delay, loss $loss_summary"
show_interface eth1
show_interface eth2
