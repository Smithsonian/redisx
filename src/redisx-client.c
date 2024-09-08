/**
 * @file
 *
 * @date Created  on Aug 26, 2024
 * @author Attila Kovacs
 *
 *  Basic I/O (send/receive) functions for the RedisX library.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#if __Lynx__
#  include <socket.h>
#else
#  include <sys/socket.h>
#endif

#include "redisx-priv.h"

#ifndef REDIS_TIMEOUT_SECONDS
/// (seconds) Abort with an error if cannot send before this timeout (<=0 for not timeout)
#endif
#  define REDIS_TIMEOUT_SECONDS           3

#ifndef REDIS_SIMPLE_STRING_SIZE
/// (bytes) Only store up to this many characters from Redis confirms and errors.
#  define REDIS_SIMPLE_STRING_SIZE      256
#endif

/// \cond PRIVATE

#if (__Lynx__ && __powerpc__)
///< Yield after this many send() calls, <= 0 to disable yielding
#  define SEND_YIELD_COUNT          1
#else
///< Yield after this many send() calls, <= 0 to disable yielding
#  define SEND_YIELD_COUNT          (-1)
#endif

/// \endcond

/**
 * Specific call for dealing with socket level transmit (send/rcv) errors. It prints a descriptive message to
 * sdterr, and calls the configured user transmit error handler routine, and either exists the program
 * (if redisExitOnConnectionError() is set), or returns X_NO_SERVICE.
 *
 * @param cp    Pointer to the client's private data structure on which the error occurred.
 * @param op    The operation that failed, e.g. 'send' or 'read'.
 * @return      X_NO_SERVICE
 *
 * @sa redisxSetTransmitErrorHandler()
 */
static int rTransmitError(ClientPrivate *cp, const char *op) {
  if(cp->isEnabled) {
    RedisPrivate *p = (RedisPrivate *) cp->redis->priv;
    // Let the handler disconnect, if it wants to....
    fprintf(stderr, "RedisX : WARNING! %s failed on %s channel %d: %s\n", op, cp->redis->id, cp->idx, strerror(errno));
    if(p->transmitErrorFunc) p->transmitErrorFunc(cp->redis, cp->idx, op);
  }
  return X_NO_SERVICE;
}

/**
 * Reads a chunk of data into the client's receive holding buffer.
 *
 * @param cp        Pointer to the private data of the client.
 * @return          X_SUCCESS (0) if successful, or else an appropriate error (see xchange.h).
 */
static int rReadChunkAsync(ClientPrivate *cp) {
  const int sock = cp->socket;      // Local copy of socket fd that won't possibly change mid-call.

  if(sock < 0) return X_NO_INIT;

  cp->next = 0;
  cp->available = recv(sock, cp->in, REDISX_RCVBUF_SIZE, 0);
  xdprintf(" ... read %d bytes from client %d socket.\n", cp->available, cp->idx);
  if(cp->available <= 0) {
    if(cp->available == 0) errno = ECONNRESET;        // 0 return is remote cleared connection. So set ECONNRESET...
    return rTransmitError(cp, "read");
  }
  return X_SUCCESS;
}

/**
 * Tries to read a REDIS "\r\n" terminated token into the specified buffer, using up to the specified amount
 * of space available, without the CR+LF termination used by Redis. The string returned in the supplied buffer
 * is properly terminated with '\0'.
 *
 * \param cp       Pointer to the private data of a Redis channel.
 * \param buf      Pointer to the buffer in which the token (or part of it) will be stored.
 * \param length   Maximum number of bytes that may be read into buf.
 *
 * \return         The number of characters read into the buffer (excluding the CR+LF termination in the stream).
 *                 Values < 0 indicate an error.
 */
