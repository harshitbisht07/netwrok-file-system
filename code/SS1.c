/*
 * SS1.c  —  Storage Server
 *
 * Usage: ./server <NM_IP> <NM_port> <client_port> [storage_dir]
 *        (storage_dir defaults to ./storage; it is created if missing)
 *        NFS_SECRET must be set (shared secret, min 8 chars).
 *
 * Security model
 *  - Every connection (clients AND the naming server) must pass the
 *    shared-secret handshake in netproto.h before any command is read.
 *  - Files live ONLY under storage_dir. Every name is validated and resolved
 *    with realpath(); absolute paths, "..", and symlinks that leave the
 *    storage root are refused here, regardless of what the caller checked.
 *  - Logs go to ./ss_log.txt, outside the storage dir, so they are never served.
 *
 * Wire protocol (all frames are length-prefixed, see netproto.h), after auth:
 *   op frame ("1".."7"), then:
 *   1 READ   : name           -> "OK" + data frames + empty frame   | error text
 *   2 WRITE  : name + data frames + empty frame (append)  -> status
 *   3 INFO   : name           -> info text | error
 *   4 CREATE : name ('/' suffix = directory)               -> status
 *   5 DELETE : name           -> status
 *   6 COPY   : src, dst       -> status
 *   7 LIST   : (none)         -> one frame, newline-separated names
 */

#define _GNU_SOURCE
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
#include <pthread.h>
#include <dirent.h>
#include <signal.h>
#include <limits.h>
#include <time.h>
#include "netproto.h"

static char        g_root[PATH_MAX];          /* realpath of the storage dir */
static char        g_nm_ip[INET_ADDRSTRLEN];
static int         g_nm_port;
static int         g_client_port;
static const char *g_secret;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ------------------------------------------------------------------ */
static void log_event(const char *peer, const char *op, const char *detail)
{
    pthread_mutex_lock(&log_mutex);
    FILE *f = fopen("ss_log.txt", "a");
    if (f)
    {
        char ts[32]; time_t t = time(NULL); struct tm tmv;
        localtime_r(&t, &tmv);
        strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tmv);
        fprintf(f, "%s %-15s %-6s %s\n", ts, peer, op, detail);
        fclose(f);
    }
    pthread_mutex_unlock(&log_mutex);
}

/* Validate `name` and map it to an absolute path inside g_root.
 * *isdir is set when the name ends with '/'. Returns 0 ok, -1 refused. */
static int resolve_path(const char *name, char *out, size_t outsz, int *isdir)
{
    if (!nfs_valid_name(name)) return -1;
    size_t n = strlen(name);
    *isdir = (name[n - 1] == '/');

    char rel[NFS_MAX_NAME + 8];
    memcpy(rel, name, n + 1);
    if (*isdir) rel[n - 1] = '\0';

    char full[PATH_MAX + NFS_MAX_NAME + 8];
    snprintf(full, sizeof full, "%s/%s", g_root, rel);
    char *slash = strrchr(full, '/');
    char leaf[NFS_MAX_NAME + 8];
    snprintf(leaf, sizeof leaf, "%s", slash + 1);
    *slash = '\0';                                  /* full = parent directory */

    char rp[PATH_MAX];
    if (!realpath(full, rp)) return -1;             /* parent must exist */
    size_t rl = strlen(g_root);
    if (strncmp(rp, g_root, rl) != 0 || (rp[rl] != '\0' && rp[rl] != '/')) return -1;

    snprintf(out, outsz, "%s/%s", rp, leaf);
    struct stat st;
    if (lstat(out, &st) == 0 && S_ISLNK(st.st_mode)) return -1;
    return 0;
}

static int get_name(int sock, char *name, size_t cap) { return recv_str(sock, name, cap); }

/* resolve or tell the client why not */
static int resolve_or_reply(int sock, const char *name, char *path, size_t psz, int *isdir)
{
    if (resolve_path(name, path, psz, isdir) == 0) return 0;
    send_str(sock, "108: Invalid file name or path");
    return -1;
}

