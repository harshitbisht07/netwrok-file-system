/*
 * Naming_server.c  —  Naming / Coordination Server
 *
 * Usage: ./naming-server [bind_ip [ss_port [client_port]]]
 *        NFS_SECRET must be set (shared secret, min 8 chars).
 *
 * What it does
 *  - Storage servers register (authenticated) with just their client port; the
 *    NM learns their IP from the connection, then pulls their file list over the
 *    network (SS op 7). The NM never reads any storage server's disk, so storage
 *    servers can run on other machines.
 *  - Clients (authenticated) ask where a file lives (ops 1-3) or ask the NM to
 *    create / delete / copy (4-6) or list (7). Create/delete/copy are forwarded to
 *    the right storage server over an authenticated connection.
 *  - The index maps path -> storage-server id (registry slot + 1). Using ids rather
 *    than ports means two machines can both run a storage server on port 5567.
 *
 * Client <-> NM protocol: every message is one length-prefixed frame (netproto.h).
 *   request : op frame, then name frame(s) (copy sends src then dst)
 *   reply   : ops 1-3 -> "ip:port" or an error text ("106: ...", "108: ..." ...)
 *             ops 4-6 -> status text from the storage server
 *             op  7   -> newline-separated listing
 */

#define _GNU_SOURCE
#include "nfs_compat.h"
#include <pthread.h>
#include <signal.h>
#include "trie.h"
#include "convenience.h"
#include "netproto.h"

/* ================================================================== */
/* Configuration                                                      */
/* ================================================================== */
static char bind_ip[INET_ADDRSTRLEN] = "0.0.0.0";
static int  nm_port_ss     = DEFAULT_NM_PORT_SS;      /* 5566 */
static int  nm_port_client = DEFAULT_NM_PORT_CLIENT;  /* 5568 */
static int  g_remote_rw_only = 0;   /* --remote-rw : other PCs get read/write only */
static const char *g_secret;

/* ================================================================== */
/* Storage-server registry                                            */
/* ================================================================== */
#define MAX_SS 10
typedef struct { char ip[INET_ADDRSTRLEN]; int client_port; int active; } SSEntry;

static SSEntry         ss_registry[MAX_SS];
static pthread_mutex_t nfs_mutex = PTHREAD_MUTEX_INITIALIZER;   /* registry + trie + LRU */
static TrieNode       *root;

static SSEntry *ss_by_id(int id)
{
    if (id < 1 || id > MAX_SS || !ss_registry[id - 1].active) return NULL;
    return &ss_registry[id - 1];
}

/* ================================================================== */
/* Logging                                                            */
/* ================================================================== */
static pthread_mutex_t nm_log_mutex = PTHREAD_MUTEX_INITIALIZER;
static void nm_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#include <stdarg.h>
static void nm_log(const char *fmt, ...)
{
    pthread_mutex_lock(&nm_log_mutex);
    FILE *f = fopen("nm_log.txt", "a");
    if (f)
    {
        char ts[32]; time_t t = time(NULL); struct tm tmv; nfs_localtime(&t, &tmv);
        strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tmv);
        fprintf(f, "%s ", ts);
        va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
        fputc('\n', f);
        fclose(f);
    }
    pthread_mutex_unlock(&nm_log_mutex);
}

/* ================================================================== */
/* LRU cache (size 10, circular): name -> ssid                        */
/* ================================================================== */
static int current_LRU_index = 0;
static LRU circularArray[10];
static LRU buffer_array[10];

/* Returns cached ssid if hit (and still registered), 0 on miss. */
static int insertLRU(const char *cmd)
{
    for (int i = 0; i < 10; i++)
    {
        if (circularArray[i].port != 0 && strcmp(circularArray[i].client_command, cmd) == 0)
        {
            int found = circularArray[i].port;
            int copy = current_LRU_index;
            for (int j = 0; j < 10; j++)
            {
                buffer_array[j] = circularArray[copy];
                copy = (copy + 1) % 10;
            }
            memcpy(circularArray, buffer_array, sizeof(circularArray));
            return found;
        }
    }
    return 0;
}

