/* nbdkit
 * Copyright (C) 2013-2016 Red Hat Inc.
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

#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <endian.h>
#include <sys/types.h>
#include <stddef.h>

#include <pthread.h>

#include "nbdkit-plugin.h"
#include "internal.h"
#include "protocol.h"

/* Maximum read or write request that we will handle. */
#define MAX_REQUEST_SIZE (64 * 1024 * 1024)

/* Maximum number of client options we allow before giving up. */
#define MAX_NR_OPTIONS 32

/* Maximum length of any option data (bytes). */
#define MAX_OPTION_LENGTH 4096

/* Connection structure. */
struct connection {
  pthread_mutex_t request_lock;
  void *handle;
  void *crypto_session;

  uint64_t exportsize;
  int readonly;
  int can_flush;
  int is_rotational;
  int can_trim;
  int using_tls;

  int sockin, sockout;
  connection_recv_function recv;
  connection_send_function send;
  connection_close_function close;
};

static struct connection *new_connection (int sockin, int sockout);
static void free_connection (struct connection *conn);
static int negotiate_handshake (struct connection *conn);
static int recv_request_send_reply (struct connection *conn);

/* Don't call these raw socket functions directly.  Use conn->recv etc. */
static int raw_recv (struct connection *, void *buf, size_t len);
static int raw_send (struct connection *, const void *buf, size_t len);
static void raw_close (struct connection *);

/* Accessors for public fields in the connection structure.
 * Everything else is private to this file.
 */
void
connection_set_handle (struct connection *conn, void *handle)
{
  conn->handle = handle;
}

void *
connection_get_handle (struct connection *conn)
{
  return conn->handle;
}

pthread_mutex_t *
connection_get_request_lock (struct connection *conn)
{
  return &conn->request_lock;
}

void
connection_set_crypto_session (struct connection *conn, void *session)
{
  conn->crypto_session = session;
}

void *
connection_get_crypto_session (struct connection *conn)
{
  return conn->crypto_session;
}

/* The code in crypto.c uses these three functions to replace the
 * recv, send and close callbacks when a connection is upgraded to
 * TLS.
 */
void
connection_set_recv (struct connection *conn, connection_recv_function recv)
{
  conn->recv = recv;
}

void
connection_set_send (struct connection *conn, connection_send_function send)
{
  conn->send = send;
}

void
connection_set_close (struct connection *conn, connection_close_function close)
{
  conn->close = close;
}

static int
_handle_single_connection (int sockin, int sockout)
{
  int r;
  struct connection *conn = new_connection (sockin, sockout);

  if (!conn)
    goto err;

  if (plugin_open (conn, readonly) == -1)
    goto err;

  threadlocal_set_name (plugin_name ());

  /* Handshake. */
  if (negotiate_handshake (conn) == -1)
    goto err;

  /* Process requests.  XXX Allow these to be dispatched in parallel using
   * a thread pool.
   */
  while (!quit) {
    r = recv_request_send_reply (conn);
    if (r == -1)
      goto err;
    if (r == 0)
      break;
  }

  free_connection (conn);
  return 0;

 err:
  free_connection (conn);
  return -1;
}

int
handle_single_connection (int sockin, int sockout)
{
  int r;

  plugin_lock_connection ();
  r = _handle_single_connection (sockin, sockout);
  plugin_unlock_connection ();

  return r;
}

static struct connection *
new_connection (int sockin, int sockout)
{
  struct connection *conn;

  conn = calloc (1, sizeof *conn);
  if (conn == NULL) {
    perror ("malloc");
    return NULL;
  }

  conn->sockin = sockin;
  conn->sockout = sockout;
  pthread_mutex_init (&conn->request_lock, NULL);

  conn->recv = raw_recv;
  conn->send = raw_send;
  conn->close = raw_close;

  return conn;
}

static void
free_connection (struct connection *conn)
{
  if (!conn)
    return;

  conn->close (conn);

  pthread_mutex_destroy (&conn->request_lock);

  if (conn->handle)
    plugin_close (conn);

  free (conn);
}

