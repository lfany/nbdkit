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

#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>

#include <pthread.h>

#include "nbdkit-plugin.h"
#include "internal.h"

/* Note that most thread-local storage data is informational, used for
 * smart error and debug messages on the server side.  However, error
 * tracking can be used to influence which error is sent to the client
 * in a reply.
 *
 * The main thread does not have any associated Thread Local Storage,
 * *unless* it is serving a request (the '-s' option).
 */

struct threadlocal {
  const char *name;             /* Can be NULL. */
  size_t instance_num;          /* Can be 0. */
  struct sockaddr *addr;
  socklen_t addrlen;
  int err;
};

static pthread_key_t threadlocal_key;

static void
free_threadlocal (void *threadlocalv)
{
  struct threadlocal *threadlocal = threadlocalv;

  free (threadlocal->addr);
  free (threadlocal);

  decr_running_threads ();
}

void
threadlocal_init (void)
{
  int err;

  err = pthread_key_create (&threadlocal_key, free_threadlocal);
  if (err != 0) {
    fprintf (stderr, "%s: pthread_key_create: %s\n",
             program_name, strerror (err));
    exit (EXIT_FAILURE);
  }
}

void
threadlocal_new_server_thread (void)
{
  struct threadlocal *threadlocal;

  incr_running_threads ();

  threadlocal = calloc (1, sizeof *threadlocal);
  if (threadlocal == NULL) {
    perror ("malloc");
    exit (EXIT_FAILURE);
  }
  pthread_setspecific (threadlocal_key, threadlocal);
}

void
threadlocal_set_name (const char *name)
{
  struct threadlocal *threadlocal = pthread_getspecific (threadlocal_key);

  if (threadlocal)
    threadlocal->name = name;
}

void
threadlocal_set_instance_num (size_t instance_num)
{
  struct threadlocal *threadlocal = pthread_getspecific (threadlocal_key);

  if (threadlocal)
    threadlocal->instance_num = instance_num;
}

void
threadlocal_set_sockaddr (struct sockaddr *addr, socklen_t addrlen)
{
  struct threadlocal *threadlocal = pthread_getspecific (threadlocal_key);

  if (threadlocal) {
    free (threadlocal->addr);
    threadlocal->addr = calloc (1, addrlen);
    if (threadlocal->addr == NULL) {
      perror ("calloc");
      exit (EXIT_FAILURE);
    }
    memcpy (threadlocal->addr, addr, addrlen);
  }
}

const char *
threadlocal_get_name (void)
{
  struct threadlocal *threadlocal = pthread_getspecific (threadlocal_key);

  if (!threadlocal)
    return NULL;

  return threadlocal->name;
}

size_t
threadlocal_get_instance_num (void)
{
  struct threadlocal *threadlocal = pthread_getspecific (threadlocal_key);

  if (!threadlocal)
    return 0;

  return threadlocal->instance_num;
}

void
threadlocal_set_error (int err)
{
  struct threadlocal *threadlocal = pthread_getspecific (threadlocal_key);

  if (threadlocal)
    threadlocal->err = err;
  else
    errno = err;
}

/* This preserves errno, for convenience.
 */
int
threadlocal_get_error (void)
{
  int err = errno;
  struct threadlocal *threadlocal = pthread_getspecific (threadlocal_key);

  errno = err;
  return threadlocal ? threadlocal->err : 0;
}

/* These functions keep track of the number of running threads. */
static pthread_mutex_t running_threads_lock = PTHREAD_MUTEX_INITIALIZER;
static size_t running_threads = 0;

size_t
get_running_threads (void)
{
  size_t r;

  pthread_mutex_lock (&running_threads_lock);
  r = running_threads;
  pthread_mutex_unlock (&running_threads_lock);
  return r;
}

void
incr_running_threads (void)
{
  pthread_mutex_lock (&running_threads_lock);
  running_threads++;
  pthread_mutex_unlock (&running_threads_lock);
}

void
decr_running_threads (void)
{
  pthread_mutex_lock (&running_threads_lock);
  running_threads--;
  pthread_mutex_unlock (&running_threads_lock);
}