static void insertIntoCircularArray(const char *cmd, int ssid)
{
    for (int i = 0; i < 10; i++)
        if (circularArray[i].port == ssid && strcmp(circularArray[i].client_command, cmd) == 0) return;
    snprintf(circularArray[current_LRU_index].client_command,
             sizeof(circularArray[0].client_command), "%s", cmd);
    circularArray[current_LRU_index].port = ssid;
    current_LRU_index = (current_LRU_index + 1) % 10;
}

/* Deleted files must not stay in the cache (old code kept them forever). */
static void lru_forget(const char *name)
{
    for (int i = 0; i < 10; i++)
        if (strcmp(circularArray[i].client_command, name) == 0)
            { circularArray[i].client_command[0] = '\0'; circularArray[i].port = 0; }
}

/* ================================================================== */
/* Talking to storage servers (always authenticated)                  */
/* ================================================================== */
static int ss_connect(const char *ip, int port)
{
    int s = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &a.sin_addr) != 1) { nfs_sockclose(s); return -1; }
    nfs_tune(s); nfs_set_timeout(s, 10);
    if (connect(s, (struct sockaddr *)&a, sizeof a) < 0 || nfs_auth_connect(s, g_secret) != 0)
        { nfs_sockclose(s); return -1; }
    return s;
}

/* One request/one status reply. Returns 0 on success. */
static int ss_call(const char *ip, int port, int op, const char *a1, const char *a2,
                   char *resp, size_t cap)
{
    int s = ss_connect(ip, port);
    if (s < 0) return -1;
    char opb[8]; snprintf(opb, sizeof opb, "%d", op);
    int bad = send_str(s, opb);
    if (!bad && a1) bad = send_str(s, a1);
    if (!bad && a2) bad = send_str(s, a2);
    if (!bad) bad = recv_str(s, resp, cap);
    nfs_sockclose(s);
    return bad ? -1 : 0;
}

/* Returns malloc'd, NUL-terminated listing text, or NULL. */
static char *fetch_listing(const char *ip, int port)
{
    int s = ss_connect(ip, port);
    if (s < 0) return NULL;
    char *buf = malloc((size_t)NFS_CTRL_MAX + 1);
    uint32_t n = 0;
    if (!buf || send_str(s, "7") || recv_frame(s, buf, NFS_CTRL_MAX, &n)) { free(buf); nfs_sockclose(s); return NULL; }
    nfs_sockclose(s);
    buf[n] = '\0';
    return buf;
}

/* ================================================================== */
/* Index lookup helpers (caller holds nfs_mutex)                      */
/* ================================================================== */
static int index_find(const char *name)        /* name or name/ ; no LRU */
{
    int id = searchTrie(root, name);
    if (!id)
    {
        char d[NFS_MAX_NAME + 8];
        snprintf(d, sizeof d, "%s/", name);
        id = searchTrie(root, d);
    }
    return id;
}

static int ends_with_slash(const char *s) { size_t n = strlen(s); return n && s[n - 1] == '/'; }

/* ================================================================== */
/* Choosing a storage server / handling dead ones                     */
/* ================================================================== */

/* A storage server that stopped answering must not block the whole system.
 * Mark it inactive and drop everything it owned from the index and the cache;
 * its files are indexed again when it re-registers (the NM re-pulls its list). */
static void ss_mark_dead(int id)
{
    if (id < 1 || id > MAX_SS) return;
    pthread_mutex_lock(&nfs_mutex);
    ss_registry[id - 1].active = 0;
    purgeTrieSS(root, id);
    for (int i = 0; i < 10; i++)
        if (circularArray[i].port == id)
            { circularArray[i].client_command[0] = '\0'; circularArray[i].port = 0; }
    pthread_mutex_unlock(&nfs_mutex);
    printf("[NM] Storage server id=%d unreachable - marked inactive\n", id);
    nm_log("SS id=%d marked inactive (unreachable)", id);
}

/* Where should a new path live?  On the storage server that already owns its
 * parent directory, so a file lands next to its directory instead of always on
 * slot 0.  Falls back to the first active server.  `tried` is a bitmask of ids
 * already attempted (bit number == id).  Fills ip/port and returns the id, or 0
 * when no untried active server is left. */