static int rReadToken(ClientPrivate *cp, char *buf, int length) {
  static const char *fn = "rReadToken";
  int foundTerms = 0, L;

  length--; // leave room for termination in incomplete tokens...

  pthread_mutex_lock(&cp->readLock);

  if(!cp->isEnabled) {
    pthread_mutex_unlock(&cp->readLock);
    return x_error(X_NO_SERVICE, ENXIO, fn, "client is not connected");
  }

  for(L=0; cp->isEnabled && foundTerms < 2; L++) {
    char c;

    // Read a chunk of available data from the socket...
    if(cp->next >= cp->available) {
      int status = rReadChunkAsync(cp);
      if(status) {
        pthread_mutex_unlock(&cp->readLock);
        return x_trace(fn, NULL, status);
      }
    }

    c = cp->in[cp->next++];

    switch(c) {
      case '\r' :
        foundTerms = 1;
        break;
      case '\n' :
        foundTerms = (foundTerms == 1) ? 2 : 0;
        break;
      default:
        foundTerms = 0;
    }

    // buffer only up to the allowed number of characters in buf...
    if(L < length) buf[L] = c;
  }

  pthread_mutex_unlock(&cp->readLock);

  // If client was disabled before we got everything, then simply return X_NO_SERVICE
  if(!cp->isEnabled) {
    *buf = '\0';
    return X_NO_SERVICE;
  }

  // From here on L is the number of characters actually buffered.
  if(L > length) L = length-1;

  // Discard "\r\n"
  if(foundTerms == 2) L -= 2;

  // Terminate string in buffer
  buf[L] = '\0';

  xdprintf("[%s]\n", buf);

  if(*buf == RESP_ERROR) {
    fprintf(stderr, "Redis-X> error message: %s\n", &buf[1]);
    return L;
  }

  return foundTerms==2 ? L : x_error(REDIS_INCOMPLETE_TRANSFER, EIO, fn, "missing \\r\\n termination");
}

/**
 * Like readToken() except it will always read 'length' number of bytes (not checking for \r\n termination)
 * It is useful mainly to read binary BULK_STRING content. For everything else, readToken() is your better
 * bet...
 *
 * \param[in]  cp        Pointer to a Redis client's private data.
 * \param[out] buf       The buffer to read into, or NULL to just consume bytes from the input.
 * \param[in]  length    Maximum number of bytes that can be read into buf.
 *
 * \return     Number of bytes successfully read (>=0), or else X_NO_SERVICE.
 */
static int rReadBytes(ClientPrivate *cp, char *buf, int length) {
  static const char *fn = "rReadBytes";
  int L;

  pthread_mutex_lock(&cp->readLock);

  if(!cp->isEnabled) {
    pthread_mutex_unlock(&cp->readLock);
    return x_error(X_NO_SERVICE, ENXIO, fn, "client not connected");
  }

  for(L=0; cp->isEnabled && L<length; L++) {
    // Read a chunk of available data from the socket...
    if(cp->next >= cp->available) {
      int status = rReadChunkAsync(cp);
      if(status) {
        pthread_mutex_unlock(&cp->readLock);
        return x_trace(fn, NULL, status);
      }
    }

    if(buf) buf[L] = cp->in[cp->next++];
  }

  pthread_mutex_unlock(&cp->readLock);

  return L;
}

/// \cond PRIVATE

/**
 * Sends a sequence of bytes to the desired socket.
 *
 * \param sock          The socket file descriptor.
 * \param buf           Pointer to the buffer containing the data to be sent.
 * \param length        The number of bytes that should be sent from the buffer.
 * \param isLast        TRUE if this is the last component of a longer message, or FALSE
 *                      if more data will follow imminently.
 * \return              0 if the data was successfully sent, otherwise the errno set by send().
 *
 */
static int rSendBytesAsync(ClientPrivate *cp, const char *buf, int length, boolean isLast) {
#if SEND_YIELD_COUNT > 0
  static int count;   // Total bytes sent;
#endif

  const int sock = cp->socket;      // Local copy of socket fd that won't possibly change mid-call.
  char *from = (char *) buf;                 // Pointer to the next byte to send from buf...

  if(!buf) return X_NULL;

  xdprintf(" >>> '%s'\n", buf);

  if(!cp->isEnabled) return X_NO_INIT;
  if(sock < 0) return X_NO_INIT;

  while(length > 0) {
    int n;

#if __linux__
    // Linux supports flagging outgoing messages to inform it whether or not more
    // imminent data is on its way
    n = send(sock, from, length, isLast ? (rIsLowLatency(cp) ? MSG_EOR : 0) : MSG_MORE);
#else
    // LynxOS PPCs do not have MSG_MORE, and MSG_EOR behaves differently -- to the point where it
    // can produce kernel panics. Stay safe and send messages with no flag, same as write()
    // On LynxOS write() has wider implementation than send(), including UNIX sockets...
    n = send(sock, from, length, 0);
#endif

    if(n < 0) return rTransmitError(cp, "send");

    from += n;
    length -= n;

#if SEND_YIELD_COUNT > 0
    if(++count % SEND_YIELD_COUNT == 0) sched_yield();
#endif
  }

  return X_SUCCESS;
}

