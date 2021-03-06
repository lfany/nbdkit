/* nbdkit
 * Copyright (C) 2013 Red Hat Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 * * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * * Neither the name of Red Hat nor the names of its contributors may be
 * used to endorse or promote products derived from this software without
 * specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY RED HAT AND CONTRIBUTORS ''AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL RED HAT OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 * USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef NBDKIT_INTERNAL_H
#define NBDKIT_INTERNAL_H

#include <stdbool.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <pthread.h>

#include "nbdkit-plugin.h"

#ifdef __APPLE__
#define UNIX_PATH_MAX 104
#else
#define UNIX_PATH_MAX 108
#endif

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif

#ifndef htobe32
#include <byteswap.h>
# if __BYTE_ORDER == __LITTLE_ENDIAN
#  define htobe16(x) __bswap_16 (x)
#  define htole16(x) (x)
#  define be16toh(x) __bswap_16 (x)
#  define le16toh(x) (x)

#  define htobe32(x) __bswap_32 (x)
#  define htole32(x) (x)
#  define be32toh(x) __bswap_32 (x)
#  define le32toh(x) (x)

#  define htobe64(x) __bswap_64 (x)
#  define htole64(x) (x)
#  define be64toh(x) __bswap_64 (x)
#  define le64toh(x) (x)

# else
#  define htobe16(x) (x)
#  define htole16(x) __bswap_16 (x)
#  define be16toh(x) (x)
#  define le16toh(x) __bswap_16 (x)

#  define htobe32(x) (x)
#  define htole32(x) __bswap_32 (x)
#  define be32toh(x) (x)
#  define le32toh(x) __bswap_32 (x)

#  define htobe64(x) (x)
#  define htole64(x) __bswap_64 (x)
#  define be64toh(x) (x)
#  define le64toh(x) __bswap_64 (x)
# endif
#endif

/* main.c */
extern const char *exportname;
extern const char *ipaddr;
extern int newstyle;
extern const char *port;
extern int readonly;
extern const char *selinux_label;
extern int tls;
extern const char *tls_certificates_dir;
extern int tls_verify_peer;
extern char *unixsocket;
extern int verbose;

extern volatile int quit;

/* cleanup.c */
extern void cleanup_free (void *ptr);
#ifdef HAVE_ATTRIBUTE_CLEANUP
#define CLEANUP_FREE __attribute__((cleanup (cleanup_free)))
#else
#define CLEANUP_FREE
#endif

/* connections.c */
struct connection;
typedef int (*connection_recv_function) (struct connection *, void *buf, size_t len);
typedef int (*connection_send_function) (struct connection *, const void *buf, size_t len);
typedef void (*connection_close_function) (struct connection *);
extern int handle_single_connection (int sockin, int sockout);
extern void connection_set_handle (struct connection *conn, void *handle);
extern void *connection_get_handle (struct connection *conn);
extern pthread_mutex_t *connection_get_request_lock (struct connection *conn);
extern void connection_set_crypto_session (struct connection *conn, void *session);
extern void *connection_get_crypto_session (struct connection *conn);
extern void connection_set_recv (struct connection *, connection_recv_function);
extern void connection_set_send (struct connection *, connection_send_function);
extern void connection_set_close (struct connection *, connection_close_function);

/* crypto.c */
#define root_tls_certificates_dir sysconfdir "/pki/" PACKAGE_NAME
extern void crypto_init (int tls_set_on_cli);
extern void crypto_free (void);
extern int crypto_negotiate_tls (struct connection *conn, int sockin, int sockout);

/* errors.c */
#define debug nbdkit_debug

/* plugins.c */
extern void plugin_register (const char *_filename, void *_dl, struct nbdkit_plugin *(*plugin_init) (void));
extern void plugin_cleanup (void);
extern const char *plugin_name (void);
extern void plugin_usage (void);
extern const char *plugin_version (void);
extern void plugin_dump_fields (void);
extern void plugin_config (const char *key, const char *value);
extern void plugin_config_complete (void);
extern void plugin_lock_connection (void);
extern void plugin_unlock_connection (void);
extern void plugin_lock_request (struct connection *conn);
extern void plugin_unlock_request (struct connection *conn);
extern int plugin_errno_is_preserved (void);
extern int plugin_open (struct connection *conn, int readonly);
extern void plugin_close (struct connection *conn);
extern int64_t plugin_get_size (struct connection *conn);
extern int plugin_can_write (struct connection *conn);
extern int plugin_can_flush (struct connection *conn);
extern int plugin_is_rotational (struct connection *conn);
extern int plugin_can_trim (struct connection *conn);
extern int plugin_pread (struct connection *conn, void *buf, uint32_t count, uint64_t offset);
extern int plugin_pwrite (struct connection *conn, void *buf, uint32_t count, uint64_t offset);
extern int plugin_flush (struct connection *conn);
extern int plugin_trim (struct connection *conn, uint32_t count, uint64_t offset);
extern int plugin_zero (struct connection *conn, uint32_t count, uint64_t offset, int may_trim);

/* sockets.c */
extern int *bind_unix_socket (size_t *);
extern int *bind_tcpip_socket (size_t *);
extern void accept_incoming_connections (int *socks, size_t nr_socks);
extern void free_listening_sockets (int *socks, size_t nr_socks);

/* threadlocal.c */
extern void threadlocal_init (void);
extern void threadlocal_new_server_thread (void);
extern void threadlocal_set_name (const char *name);
extern void threadlocal_set_instance_num (size_t instance_num);
extern void threadlocal_set_sockaddr (struct sockaddr *addr, socklen_t addrlen);
extern const char *threadlocal_get_name (void);
extern size_t threadlocal_get_instance_num (void);
extern void threadlocal_set_error (int err);
extern int threadlocal_get_error (void);
/*extern void threadlocal_get_sockaddr ();*/
extern size_t get_running_threads (void);
extern void incr_running_threads (void);
extern void decr_running_threads (void);

/* Declare program_name. */
#if HAVE_DECL_PROGRAM_INVOCATION_SHORT_NAME == 1
#include <errno.h>
#define program_name program_invocation_short_name
#else
#define program_name "nbdkit"
#endif

#endif /* NBDKIT_INTERNAL_H */