static int
_negotiate_handshake_oldstyle (struct connection *conn)
{
  struct old_handshake handshake;
  int64_t r;
  uint64_t exportsize;
  uint16_t gflags, eflags;
  int fl;

  /* In --tls=require / FORCEDTLS mode, old style handshakes are
   * rejected because they cannot support TLS.
   */
  if (tls == 2) {
    nbdkit_error ("non-TLS client tried to connect in --tls=require mode");
    return -1;
  }

  r = plugin_get_size (conn);
  if (r == -1)
    return -1;
  if (r < 0) {
    nbdkit_error (".get_size function returned invalid value "
                  "(%" PRIi64 ")", r);
    return -1;
  }
  exportsize = (uint64_t) r;
  conn->exportsize = exportsize;

  gflags = 0;
  eflags = NBD_FLAG_HAS_FLAGS;

  fl = plugin_can_write (conn);
  if (fl == -1)
    return -1;
  if (readonly || !fl) {
    eflags |= NBD_FLAG_READ_ONLY;
    conn->readonly = 1;
  }
  if (!conn->readonly) {
    eflags |= NBD_FLAG_SEND_WRITE_ZEROES;
  }


  fl = plugin_can_flush (conn);
  if (fl == -1)
    return -1;
  if (fl) {
    eflags |= NBD_FLAG_SEND_FLUSH | NBD_FLAG_SEND_FUA;
    conn->can_flush = 1;
  }

  fl = plugin_is_rotational (conn);
  if (fl == -1)
    return -1;
  if (fl) {
    eflags |= NBD_FLAG_ROTATIONAL;
    conn->is_rotational = 1;
  }

  fl = plugin_can_trim (conn);
  if (fl == -1)
    return -1;
  if (fl) {
    eflags |= NBD_FLAG_SEND_TRIM;
    conn->can_trim = 1;
  }

  debug ("oldstyle negotiation: flags: global 0x%x export 0x%x",
         gflags, eflags);

  memset (&handshake, 0, sizeof handshake);
  memcpy (handshake.nbdmagic, "NBDMAGIC", 8);
  handshake.version = htobe64 (OLD_VERSION);
  handshake.exportsize = htobe64 (exportsize);
  handshake.gflags = htobe16 (gflags);
  handshake.eflags = htobe16 (eflags);

  if (conn->send (conn, &handshake, sizeof handshake) == -1) {
    nbdkit_error ("write: %m");
    return -1;
  }

  return 0;
}

/* Receive newstyle options. */

static int
send_newstyle_option_reply (struct connection *conn,
                            uint32_t option, uint32_t reply)
{
  struct fixed_new_option_reply fixed_new_option_reply;

  fixed_new_option_reply.magic = htobe64 (NBD_REP_MAGIC);
  fixed_new_option_reply.option = htobe32 (option);
  fixed_new_option_reply.reply = htobe32 (reply);
  fixed_new_option_reply.replylen = htobe32 (0);

  if (conn->send (conn,
                  &fixed_new_option_reply,
                  sizeof fixed_new_option_reply) == -1) {
    nbdkit_error ("write: %m");
    return -1;
  }

  return 0;
}

static int
send_newstyle_option_reply_exportname (struct connection *conn,
                                       uint32_t option, uint32_t reply,
                                       const char *exportname)
{
  struct fixed_new_option_reply fixed_new_option_reply;
  size_t name_len = strlen (exportname);
  uint32_t len;

  fixed_new_option_reply.magic = htobe64 (NBD_REP_MAGIC);
  fixed_new_option_reply.option = htobe32 (option);
  fixed_new_option_reply.reply = htobe32 (reply);
  fixed_new_option_reply.replylen = htobe32 (name_len + sizeof (len));

  if (conn->send (conn,
                  &fixed_new_option_reply,
                  sizeof fixed_new_option_reply) == -1) {
    nbdkit_error ("write: %m");
    return -1;
  }

  len = htobe32 (name_len);
  if (conn->send (conn, &len, sizeof len) == -1) {
    nbdkit_error ("write: %m");
    return -1;
  }
  if (conn->send (conn, exportname, name_len) == -1) {
    nbdkit_error ("write: %m");
    return -1;
  }

  return 0;
}