/* ------------------------------------------------------------------ */
/* Operations                                                          */
/* ------------------------------------------------------------------ */
static void op_read(int sock, const char *peer)
{
    char name[NFS_MAX_NAME + 8], path[PATH_MAX + NFS_MAX_NAME + 8]; int isdir;
    if (get_name(sock, name, sizeof name)) return;
    if (resolve_or_reply(sock, name, path, sizeof path, &isdir)) return;

    struct stat st;
    if (isdir || stat(path, &st) != 0 || !S_ISREG(st.st_mode))
    {
        send_str(sock, errno == ENOENT ? "106: File not found" : "101: Error opening file for reading");
        return;
    }
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) { send_str(sock, "101: Error opening file for reading"); return; }

    if (send_str(sock, "OK") == 0)
    {
        char buf[NFS_CHUNK];
        for (;;)
        {
            ssize_t n = read(fd, buf, sizeof buf);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            if (send_frame(sock, buf, (uint32_t)n)) { close(fd); return; }
        }
        send_frame(sock, "", 0);
    }
    close(fd);
    log_event(peer, "READ", name);
}

static void op_write(int sock, const char *peer)
{
    char name[NFS_MAX_NAME + 8], path[PATH_MAX + NFS_MAX_NAME + 8]; int isdir = 0;
    if (get_name(sock, name, sizeof name)) return;

    /* Decide the outcome first but ALWAYS drain the client's data frames
     * before answering, otherwise an early reply can be lost to a TCP reset. */
    const char *err = NULL; int fd = -1;
    if (resolve_path(name, path, sizeof path, &isdir) != 0 || isdir) err = "108: Invalid file name or path";
    else
    {
        fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW, 0644);
        if (fd < 0) err = "101: Error opening file for writing";
    }

    char buf[NFS_CHUNK]; size_t total = 0; int toobig = 0, werr = 0;
    for (;;)
    {
        uint32_t n;
        if (recv_frame(sock, buf, sizeof buf, &n)) { if (fd >= 0) close(fd); return; }
        if (n == 0) break;
        if (fd >= 0 && !toobig && !werr)
        {
            if (total + n > NFS_MAX_WRITE) toobig = 1;
            else if (fd_write_all(fd, buf, n)) werr = 1;
            else total += n;
        }
    }
    if (fd >= 0) close(fd);

    if (!err && toobig) err = "109: Write too large (limit 16 MB per request)";
    if (!err && werr)   err = "101: Error writing file";
    send_str(sock, err ? err : "[ACK] File write successful");
    if (!err) log_event(peer, "WRITE", name);
}

static void op_info(int sock, const char *peer)
{
    char name[NFS_MAX_NAME + 8], path[PATH_MAX + NFS_MAX_NAME + 8]; int isdir;
    if (get_name(sock, name, sizeof name)) return;
    if (resolve_or_reply(sock, name, path, sizeof path, &isdir)) return;

    struct stat st;
    if (stat(path, &st) != 0) { send_str(sock, errno == ENOENT ? "106: File not found" : "105: Error getting file information"); return; }
    char ts[32]; struct tm tmv; localtime_r(&st.st_mtime, &tmv);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tmv);
    char info[2048];
    snprintf(info, sizeof info, "Name: %s | Type: %s | Size: %lld bytes | Permissions: %o | Modified: %s",
             name, S_ISDIR(st.st_mode) ? "directory" : "file",
             (long long)st.st_size, st.st_mode & 0777, ts);
    send_str(sock, info);
    log_event(peer, "INFO", name);
}