/// \endcond

/**
 * Returns the redis client for a given connection type in a Redis instance.
 *
 * \param redis         Pointer to a Redis instance.
 * \param channel       REDISX_INTERACTIVE_CHANNEL, REDISX_PIPELINE_CHANNEL, or REDISX_SUBSCRIPTION_CHANNEL
 *
 * \return      Pointer to the matching Redis client, or NULL if the channel argument is invalid.
 *
 */
RedisClient *redisxGetClient(Redis *redis, enum redisx_channel channel) {
  RedisPrivate *p;

  if(redis == NULL) {
    x_error(0, EINVAL, "redisxGetClient", "redis is NULL");
    return NULL;
  }

  p = (RedisPrivate *) redis->priv;
  if(channel < 0 || channel >= REDISX_CHANNELS) return NULL;
  return &p->clients[channel];
}


/**
 * Returns the redis client for a given connection type in a Redis instance, with the exclusive access lock
 * if the client is valid and is connected, or else NULL. It is effectively the combination of `redisxGetClient()`
 * followed by `redisxLockConnected()`.
 *
 * @param redis     Pointer to a Redis instance.
 * @param channel   REDISX_INTERACTIVE_CHANNEL, REDISX_PIPELINE_CHANNEL, or REDISX_SUBSCRIPTION_CHANNEL
 * @return          The locked client, if it is enabled, or NULL if the redis argument is NULL, the channel is
 *                  invalid, or the requested client is not currently connected.
 *
 * @sa redisxGetClient()
 * @sa redisxUnlockClient()
 * @sa redisxLockConnected()
 */
RedisClient *redisxGetLockedConnectedClient(Redis *redis, enum redisx_channel channel) {
  static const char *fn = "redisxGetLockedConnectedClient";

  RedisClient *cl = redisxGetClient(redis, channel);
  if(!cl) return x_trace_null(fn, NULL);

  if(redisxLockConnected(cl) != X_SUCCESS) return x_trace_null(fn, NULL);
  return cl;
}

/**
 * Get exclusive write access to the specified REDIS channel.
 *
 * \param cl        Pointer to the Redis client instance.
 *
 * \return          X_SUCCESS           if the exclusive lock for the channel was successfully obtained
 *                  X_FAILURE           if pthread_mutex_lock() returned an error
 *                  X_NULL              if the client is NULL.
 *
 * @sa redisxLockConnected()
 * @sa redisxUnlockClient()
 */
int redisxLockClient(RedisClient *cl) {
  static const char *fn = "redisxLockClient";
  ClientPrivate *cp;
  int status;

  if(cl == NULL) return x_error(X_NULL, EINVAL, fn, "client is NULL");
  cp = (ClientPrivate *) cl->priv;

  status = pthread_mutex_lock(&cp->writeLock);
  if(status) return x_error(X_FAILURE, errno, fn, "mutex error");

  return X_SUCCESS;
}

/**
 * Lock a channel, but only if it has been enabled for communication.
 *
 * \param cl     Pointer to the Redis client instance
 *
 * \return       X_SUCCESS (0)          if an excusive lock to the channel has been granted.
 *               X_FAILURE              if pthread_mutex_lock() returned an error
 *               X_NULL                 if the client is NULL
 *               REDIS_INVALID_CHANNEL  if the channel is not enabled/connected.
 *
 * @sa redisxLockClient()
 * @sa redisxUnlockClient()
 * @sa redisxGetLockedConnectedClient()
 */
int redisxLockConnected(RedisClient *cl) {
  static const char *fn = "redisxLockConnected";
  const ClientPrivate *cp;

  prop_error(fn, redisxLockClient(cl));

  cp = (ClientPrivate *) cl->priv;
  if(!cp->isEnabled) {
    redisxUnlockClient(cl);
    return x_error(X_NO_SERVICE, ENXIO, fn, "client is not connected");
  }

  return X_SUCCESS;
}

