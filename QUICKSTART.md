# Quick Start

## One machine

    ./bash.sh
    ./launch_all.sh            # Linux/WSL: opens 3 terminals and prints a random shared secret
    # Windows PowerShell:  .\launch_all.ps1   (runs the Linux binaries through WSL)

Or by hand, in three terminals (use the same secret in each):

    export NFS_SECRET='some-long-random-string'
    ./naming-server
    ./server 127.0.0.1 5566 5567        # stores files in ./storage
    ./client 127.0.0.1 5568

If `NFS_SECRET` is not set, the client asks for it; the servers refuse to start without it.

## Several PCs on the same network

| PC | Command |
|----|---------|
| A (naming server) | `./naming-server 0.0.0.0` |
| B (storage; can be A itself) | `./server <A_LAN_IP> 5566 5567 ./storage` |
| any other PC | `./client <A_LAN_IP> 5568` |

- Set the same `NFS_SECRET` on every machine.
- Use A's **LAN IP** (e.g. 192.168.1.20), never `127.0.0.1`, even when the storage server runs on A:
  the NM hands out the address it saw the storage server connect from, and remote clients cannot use 127.0.0.1.
- Firewall: A must accept TCP 5566 and 5568; each storage server must accept TCP 5567 (or the port you chose)
  from the clients **and** from the NM, because clients connect to storage servers directly.
- Several storage servers: give each its own folder and register them all with the same NM
  (`./server <A_LAN_IP> 5566 5567 ./storage` on each machine, or different ports on one machine).
- Traffic is authenticated but not encrypted; on an untrusted network use a VPN or SSH tunnel.

## Menu

    1 Read   2 Write/Append   3 Info   4 Create (add / for a directory)
    5 Delete 6 Copy           7 List   0 Exit

Quick test: `4` `notes.txt` -> `2` `notes.txt` `hello world` -> `1` `notes.txt` -> `3` `notes.txt` -> `7` -> `0`.

## Ports

- 5566 NM <- storage servers register
- 5568 NM <- clients
- 5567 SS <- clients and the NM (default; change with the 3rd argument)

## Troubleshooting

- *Authentication failed (wrong shared secret?)* - `NFS_SECRET` differs between machines.
- *Cannot connect* - start the naming server first; check the IP and firewall.
- *Port already in use* - `pkill naming-server; pkill -f '^./server'`.
- *Client lists nothing / file not found after adding files by hand* - restart the storage server so it re-registers.
- Logs: `nm_log.txt` (naming server) and `ss_log.txt` (storage server) in each program's working directory.
