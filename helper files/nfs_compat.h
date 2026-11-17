/* nfs_compat.h - one place for everything that differs between Windows and POSIX.
 *
 * Every .c file includes this FIRST.  On Linux/macOS it just pulls in the usual
 * POSIX headers, so the original build (bash.sh) keeps working unchanged.  On
 * Windows it maps the handful of POSIX calls this project uses onto Winsock and
 * the Win32 CRT, so the three programs build as native .exe files with MinGW-w64
 * - no WSL, no Cygwin DLLs, which means one client.exe can simply be copied to
 * another PC on the LAN.
 *
 * What it provides on Windows:
 *   - Winsock in place of <sys/socket.h> & friends, plus WSAStartup()
 *   - nfs_sockclose() / nfs_sockerrstr()  (closesocket + WSAGetLastError)
 *   - nfs_realpath()                      (GetFullPathName + existence check)
 *   - nfs_mkdir() / nfs_remove_path()     (directories need _mkdir / _rmdir)
 *   - nfs_random()                        (BCryptGenRandom, not /dev/urandom)
 *   - nfs_getpass()                       (console echo turned off)
 *   - localtime_r, sleep, O_BINARY, S_ISLNK stubs, UTF-8 console output
 */
#ifndef NFS_COMPAT_H
#define NFS_COMPAT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>

/* ================================================================== */
/*  W I N D O W S                                                      */
/* ================================================================== */
#ifdef _WIN32

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601          /* Windows 7+: inet_pton / inet_ntop */
#endif
#define WIN32_LEAN_AND_MEAN

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <io.h>
#include <direct.h>

/* Bigger path buffers than MinGW's PATH_MAX (260); only the buffer grows. */
#ifdef PATH_MAX
#undef PATH_MAX
#endif
#define PATH_MAX 1024

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0               /* Windows has no SIGPIPE */
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0                 /* no symlink-open guard available */
#endif
#ifndef S_ISLNK
#define S_ISLNK(m) (0)               /* stat() never reports a link here */
#endif
#ifndef S_ISREG
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#endif
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#endif
#define lstat(p, st) stat((p), (st))
#define NFS_O_BIN    _O_BINARY       /* never translate CRLF in file data */

typedef int nfs_socklen_t;

/* ---- start-up ---------------------------------------------------- */
static inline void nfs_net_init(void)
{
    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w) != 0)
    {
        fprintf(stderr, "[!] WSAStartup failed - cannot use the network\n");
        exit(1);
    }
    SetConsoleOutputCP(CP_UTF8);     /* the banners use box-drawing characters */
    setvbuf(stdout, NULL, _IONBF, 0);
}

/* ---- sockets ----------------------------------------------------- */
static inline int nfs_sockclose(int s) { return closesocket((SOCKET)s); }

static inline const char *nfs_sockerrstr(void)
{
    static char buf[256];
    int e = WSAGetLastError();
    if (FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       NULL, (DWORD)e, 0, buf, (DWORD)sizeof buf - 1, NULL) == 0)
        snprintf(buf, sizeof buf, "winsock error %d", e);
    else
    {
        char *p = buf + strlen(buf);          /* trim the trailing CR/LF */
        while (p > buf && (p[-1] == '\n' || p[-1] == '\r' || p[-1] == ' ')) *--p = '\0';
    }
    return buf;
}

/* Winsock never reports EINTR, so the retry loops in netproto.h stay quiet. */
static inline int nfs_sock_eintr(void) { return 0; }

/* ---- paths ------------------------------------------------------- */
/* realpath() work-alike: absolute, '/'-separated, and must already exist. */
static inline char *nfs_realpath(const char *in, char *out)
{
    char full[PATH_MAX];
    if (GetFullPathNameA(in, (DWORD)sizeof full, full, NULL) == 0) return NULL;
    for (char *p = full; *p; p++) if (*p == '\\') *p = '/';
    size_t n = strlen(full);
    while (n > 3 && full[n - 1] == '/') full[--n] = '\0';   /* but keep "C:/" */
    struct stat st;
    if (stat(full, &st) != 0) return NULL;                  /* realpath needs existence */
    memcpy(out, full, n + 1);
    return out;
}

