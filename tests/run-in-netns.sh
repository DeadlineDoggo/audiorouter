#!/bin/bash
# Run a command in a private network namespace with one multicast-capable
# interface and no firewall - e.g. the multicast discovery test, which a host
# firewall would otherwise make skip:
#
#   tests/run-in-netns.sh build/tests/tst_netengine
#
# Needs unprivileged user namespaces (the default on most distributions).
set -e
if [[ "${SOUNDROOT_IN_NETNS:-}" != 1 ]]; then
    exec env SOUNDROOT_IN_NETNS=1 unshare --user --net --map-root-user "$0" "$@"
fi
ip link set lo up
ip link add dummy0 type dummy
ip link set dummy0 multicast on
ip addr add 10.99.0.1/24 dev dummy0
ip link set dummy0 up
exec "$@"