/**
 * Relinquish exclusive write access to the specified REDIS channel
 *
 * \param cl        Pointer to the Redis client instance
 *
 * \return          X_SUCCESS           if the exclusive lock for the channel was successfully obtained
 *                  X_FAILURE           if pthread_mutex_lock() returned an error
 *                  X_NULL              if the client is NULL
 *
 * @sa redisxLockClient()
 * @sa redisxLockConnected()
 */
int redisxUnlockClient(RedisClient *cl) {
  static const char *fn = "redisxUnlockClient";
  ClientPrivate *cp;
  int status;

  if(cl == NULL) return x_error(X_NULL, EINVAL, fn, "client is NULL");
  cp = (ClientPrivate *) cl->priv;

  status = pthread_mutex_unlock(&cp->writeLock);
  if(status) return x_error(X_FAILURE, errno, fn, "mutex error");

  return X_SUCCESS;
}

/**
 * Instructs Redis to skip sending a reply for the next command.
 *
 * Sends <code>CLIENT REPLY SKIP</code>
 *
 * \param cl            Pointer to the Redis client to use.
 *
 * \return              X_SUCCESS (0) on success or an error code on failure, is either X_NO_SERVICE
 *                      (if not connected to the REDIS server on the requested channel)
 *                      or the errno set by send().
 *
 *                          X_NULL      if the client is NULL.
 */
int redisxSkipReplyAsync(RedisClient *cl) {
  static const char *fn = "redisSkipReplyAsync";
  static const char cmd[] = "*3\r\n$6\r\nCLIENT\r\n$5\r\nREPLY\r\n$4\r\nSKIP\r\n";

  if(cl == NULL) return x_error(X_NULL, EINVAL, fn, "client is NULL");

  prop_error(fn, rSendBytesAsync((ClientPrivate *) cl->priv, cmd, sizeof(cmd) - 1, TRUE));

  return X_SUCCESS;
}

/**
 * Starts an atomic Redis transaction block, by sending <code>MULTI</code> on the specified client connection.
 * Redis transaction blocks behave just like scripts (in fact they are effectively improptu scripts
 * themselves). As such the rules of Redis scripting apply, such as you cannot call LUA from within
 * a transaction block (which is a real pity...)
 *
 * Once you start a transaction block you may ignore all acknowledgedments such as <code>OK</code> and
 * <code>QUEUED</code> responses that Redis sends back. These will be 'processed' in bulk by redisEndBlockAsync(),
 * at the end of the transaction block.
 *
 * \param cl        Pointer to a Redis client.
 *
 * \return          X_SUCCESS (0)   if successful, or
 *                  X_NULL          if the Redis client is NULL
 *
 *                  or else the error set by send().
 *
 * @sa redisxExecBlockAsync()
 * @sa redisxAbortBlockAsync()
 *
 */
int redisxStartBlockAsync(RedisClient *cl) {
  static const char *fn = "redisxStartBlockAsync";
  static const char cmd[] = "*1\r\n$5\r\nMULTI\r\n";

  if(cl == NULL) return x_error(X_NULL, EINVAL, fn, "client is NULL");

  prop_error(fn, rSendBytesAsync((ClientPrivate *) cl->priv, cmd, sizeof(cmd) - 1, TRUE));

  return X_SUCCESS;
}

/**
 * Abort an atomic transaction block. It sends <code>DISCARD</code>.
 *
 * \param cl    Pointer to a Redis client
 *
 * \return      X_SUCCESS (0) if successful or else an error code from send() (see errno.h).
 *
 * @sa redisxStartBlockAsync()
 *
 */
int redisxAbortBlockAsync(RedisClient *cl) {
  static const char *fn = "redisxAbortBlockAsync";
  static const char cmd[] = "*1\r\n$7\r\nDISCARD\r\n";

  if(cl == NULL) return x_error(X_NULL, EINVAL, fn, "client is NULL");

  prop_error(fn, rSendBytesAsync((ClientPrivate *) cl->priv, cmd, sizeof(cmd) - 1, TRUE));

  redisxIgnoreReplyAsync(cl);

  return X_SUCCESS;
}