static int
_negotiate_handshake_newstyle_options (struct connection *conn)
{
  struct new_option new_option;
  size_t nr_options;
  uint64_t version;
  uint32_t option;
  uint32_t optlen;
  char data[MAX_OPTION_LENGTH+1];

  for (nr_options = 0; nr_options < MAX_NR_OPTIONS; ++nr_options) {
    if (conn->recv (conn, &new_option, sizeof new_option) == -1) {
      nbdkit_error ("read: %m");
      return -1;
    }

    version = be64toh (new_option.version);
    if (version != NEW_VERSION) {
      nbdkit_error ("unknown option version %" PRIx64
                    ", expecting %" PRIx64,
                    version, NEW_VERSION);
      return -1;
    }

    /* There is a maximum option length we will accept, regardless
     * of the option type.
     */
    optlen = be32toh (new_option.optlen);
    if (optlen > MAX_OPTION_LENGTH) {
      nbdkit_error ("client option data too long (%" PRIu32 ")", optlen);
      return -1;
    }

    option = be32toh (new_option.option);

    /* In --tls=require / FORCEDTLS mode the only options allowed
     * before TLS negotiation are NBD_OPT_ABORT and NBD_OPT_STARTTLS.
     */
    if (tls == 2 && !conn->using_tls &&
        !(option == NBD_OPT_ABORT || option == NBD_OPT_STARTTLS)) {
      if (send_newstyle_option_reply (conn, option, NBD_REP_ERR_TLS_REQD))
        return -1;
      continue;
    }

    switch (option) {
    case NBD_OPT_EXPORT_NAME:
      if (conn->recv (conn, data, optlen) == -1) {
        nbdkit_error ("read: %m");
        return -1;
      }
      /* Apart from printing it, ignore the export name. */
      data[optlen] = '\0';
      debug ("newstyle negotiation: client requested export '%s' (ignored)",
             data);
      break;

    case NBD_OPT_ABORT:
      if (send_newstyle_option_reply (conn, option, NBD_REP_ACK) == -1)
        return -1;
      nbdkit_error ("client sent NBD_OPT_ABORT to abort the connection");
      return -1;

    case NBD_OPT_LIST:
      if (optlen != 0) {
        if (send_newstyle_option_reply (conn, option, NBD_REP_ERR_INVALID)
            == -1)
          return -1;
        if (conn->recv (conn, data, optlen) == -1) {
          nbdkit_error ("read: %m");
          return -1;
        }
        continue;
      }

      /* Send back the exportname. */
      debug ("newstyle negotiation: advertising export '%s'", exportname);
      if (send_newstyle_option_reply_exportname (conn, option, NBD_REP_SERVER,
                                                 exportname) == -1)
        return -1;

      if (send_newstyle_option_reply (conn, option, NBD_REP_ACK) == -1)
        return -1;
      break;

    case NBD_OPT_STARTTLS:
      if (optlen != 0) {
        if (send_newstyle_option_reply (conn, option, NBD_REP_ERR_INVALID)
            == -1)
          return -1;
        if (conn->recv (conn, data, optlen) == -1) {
          nbdkit_error ("read: %m");
          return -1;
        }
        continue;
      }

      if (tls == 0) {           /* --tls=off (NOTLS mode). */
#ifdef HAVE_GNUTLS
#define NO_TLS_REPLY NBD_REP_ERR_POLICY
#else
#define NO_TLS_REPLY NBD_REP_ERR_UNSUP
#endif
        if (send_newstyle_option_reply (conn, option, NO_TLS_REPLY) == -1)
          return -1;
      }
      else /* --tls=on or --tls=require */ {
        /* We can't upgrade to TLS twice on the same connection. */
        if (conn->using_tls) {
          if (send_newstyle_option_reply (conn, option,
                                          NBD_REP_ERR_INVALID) == -1)
            return -1;
          continue;
        }

        /* We have to send the (unencrypted) reply before starting
         * the handshake.
         */
        if (send_newstyle_option_reply (conn, option, NBD_REP_ACK) == -1)
          return -1;

        /* Upgrade the connection to TLS.  Also performs access control. */
        if (crypto_negotiate_tls (conn, conn->sockin, conn->sockout) == -1)
          return -1;
        conn->using_tls = 1;
        debug ("using TLS on this connection");
      }
      break;

    default:
      /* Unknown option. */
      if (send_newstyle_option_reply (conn, option, NBD_REP_ERR_UNSUP) == -1)
        return -1;
      if (conn->recv (conn, data, optlen) == -1) {
        nbdkit_error ("read: %m");
        return -1;
      }
    }

    /* Note, since it's not very clear from the protocol doc, that the
     * client must send NBD_OPT_EXPORT_NAME last, and that ends option
     * negotiation.
     */
    if (option == NBD_OPT_EXPORT_NAME)
      break;
  }

  if (nr_options >= MAX_NR_OPTIONS) {
    nbdkit_error ("client exceeded maximum number of options (%d)",
                  MAX_NR_OPTIONS);
    return -1;
  }

  /* In --tls=require / FORCEDTLS mode, we must have upgraded to TLS
   * by the time we finish option negotiation.  If not, give up.
   */
  if (tls == 2 && !conn->using_tls) {
    nbdkit_error ("non-TLS client tried to connect in --tls=require mode");
    return -1;
  }

  return 0;
}