static int pick_target_ss(const char *name, unsigned tried, char *ip, size_t ipsz, int *port)
{
    char parent[NFS_MAX_NAME + 8];
    snprintf(parent, sizeof parent, "%s", name);
    size_t pl = strlen(parent);
    while (pl && parent[pl - 1] == '/') parent[--pl] = '\0';   /* "a/b/" -> "a/b" */
    char *slash = strrchr(parent, '/');
    int id = 0;

    pthread_mutex_lock(&nfs_mutex);
    if (slash)                                                 /* the path has a parent dir */
    {
        slash[1] = '\0';                                       /* "a/b/c" -> "a/b/" */
        int pid = searchTrie(root, parent);
        if (!pid) { slash[0] = '\0'; pid = searchTrie(root, parent); }   /* indexed without the slash */
        if (pid >= 1 && pid <= MAX_SS && ss_registry[pid - 1].active && !(tried & (1u << pid)))
            id = pid;
    }
    if (!id)
        for (int i = 0; i < MAX_SS; i++)
            if (ss_registry[i].active && !(tried & (1u << (i + 1)))) { id = i + 1; break; }
    if (id)
    {
        snprintf(ip, ipsz, "%s", ss_registry[id - 1].ip);
        *port = ss_registry[id - 1].client_port;
    }
    pthread_mutex_unlock(&nfs_mutex);
    return id;
}

/* Ask storage servers to create `name` (SS op 4), preferring the one that owns
 * the parent directory and skipping any that turn out to be dead.
 * Returns the id of the server that answered (its reply is in resp), 0 when no
 * storage server is registered, -1 when every one of them was unreachable. */
static int create_on_some_ss(const char *name, char *resp, size_t cap,
                             char *ip, size_t ipsz, int *port)
{
    unsigned tried = 0;
    int id;
    while ((id = pick_target_ss(name, tried, ip, ipsz, port)) != 0)
    {
        tried |= (1u << id);
        if (ss_call(ip, *port, 4, name, NULL, resp, cap) == 0) return id;
        ss_mark_dead(id);
    }
    return tried ? -1 : 0;
}

/* The storage server confirms a path is really there with [ACK] or 104. */
static int ss_says_present(const char *resp)
{
    return strncmp(resp, "[ACK]", 5) == 0 || strncmp(resp, "104", 3) == 0;
}

/* ================================================================== */
/* Storage-server registration (one thread per registering SS)        */
/* ================================================================== */
typedef struct { int sock; char ip[INET_ADDRSTRLEN]; } PeerArg;

static void *ss_register_thread(void *varg)
{
    PeerArg *a = (PeerArg *)varg;
    int sock = a->sock; char ip[INET_ADDRSTRLEN];
    snprintf(ip, sizeof ip, "%s", a->ip);
    free(a);

    nfs_tune(sock); nfs_set_timeout(sock, 5);
    if (nfs_auth_accept(sock, g_secret) != 0)
    {
        printf("[NM] Rejected storage-server connection from %s (authentication failed)\n", ip);
        nm_log("AUTH rejected SS connection from %s", ip);
        nfs_sockclose(sock); return NULL;
    }
    char portbuf[16];
    if (recv_str(sock, portbuf, sizeof portbuf)) { nfs_sockclose(sock); return NULL; }
    int port = atoi(portbuf);
    if (port <= 0 || port > 65535) { send_str(sock, "400: bad port"); nfs_sockclose(sock); return NULL; }

    pthread_mutex_lock(&nfs_mutex);
    int slot = -1;
    for (int i = 0; i < MAX_SS; i++)                          /* re-registration → same slot */
        if (ss_registry[i].active && ss_registry[i].client_port == port && strcmp(ss_registry[i].ip, ip) == 0)
            { slot = i; break; }
    if (slot < 0)
        for (int i = 0; i < MAX_SS; i++) if (!ss_registry[i].active) { slot = i; break; }
    if (slot < 0)
    {
        pthread_mutex_unlock(&nfs_mutex);
        send_str(sock, "503: registry full");
        printf("[NM] SS registry full — rejecting %s:%d\n", ip, port);
        nfs_sockclose(sock); return NULL;
    }
    snprintf(ss_registry[slot].ip, sizeof ss_registry[slot].ip, "%s", ip);
    ss_registry[slot].client_port = port;
    ss_registry[slot].active = 1;
    int id = slot + 1;
    purgeTrieSS(root, id);                                    /* rebuild this SS's index from scratch */
    for (int i = 0; i < 10; i++)
        if (circularArray[i].port == id) { circularArray[i].client_command[0] = '\0'; circularArray[i].port = 0; }
    pthread_mutex_unlock(&nfs_mutex);

    send_str(sock, "OK");
    nfs_sockclose(sock);

    /* Pull the file list over the network (NOT from the NM's own disk). */
    char *listing = fetch_listing(ip, port);
    int count = 0;
    if (listing)
    {
        pthread_mutex_lock(&nfs_mutex);
        for (char *line = strtok(listing, "\n"); line; line = strtok(NULL, "\n"))
            if (nfs_valid_name(line)) { insertTrie(root, line, id); count++; }
        pthread_mutex_unlock(&nfs_mutex);
        free(listing);
    }
    else printf("[NM] Warning: could not fetch file list from %s:%d\n", ip, port);

    printf("[NM] Storage Server registered  ip=%-15s port=%d  id=%d  (%d entries indexed)\n", ip, port, id, count);
    nm_log("SS registered %s:%d id=%d entries=%d", ip, port, id, count);
    return NULL;
}

