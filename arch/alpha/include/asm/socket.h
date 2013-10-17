#ifndef _ASM_SOCKET_H
#define _ASM_SOCKET_H

#include <uapi/asm/socket.h>

#define SO_WIFI_STATUS		41
#define SCM_WIFI_STATUS		SO_WIFI_STATUS

/* O_NONBLOCK clashes with the bits used for socket types.  Therefore we
 * have to define SOCK_NONBLOCK to a different value here.
 */
#define SOCK_NONBLOCK	0x40000000
#endif /* _ASM_SOCKET_H */