static inline int nfs_mkdir(const char *p) { return _mkdir(p); }

/* remove() only deletes files on Windows; directories need _rmdir(). */
static inline int nfs_remove_path(const char *p)
{
    struct stat st;
    if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) return _rmdir(p);
    return remove(p);
}

/* ---- misc -------------------------------------------------------- */
static inline int nfs_random(void *buf, size_t n)
{
    return BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)n,
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0 ? 0 : -1;
}

static inline void nfs_sleep(unsigned secs) { Sleep(secs * 1000u); }

static inline struct tm *nfs_localtime(const time_t *t, struct tm *out)
{
    return localtime_s(out, t) == 0 ? out : NULL;
}

/* getpass() replacement: read one line with the console echo turned off. */
static inline const char *nfs_getpass(const char *prompt)
{
    static char pw[256];
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    int cooked = GetConsoleMode(h, &mode) != 0;
    if (cooked) SetConsoleMode(h, mode & ~(DWORD)ENABLE_ECHO_INPUT);
    fputs(prompt, stdout);
    fflush(stdout);
    if (!fgets(pw, sizeof pw, stdin)) pw[0] = '\0';
    if (cooked) { SetConsoleMode(h, mode); fputc('\n', stdout); }
    pw[strcspn(pw, "\r\n")] = '\0';
    return pw;
}

/* ================================================================== */
/*  P O S I X                                                          */
/* ================================================================== */
#else  /* !_WIN32 */

#include <unistd.h>
#include <limits.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#define NFS_O_BIN 0

typedef socklen_t nfs_socklen_t;

static inline void nfs_net_init(void) { }
static inline int  nfs_sockclose(int s) { return close(s); }
static inline const char *nfs_sockerrstr(void) { return strerror(errno); }
static inline int  nfs_sock_eintr(void) { return errno == EINTR; }

static inline char *nfs_realpath(const char *in, char *out) { return realpath(in, out); }
static inline int   nfs_mkdir(const char *p) { return mkdir(p, 0755); }
static inline int   nfs_remove_path(const char *p) { return remove(p); }
static inline void  nfs_sleep(unsigned secs) { sleep(secs); }
static inline struct tm *nfs_localtime(const time_t *t, struct tm *out) { return localtime_r(t, out); }

static inline int nfs_random(void *buf, size_t n)
{
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return -1;
    size_t got = 0;
    char *p = (char *)buf;
    while (got < n)
    {
        ssize_t r = read(fd, p + got, n - got);
        if (r <= 0) { if (r < 0 && errno == EINTR) continue; close(fd); return -1; }
        got += (size_t)r;
    }
    close(fd);
    return 0;
}

static inline const char *nfs_getpass(const char *prompt) { return getpass(prompt); }

#endif /* _WIN32 */

/* ---- used by the naming server's access policy ------------------- */
static inline int nfs_is_loopback(const char *ip)
{
    return strncmp(ip, "127.", 4) == 0 || strcmp(ip, "::1") == 0;
}

/* Is `ip` one of THIS machine's own addresses?  A client started on the server
 * PC with that PC's LAN IP (which is how the launcher does it) arrives from the
 * LAN address, not 127.0.0.1, and must still count as local. */
static inline int nfs_is_local_ip(const char *ip)
{
    if (nfs_is_loopback(ip)) return 1;

    char host[256];
    if (gethostname(host, sizeof host) != 0) return 0;

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hints, &res) != 0) return 0;

    int match = 0;
    char buf[INET_ADDRSTRLEN];
    for (struct addrinfo *a = res; a && !match; a = a->ai_next)
    {
        struct sockaddr_in *sa = (struct sockaddr_in *)a->ai_addr;
        if (inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof buf) && strcmp(buf, ip) == 0)
            match = 1;
    }
    freeaddrinfo(res);
    return match;
}

#endif /* NFS_COMPAT_H */
