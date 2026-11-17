#!/bin/bash
# bash.sh — Build all DFS components
#
# -I"./helper files"  lets gcc find trie.h / convenience.h / netproto.h
# -pthread            required by the Naming Server and Storage Server

set -e   # stop on first error
cd "$(dirname "${BASH_SOURCE[0]}")"

CFLAGS="-Wall -Wextra -O2"

echo "==> Compiling Storage Server  (SS1.c → server)"
gcc $CFLAGS ./code/SS1.c           -pthread -I"./helper files" -o server

echo "==> Compiling Naming Server   (Naming_server.c → naming-server)"
gcc $CFLAGS ./code/Naming_server.c -pthread -I"./helper files" -o naming-server

echo "==> Compiling Client          (client.c → client)"
gcc $CFLAGS ./code/client.c                  -I"./helper files" -o client

chmod +x server naming-server client

echo ""
echo "Build complete. All three programs need the same shared secret:"
echo "  export NFS_SECRET='<at least 8 characters, ideally a long random string>'"
echo ""
echo "Launch order:"
echo "  1.  ./naming-server [bind_ip]                              (default 0.0.0.0)"
echo "  2.  ./server <NM_IP> <NM_port> <client_port> [storage_dir] (e.g. ./server 127.0.0.1 5566 5567)"
echo "  3.  ./client [NM_IP] [NM_port]                             (e.g. ./client 127.0.0.1 5568)"
