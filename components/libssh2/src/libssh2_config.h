/* Minimal libssh2 config for ESP-IDF (lwIP sockets, mbedTLS crypto backend) */
#ifndef LIBSSH2_CONFIG_H
#define LIBSSH2_CONFIG_H
#define HAVE_UNISTD_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_SYS_SELECT_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_ARPA_INET_H 1
#define HAVE_NETINET_IN_H 1
#define HAVE_SYS_SOCKET_H 1
#define HAVE_O_NONBLOCK 1
#define HAVE_SELECT 1
#define HAVE_GETTIMEOFDAY 1
#define HAVE_SNPRINTF 1
#define HAVE_LONGLONG 1
#ifndef LIBSSH2_API
#define LIBSSH2_API
#endif
#endif