static int
_negotiate_handshake_newstyle (struct connection *conn)
{
  struct new_handshake handshake;
  uint16_t gflags;
  uint32_t cflags;
  struct new_handshake_finish handshake_finish;
  int64_t r;
  uint64_t exportsize;
  uint16_t eflags;
  int fl;

  gflags = NBD_FLAG_FIXED_NEWSTYLE | NBD_FLAG_NO_ZEROES;

  debug ("newstyle negotiation: flags: global 0x%x", gflags);

  memcpy (handshake.nbdmagic, "NBDMAGIC", 8);
  handshake.version = htobe64 (NEW_VERSION);
  handshake.gflags = htobe16 (gflags);

  if (conn->send (conn, &handshake, sizeof handshake) == -1) {
    nbdkit_error ("write: %m");
    return -1;
  }

  /* Client now sends us its 32 bit flags word ... */
  if (conn->recv (conn, &cflags, sizeof cflags) == -1) {
    nbdkit_error ("read: %m");
    return -1;
  }
  cflags = be32toh (cflags);
  /* ... which we check for accuracy. */
  debug ("newstyle negotiation: client flags: 0x%x", cflags);
  if (cflags & ~gflags) {
    nbdkit_error ("client requested unknown flags 0x%x", cflags);
    return -1;
  }

  /* Receive newstyle options. */
  if (_negotiate_handshake_newstyle_options (conn) == -1)
    return -1;

  /* Finish the newstyle handshake. */
  r = plugin_get_size (conn);
  if (r == -1)
    return -1;
  if (r < 0) {
    nbdkit_error (".get_size function returned invalid value "
                  "(%" PRIi64 ")", r);
    return -1;
  }
  exportsize = (uint64_t) r;
  conn->exportsize = exportsize;

  eflags = NBD_FLAG_HAS_FLAGS;

  fl = plugin_can_write (conn);
  if (fl == -1)
    return -1;
  if (readonly || !fl) {
    eflags |= NBD_FLAG_READ_ONLY;
    conn->readonly = 1;
  }
  if (!conn->readonly) {
    eflags |= NBD_FLAG_SEND_WRITE_ZEROES;
  }

  fl = plugin_can_flush (conn);
  if (fl == -1)
    return -1;
  if (fl) {
    eflags |= NBD_FLAG_SEND_FLUSH | NBD_FLAG_SEND_FUA;
    conn->can_flush = 1;
  }

  fl = plugin_is_rotational (conn);
  if (fl == -1)
    return -1;
  if (fl) {
    eflags |= NBD_FLAG_ROTATIONAL;
    conn->is_rotational = 1;
  }

  fl = plugin_can_trim (conn);
  if (fl == -1)
    return -1;
  if (fl) {
    eflags |= NBD_FLAG_SEND_TRIM;
    conn->can_trim = 1;
  }

  debug ("newstyle negotiation: flags: export 0x%x", eflags);

  memset (&handshake_finish, 0, sizeof handshake_finish);
  handshake_finish.exportsize = htobe64 (exportsize);
  handshake_finish.eflags = htobe16 (eflags);

  if (conn->send (conn,
                  &handshake_finish,
                  (cflags & NBD_FLAG_NO_ZEROES)
                  ? offsetof (struct new_handshake_finish, zeroes)
                  : sizeof handshake_finish) == -1) {
    nbdkit_error ("write: %m");
    return -1;
  }

  return 0;
}

