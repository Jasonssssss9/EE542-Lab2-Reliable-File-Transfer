#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat >&2 <<EOF
Usage: $0 <role> <case> <mtu>
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

case "$test_case" in
    1)
        expected_rtt='~10 ms'
        router_rate='100 Mbps'
        netem_summary='5 ms delay and 1% loss per router egress'
        iperf_rate=95
        ;;
    2)
        expected_rtt='~200 ms'
        router_rate='100 Mbps'
        netem_summary='100 ms delay and 20% loss per router egress'
        iperf_rate=95
        ;;
    3)
        expected_rtt='~200 ms'
        router_rate='80 Mbps'
        netem_summary='100 ms delay and no configured loss per router egress'
        iperf_rate=75
        ;;
esac

show_interface() {
    local iface=$1

    echo
    echo "Interface: $iface"
    if [[ ! -e "/sys/class/net/$iface/mtu" ]]; then
        echo "ERROR: interface $iface does not exist" >&2
        return 1
    fi

    local actual_mtu
    actual_mtu=$(<"/sys/class/net/$iface/mtu")
    if [[ "$actual_mtu" == "$mtu" ]]; then
        echo "MTU check: PASS ($actual_mtu)"
    else
        echo "MTU check: WARNING (expected $mtu, found $actual_mtu)"
    fi
    ip link show dev "$iface"
    tc -s qdisc show dev "$iface"
}

print_endpoint_commands() {
    local destination=$1
    local destination_role=$2
    local datagram_size=1448

    if [[ "$mtu" == "9001" ]]; then
        datagram_size=8949
    fi

    echo
    echo "Recommended manual validation commands"
    echo "Start 'iperf3 -s' manually on the $destination_role VM before the iperf test."
    echo "  ping -i 0.2 -c 200 $destination"
    if [[ "$mtu" == "9001" ]]; then
        echo "  ping -M do -s 8973 -c 20 $destination"
    fi
    echo "  iperf3 -c $destination -u -b ${iperf_rate}M -l $datagram_size -t 20"
}

echo "Verifying local $role configuration for Case $test_case, MTU $mtu"
echo "Expected Case $test_case RTT: $expected_rtt"

case "$role" in
    client)
        echo "Expected local rate: 100 Mbps"
        echo "Test destination: server = 192.168.10.100"
        show_interface ens33
        print_endpoint_commands 192.168.10.100 server
        ;;
    server)
        echo "Expected local rate: 100 Mbps"
        echo "Test destination: client = 192.168.20.100"
        show_interface ens33
        print_endpoint_commands 192.168.20.100 client
        ;;
    router)
        echo "Expected router rate: $router_rate"
        echo "Expected netem: $netem_summary"
        show_interface eth1
        show_interface eth2
        echo
        echo "Review both qdisc outputs for root TBF handle 1: and child netem handle 10:."
        ;;
esac