static void op_create(int sock, const char *peer)
{
    char name[NFS_MAX_NAME + 8], path[PATH_MAX + NFS_MAX_NAME + 8]; int isdir;
    if (get_name(sock, name, sizeof name)) return;
    if (resolve_or_reply(sock, name, path, sizeof path, &isdir)) return;

    if (isdir)
    {
        if (mkdir(path, 0755) == 0) { send_str(sock, "[ACK] Directory created successfully!"); log_event(peer, "MKDIR", name); }
        else send_str(sock, errno == EEXIST ? "104: File already exists!" : "102: Error creating directory!");
        return;
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
    if (fd >= 0) { close(fd); send_str(sock, "[ACK] File creation successful!"); log_event(peer, "CREATE", name); }
    else send_str(sock, errno == EEXIST ? "104: File already exists!" : "102: Error creating file!");
}

static void op_delete(int sock, const char *peer)
{
    char name[NFS_MAX_NAME + 8], path[PATH_MAX + NFS_MAX_NAME + 8]; int isdir;
    if (get_name(sock, name, sizeof name)) return;
    if (resolve_or_reply(sock, name, path, sizeof path, &isdir)) return;

    if (remove(path) == 0) { send_str(sock, "[ACK] File deletion successful!"); log_event(peer, "DELETE", name); }
    else send_str(sock, errno == ENOENT ? "106: File not found" : "103: Error deleting file!");
}

static void op_copy(int sock, const char *peer)
{
    char src[NFS_MAX_NAME + 8], dst[NFS_MAX_NAME + 8];
    char psrc[PATH_MAX + NFS_MAX_NAME + 8], pdst[PATH_MAX + NFS_MAX_NAME + 8];
    int d1, d2;
    if (get_name(sock, src, sizeof src) || get_name(sock, dst, sizeof dst)) return;
    if (resolve_or_reply(sock, src, psrc, sizeof psrc, &d1)) return;
    if (resolve_or_reply(sock, dst, pdst, sizeof pdst, &d2)) return;
    if (d1 || d2) { send_str(sock, "108: Copy works on files, not directories"); return; }

    struct stat st;
    if (stat(psrc, &st) != 0 || !S_ISREG(st.st_mode)) { send_str(sock, "106: Source file not found"); return; }
    int in = open(psrc, O_RDONLY | O_NOFOLLOW);
    if (in < 0) { send_str(sock, "101: Error opening source file!"); return; }
    /* O_EXCL: never overwrite (the old code opened dst with "w", so copying a
     * file onto itself truncated it to zero bytes). */
    int out = open(pdst, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
    if (out < 0)
    {
        close(in);
        send_str(sock, errno == EEXIST ? "104: File already exists!" : "102: Error creating destination!");
        return;
    }
    char buf[NFS_CHUNK]; int bad = 0;
    for (;;)
    {
        ssize_t n = read(in, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { bad = 1; break; }
        if (n == 0) break;
        if (fd_write_all(out, buf, (size_t)n)) { bad = 1; break; }
    }
    close(in); close(out);
    if (bad) { unlink(pdst); send_str(sock, "102: Error copying file!"); return; }
    send_str(sock, "[ACK] File copy successful!");
    char d[2 * (NFS_MAX_NAME + 8) + 8]; snprintf(d, sizeof d, "%s -> %s", src, dst);
    log_event(peer, "COPY", d);
}

/* ---- LIST: every regular file and directory (dirs end with '/') ---- */
#define LIST_MAX_ENTRIES 20000
typedef struct { char **v; size_t n; size_t bytes; } NameList;

static void list_add(NameList *l, const char *s)
{
    if (l->n >= LIST_MAX_ENTRIES || l->bytes + strlen(s) + 2 > NFS_CTRL_MAX - 4096) return;
    char *c = strdup(s);
    if (!c) return;
    char **nv = realloc(l->v, (l->n + 1) * sizeof(char *));
    if (!nv) { free(c); return; }
    l->v = nv; l->v[l->n++] = c; l->bytes += strlen(s) + 1;
}

static void walk(const char *abs, const char *rel, int depth, NameList *l)
{
    if (depth > 16) return;
    DIR *d = opendir(abs);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
    {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char a[PATH_MAX + 300], r[NFS_MAX_NAME + 300];
        snprintf(a, sizeof a, "%s/%s", abs, e->d_name);
        snprintf(r, sizeof r, "%s%s", rel, e->d_name);
        if (strlen(r) > NFS_MAX_NAME - 1) continue;
        struct stat st;
        if (lstat(a, &st) != 0) continue;
        if (S_ISDIR(st.st_mode))
        {
            strcat(r, "/");
            if (nfs_valid_name(r)) list_add(l, r);
            walk(a, r, depth + 1, l);
        }
        else if (S_ISREG(st.st_mode) && nfs_valid_name(r)) list_add(l, r);
    }
    closedir(d);
}

static int cmp_names(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void op_list(int sock, const char *peer)
{
    NameList l = {0};
    walk(g_root, "", 0, &l);
    qsort(l.v, l.n, sizeof(char *), cmp_names);
    char *out = malloc(l.bytes + 1);
    if (!out) { send_str(sock, ""); goto cleanup; }
    size_t pos = 0;
    for (size_t i = 0; i < l.n; i++) { size_t k = strlen(l.v[i]); memcpy(out + pos, l.v[i], k); pos += k; out[pos++] = '\n'; }
    send_frame(sock, out, (uint32_t)pos);
    free(out);
cleanup:
    for (size_t i = 0; i < l.n; i++) free(l.v[i]);
    free(l.v);
    log_event(peer, "LIST", "");
}

/* ------------------------------------------------------------------ */
typedef struct { int sock; char ip[INET_ADDRSTRLEN]; } ClientArg;

static void *handle_client(void *varg)
{
    ClientArg *arg = (ClientArg *)varg;
    int sock = arg->sock;
    char peer[INET_ADDRSTRLEN];
    snprintf(peer, sizeof peer, "%s", arg->ip);
    free(arg);

    nfs_tune(sock);
    nfs_set_timeout(sock, 5);                       /* handshake must be quick */
    if (nfs_auth_accept(sock, g_secret) != 0)
    {
        printf("[SS] Rejected connection from %s (authentication failed)\n", peer);
        log_event(peer, "AUTH", "rejected");
        goto done;
    }
    nfs_set_timeout(sock, 30);

    char op[16];
    if (recv_str(sock, op, sizeof op)) goto done;
    switch (atoi(op))
    {
        case 1: op_read(sock, peer);   break;
        case 2: op_write(sock, peer);  break;
        case 3: op_info(sock, peer);   break;
        case 4: op_create(sock, peer); break;
        case 5: op_delete(sock, peer); break;
        case 6: op_copy(sock, peer);   break;
        case 7: op_list(sock, peer);   break;
        default: send_str(sock, "400: Unknown command");
    }
done:
    close(sock);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Registration with the Naming Server                                 */
/* Sends only our client port; NM learns our IP from the connection    */
/* and then pulls our file list with LIST, so it never needs to read   */
/* anything from its own disk.                                         */
/* ------------------------------------------------------------------ */
static int register_with_nm(void)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(g_nm_port);
    if (inet_pton(AF_INET, g_nm_ip, &a.sin_addr) != 1) { close(s); return -2; }
    nfs_tune(s); nfs_set_timeout(s, 10);
    if (connect(s, (struct sockaddr *)&a, sizeof a) < 0) { close(s); return -1; }
    if (nfs_auth_connect(s, g_secret) != 0) { close(s); return -3; }
    char port[16], resp[128];
    snprintf(port, sizeof port, "%d", g_client_port);
    if (send_str(s, port) || recv_str(s, resp, sizeof resp)) { close(s); return -1; }
    close(s);
    if (strcmp(resp, "OK") != 0) { fprintf(stderr, "[SS] NM refused registration: %s\n", resp); return -4; }
    return 0;
}

static void *nm_reg_thread(void *arg)
{
    (void)arg;
    for (int attempt = 1;; attempt++)
    {
        int r = register_with_nm();
        if (r == 0)
        {
            printf("[SS] Registered with Naming Server at %s:%d\n", g_nm_ip, g_nm_port);
            return NULL;
        }
        if (r == -3) fprintf(stderr, "[SS] Naming Server rejected our secret — check NFS_SECRET\n");
        else if (r == -2) { fprintf(stderr, "[SS] Bad NM address: %s\n", g_nm_ip); return NULL; }
        else if (attempt == 1 || attempt % 10 == 0)
            fprintf(stderr, "[SS] Naming Server %s:%d not reachable yet — retrying...\n", g_nm_ip, g_nm_port);
        sleep(3);
    }
}

/* ------------------------------------------------------------------ */
int main(int argc, char *argv[])
{
    if (argc < 4)
    {
        fprintf(stderr, "Usage: ./server <NM_IP> <NM_port> <client_port> [storage_dir]\n"
                        "  Needs the shared secret in the NFS_SECRET environment variable.\n"
                        "Example: NFS_SECRET=... ./server 192.168.1.10 5566 5567 ./storage\n");
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);

    g_secret = nfs_load_secret(0);
    if (!g_secret) return 1;

    snprintf(g_nm_ip, sizeof g_nm_ip, "%s", argv[1]);
    g_nm_port     = atoi(argv[2]);
    g_client_port = atoi(argv[3]);
    if (g_nm_port <= 0 || g_nm_port > 65535 || g_client_port <= 0 || g_client_port > 65535)
        { fprintf(stderr, "[SS] Invalid port\n"); return 1; }

    const char *dir = (argc >= 5) ? argv[4] : "storage";
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) { perror("[SS] mkdir storage dir"); return 1; }
    if (!realpath(dir, g_root)) { perror("[SS] storage dir"); return 1; }
    struct stat st;
    if (stat(g_root, &st) != 0 || !S_ISDIR(st.st_mode)) { fprintf(stderr, "[SS] %s is not a directory\n", g_root); return 1; }

    printf("╔══════════════════════════════════╗\n");
    printf("║       Storage Server (SS)        ║\n");
    printf("╠══════════════════════════════════╣\n");
    printf("║ NM address  : %s:%d\n", g_nm_ip, g_nm_port);
    printf("║ Client port : %d\n", g_client_port);
    printf("║ Storage dir : %s\n", g_root);
    printf("╚══════════════════════════════════╝\n\n");

    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) { perror("[SS] socket"); return 1; }
    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
    struct sockaddr_in addr; memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET; addr.sin_port = htons(g_client_port); addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(server_sock, (struct sockaddr *)&addr, sizeof addr) < 0) { perror("[SS] bind"); return 1; }
    if (listen(server_sock, 32) < 0) { perror("[SS] listen"); return 1; }
    printf("[SS] Listening on port %d (authentication required)\n", g_client_port);

    /* Only register once we are really listening: NM connects back for LIST. */
    pthread_t reg_t;
    pthread_create(&reg_t, NULL, nm_reg_thread, NULL);
    pthread_detach(reg_t);

    for (;;)
    {
        struct sockaddr_in ca; socklen_t len = sizeof ca;
        int cs = accept(server_sock, (struct sockaddr *)&ca, &len);
        if (cs < 0) { if (errno != EINTR) perror("[SS] accept"); continue; }
        ClientArg *a = malloc(sizeof *a);
        if (!a) { close(cs); continue; }
        a->sock = cs;
        inet_ntop(AF_INET, &ca.sin_addr, a->ip, sizeof a->ip);
        pthread_t t;
        if (pthread_create(&t, NULL, handle_client, a) != 0) { free(a); close(cs); }
        else pthread_detach(t);
    }
}