static int
negotiate_handshake (struct connection *conn)
{
  int r;

  plugin_lock_request (conn);
  if (!newstyle)
    r = _negotiate_handshake_oldstyle (conn);
  else
    r = _negotiate_handshake_newstyle (conn);
  plugin_unlock_request (conn);

  return r;
}

static int
valid_range (struct connection *conn, uint64_t offset, uint32_t count)
{
  uint64_t exportsize = conn->exportsize;

  return count > 0 && offset <= exportsize && offset + count <= exportsize;
}

static int
validate_request (struct connection *conn,
                  uint32_t cmd, uint32_t flags, uint64_t offset, uint32_t count,
                  uint32_t *error)
{
  int r;

  /* Validate cmd, offset, count. */
  switch (cmd) {
  case NBD_CMD_READ:
  case NBD_CMD_WRITE:
  case NBD_CMD_TRIM:
  case NBD_CMD_WRITE_ZEROES:
    r = valid_range (conn, offset, count);
    if (r == -1)
      return -1;
    if (r == 0) {
      /* XXX Allow writes to extend the disk? */
      nbdkit_error ("invalid request: offset and length are out of range");
      *error = EIO;
      return 0;
    }
    break;

  case NBD_CMD_FLUSH:
    if (offset != 0 || count != 0) {
      nbdkit_error ("invalid flush request: expecting offset and length == 0");
      *error = EINVAL;
      return 0;
    }
    break;

  default:
    nbdkit_error ("invalid request: unknown command (%" PRIu32 ") ignored",
                  cmd);
    *error = EINVAL;
    return 0;
  }

  /* Validate flags */
  if (flags & ~(NBD_CMD_FLAG_FUA | NBD_CMD_FLAG_NO_HOLE)) {
    nbdkit_error ("invalid request: unknown flag (0x%x)", flags);
    *error = EINVAL;
    return 0;
  }
  if ((flags & NBD_CMD_FLAG_NO_HOLE) &&
      cmd != NBD_CMD_WRITE_ZEROES) {
    nbdkit_error ("invalid request: NO_HOLE flag needs WRITE_ZEROES request");
    *error = EINVAL;
    return 0;
  }

  /* Refuse over-large read and write requests. */
  if ((cmd == NBD_CMD_WRITE || cmd == NBD_CMD_READ) &&
      count > MAX_REQUEST_SIZE) {
    nbdkit_error ("invalid request: data request is too large (%" PRIu32
                  " > %d)", count, MAX_REQUEST_SIZE);
    *error = ENOMEM;
    return 0;
  }

  /* Readonly connection? */
  if (conn->readonly &&
      (cmd == NBD_CMD_WRITE || cmd == NBD_CMD_FLUSH ||
       cmd == NBD_CMD_TRIM || cmd == NBD_CMD_WRITE_ZEROES)) {
    nbdkit_error ("invalid request: write request on readonly connection");
    *error = EROFS;
    return 0;
  }

  /* Flush allowed? */
  if (!conn->can_flush && cmd == NBD_CMD_FLUSH) {
    nbdkit_error ("invalid request: flush operation not supported");
    *error = EINVAL;
    return 0;
  }

  /* Trim allowed? */
  if (!conn->can_trim && cmd == NBD_CMD_TRIM) {
    nbdkit_error ("invalid request: trim operation not supported");
    *error = EINVAL;
    return 0;
  }

  return 1;                     /* Commands validates. */
}

/* Grab the appropriate error value.
 */
static int
get_error (struct connection *conn)
{
  int ret = threadlocal_get_error ();

  if (!ret && plugin_errno_is_preserved ())
    ret = errno;
  return ret ? ret : EIO;
}

