/* convenience.h
 *
 * Changes vs original:
 *  - Removed hardcoded IP/port #defines (now passed as CLI args)
 *  - Kept DEFAULT_* constants as safe fallbacks
 *  - Added include guard
 */
#ifndef CONVENIENCE_H
#define CONVENIENCE_H

#include "nfs_compat.h"
#include <stdbool.h>
#include <pthread.h>

/* Default ports — all overridable via CLI args */
#define DEFAULT_NM_PORT_SS      5566   /* NM port for Storage Server registration */
#define DEFAULT_NM_PORT_CLIENT  5568   /* NM port for Client connections           */
#define DEFAULT_SS_CLIENT_PORT  5567   /* SS port for Client connections           */

/* LRU cache entry */
typedef struct LRU
{
    char client_command[1024];
    int  port;
} LRU;

#endif /* CONVENIENCE_H */