/**
 * Finish and execute an atomic transaction block. It sends <code>EXEC</code>, skips through all
 * <code>OK</code> and <code>QUEUED</code> acknowledgements, and returns the reply to the transaction
 * block itself.
 *
 * \param cl    Pointer to a Redis client
 *
 * \return      The array RESP returned by EXEC, or NULL if there was an error.
 *
 * @sa redisxStartBlockAsync()
 * @sa redisxAbortBlockAsync()
 *
 */
RESP *redisxExecBlockAsync(RedisClient *cl) {
  static const char *fn = "redisxExecBlockAsync";
  static const char cmd[] = "*1\r\n$4\r\nEXEC\r\n";

  int status;

  if(cl == NULL) {
    x_error(0, EINVAL, fn, "client is NULL");
    return NULL;
  }

  status = redisxSkipReplyAsync(cl);
  if(status) return x_trace_null(fn, NULL);

  status = rSendBytesAsync((ClientPrivate *) cl->priv, cmd, sizeof(cmd) - 1, TRUE);
  if(status) return x_trace_null(fn, NULL);

  for(;;) {
    RESP *reply = redisxReadReplyAsync(cl);
    if(!reply) return x_trace_null(fn, NULL);

    if(reply->type == RESP_ARRAY) return reply;
    if(reply->type == RESP_ERROR) {
      redisxDestroyRESP(reply);
      return NULL;
    }
    redisxDestroyRESP(reply);
  }

  return NULL;
}

/**
 * Send a command (with up to 3 arguments) to the REDIS server. The caller must have an
 * exclusive lock on the client for this version. The arguments supplied will be used up
 * to the first non-NULL value.
 *
 * \param cl            Pointer to the Redis client instance.
 * \param command       REDIS command string.
 * \param arg1          Optional first string argument or NULL.
 * \param arg2          Optional second string argument or NULL.
 * \param arg3          Optional third string argument or NULL.
 *
 * \return              0 on success or an error code on failure, is either X_NO_SERVICE
 *                      (if not connected to the REDIS server on the requested channel)
 *                      or the errno set by send().
 */
int redisxSendRequestAsync(RedisClient *cl, const char *command, const char *arg1, const char *arg2, const char *arg3) {
  static const char *fn = "redisxSendRequestAsync";

  const char *args[] = { command, arg1, arg2, arg3 };
  int n;

  if(cl == NULL) return x_error(X_NULL, EINVAL, fn, "client is NULL");
  if(command == NULL) return x_error(X_NAME_INVALID, EINVAL, fn, "command is NULL");

  // Count the non-null arguments...
  if(arg1 == NULL) n = 1;
  else if(arg2 == NULL) n = 2;
  else if(arg3 == NULL) n = 3;
  else n = 4;

  prop_error("redisxSendRequestAsync", redisxSendArrayRequestAsync(cl, (char **) args, NULL, n));
  return X_SUCCESS;
}

/**
 * Send a Redis request with an arbitrary number of arguments.
 *
 * \param cl            Pointer to the Redis client.
 * \param args          The array of string arguments to send.
 * \param lengths       Array indicating the number of bytes to send from each string argument. Zero
 *                      or negative values can be used to determine the string length automatically
 *                      using strlen(), and the length argument itself may be NULL to determine the
 *                      lengths of all string arguments automatically.
 * \param n             The number of arguments to send.
 *
 * \return              0 on success or an error code on failure, is either X_NO_SERVICE
 *                      (if not connected to the REDIS server on the requested channel)
 *                      or the errno set by send().
 */