/* This is called with the request lock held to actually execute the
 * request (by calling the plugin).  Note that the request fields have
 * been validated already in 'validate_request' so we don't have to
 * check them again.  'buf' is either the data to be written or the
 * data to be returned, and points to a buffer of size 'count' bytes.
 *
 * Only returns -1 if there is a fatal error and the connection cannot
 * continue.
 *
 * On read/write errors, sets *error appropriately and returns 0.
 */
static int
_handle_request (struct connection *conn,
                 uint32_t cmd, uint32_t flags, uint64_t offset, uint32_t count,
                 void *buf,
                 uint32_t *error)
{
  bool flush_after_command;
  int r;

  /* Flush after command performed? */
  flush_after_command = (flags & NBD_CMD_FLAG_FUA) != 0;
  if (!conn->can_flush || conn->readonly)
    flush_after_command = false;

  /* The plugin should call nbdkit_set_error() to request a particular
     error, otherwise we fallback to errno or EIO. */
  threadlocal_set_error (0);

  switch (cmd) {
  case NBD_CMD_READ:
    r = plugin_pread (conn, buf, count, offset);
    if (r == -1) {
      *error = get_error (conn);
      return 0;
    }
    break;

  case NBD_CMD_WRITE:
    r = plugin_pwrite (conn, buf, count, offset);
    if (r == -1) {
      *error = get_error (conn);
      return 0;
    }
    break;

  case NBD_CMD_FLUSH:
    r = plugin_flush (conn);
    if (r == -1) {
      *error = get_error (conn);
      return 0;
    }
    break;

  case NBD_CMD_TRIM:
    r = plugin_trim (conn, count, offset);
    if (r == -1) {
      *error = get_error (conn);
      return 0;
    }
    break;

  case NBD_CMD_WRITE_ZEROES:
    r = plugin_zero (conn, count, offset, !(flags & NBD_CMD_FLAG_NO_HOLE));
    if (r == -1) {
      *error = get_error (conn);
      return 0;
    }
    break;

  default:
    abort ();
  }

  if (flush_after_command) {
    r = plugin_flush (conn);
    if (r == -1) {
      *error = get_error (conn);
      return 0;
    }
  }

  return 0;
}

static int
handle_request (struct connection *conn,
                uint32_t cmd, uint32_t flags, uint64_t offset, uint32_t count,
                void *buf,
                uint32_t *error)
{
  int r;

  plugin_lock_request (conn);
  r = _handle_request (conn, cmd, flags, offset, count, buf, error);
  plugin_unlock_request (conn);

  return r;
}

static void
skip_over_write_buffer (int sock, size_t count)
{
  char buf[BUFSIZ];
  ssize_t r;

  while (count > 0) {
    r = read (sock, buf, count > BUFSIZ ? BUFSIZ : count);
    if (r == -1) {
      nbdkit_error ("skipping write buffer: %m");
      return;
    }
    if (r == 0)
      return;
    count -= r;
  }
}

/* Convert a system errno to an NBD_E* error code. */
static int
nbd_errno (int error)
{
  switch (error) {
  case 0:
    return NBD_SUCCESS;
  case EROFS:
  case EPERM:
    return NBD_EPERM;
  case EIO:
    return NBD_EIO;
  case ENOMEM:
    return NBD_ENOMEM;
#ifdef EDQUOT
  case EDQUOT:
#endif
  case EFBIG:
  case ENOSPC:
    return NBD_ENOSPC;
#ifdef ESHUTDOWN
  case ESHUTDOWN:
    return NBD_ESHUTDOWN;
#endif
  case EINVAL:
  default:
    return NBD_EINVAL;
  }
}

