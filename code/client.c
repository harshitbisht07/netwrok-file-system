/*
 * client.c  —  DFS Client
 *
 * Usage: ./client [NM_IP] [NM_port]        (defaults: 127.0.0.1 5568)
 *        The shared secret is read from $NFS_SECRET, or prompted for.
 *
 * Fix: the file name is asked for ONCE. It used to be asked in the menu (to find
 * the storage server) and then again by the storage-server step; the second
 * prompt swallowed whatever came next. Everything the operation needs is now
 * collected first, then the connection is made.
 */

#define _GNU_SOURCE
#include "nfs_compat.h"
#include "netproto.h"

static char        nm_ip[INET_ADDRSTRLEN] = "127.0.0.1";
static int         nm_port = 5568;
static const char *g_secret;

/* ------------------------------------------------------------------ */
static int read_line(const char *prompt, char *buf, int bufsz)
{
    printf("%s", prompt);
    fflush(stdout);
    if (!fgets(buf, bufsz, stdin)) { buf[0] = '\0'; return -1; }      /* EOF */
    size_t n = strcspn(buf, "\r\n");
    if (buf[n] == '\0' && n == (size_t)bufsz - 1)                     /* line too long: drop the rest */
    { int c; while ((c = getchar()) != '\n' && c != EOF); }
    buf[n] = '\0';
    return 0;
}

static int connect_auth(const char *ip, int port, int timeout_s, const char *what)
{
    int s = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { perror("[-] socket"); return -1; }
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &a.sin_addr) != 1)
        { fprintf(stderr, "[-] '%s' is not a valid IPv4 address\n", ip); nfs_sockclose(s); return -1; }
    nfs_tune(s); nfs_set_timeout(s, 10);
    if (connect(s, (struct sockaddr *)&a, sizeof a) < 0)
        { fprintf(stderr, "[-] Cannot connect to %s %s:%d: %s\n", what, ip, port, nfs_sockerrstr()); nfs_sockclose(s); return -1; }
    if (nfs_auth_connect(s, g_secret) != 0)
        { fprintf(stderr, "[-] Authentication with %s failed (wrong shared secret?)\n", what); nfs_sockclose(s); return -1; }
    nfs_set_timeout(s, timeout_s);
    return s;
}