int redisxSendArrayRequestAsync(RedisClient *cl, char *args[], int lengths[], int n) {
  const char *fn = "redisxSendArrayRequestAsync";
  char buf[REDISX_CMDBUF_SIZE];
  int i, L;
  ClientPrivate *cp;

  if(cl == NULL) return x_error(X_NULL, EINVAL, fn, "client is NULL");
  cp = (ClientPrivate *) cl->priv;

  // Send the number of string elements in the command...
  L = sprintf(buf, "*%d\r\n", n);

  for(i=0; i<n; i++) {
    int l, L1;

    if(!lengths) l = strlen(args[i]);
    else l = lengths[i] > 0 ? lengths[i] : strlen(args[i]);

    L += sprintf(buf + L, "$%d\r\n", l);

    // length of next RESP the bulk string component including \r\n\0 termination.
    L1 = l + 3;

    if((L + L1) > REDISX_CMDBUF_SIZE) {
      // If buf cannot include the next argument, then flush the buffer...
      prop_error(fn, rSendBytesAsync(cp, buf, L, FALSE));

      L = 0;

      // If the next argument does not fit into the buffer, then send it piecemeal
      if(L1 > REDISX_CMDBUF_SIZE) {
        prop_error(fn, rSendBytesAsync(cp, args[i], l, FALSE));
        prop_error(fn, rSendBytesAsync(cp, "\r\n", 2, i == (n-1)));
      }
      else {
        memcpy(buf, args[i], l);            // Copy argument into buffer.
        L = l + sprintf(buf+l, "\r\n");     // Add \r\n\0...
      }
    }
    else {
      memcpy(buf+L, args[i], l);            // Copy argument into buffer.
      L += l;
      L += sprintf(buf+L, "\r\n");          // Add \r\n\0
    }
  }

  // flush the remaining bits in the buffer...
  if(L > 0) {
    prop_error(fn, rSendBytesAsync(cp, buf, L, TRUE));
  }

  pthread_mutex_lock(&cp->pendingLock);
  cp->pendingRequests++;
  pthread_mutex_unlock(&cp->pendingLock);

  return X_SUCCESS;
}

/**
 * Silently consumes a reply from the specified Redis channel.
 *
 * \param cl    Pointer to a Redis channel.
 *
 * \return      X_SUCCESS if a response was successfully consumed, or
 *              REDIS_NULL if a valid response could not be obtained.
 *
 */
int redisxIgnoreReplyAsync(RedisClient *cl) {
  static const char *fn = "redisxIgnoreReplyAsync";
  RESP *resp;

  if(cl == NULL) return x_error(X_NULL, EINVAL, fn, "client is NULL");

  resp = redisxReadReplyAsync(cl);
  if(resp == NULL) return x_trace(fn, NULL, REDIS_NULL);
  else redisxDestroyRESP(resp);
  return X_SUCCESS;
}

/**
 * Reads a response from Redis and returns it.
 *
 * \param cl    Pointer to a Redis channel
 *
 * \return      The RESP structure for the reponse received from Redis, or NULL if an error was encountered
 *              (errno will be set to describe the error, which may either be an errno produced by recv()
 *              or EBADMSG if the message was corrupted and/or unparseable.
 */
