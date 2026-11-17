# Network File System (NFS)

A small distributed file system written in C with TCP sockets. Clients create, read, write, copy,
delete and list files that live on one or more storage servers, coordinated by a naming server.

## Build and run

    ./bash.sh                                   # builds: server, naming-server, client
    export NFS_SECRET='some-long-random-string' # the SAME value on every machine, min 8 chars

    ./naming-server                             # 1. Naming Server (ports 5566 for SS, 5568 for clients)
    ./server 127.0.0.1 5566 5567                # 2. Storage Server (files go in ./storage)
    ./client 127.0.0.1 5568                     # 3. Client

Or run `./launch_all.sh` (Linux/WSL) / `launch_all.ps1` (Windows, via WSL) to open all three.
See `QUICKSTART.md` for running across several PCs.

Dependencies: a C compiler and POSIX/Linux libc (no third-party libraries).

## Components

1. **Client** - interactive menu: read, write/append, get info, create (file or directory), delete, copy, list.
2. **Naming Server (NM)** - keeps an index (trie + LRU cache) of which storage server holds which path, hands
   out storage-server addresses, and forwards create/delete/copy.
3. **Storage Server (SS)** - stores the files inside one dedicated directory (`./storage` by default, or the
   4th argument). Its logs (`ss_log.txt`) are kept outside that directory. The NM learns an SS's IP from the
   connection and fetches its file list over the network, so storage servers can run on other machines.

## Security

- **Authentication:** every connection (client->NM, SS->NM, NM->SS, client->SS) must pass a mutual
  challenge-response using the shared secret (`NFS_SECRET`, HMAC-SHA256, fresh nonces per connection). The
  secret itself is never sent. A wrong or missing secret is refused before any command is read.
- **Path safety:** the storage server validates every name itself and resolves it with `realpath()`.
  Absolute paths, `..`, backslashes, control characters and symlinks that leave the storage directory are refused.
- **NOT encrypted:** traffic (file contents, names) travels in clear text. Anyone who can sniff the
  network can read it, and an active attacker can tamper with it after the handshake. On an untrusted
  network, run it over a VPN (e.g. WireGuard) or an SSH tunnel. Use a long random secret: a weak one can be
  guessed offline from a captured handshake.
- Everyone who knows the secret has full access; there are no per-user permissions.

## File names

Any normal name works: upper-case, dots, digits, spaces, sub-folders (`reports/Q3.v2.TXT`).
End a name with `/` to create a directory (parent folders must already exist).
Rejected (error 108): empty names, names starting with `/`, `.` or `..` parts, empty parts (`a//b`), backslashes,
control characters, names over 1000 characters. Names are case-sensitive.

## Behaviour notes

1. Write **appends** to the file (creating it if it does not exist). A single write is one line of up to 4096
   characters, spaces allowed. Nothing is added between appends, so add your own line break by writing
   separate lines of text as needed.
2. Concurrent appends from several clients are all applied; there is no read/write locking.
3. Create/delete/copy are forwarded to the owning storage server. Create places new files on the first
   registered storage server. Copy stays on the server that holds the source, works on files (not directories),
   and refuses to overwrite an existing destination (error 104).
4. Delete works on files and empty directories.
5. Names are relative to the storage directory; give copy source and destination the same way as any other name
   (`a.txt`, `docs/b.txt`) - not absolute paths.
6. The NM indexes a storage server's files when it registers. A file added directly on disk while it runs is
   found after the storage server is restarted (it re-registers and the index is rebuilt). A storage server
   retries registration until the NM is reachable. If the NM restarts, restart the storage servers too.
7. The LRU cache (10 entries) is cleared of a file when it is deleted.
8. If two storage servers hold the same path, the most recently indexed one wins.

## Not implemented

- Replication / fault tolerance across storage servers (the original README listed it; there is no code for it).
- Reader/writer exclusion between clients.
- Encryption (see Security).
- Failure detection: a storage server that dies stays in the NM's registry until the NM restarts.

## Error codes

    101  error opening/writing file          106  file not found
    102  error creating file/directory       108  invalid file name
    103  error deleting (e.g. dir not empty)  109  write too large (16 MB per request)
    104  file already exists                  400  unknown command
    105  error getting file info              503  storage server unavailable
