# Quick Start (Windows, two PCs on one LAN)

The layout this is set up for:

| | PC 1 — "primary" | PC 2 — over the LAN |
|---|---|---|
| runs | naming server + storage server + client | client only |
| files live | here, in `.\storage` | nowhere (it is just a client) |
| access | full (read, write, create, delete, copy) | read / write / create / list — **no delete, no copy** |

## 0. Build (PC 1 only, once)

```powershell
cd <this folder>
.\build.ps1
```

Needs MinGW-w64 gcc on PATH. If you don't have it:

```powershell
winget install MSYS2.MSYS2
C:\msys64\usr\bin\bash -lc "pacman -S --noconfirm mingw-w64-ucrt-x86_64-gcc"
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
```

You get three standalone `.exe` files — `naming-server.exe`, `server.exe`, `client.exe`.
They are statically linked: nothing else has to be installed to run them.

## 1. PC 1 — start everything

Run PowerShell **as Administrator** the first time, so the firewall rules get added:

```powershell
.\launch_all.ps1 -OpenFirewall
```

It prints this PC's LAN IP and a random shared secret, then opens three windows
(naming server, storage server, client). Note the IP and the secret — PC 2 needs both.

To reuse a fixed secret instead of a random one:

```powershell
.\launch_all.ps1 -Secret 'some-long-random-string'
```

## 2. PC 2 — client only

Copy just two files over (a USB stick or a network share is fine): `client.exe`
and `start_client.ps1`. Then:

```powershell
.\start_client.ps1 -ServerIp 192.168.1.20 -Secret 'the-secret-PC1-printed'
```

Use the IP and secret that PC 1 printed. That's all PC 2 needs — no build, no WSL,
no storage folder.

Doing it by hand works too:

```powershell
$env:NFS_SECRET = 'the-secret-PC1-printed'
.\client.exe 192.168.1.20 5568
```

## 3. Try it

In the client menu: `4` `notes.txt` → `2` `notes.txt` `hello from PC2` → `1` `notes.txt` → `7`.

The file appears in `.\storage\` on PC 1, and the client on PC 1 sees it too.
On PC 2, `5` (delete) and `6` (copy) answer `403: Read/write access only from this PC`.

## Menu

```
1 Read   2 Write/Append   3 Info   4 Create (add / for a directory)
5 Delete 6 Copy           7 List   0 Exit
```

## Ports

| Port | Who listens | Who connects |
|------|-------------|--------------|
| 5566 | naming server | storage servers (registration) |
| 5568 | naming server | clients |
| 5567 | storage server | clients **and** the naming server |

Clients talk to the storage server directly for file data, so PC 2 needs to reach
**5567 and 5568** on PC 1. `-OpenFirewall` opens all three.

## Access control

The naming server decides this, not the client, so it cannot be bypassed by
editing the client:

- `.\naming-server.exe 0.0.0.0 5566 5568 --remote-rw` — other PCs may read, write,
  get info, create and list; delete and copy are refused with `403`.
  (`launch_all.ps1` uses this by default.)
- `--full-access` (or `.\launch_all.ps1 -RemoteFullAccess`) — every client may do everything.

A client started **on PC 1** always has full access, whether it connects to
`127.0.0.1` or to PC 1's own LAN IP.

Everyone who knows the shared secret can connect; there are no per-user accounts.

## Running it by hand on PC 1

Three terminals, same secret in each:

```powershell
$env:NFS_SECRET = 'some-long-random-string'
.\naming-server.exe 0.0.0.0 5566 5568 --remote-rw
```

```powershell
$env:NFS_SECRET = 'some-long-random-string'
.\server.exe <PC1-LAN-IP> 5566 5567 .\storage
```

```powershell
$env:NFS_SECRET = 'some-long-random-string'
.\client.exe <PC1-LAN-IP> 5568
```

Give the storage server PC 1's **LAN IP**, not `127.0.0.1`: the naming server hands
clients the address it saw the storage server connect from, and PC 2 cannot use
`127.0.0.1`.

## Troubleshooting

| What you see | Fix |
|---|---|
| `Authentication with naming server failed` | `NFS_SECRET` differs between the two PCs |
| `Cannot connect to naming server` | naming server not started, wrong IP, or the firewall is closed — run `.\launch_all.ps1 -OpenFirewall` as Administrator on PC 1 |
| `503: Cannot reach storage server` | PC 2 can reach 5568 but not 5567; open TCP 5567 on PC 1 |
| `bind port 5568: ... normally permitted` | something is already running on that port — close the old windows |
| Client lists nothing after you add files into `.\storage` by hand | restart the storage server so it re-registers and re-indexes |
| `403: Read/write access only from this PC` | expected on PC 2; use `-RemoteFullAccess` on PC 1 to lift it |

Logs: `nm_log.txt` (naming server) and `ss_log.txt` (storage server), next to the `.exe` files.

## Linux / WSL

Unchanged: `./bash.sh` builds `naming-server`, `server` and `client`, and
`./launch_all.sh` runs all three. The same binaries interoperate with the Windows
ones over the network.