RESP *redisxReadReplyAsync(RedisClient *cl) {
  static const char *fn = "redisxReadReplyAsync";

  ClientPrivate *cp;
  RESP *resp = NULL;
  char buf[REDIS_SIMPLE_STRING_SIZE+2];   // +<string>\0
  int size = 0;
  int status = X_SUCCESS;

  if(cl == NULL) return NULL;
  cp = (ClientPrivate *) cl->priv;

  if(!cp->isEnabled) return NULL;

  size = rReadToken(cp, buf, REDIS_SIMPLE_STRING_SIZE + 1);
  if(size < 0) {
    // Either read/recv had an error, or we got garbage...
    if(cp->isEnabled) x_trace_null(fn, NULL);
    cp->isEnabled = FALSE;  // Disable this client so we don't attempt to read from it again...
    return NULL;
  }

  resp = (RESP *) calloc(1, sizeof(RESP));
  if(!resp) {
    perror("ERROR! calloc() error");
    exit(errno);
  }
  resp->type = buf[0];

  // Get the integer / size value...
  if(resp->type == RESP_ARRAY || resp->type == RESP_INT || resp->type == RESP_BULK_STRING) {
    char *tail;
    resp->n = strtol(&buf[1], &tail, 10);
    if(errno == ERANGE || tail == &buf[1]) {
      fprintf(stderr, "WARNING! Redis-X : unparseable dimension '%s'\n", &buf[1]);
      status = X_PARSE_ERROR;
    }
  }

  // Now get the body of the response...
  if(!status) switch(resp->type) {

    case RESP_ARRAY: {
      RESP **component;
      int i;

      if(resp->n <= 0) break;

      resp->value = (RESP **) malloc(resp->n * sizeof(RESP *));
      if(resp->value == NULL) {
        status = x_error(X_FAILURE, errno, fn, "malloc() error (%d RESP)", resp->n);
        // We should get the data from the input even if we have nowhere to store...
      }

      component = (RESP **) resp->value;

      for(i=0; i<resp->n; i++) {
        RESP* r = redisxReadReplyAsync(cl);     // Always read RESP even if we don't have storage for it...
        if(resp->value) component[i] = r;
      }

      // Consistency check. Discard response if incomplete (because of read errors...)
      if(resp->value) for(i = 0; i < resp->n; i++) if(component[i] == NULL) {
        fprintf(stderr, "WARNING! Redis-X : incomplete array received (index %d of %d).\n", (i+1), resp->n);
        if(!status) status = REDIS_INCOMPLETE_TRANSFER;
        break;
      }

      break;
    }

    case RESP_BULK_STRING:
      if(resp->n < 0) break;                          // no string token following!

      resp->value = malloc(resp->n + 2);              // <string>\r\n
      if(resp->value == NULL) {
        status = x_error(X_FAILURE, errno, fn, "malloc() error (%d bytes)", (resp->n + 2));
        fprintf(stderr, "WARNING! Redis-X : not enough memory for bulk string reply (%d bytes). Skipping.\n", (resp->n + 2));
        // We still want to consume the bytes from the input...
      }

      size = rReadBytes(cp, (char *) resp->value, resp->n + 2);

      if(size < 0) {
        if(!status) status = size;
      }
      else if(resp->value) {
        ((char *) resp->value)[resp->n] = '\0';
        xdprintf("\"%s\"\n", (char *) resp->value);
      }

      break;

    case RESP_SIMPLE_STRING:
    case RESP_ERROR:
      resp->value = malloc(size);

      if(resp->value == NULL) {
        status = x_error(X_FAILURE, errno, fn, "malloc() error (%d bytes)", size);
        fprintf(stderr, "WARNING! Redis-X : not enough memory for simple string reply (%d bytes). Skipping.\n", size);
        break;
      }

      memcpy(resp->value, &buf[1], size-1);
      resp->n = size-1;
      ((char *)resp->value)[resp->n] = '\0';

      break;

    case RESP_INT:          // Nothing left to do for INT type response.
      break;

    default:
      // FIXME workaround for Redis 4.x improper OK reply to QUIT
      if(!strcmp(buf, "OK")) {
        resp->type = RESP_SIMPLE_STRING;
        resp->value = strdup("OK");
      }
      else if(cp->isEnabled) {
        fprintf(stderr, "WARNING! Redis-X : invalid type '%c' in '%s'\n", buf[0], buf);
        status = REDIS_UNEXPECTED_RESP;
      }
  }

  // Check for errors, and return NULL if there were any.
  if(status) {
    redisxDestroyRESP(resp);
    return x_trace_null(fn, NULL);
  }

  pthread_mutex_lock(&cp->pendingLock);
  cp->pendingRequests--;
  pthread_mutex_unlock(&cp->pendingLock);

  return resp;
}

/**
 * Sends a `RESET` request to the specified Redis client. The server will perform a reset as if the
 * client disconnected and reconnected again.
 *
 * @param cl    The Redis client
 * @return      X_SUCCESS (0) if successful, or else an error code (&lt;0) from redisx.h / xchange.h.
 */
int redisxResetClient(RedisClient *cl) {
  static const char *fn = "redisxResetClient";

  int status = X_SUCCESS;

  if(cl == NULL) return x_error(X_NULL, EINVAL, fn, "client is NULL");

  prop_error(fn, redisxLockConnected(cl));

  status = redisxSendRequestAsync(cl, "RESET", NULL, NULL, NULL);
  if(!status) {
    RESP *reply = redisxReadReplyAsync(cl);
    status = redisxCheckRESP(reply, RESP_SIMPLE_STRING, 0);
    if(!status) if(strcmp("RESET", (char *) reply->value) != 0)
      status = x_error(REDIS_UNEXPECTED_RESP, EBADE, fn, "expected 'RESET', got '%s'", (char *) reply->value);
    redisxDestroyRESP(reply);
  }

  redisxUnlockClient(cl);

  prop_error(fn, status);

  return X_SUCCESS;
}