/* ================================================================== */
/* Client request handling                                            */
/* ================================================================== */
static void reply_ss_unreachable(int sock) { send_str(sock, "503: Cannot reach storage server"); }

/* ops 1,2,3 — tell the client which storage server to talk to */
static void handle_lookup(int sock, int op, const char *name)
{
    if (!nfs_valid_name(name) || (op == 2 && ends_with_slash(name)))
        { send_str(sock, "108: Invalid file name"); return; }

    pthread_mutex_lock(&nfs_mutex);
    int id = insertLRU(name);
    if (id && !ss_by_id(id)) id = 0;                          /* cached server is gone */
    if (!id) id = searchTrie(root, name);
    if (!id && op != 2) id = index_find(name);
    SSEntry *ss = id ? ss_by_id(id) : NULL;
    char resp[64];
    if (ss) { snprintf(resp, sizeof resp, "%s:%d", ss->ip, ss->client_port); insertIntoCircularArray(name, id); }
    pthread_mutex_unlock(&nfs_mutex);

    if (ss) { send_str(sock, resp); return; }
    if (op != 2) { send_str(sock, "106: File not found on any storage server"); return; }

    /* A write to an unknown path used to be indexed right here, before the file
     * existed.  A write that then failed (missing parent directory, say) left a
     * phantom entry behind: create answered "104 already exists" while read
     * answered 106.  Now the storage server creates the file first and the index
     * is updated only once it confirms the file is there. */
    char nip[INET_ADDRSTRLEN]; int nport = 0; char sresp[256];
    int newid = create_on_some_ss(name, sresp, sizeof sresp, nip, sizeof nip, &nport);
    if (newid == 0) { send_str(sock, "503: No storage server connected"); return; }
    if (newid < 0)  { reply_ss_unreachable(sock); return; }
    if (!ss_says_present(sresp)) { send_str(sock, sresp); return; }   /* e.g. parent dir missing */

    pthread_mutex_lock(&nfs_mutex);
    insertTrie(root, name, newid);
    insertIntoCircularArray(name, newid);
    pthread_mutex_unlock(&nfs_mutex);

    snprintf(resp, sizeof resp, "%s:%d", nip, nport);
    send_str(sock, resp);
    nm_log("WRITE-CREATE %s on %s:%d", name, nip, nport);
}

static void handle_create(int sock, const char *name)
{
    if (!nfs_valid_name(name)) { send_str(sock, "108: Invalid file name"); return; }
    char ip[INET_ADDRSTRLEN]; int port = 0;

    pthread_mutex_lock(&nfs_mutex);
    int exists = index_find(name);
    pthread_mutex_unlock(&nfs_mutex);
    if (exists) { send_str(sock, "104: File already exists!"); return; }

    /* Try the storage server that owns the parent directory first, and keep
     * going past dead ones instead of giving up on slot 0. */
    char resp[256];
    int id = create_on_some_ss(name, resp, sizeof resp, ip, sizeof ip, &port);
    if (id == 0) { send_str(sock, "503: No storage server connected"); return; }
    if (id < 0)  { reply_ss_unreachable(sock); return; }
    if (ss_says_present(resp))
    {
        pthread_mutex_lock(&nfs_mutex); insertTrie(root, name, id); pthread_mutex_unlock(&nfs_mutex);
        nm_log("CREATE %s on %s:%d", name, ip, port);
    }
    send_str(sock, resp);
}