/* "a.b.c.d:port" -> 1, anything else (error text from NM) -> 0 */
static int parse_ss_info(const char *resp, char *ip_out, size_t ipsz, int *port_out)
{
    const char *colon = strchr(resp, ':');
    if (!colon || (size_t)(colon - resp) >= ipsz) return 0;
    char ip[INET_ADDRSTRLEN]; struct in_addr tmp;
    size_t n = (size_t)(colon - resp);
    if (n >= sizeof ip) return 0;
    memcpy(ip, resp, n); ip[n] = '\0';
    if (inet_pton(AF_INET, ip, &tmp) != 1) return 0;
    int port = atoi(colon + 1);
    if (port <= 0 || port > 65535) return 0;
    snprintf(ip_out, ipsz, "%s", ip); *port_out = port;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Storage-server operations (everything already collected)           */
/* ------------------------------------------------------------------ */
static void ss_read(const char *ip, int port, const char *name)
{
    int s = connect_auth(ip, port, 30, "storage server");
    if (s < 0) return;
    char status[256];
    if (send_str(s, "1") || send_str(s, name) || recv_str(s, status, sizeof status))
        { printf("[!] Storage server did not answer.\n\n"); nfs_sockclose(s); return; }
    if (strcmp(status, "OK") != 0) { printf("[ERROR] %s\n\n", status); nfs_sockclose(s); return; }

    printf("\n--- File Contents ---\n");
    char *buf = malloc(NFS_CHUNK);
    int last = '\n';
    for (;;)
    {
        uint32_t n;
        if (!buf || recv_frame(s, buf, NFS_CHUNK, &n)) { printf("\n[!] Connection lost while reading.\n"); break; }
        if (n == 0) break;
        fwrite(buf, 1, n, stdout);
        last = (unsigned char)buf[n - 1];
    }
    if (last != '\n') putchar('\n');
    printf("--- End of File ---\n\n");
    free(buf);
    nfs_sockclose(s);
}

static void ss_write(const char *ip, int port, const char *name, const char *data)
{
    int s = connect_auth(ip, port, 30, "storage server");
    if (s < 0) return;
    size_t len = strlen(data);
    char resp[256];
    int bad = send_str(s, "2") || send_str(s, name);
    if (!bad && len) bad = send_frame(s, data, (uint32_t)len);
    if (!bad) bad = send_frame(s, "", 0);                 /* end of data */
    if (bad || recv_str(s, resp, sizeof resp)) printf("[!] Storage server did not answer.\n\n");
    else printf("[+] %s\n\n", resp);
    nfs_sockclose(s);
}

static void ss_info(const char *ip, int port, const char *name)
{
    int s = connect_auth(ip, port, 30, "storage server");
    if (s < 0) return;
    char resp[2200];
    if (send_str(s, "3") || send_str(s, name) || recv_str(s, resp, sizeof resp)) printf("[!] Storage server did not answer.\n\n");
    else printf("\n%s\n\n", resp);
    nfs_sockclose(s);
}

/* ------------------------------------------------------------------ */
/* Session with the Naming Server                                      */
/* ------------------------------------------------------------------ */
static int ask_name(const char *prompt, char *out, size_t cap)
{
    if (read_line(prompt, out, (int)cap) != 0) return -1;
    if (!nfs_valid_name(out))
    {
        printf("[!] 108: Invalid file name (no empty name, no leading '/', no '..', no backslashes or control characters).\n\n");
        return 1;
    }
    return 0;
}

static void print_reply(const char *resp)
{
    printf("%s\n\n", resp);
}

static void session(void)
{
    int nm = connect_auth(nm_ip, nm_port, 60, "naming server");
    if (nm < 0) exit(1);
    printf("[+] Connected to Naming Server at %s:%d\n\n", nm_ip, nm_port);

    for (;;)
    {
        printf("┌─────────────────────────────────┐\n");
        printf("│    Distributed File System      │\n");
        printf("├─────────────────────────────────┤\n");
        printf("│  1. Read File                   │\n");
        printf("│  2. Write / Append to File      │\n");
        printf("│  3. Get File Info               │\n");
        printf("│  4. Create File / Directory     │\n");
        printf("│  5. Delete File                 │\n");
        printf("│  6. Copy File                   │\n");
        printf("│  7. List All Files              │\n");
        printf("│  0. Exit                        │\n");
        printf("└─────────────────────────────────┘\n");

        char line[64];
        if (read_line("Choice: ", line, sizeof line) != 0) break;       /* EOF */
        char *end; long choice = strtol(line, &end, 10);
        if (end == line || *end != '\0') { printf("[!] Invalid input. Enter 0-7.\n\n"); continue; }
        if (choice == 0) { printf("Goodbye!\n"); break; }

        char name[NFS_MAX_NAME + 8], name2[NFS_MAX_NAME + 8], data[4096], resp[NFS_CTRL_MAX > 4096 ? 4096 : 4096];
        char opb[8];
        snprintf(opb, sizeof opb, "%ld", choice);
        int rc;

        if (choice >= 1 && choice <= 3)
        {
            const char *prompt = choice == 1 ? "Enter filename to read: "
                               : choice == 2 ? "Enter filename to write: "
                                             : "Enter filename for info: ";
            if ((rc = ask_name(prompt, name, sizeof name)) < 0) break;
            if (rc) continue;
            data[0] = '\0';
            if (choice == 2 && read_line("Enter data to append: ", data, sizeof data) != 0) break;

            /* ask the NM which storage server holds / should hold the file */
            if (send_str(nm, opb) || send_str(nm, name) || recv_str(nm, resp, sizeof resp))
                { printf("[!] Lost connection to the Naming Server.\n"); break; }
            char ss_ip[INET_ADDRSTRLEN]; int ss_port;
            if (!parse_ss_info(resp, ss_ip, sizeof ss_ip, &ss_port)) { printf("[!] %s\n\n", resp); continue; }

            if      (choice == 1) ss_read(ss_ip, ss_port, name);
            else if (choice == 2) ss_write(ss_ip, ss_port, name, data);
            else                  ss_info(ss_ip, ss_port, name);
        }
        else if (choice == 4 || choice == 5)
        {
            const char *prompt = choice == 4 ? "Enter filename (add / at end for directory): "
                                             : "Enter filename to delete: ";
            if ((rc = ask_name(prompt, name, sizeof name)) < 0) break;
            if (rc) continue;
            if (send_str(nm, opb) || send_str(nm, name) || recv_str(nm, resp, sizeof resp))
                { printf("[!] Lost connection to the Naming Server.\n"); break; }
            print_reply(resp);
        }
        else if (choice == 6)
        {
            if ((rc = ask_name("Enter source filename: ", name, sizeof name)) < 0) break;
            if (rc) continue;
            if ((rc = ask_name("Enter destination filename: ", name2, sizeof name2)) < 0) break;
            if (rc) continue;
            if (send_str(nm, opb) || send_str(nm, name) || send_str(nm, name2) || recv_str(nm, resp, sizeof resp))
                { printf("[!] Lost connection to the Naming Server.\n"); break; }
            print_reply(resp);
        }
        else if (choice == 7)
        {
            char *big = malloc((size_t)NFS_CTRL_MAX + 1);
            uint32_t n = 0;
            if (!big || send_str(nm, opb) || recv_frame(nm, big, NFS_CTRL_MAX, &n))
                { printf("[!] Lost connection to the Naming Server.\n"); free(big); break; }
            big[n] = '\0';
            printf("\n--- Files on Storage Server ---\n%s", big);
            if (n && big[n - 1] != '\n') putchar('\n');
            printf("--- End of Listing ---\n\n");
            free(big);
        }
        else printf("[!] Invalid choice. Enter 0-7.\n\n");
    }
    nfs_sockclose(nm);
}

int main(int argc, char *argv[])
{
    nfs_net_init();
    if (argc >= 2) snprintf(nm_ip, sizeof nm_ip, "%s", argv[1]);
    if (argc >= 3) nm_port = atoi(argv[2]);

    printf("╔═════════════════════════════════╗\n");
    printf("║    Distributed File System      ║\n");
    printf("╠═════════════════════════════════╣\n");
    printf("║ Naming Server : %s:%d\n", nm_ip, nm_port);
    printf("╚═════════════════════════════════╝\n\n");

    g_secret = nfs_load_secret(1);
    if (!g_secret) return 1;
    session();
    return 0;
}
