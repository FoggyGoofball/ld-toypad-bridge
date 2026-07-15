#!/usr/bin/env bash
set -euo pipefail

# Usage:
#   ./fix_sources.sh [source_dir]
#
# Optional env:
#   RESTORE_MAIN_FROM=/path/to/main.c

SRC_DIR="${1:-$(pwd)}"

if [[ ! -d "$SRC_DIR" ]]; then
	echo "[fix_sources] Source directory not found: $SRC_DIR" >&2
	exit 1
fi

cd "$SRC_DIR"

if [[ -n "${RESTORE_MAIN_FROM:-}" ]]; then
	if [[ -f "$RESTORE_MAIN_FROM" ]]; then
		cp -f "$RESTORE_MAIN_FROM" ./main.c
		echo "[fix_sources] Restored main.c from: $RESTORE_MAIN_FROM"
	else
		echo "[fix_sources] RESTORE_MAIN_FROM does not exist: $RESTORE_MAIN_FROM" >&2
		exit 1
	fi
fi

echo "[fix_sources] Applying API/type/constant substitutions in $SRC_DIR"

sed -i 's/sys_net_socket(/sysNetSocket(/g' *.c
sed -i 's/sys_net_bind(/sysNetBind(/g' *.c
sed -i 's/sys_net_sendto(/sysNetSendto(/g' *.c
sed -i 's/sys_net_recvfrom(/sysNetRecvfrom(/g' *.c
sed -i 's/sys_net_close(/sysNetClose(/g' *.c
sed -i 's/sys_net_setsockopt(/sysNetSetsockopt(/g' *.c
sed -i 's/sys_net_htons(/htons(/g' *.c
sed -i 's/sys_net_htonl(/htonl(/g' *.c
sed -i 's/sys_net_ntohs(/ntohs(/g' *.c

sed -i 's/SYS_NET_AF_INET/AF_INET/g' *.c
sed -i 's/SYS_NET_SOCK_DGRAM/SOCK_DGRAM/g' *.c
sed -i 's/SYS_NET_INADDR_ANY/INADDR_ANY/g' *.c
sed -i 's/SYS_NET_SOL_SOCKET/SOL_SOCKET/g' *.c
sed -i 's/SYS_NET_SO_NBIO/SO_NBIO/g' *.c
sed -i 's/SYS_NET_SO_RCVTIMEO/SO_RCVTIMEO/g' *.c
sed -i 's/SYS_NET_SO_SNDBUF/SO_SNDBUF/g' *.c
sed -i 's/SYS_NET_SO_RCVBUF/SO_RCVBUF/g' *.c
sed -i 's/SYS_NET_EWOULDBLOCK/EWOULDBLOCK/g' *.c

sed -i 's/sys_net_sockaddr_in/sockaddr_in/g' *.c
sed -i 's/(sys_net_sockaddr\*)/(struct sockaddr*)/g' *.c
sed -i 's/sys_net_socklen_t/socklen_t/g' *.c
sed -i 's/sys_net_timeval/timeval/g' *.c
sed -i 's/\bsys_net_sockaddr\b/struct sockaddr/g' *.c

# Include normalization for malformed lv2 includes and PSL1GHT expected headers.
sed -i -E 's|#include[[:space:]]+lv2/sys_usbd.h>|#include <sys/usbd.h>|g' *.c
sed -i -E 's|#include[[:space:]]+lv2/sys_net.h>|#include <sys/socket.h>\n#include <net/socket.h>|g' network.c
sed -i -E 's|#include[[:space:]]+lv2/sys_memory.h>|#include <sys/memory.h>|g' *.c
sed -i -E 's|#include[[:space:]]+lv2/sys_process.h>|#include <sys/process.h>|g' *.c
sed -i -E 's|#include[[:space:]]+lv2/paf.h>|#include <sys/prx.h>|g' *.c
sed -i -E 's|#include[[:space:]]+lv2/syscalls.h>|#include <lv2/syscalls.h>|g' *.c
sed -i -E 's|#include[[:space:]]+lv2/([^>]+)>|#include <lv2/\1>|g' *.c

echo "[fix_sources] Done"