static void handle_delete(int sock, const char *name)
{
    if (!nfs_valid_name(name)) { send_str(sock, "108: Invalid file name"); return; }
    char ip[INET_ADDRSTRLEN]; int port;

    pthread_mutex_lock(&nfs_mutex);
    int id = index_find(name);
    SSEntry *ss = id ? ss_by_id(id) : NULL;
    if (ss) { snprintf(ip, sizeof ip, "%s", ss->ip); port = ss->client_port; }
    pthread_mutex_unlock(&nfs_mutex);
    if (!ss) { send_str(sock, "106: File not found on any storage server"); return; }

    char resp[256];
    if (ss_call(ip, port, 5, name, NULL, resp, sizeof resp)) { ss_mark_dead(id); reply_ss_unreachable(sock); return; }
    if (strncmp(resp, "[ACK]", 5) == 0)
    {
        char d[NFS_MAX_NAME + 8]; snprintf(d, sizeof d, "%s/", name);
        pthread_mutex_lock(&nfs_mutex);
        removeTrie(root, name); removeTrie(root, d);
        lru_forget(name); lru_forget(d);
        pthread_mutex_unlock(&nfs_mutex);
        nm_log("DELETE %s on %s:%d", name, ip, port);
    }
    send_str(sock, resp);
}

static void handle_copy(int sock, const char *src, const char *dst)
{
    if (!nfs_valid_name(src) || !nfs_valid_name(dst) || ends_with_slash(src) || ends_with_slash(dst))
        { send_str(sock, "108: Invalid file name (copy works on files)"); return; }
    char ip[INET_ADDRSTRLEN]; int port, id;

    pthread_mutex_lock(&nfs_mutex);
    id = searchTrie(root, src);
    SSEntry *ss = id ? ss_by_id(id) : NULL;
    int dst_exists = index_find(dst);
    if (ss) { snprintf(ip, sizeof ip, "%s", ss->ip); port = ss->client_port; }
    pthread_mutex_unlock(&nfs_mutex);
    if (!ss) { send_str(sock, "106: Source file not found"); return; }
    if (dst_exists) { send_str(sock, "104: File already exists!"); return; }

    char resp[256];
    if (ss_call(ip, port, 6, src, dst, resp, sizeof resp)) { ss_mark_dead(id); reply_ss_unreachable(sock); return; }
    if (strncmp(resp, "[ACK]", 5) == 0)          /* copy happens on the SS that holds src */
    {
        pthread_mutex_lock(&nfs_mutex); insertTrie(root, dst, id); pthread_mutex_unlock(&nfs_mutex);
        nm_log("COPY %s -> %s on %s:%d", src, dst, ip, port);
    }
    send_str(sock, resp);
}

static void handle_list(int sock)
{
    SSEntry snap[MAX_SS]; int n = 0;
    pthread_mutex_lock(&nfs_mutex);
    for (int i = 0; i < MAX_SS; i++) if (ss_registry[i].active) snap[n++] = ss_registry[i];
    pthread_mutex_unlock(&nfs_mutex);
    if (n == 0) { send_str(sock, "503: No storage server connected"); return; }

    size_t cap = NFS_CTRL_MAX, len = 0;
    char *out = malloc(cap + 1);
    if (!out) { send_str(sock, "500: out of memory"); return; }
    out[0] = '\0';
    for (int i = 0; i < n; i++)
    {
        char *l = fetch_listing(snap[i].ip, snap[i].client_port);
        char head[400];
        if (n > 1) snprintf(head, sizeof head, "== %s:%d ==\n", snap[i].ip, snap[i].client_port); else head[0] = '\0';
        if (!l)
        {
            char e[400]; snprintf(e, sizeof e, "%s(unreachable)\n", head);
            if (len + strlen(e) < cap) { strcpy(out + len, e); len += strlen(e); }
            continue;
        }
        size_t need = strlen(head) + strlen(l);
        if (len + need < cap) { strcpy(out + len, head); strcat(out + len, l); len += need; }
        free(l);
    }
    if (len == 0) send_str(sock, "(no files)\n");
    else send_frame(sock, out, (uint32_t)len);
    free(out);
}