static int
recv_request_send_reply (struct connection *conn)
{
  int r;
  struct request request;
  struct reply reply;
  uint32_t magic, cmd, flags, count, error = 0;
  uint64_t offset;
  CLEANUP_FREE char *buf = NULL;

  /* Read the request packet. */
  r = conn->recv (conn, &request, sizeof request);
  if (r == -1) {
    nbdkit_error ("read request: %m");
    return -1;
  }
  if (r == 0) {
    debug ("client closed input socket, closing connection");
    return 0;                   /* disconnect */
  }

  magic = be32toh (request.magic);
  if (magic != NBD_REQUEST_MAGIC) {
    nbdkit_error ("invalid request: 'magic' field is incorrect (0x%x)", magic);
    return -1;
  }

  cmd = be32toh (request.type);
  flags = cmd & ~NBD_CMD_MASK_COMMAND;
  cmd &= NBD_CMD_MASK_COMMAND;

  offset = be64toh (request.offset);
  count = be32toh (request.count);

  if (cmd == NBD_CMD_DISC) {
    debug ("client sent disconnect command, closing connection");
    return 0;                   /* disconnect */
  }

  /* Validate the request. */
  r = validate_request (conn, cmd, flags, offset, count, &error);
  if (r == -1)
    return -1;
  if (r == 0) {                 /* request not valid */
    if (cmd == NBD_CMD_WRITE)
      skip_over_write_buffer (conn->sockin, count);
    goto send_reply;
  }

  /* Allocate the data buffer used for either read or write requests. */
  if (cmd == NBD_CMD_READ || cmd == NBD_CMD_WRITE) {
    buf = malloc (count);
    if (buf == NULL) {
      perror ("malloc");
      error = ENOMEM;
      if (cmd == NBD_CMD_WRITE)
        skip_over_write_buffer (conn->sockin, count);
      goto send_reply;
    }
  }

  /* Receive the write data buffer. */
  if (cmd == NBD_CMD_WRITE) {
    r = conn->recv (conn, buf, count);
    if (r == -1) {
      nbdkit_error ("read data: %m");
      return -1;
    }
    if (r == 0) {
      debug ("client closed input unexpectedly, closing connection");
      return 0;                 /* disconnect */
    }
  }

  /* Perform the request.  Only this part happens inside the request lock. */
  r = handle_request (conn, cmd, flags, offset, count, buf, &error);
  if (r == -1)
    return -1;

  /* Send the reply packet. */
 send_reply:
  reply.magic = htobe32 (NBD_REPLY_MAGIC);
  reply.handle = request.handle;
  reply.error = htobe32 (nbd_errno (error));

  if (error != 0) {
    /* Since we're about to send only the limited NBD_E* errno to the
     * client, don't lose the information about what really happened
     * on the server side.  Make sure there is a way for the operator
     * to retrieve the real error.
     */
    debug ("sending error reply: %s", strerror (error));
  }

  r = conn->send (conn, &reply, sizeof reply);
  if (r == -1) {
    nbdkit_error ("write reply: %m");
    return -1;
  }

  /* Send the read data buffer. */
  if (cmd == NBD_CMD_READ) {
    r = conn->send (conn, buf, count);
    if (r == -1) {
      nbdkit_error ("write data: %m");
      return -1;
    }
  }

  return 1;                     /* command processed ok */
}

/* Write buffer to conn->sockout and either succeed completely
 * (returns 0) or fail (returns -1).
 */
static int
raw_send (struct connection *conn, const void *vbuf, size_t len)
{
  int sock = conn->sockout;
  const char *buf = vbuf;
  ssize_t r;

  while (len > 0) {
    r = write (sock, buf, len);
    if (r == -1) {
      if (errno == EINTR || errno == EAGAIN)
        continue;
      return -1;
    }
    buf += r;
    len -= r;
  }

  return 0;
}

/* Read buffer from conn->sockin and either succeed completely
 * (returns > 0), read an EOF (returns 0), or fail (returns -1).
 */
static int
raw_recv (struct connection *conn, void *vbuf, size_t len)
{
  int sock = conn->sockin;
  char *buf = vbuf;
  ssize_t r;
  bool first_read = true;

  while (len > 0) {
    r = read (sock, buf, len);
    if (r == -1) {
      if (errno == EINTR || errno == EAGAIN)
        continue;
      return -1;
    }
    if (r == 0) {
      if (first_read)
        return 0;
      /* Partial record read.  This is an error. */
      errno = EBADMSG;
      return -1;
    }
    first_read = false;
    buf += r;
    len -= r;
  }

  return 1;
}

/* There's no place in the NBD protocol to send back errors from
 * close, so this function ignores errors.
 */
static void
raw_close (struct connection *conn)
{
  if (conn->sockin >= 0)
    close (conn->sockin);
  if (conn->sockout >= 0 && conn->sockin != conn->sockout)
    close (conn->sockout);
}