static void *client_thread(void *varg)
{
    PeerArg *a = (PeerArg *)varg;
    int sock = a->sock; char ip[INET_ADDRSTRLEN];
    snprintf(ip, sizeof ip, "%s", a->ip);
    free(a);

    nfs_tune(sock); nfs_set_timeout(sock, 5);
    if (nfs_auth_accept(sock, g_secret) != 0)
    {
        printf("[NM] Rejected client %s (authentication failed)\n", ip);
        nm_log("AUTH rejected client %s", ip);
        nfs_sockclose(sock); return NULL;
    }
    nfs_set_timeout(sock, 0);                     /* sessions may sit idle at the menu */

    /* Access policy: with --remote-rw, a client on another PC may read, write,
     * get info, create and list, but not delete or copy.  A client running on
     * this PC always has full access (it arrives either from 127.0.0.1 or from
     * one of this machine's own addresses). */
    int remote = !nfs_is_local_ip(ip);
    int limited = g_remote_rw_only && remote;
    printf("[NM] Client connected from %s  (%s, %s access)\n", ip,
           remote ? "remote" : "this PC", limited ? "read/write" : "full");
    nm_log("CLIENT connect %s %s %s-access", ip, remote ? "remote" : "local",
           limited ? "rw" : "full");

    char op[16], n1[NFS_MAX_NAME + 8], n2[NFS_MAX_NAME + 8];
    for (;;)
    {
        if (recv_str(sock, op, sizeof op)) break;
        int c = atoi(op);

        if (limited && (c == 5 || c == 6))        /* delete / copy */
        {
            if (recv_str(sock, n1, sizeof n1)) break;
            if (c == 6 && recv_str(sock, n2, sizeof n2)) break;
            send_str(sock, "403: Read/write access only from this PC (delete and copy are not allowed)");
            nm_log("DENIED op=%d from %s (read/write access only)", c, ip);
            continue;
        }

        if (c >= 1 && c <= 5)
        {
            if (recv_str(sock, n1, sizeof n1)) break;
            if      (c <= 3) handle_lookup(sock, c, n1);
            else if (c == 4) handle_create(sock, n1);
            else             handle_delete(sock, n1);
        }
        else if (c == 6)
        {
            if (recv_str(sock, n1, sizeof n1) || recv_str(sock, n2, sizeof n2)) break;
            handle_copy(sock, n1, n2);
        }
        else if (c == 7) handle_list(sock);
        else send_str(sock, "400: Unknown command");
    }
    nfs_sockclose(sock);
    printf("[NM] Client %s disconnected\n", ip);
    return NULL;
}

/* ================================================================== */
/* Listeners                                                          */
/* ================================================================== */
static int make_listening_socket(int port, const char *ip)
{
    int s = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { fprintf(stderr, "[NM] socket: %s\n", nfs_sockerrstr()); exit(1); }
    nfs_reuseaddr(s);
    struct sockaddr_in addr; memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET; addr.sin_port = htons(port);
    if (strcmp(ip, "0.0.0.0") == 0) addr.sin_addr.s_addr = INADDR_ANY;
    else if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) { fprintf(stderr, "[NM] Bad bind IP: %s\n", ip); exit(1); }
    if (bind(s, (struct sockaddr *)&addr, sizeof addr) < 0)
        { fprintf(stderr, "[NM] bind port %d: %s\n", port, nfs_sockerrstr()); exit(1); }
    if (listen(s, 32) < 0) { fprintf(stderr, "[NM] listen: %s\n", nfs_sockerrstr()); exit(1); }
    return s;
}

typedef struct { int port; void *(*fn)(void *); const char *label; } ListenCfg;

static void *listener(void *varg)
{
    ListenCfg *cfg = (ListenCfg *)varg;
    int ls = make_listening_socket(cfg->port, bind_ip);
    printf("[NM] %-28s: %d\n", cfg->label, cfg->port);
    for (;;)
    {
        struct sockaddr_in ca; nfs_socklen_t len = sizeof ca;
        int cs = (int)accept(ls, (struct sockaddr *)&ca, &len);
        if (cs < 0) { if (!nfs_sock_eintr()) fprintf(stderr, "[NM] accept: %s\n", nfs_sockerrstr()); continue; }
        PeerArg *a = malloc(sizeof *a);
        if (!a) { nfs_sockclose(cs); continue; }
        a->sock = cs;
        inet_ntop(AF_INET, &ca.sin_addr, a->ip, sizeof a->ip);
        pthread_t t;
        if (pthread_create(&t, NULL, cfg->fn, a) != 0) { free(a); nfs_sockclose(cs); }
        else pthread_detach(t);
    }
    return NULL;
}

int main(int argc, char *argv[])
{
    nfs_net_init();
#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif
    g_secret = nfs_load_secret(0);
    if (!g_secret) return 1;

    memset(ss_registry, 0, sizeof ss_registry);
    memset(circularArray, 0, sizeof circularArray);
    root = createNode();

    /* Positional args (bind ip, ss port, client port) with --flags anywhere. */
    int pos = 0;
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--remote-rw") == 0)   { g_remote_rw_only = 1; continue; }
        if (strcmp(argv[i], "--full-access") == 0) { g_remote_rw_only = 0; continue; }
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
        {
            printf("Usage: naming-server [bind_ip [ss_port [client_port]]] [--remote-rw]\n"
                   "  --remote-rw   clients on OTHER PCs may read, write, get info, create\n"
                   "                and list, but not delete or copy.  A client on this PC\n"
                   "                always keeps full access.\n"
                   "  NFS_SECRET must be set (shared secret, min %d characters).\n", NFS_MIN_SECRET);
            return 0;
        }
        switch (++pos)
        {
            case 1: snprintf(bind_ip, sizeof bind_ip, "%s", argv[i]); break;
            case 2: nm_port_ss     = atoi(argv[i]); break;
            case 3: nm_port_client = atoi(argv[i]); break;
            default: fprintf(stderr, "[NM] Ignoring extra argument: %s\n", argv[i]); break;
        }
    }
    if (!g_remote_rw_only)
    {
        const char *e = getenv("NFS_REMOTE_RW_ONLY");
        if (e && (*e == '1' || *e == 'y' || *e == 'Y' || *e == 't' || *e == 'T')) g_remote_rw_only = 1;
    }
    if (nm_port_ss <= 0 || nm_port_ss > 65535 || nm_port_client <= 0 || nm_port_client > 65535)
        { fprintf(stderr, "[NM] Invalid port\n"); return 1; }
    if (nm_port_ss == nm_port_client)
        { fprintf(stderr, "[NM] The SS port and the client port must differ\n"); return 1; }

    printf("╔══════════════════════════════════╗\n");
    printf("║       Naming Server (NM)         ║\n");
    printf("╠══════════════════════════════════╣\n");
    printf("║ Bind IP     : %s\n", bind_ip);
    printf("║ SS port     : %d\n", nm_port_ss);
    printf("║ Client port : %d\n", nm_port_client);
    printf("║ Auth        : shared secret required\n");
    printf("║ Other PCs   : %s\n", g_remote_rw_only
           ? "read / write only (no delete, no copy)" : "full access");
    printf("╚══════════════════════════════════╝\n\n");

    static ListenCfg ss_cfg, cl_cfg;
    ss_cfg = (ListenCfg){ nm_port_ss,     ss_register_thread, "SS registration port"  };
    cl_cfg = (ListenCfg){ nm_port_client, client_thread,      "Client connection port" };
    pthread_t t1, t2;
    if (pthread_create(&t1, NULL, listener, &ss_cfg) != 0) { perror("ss listener");     return 1; }
    if (pthread_create(&t2, NULL, listener, &cl_cfg) != 0) { perror("client listener"); return 1; }
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    return 0;
}
