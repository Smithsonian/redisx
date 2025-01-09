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
#include <ctype.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#if __Lynx__
#  include <socket.h>
#else
#  include <sys/socket.h>
#endif

#include "redisx-priv.h"

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

#define trprintf if(debugTraffic) printf  ///< Use for debugging Redis bound traffic

int debugTraffic = FALSE;    ///< Whether to print excerpts of all traffic to/from the Redis server.

/// \endcond

/// \cond PROTECTED

/**
 * Checks that a redis instance is valid.
 *
 * @param cl      The Redis client instance
 * @return        X_SUCCESS (0) if the client is valid, or X_NULL (errno = EINVAL) if the argument
 *                is NULL, or else X_NO_INIT (errno = ENXIO) if the redis instance is not initialized.
 */
int rCheckClient(const RedisClient *cl) {
  static const char *fn = "rCheckRedis";
  if(!cl) return x_error(X_NULL, EINVAL, fn, "Redis client is NULL");
  if(!cl->priv) return x_error(X_NO_INIT, ENXIO, fn, "Redis client is not initialized");
  return X_SUCCESS;
}

/// \endcond

/**
 * Specific call for dealing with socket level transmit (send/rcv) errors. It prints a descriptive message to
 * sdterr, and calls the configured user transmit error handler routine, and either exists the program
 * (if redisExitOnConnectionError() is set), or returns X_NO_SERVICE.
 *
 * @param cp    Pointer to the client's private data structure on which the error occurred.
 * @param op    The operation that failed, e.g. 'send' or 'read'.
 * @return      X_NO_SERVICE if the client cannot read / write any data, or else X_TIMEDOUT if the operation
 *              simply timed out but the client is still usable in principle.
 *
 * @sa redisxSetSocketErrorHandler()
 */
static int rTransmitErrorAsync(ClientPrivate *cp, const char *op) {
  int status = X_NO_SERVICE;

  if(cp->isEnabled) {
    RedisPrivate *p = (RedisPrivate *) cp->redis->priv;
    // Let the handler disconnect, if it wants to....
    if(p->config.transmitErrorFunc) {
      p->config.transmitErrorFunc(cp->redis, cp->idx, op);
      if(cp->isEnabled) {
        if(errno == EAGAIN || errno == EWOULDBLOCK) status = x_error(X_TIMEDOUT, errno, "rTransmitErrorAsync", "%s timed out on %s channel %d\n", op, cp->redis->id, cp->idx);
        else status = x_error(X_NO_SERVICE, errno, "rTransmitErrorAsync", "%s failed on %s channel %d\n", op, cp->redis->id, cp->idx);
      }
    }
  }
  return status;
}

/**
 * Reads a chunk of data into the client's receive holding buffer.
 *
 * @param cp        Pointer to the private data of the client.
 * @return          X_SUCCESS (0) if successful, or else an appropriate error (see xchange.h).
 */
static int rReadChunkAsync(ClientPrivate *cp) {
  const int sock = cp->socket;      // Local copy of socket fd that won't possibly change mid-call.
  struct pollfd pfd = {};
  int status;

  if(sock < 0) return x_error(X_NO_SERVICE, ENOTCONN, "rReadChunkAsync", "client %d: not connected", cp->idx);

  // Reset errno prior to the call.
  errno = 0;

  // Wait for data to be available on the input.
  pfd.fd = sock;
  pfd.events = POLLIN;

  status = poll(&pfd, 1, cp->timeoutMillis > 0 ? cp->timeoutMillis : -1);

  if(status < 1) cp->available = status;
  else if(pfd.revents & POLLIN) {
    cp->next = 0;

#if WITH_TLS
    if(cp->ssl) {
      // For SSL we cannot have concurrent reads/writes...
      pthread_mutex_lock(&cp->writeLock);
      cp->available = SSL_read(cp->ssl, cp->in, REDISX_RCVBUF_SIZE);
      pthread_mutex_unlock(&cp->writeLock);
    }
    else
#endif
    cp->available = recv(sock, cp->in, REDISX_RCVBUF_SIZE, 0);
    trprintf(" ... read %d bytes from client %d socket.\n", cp->available, cp->idx);
  }
  else cp->available = -1;

  if(cp->available <= 0) {
    status = rTransmitErrorAsync(cp, "read");
    if(cp->available == 0) errno = ECONNRESET;        // 0 return is remote cleared connection. So set ECONNRESET...
    if(cp->isEnabled) x_trace("rReadChunkAsync", NULL, status);
    return status;
  }
  return X_SUCCESS;
}

/**
 * Tries to read a Redis "\r\n" terminated token into the specified buffer, using up to the specified amount
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
    return x_error(X_NO_SERVICE, ENOTCONN, fn, "client is not connected");
  }

  for(L=0; cp->isEnabled && foundTerms < 2; L++) {
    char c;

    // Read a chunk of available data from the socket...
    if(cp->next >= cp->available) {
      int status = rReadChunkAsync(cp);
      if(status) {
        pthread_mutex_unlock(&cp->readLock);
        if(cp->isEnabled) return x_trace(fn, NULL, status);
        return status;
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
    if(L > 0) return x_trace(fn, NULL, X_NO_SERVICE);
    return X_NO_SERVICE;
  }

  // From here on L is the number of characters actually buffered.
  if(L > length) L = length-1;

  // Discard "\r\n"
  if(foundTerms == 2) L -= 2;

  // Terminate string in buffer
  buf[L] = '\0';

  trprintf("[%s]\n", buf);

  if(*buf == RESP_ERROR) {
    fprintf(stderr, "Redis-X> error message: %s\n", &buf[1]);
    return L;
  }

  return foundTerms==2 ? L : x_error(REDIS_INCOMPLETE_TRANSFER, EBADMSG, fn, "missing \\r\\n termination");
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
    return x_error(X_NO_SERVICE, ENOTCONN, fn, "client not connected");
  }

  for(L=0; cp->isEnabled && L<length; L++) {
    // Read a chunk of available data from the socket...
    if(cp->next >= cp->available) {
      int status = rReadChunkAsync(cp);
      if(status) {
        pthread_mutex_unlock(&cp->readLock);
        if(cp->isEnabled) return x_trace(fn, NULL, status);
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
 * \return              0 if the data was successfully sent, or X_NO_SERVICE if there was
 *                      an error with send().
 *
 */
static int rSendBytesAsync(ClientPrivate *cp, const char *buf, int length, boolean isLast) {
  static const char *fn = "rSendBytesAsync";

#if SEND_YIELD_COUNT > 0
  static int count;   // Total bytes sent;
#endif

  const int sock = cp->socket;      // Local copy of socket fd that won't possibly change mid-call.
  char *from = (char *) buf;                 // Pointer to the next byte to send from buf...

  if(!buf) return x_error(X_NULL, EINVAL, fn, "buffer is NULL");

  trprintf(" >>> '%s'\n", buf);

  if(!cp->isEnabled) return x_error(X_NO_SERVICE, ENOTCONN, fn, "client %d: disabled", cp->idx);
  if(sock < 0) return x_error(X_NO_SERVICE, ENOTCONN, fn, "client %d: not connected", cp->idx);

  while(length > 0) {
    int n;

#if WITH_TLS
    if(cp->ssl) n = SSL_write(cp->ssl, from, length);
    else
#endif
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

    if(n <= 0) {
      int status = rTransmitErrorAsync(cp, "send");
      if(cp->isEnabled) x_trace(fn, NULL, status);
      cp->isEnabled = FALSE;
      return status;
    }

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
 * \return      Pointer to the matching Redis client, or NULL if redis is null (EINVAL) or not initialized
 *              (EAGAIN) or if the channel argument is invalid (ECHRNG).
 *
 * @sa redisxGetLockedConnectedClient()
 */
RedisClient *redisxGetClient(Redis *redis, enum redisx_channel channel) {
  static const char *fn = "redisxGetClient";

  RedisPrivate *p;

  if(redisxCheckValid(redis) != X_SUCCESS) return x_trace_null(fn, NULL);

  p = (RedisPrivate *) redis->priv;
  if(channel < 0 || channel >= REDISX_CHANNELS) {
    x_error(0, EINVAL, fn, "channel %d is our of range", channel);
    return NULL;
  }
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
  RedisClient *cl = redisxGetClient(redis, channel);
  if(redisxLockConnected(cl) != X_SUCCESS) return x_trace_null("redisxGetLockedConnectedClient", NULL);
  return cl;
}

/**
 * Get exclusive write access to the specified Redis channel.
 *
 * \param cl        Pointer to the Redis client instance.
 *
 * \return          X_SUCCESS           if the exclusive lock for the channel was successfully obtained, or
 *                  X_FAILURE           if pthread_mutex_lock() returned an error, or
 *                  X_NULL              if the client is NULL, or
 *                  X_NO_INIT           if the client was not initialized.
 *
 * @sa redisxLockConnected()
 * @sa redisxUnlockClient()
 */
int redisxLockClient(RedisClient *cl) {
  static const char *fn = "redisxLockClient";
  ClientPrivate *cp;
  int status;

  prop_error(fn, rCheckClient(cl));

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
 * \return       X_SUCCESS (0)          if an excusive lock to the channel has been granted, or
 *               X_FAILURE              if pthread_mutex_lock() returned an error, or
 *               X_NULL                 if the client is NULL, or
 *               X_NO_INIT              if the client was not initialized.
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
    return x_error(X_NO_SERVICE, ENOTCONN, fn, "client is not connected");
  }

  return X_SUCCESS;
}

/**
 * Relinquish exclusive write access to the specified Redis channel
 *
 * \param cl        Pointer to the Redis client instance
 *
 * \return          X_SUCCESS           if the exclusive lock for the channel was successfully obtained, or
 *                  X_FAILURE           if pthread_mutex_lock() returned an error, or
 *                  X_NULL              if the client is NULL, or
 *                  X_NO_INIT           if the client was not initialized.
 *
 * @sa redisxLockClient()
 * @sa redisxLockConnected()
 */
int redisxUnlockClient(RedisClient *cl) {
  static const char *fn = "redisxUnlockClient";
  ClientPrivate *cp;
  int status;

  prop_error(fn, rCheckClient(cl));

  cp = (ClientPrivate *) cl->priv;

  status = pthread_mutex_unlock(&cp->writeLock);
  if(status) return x_error(X_FAILURE, errno, fn, "mutex error");

  return X_SUCCESS;
}

/**
 * Instructs Redis to skip sending a reply for the next command. This function should be called
 * with an exclusive lock on a connected client, and just before redisxSendRequest() or
 * redisxSendArrayRequestAsync().
 *
 * Sends <code>CLIENT REPLY SKIP</code>
 *
 * \param cl            Pointer to the Redis client to use.
 *
 * \return              X_SUCCESS (0) on success or X_NULL if the client is NULL, or else X_NO_SERVICE
 *                      if not connected to the Redis server on the requested channel, or if send()
 *                      failed, or else X_NO_INIT if the client was not initialized.
 *
 * @sa redixSendRequestAsync()
 * @sa redisxSendArrayRequestAsync()
 * @sa redisxGetLockedConnected()
 */
int redisxSkipReplyAsync(RedisClient *cl) {
  static const char *fn = "redisSkipReplyAsync";
  static const char cmd[] = "*3\r\n$6\r\nCLIENT\r\n$5\r\nREPLY\r\n$4\r\nSKIP\r\n";

  prop_error(fn, rCheckClient(cl));
  prop_error(fn, rSendBytesAsync((ClientPrivate *) cl->priv, cmd, sizeof(cmd) - 1, TRUE));

  return X_SUCCESS;
}

/**
 * Starts an atomic Redis transaction block, by sending <code>MULTI</code> on the specified client connection.
 * This function should be called with an exclusive lock on a connected client.
 *
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
 *                  X_NULL          if the Redis client is NULL, or
 *                  X_NO_SERVICE    if not connected to the client server or if send() failed, or
 *                  X_NO_INIT       if the client was not initialized.
 *
 * @sa redisxExecBlockAsync()
 * @sa redisxAbortBlockAsync()
 * @sa redisxGetLockedConnected()
 */
int redisxStartBlockAsync(RedisClient *cl) {
  static const char *fn = "redisxStartBlockAsync";
  static const char cmd[] = "*1\r\n$5\r\nMULTI\r\n";

  prop_error(fn, rCheckClient(cl));
  prop_error(fn, rSendBytesAsync((ClientPrivate *) cl->priv, cmd, sizeof(cmd) - 1, TRUE));

  return X_SUCCESS;
}

/**
 * Abort an atomic transaction block. It sends <code>DISCARD</code>. This function should be called
 * with an exclusive lock on a connected client, and after starting an execution block with
 * redisxStartBlockAsync().
 *
 * \param cl    Pointer to a Redis client
 *
 * \return      X_SUCCESS (0)   if successful, or X_NULL if the client is NULL, or
 *              X_NO_SERVICE    if not connected ot the client or if send() failed, or
 *              X_NO_INIT       if the client was not initialized.
 *
 * @sa redisxStartBlockAsync()
 */
int redisxAbortBlockAsync(RedisClient *cl) {
  static const char *fn = "redisxAbortBlockAsync";
  static const char cmd[] = "*1\r\n$7\r\nDISCARD\r\n";

  prop_error(fn, rCheckClient(cl));
  prop_error(fn, rSendBytesAsync((ClientPrivate *) cl->priv, cmd, sizeof(cmd) - 1, TRUE));

  redisxIgnoreReplyAsync(cl);

  return X_SUCCESS;
}

/**
 * Finish and execute an atomic transaction block. It sends <code>EXEC</code>, skips through all
 * <code>OK</code> and <code>QUEUED</code> acknowledgements, and returns the reply to the transaction
 * block itself. This function should be called with an exclusive lock on a connected client.
 *
 * \param cl        Pointer to a Redis client
 * \param pStatus   Pointer to int in which to return error status. or NULL if not required.
 *
 * \return      The array RESP returned by EXEC, or NULL if there was an error.
 *
 * @sa redisxStartBlockAsync()
 * @sa redisxAbortBlockAsync()
 * @sa redisxGetLockedConnected()
 */
RESP *redisxExecBlockAsync(RedisClient *cl, int *pStatus) {
  static const char *fn = "redisxExecBlockAsync";
  static const char cmd[] = "*1\r\n$4\r\nEXEC\r\n";

  int status;

  if(rCheckClient(cl) != X_SUCCESS) return x_trace_null(fn, NULL);

  if(cl->priv == NULL) {
    x_error(0, EINVAL, fn, "client is not initialized");
    return NULL;
  }

  status = redisxSkipReplyAsync(cl);
  if(status) {
    if(pStatus) *pStatus = status;
    return x_trace_null(fn, NULL);
  }

  status = rSendBytesAsync((ClientPrivate *) cl->priv, cmd, sizeof(cmd) - 1, TRUE);
  if(status) {
    if(pStatus) *pStatus = status;
    return x_trace_null(fn, NULL);
  }

  for(;;) {
    RESP *reply = redisxReadReplyAsync(cl, pStatus);
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
 * Send a command (with up to 3 arguments) to the Redis server. The caller must have an
 * exclusive lock on the client for this version. The arguments supplied will be used up
 * to the first non-NULL value.
 *
 * Unlike its interactive counterpart, redisxRequest(), this method does not follow cluster
 * MOVED or ASK redirections automatically. It cannot, since it returns without waiting
 * for a response. To implement redirections, the caller must keep track of the asynchronous
 * requests sent, and check for redirections when processing responses via
 * redisxReadReplyAsync(). If the response is a redirection, then the caller can decide if
 * and how to re-submit the request to follow the redirection.
 *
 * \param cl            Pointer to the Redis client instance.
 * \param command       Redis command string.
 * \param arg1          Optional first string argument or NULL.
 * \param arg2          Optional second string argument or NULL.
 * \param arg3          Optional third string argument or NULL.
 *
 * \return              X_SUCCESS (0) on success or X_NULL if the client is NULL, or else
 *                      X_NO_SERVICE if not connected to the client or if send() failed
 *
 * @sa redisxSendArrayRequestAsync()
 * @sa redisxRequest()
 * @sa redisxReadReplyAsync()
 * @sa redisxGetLockedConnected()
 * @sa redisxSkipReplyAsync()
 */
int redisxSendRequestAsync(RedisClient *cl, const char *command, const char *arg1, const char *arg2, const char *arg3) {
  static const char *fn = "redisxSendRequestAsync";

  const char *args[] = { command, arg1, arg2, arg3 };
  int n;

  prop_error(fn, rCheckClient(cl));

  if(command == NULL) return x_error(X_NAME_INVALID, EINVAL, fn, "command is NULL");

  // Count the non-null arguments...
  if(arg1 == NULL) n = 1;
  else if(arg2 == NULL) n = 2;
  else if(arg3 == NULL) n = 3;
  else n = 4;

  prop_error("redisxSendRequestAsync", redisxSendArrayRequestAsync(cl, args, NULL, n));
  return X_SUCCESS;
}

/**
 * Send a Redis request with an arbitrary number of arguments. This function should be called
 * with an exclusive lock on a connected client.
 *
 * Unlike its interactive counterpart, redisxArrayRequest(), this method does not follow cluster
 * MOVED or ASK redirections automatically. It cannot, since it returns without waiting
 * for a response. To implement redirections, the caller must keep track of the asynchronous
 * requests sent, and check for redirections when processing responses via
 * redisxReadReplyAsync(). If the response is a redirection, then the caller can decide if
 * and how to re-submit the request to follow the redirection.
 *
 * \param cl            Pointer to the Redis client.
 * \param args          The array of string arguments to send. If you have an `char **` array, you
 *                      may need to cast to `(const char **)` to avoid compiler warnings.
 * \param lengths       Array indicating the number of bytes to send from each string argument. Zero
 *                      or negative values can be used to determine the string length automatically
 *                      using strlen(), and the length argument itself may be NULL to determine the
 *                      lengths of all string arguments automatically.
 * \param n             The number of arguments to send.
 *
 * \return              X_SUCCESS (0) on success or X_NULL if the client is NULL, or
 *                      X_NO_SERVICE if not connected to the client or if send() failed, or
 *                      X_NO_INIT if the client was not initialized.
 *
 * @sa redisxSendRequestAsync()
 * @sa redisxArrayRequest()
 * @sa redisxReadReplyAsync()
 * @sa redisxGetLockedConnected()
 * @sa redisxSkipReplyAsync()
 */
int redisxSendArrayRequestAsync(RedisClient *cl, const char **args, const int *lengths, int n) {
  static const char *fn = "redisxSendArrayRequestAsync";
  char buf[REDISX_CMDBUF_SIZE];
  int i, L;
  ClientPrivate *cp;

  prop_error(fn, rCheckClient(cl));

  cp = (ClientPrivate *) cl->priv;
  if(!cp->isEnabled) return x_error(X_NO_SERVICE, ENOTCONN, fn, "client is not connected");

  // Send the number of string elements in the command...
  L = sprintf(buf, "*%d\r\n", n);

  xvprintf("Redis-X> request[%d]", n);
  for(i = 0; i < n; i++) {
    if(args[i]) xvprintf(" %s", args[i]);
    if(i == 4) {
      xvprintf("...");
    }
  }
  xvprintf("\n");

  for(i = 0; i < n; i++) {
    int l, L1;

    if(!args[i]) l = 0; // Check for potential NULL parameters...
    else if(lengths) l = lengths[i] > 0 ? lengths[i] : (int) strlen(args[i]);
    else l = (int) strlen(args[i]);


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
        if(l > 0) memcpy(buf, args[i], l);            // Copy argument into buffer.
        L = l + sprintf(buf+l, "\r\n");     // Add \r\n\0...
      }
    }
    else {
      if(l > 0) memcpy(buf+L, args[i], l);            // Copy argument into buffer.
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
 * Silently consumes a reply from the specified Redis channel. This function should be called
 * with an exclusive lock on a connected client.
 *
 * \param cl    Pointer to a Redis channel.
 *
 * \return      X_SUCCESS if a response was successfully consumed, or
 *              REDIS_NULL if a valid response could not be obtained.
 *
 * @sa redisxReadReplyAsync()
 * @sa redisxGetLockedConnected()
 */
int redisxIgnoreReplyAsync(RedisClient *cl) {
  static const char *fn = "redisxIgnoreReplyAsync";
  RESP *resp;

  int status = X_SUCCESS;

  prop_error(fn, rCheckClient(cl));

  resp = redisxReadReplyAsync(cl, &status);
  if(resp == NULL) return x_trace(fn, NULL, REDIS_NULL);
  else redisxDestroyRESP(resp);
  return status;
}

static int rTypeIsParametrized(char type) {
  switch(type) {
    case RESP_INT:
    case RESP_BULK_STRING:
    case RESP_ARRAY:
    case RESP3_SET:
    case RESP3_PUSH:
    case RESP3_MAP:
    case RESP3_ATTRIBUTE:
    case RESP3_BLOB_ERROR:
    case RESP3_VERBATIM_STRING:
    case RESP3_CONTINUED:
      return TRUE;
    default:
      return FALSE;
  }
}



static void rPushMessageAsync(RedisClient *cl, RESP *resp) {
  int i;
  ClientPrivate *cp = (ClientPrivate *) cl->priv;
  RedisPrivate *p = (RedisPrivate *) cp->redis->priv;
  RESP **array;

  if(resp->n < 0) return;

  array = (RESP **) calloc(resp->n, sizeof(RESP *));
  if(!array) fprintf(stderr, "WARNING! Redis-X : not enough memory for push message (%d elements). Skipping.\n", resp->n);

  resp->value = array;

  for(i = 0; i < resp->n; i++) {
    int status = X_SUCCESS;
    RESP *r = redisxReadReplyAsync(cl, &status);
    if(status) {
      redisxDestroyRESP(resp);
      x_trace_null("rPushMessageAsync", NULL);
      return;
    }
    if(array) array[i] = r;
    else redisxDestroyRESP(r);
  }

  if(p->config.pushConsumer) p->config.pushConsumer(cl, resp, p->config.pushArg);

  redisxDestroyRESP(resp);
}

/**
 * Returns the attributes (if any) that were last sent along a response to the client.
 * This function should be called only if the caller has an exclusive lock on the client's
 * mutex. Also, there are a few rules the caller should follow:
 *
 * <ul>
 * <li>The caller should not block the client for long and return quickly. If it has
 * blocking calls, or requires extensive processing, it should make a copy of the
 * RESP first, and release the lock immediately after.</li>
 * <li>The caller must not attempt to call free() on the returned RESP</li>
 * </ul>
 *
 * Normally the user would typically call this function right after a redisxReadReplyAsync()
 * call, for which atributes are expected. The caller might also want to call
 * redisxClearAttributeAsync() before attempting to read the response to ensure that
 * the attributes returned are for the same reply from the server.
 *
 * @param cl    The Redis client instance
 * @return      The attributes last received (possibly NULL).
 *
 * @sa redisxGetAttributes()
 * @sa redisxClearAttributesAsync()
 * @sa redisxReadReplyAsync()
 * @sa redisxGetLockedConnected()
 *
 */
const RESP *redisxGetAttributesAsync(const RedisClient *cl) {
  const ClientPrivate *cp;
  if(!cl) {
    x_error(0, EINVAL, "redisxGetAttributesAsync", "client is NULL");
    return NULL;
  }

  cp = (ClientPrivate *) cl->priv;
  return cp->attributes;
}

static void rSetAttributeAsync(ClientPrivate *cp, RESP *resp) {
  redisxDestroyRESP(cp->attributes);
  cp->attributes = resp;
}

/**
 * Clears the attributes for the specified client. The caller should have an exclusive lock
 * on the client's mutex prior to making this call.
 *
 * Typically a user migh call this function prior to calling redisxReadReplyAsync() on the
 * same client, to ensure that any attributes that are available after the read will be the
 * ones that were sent with the last response from the server.
 *
 * @param cl    The Redis client instance
 * @return      X_SUCCESS (0) if successful, or else X_NULL if the client is NULL.
 *
 * @sa redisxGetAttributesAsync()
 * @sa redisxReadReplyAsync()
 * @sa redisxGetLockedConnected()
 */
int redisxClearAttributesAsync(RedisClient *cl) {
  prop_error("redisxClearAttributesAsync", rCheckClient(cl));

  rSetAttributeAsync((ClientPrivate *) cl->priv, NULL);
  return X_SUCCESS;
}

/**
 * Returns the number of bytes of response available on the given Redis client connection. This version
 * assumes the caller has exclusive access to the client.
 *
 * @param cl    a locked and connected Redis client
 * @return      the number of bytes of response available on the client, or else an error code &lt;0.
 *
 * @sa redisxGetAvailable()
 * @sa redisxGetLockConnected()
 * @sa redisxReadReplyAsync()
 */
int redisxGetAvailableAsync(RedisClient *cl) {
  static const char *fn = "redisxGetAvailable";

  ClientPrivate *cp;
  int n = 0;

  prop_error(fn, rCheckClient(cl));

  cp = cl->priv;
  if(ioctl(cp->socket, FIONREAD, &n) < 0) return x_error(X_FAILURE, errno, fn, "ioctl() error: %s", strerror(errno));

  return n;
}

/**
 * Returns the number of bytes of response available on the given Redis client connection. This is the
 * synchronized version, which will obtain a exclusive lock on the client before determining the result.
 *
 * @param cl    a locked and connected Redis client
 * @return      the number of bytes of response available on the client, or else an error code &lt;0.
 *
 * @sa redisxGetAvailableAsync()
 */
int redisxGetAvailable(RedisClient *cl) {
  static const char *fn = "redisxGetAvailable";

  int n;

  prop_error(fn, redisxLockConnected(cl));
  n = redisxGetAvailable(cl);
  redisxUnlockClient(cl);

  prop_error(fn, n);
  return n;
}

/**
 * Reads a response from Redis and returns it. It should be used with an exclusive lock on a connected
 * client, to collect responses for requests sent previously. It is up to the caller to keep track of
 * what request the response is for. The responses arrive in the same order (and same nummber) as
 * the requests that were sent out.
 *
 * To follow cluster MOVED or ASK redirections, the caller should check the reponse for redirections
 * (e.g. via redisxIsRedirected()) and then act accordingly to re-submit the corresponding request,
 * as is or with an ASKING directive to follow the redirection.
 *
 * \param cl         Pointer to a Redis channel
 * \param pStatus    Pointer to int in which to return an error status, or NULL if not required.
 *
 * \return      The RESP structure for the reponse received from Redis, or NULL if an error was encountered
 *              (errno will be set to describe the error, which may either be an errno produced by recv()
 *              or EBADMSG if the message was corrupted and/or unparseable. If the error is irrecoverable
 *              i.e., other than a timeout, the client will be disabled.)
 *
 * @sa redisxIgnoreReplyAsync()
 * @sa redisxSetReplyTimeout()
 * @sa redisxGetAvailableAsync()
 * @sa redisxSendRequestAsync()
 * @sa redisxSendArrayRequestAsync()
 * @sa redisxGetLockedConnected()
 * @sa redisxCheckRESP()
 * @sa redisxIsRedirected()
 */
RESP *redisxReadReplyAsync(RedisClient *cl, int *pStatus) {
  static const char *fn = "redisxReadReplyAsync";

  ClientPrivate *cp;
  RESP *resp = NULL;
  char buf[REDIS_SIMPLE_STRING_SIZE+2];   // +<string>\0
  int size = 0;
  int status = X_SUCCESS;

  if(rCheckClient(cl) != X_SUCCESS) return x_trace_null(fn, NULL);

  cp = cl->priv;

  if(cp == NULL) {
    x_error(0, ENOTCONN, fn, "client is not initialized");
    if(pStatus) *pStatus = X_NO_INIT;
    return NULL;
  }
  if(!cp->isEnabled) {
    x_error(0, ENOTCONN, fn, "client is not connected");
    if(pStatus) *pStatus = X_NO_SERVICE;
    return NULL;
  }

  for(;;) {
    size = rReadToken(cp, buf, REDIS_SIMPLE_STRING_SIZE + 1);
    if(size < 0) {
      // Either read/recv had an error, or we got garbage...
      if(cp->isEnabled) x_trace_null(fn, NULL);

      // If persistent error disable this client so we don't attempt to read from it again...
      if(size == X_NO_SERVICE) rCloseClientAsync(cl);

      if(pStatus) *pStatus = size;
      return NULL;
    }

    if(pStatus) *pStatus = X_SUCCESS;

    resp = (RESP *) calloc(1, sizeof(RESP));
    x_check_alloc(resp);
    resp->type = buf[0];

    // Parametrized type.
    if(rTypeIsParametrized(resp->type)) {

      if(buf[1] == '?') {
        // Streaming RESP in parts...
        for(;;) {
          RESP *r = redisxReadReplyAsync(cl, pStatus);
          if(r->type != RESP3_CONTINUED) {
            int type = r->type;
            redisxDestroyRESP(r);
            fprintf(stderr, "WARNIG! Redis-X: expected type '%c', got type '%c'.", resp->type, type);
            return resp;
          }

          if(r->n == 0) {
            if(resp->type == RESP3_PUSH || resp->type == RESP3_ATTRIBUTE) break;
            if(redisxHasComponents(resp)) break;
            return resp; // We are done, return the result.
          }

          r->type = resp->type;
          redisxAppendRESP(resp, r);
        }
      }
      else {
        // Get the integer / size value...
        char *tail;
        errno = 0;
        resp->n = (int) strtol(&buf[1], &tail, 10);
        if(errno) {
          fprintf(stderr, "WARNING! Redis-X : unparseable dimension '%s'\n", &buf[1]);
          status = X_PARSE_ERROR;
        }
      }
    }

    // Deal with push messages and attributes...
    if(resp->type == RESP3_PUSH) rPushMessageAsync(cl, resp);
    else if(resp->type == RESP3_ATTRIBUTE) rSetAttributeAsync(cp, resp);
    else break;
  }

  // Now get the body of the response...
  if(!status) switch(resp->type) {

    case RESP3_NULL:
      resp->n = 0;
      break;

    case RESP_INT:          // Nothing left to do for INT type response.
      break;

    case RESP3_BOOLEAN: {
      switch(tolower(buf[1])) {
        case 't': resp->n = TRUE; break;
        case 'f': resp->n = FALSE; break;
        default:
          resp->n = -1;
          fprintf(stderr, "WARNING! Redis-X : invalid boolean value '%c'\n", buf[1]);
          status = X_PARSE_ERROR;
      }
      break;
    }

    case RESP3_DOUBLE: {
      double *dval = (double *) calloc(1, sizeof(double));
      x_check_alloc(dval);

      *dval = xParseDouble(&buf[1], NULL);
      if(errno) {
        fprintf(stderr, "WARNING! Redis-X : invalid double value '%s'\n", &buf[1]);
        status = X_PARSE_ERROR;
      }
      resp->value = dval;
      break;
    }

    case RESP_ARRAY:
    case RESP3_SET:
    case RESP3_PUSH: {
      RESP **component;
      int i;

      if(resp->n <= 0) break;

      component = (RESP **) malloc(resp->n * sizeof(RESP *));
      if(component == NULL) {
        status = x_error(X_FAILURE, errno, fn, "malloc() error (%d RESP)", resp->n);
        // We should get the data from the input even if we have nowhere to store...
      }


      for(i = 0; i < resp->n; i++) {
        RESP *r = redisxReadReplyAsync(cl, pStatus);     // Always read RESP even if we don't have storage for it...
        if(component) component[i] = r;
        else redisxDestroyRESP(r);
      }

      // Consistency check. Discard response if incomplete (because of read errors...)
      if(component) for(i = 0; i < resp->n; i++) if(component[i] == NULL || component[i]->type == RESP3_NULL) {
        fprintf(stderr, "WARNING! Redis-X : incomplete array received (index %d of %d).\n", (i+1), resp->n);
        if(!status) status = REDIS_INCOMPLETE_TRANSFER;
        break;
      }

      resp->value = component;

      break;
    }

    case RESP3_MAP:
    case RESP3_ATTRIBUTE: {
      RedisMap *dict;
      int i;

      if(resp->n <= 0) break;

      dict = (RedisMap *) calloc(resp->n, sizeof(RedisMap));
      x_check_alloc(dict);

      for(i = 0; i < resp->n; i++) {
        RedisMap *e = &dict[i];
        e->key = redisxReadReplyAsync(cl, pStatus);
        e->value = redisxReadReplyAsync(cl, pStatus);
      }
      resp->value = dict;

      break;
    }

    case RESP_BULK_STRING:
    case RESP3_BLOB_ERROR:
    case RESP3_VERBATIM_STRING:
      if(resp->n < 0) break;                          // no string token following!

      resp->value = malloc(resp->n + 2);              // <string>\r\n
      if(resp->value == NULL) {
        status = x_error(X_FAILURE, errno, fn, "malloc() error (%d bytes)", (resp->n + 2));
        fprintf(stderr, "WARNING! Redis-X : not enough memory for bulk string reply (%d bytes). Skipping.\n", (resp->n + 2));
        // We still want to consume the bytes from the input...
      }

      size = rReadBytes(cp, (char *) resp->value, resp->n + 2);

      if(size < 0) {
        // Either read/recv had an error, or we got garbage...
        if(cp->isEnabled) x_trace_null(fn, NULL);
        if(!status) status = size;
      }
      else if(resp->value) {
        ((char *) resp->value)[resp->n] = '\0';
        trprintf("\"%s\"\n", (char *) resp->value);
      }

      break;

    case RESP_SIMPLE_STRING:
    case RESP_ERROR:
    case RESP3_BIG_NUMBER:
      resp->value = malloc(size);

      if(resp->value == NULL) {
        status = x_error(X_FAILURE, errno, fn, "malloc() error (%d bytes)", size);
        fprintf(stderr, "WARNING! Redis-X : not enough memory for simple string reply (%d bytes). Skipping.\n", size);
        break;
      }

      memcpy(resp->value, &buf[1], size-1);
      resp->n = size-1;
      ((char *)resp->value)[resp->n] = '\0';

      if(redisxClusterMoved(resp)) {
        // If cluster was reconfigured, refresh the cluster configuration automatically.
        const RedisPrivate *p = (RedisPrivate *) cp->redis->priv;
        rClusterRefresh(p->cluster);
      }

      break;

    default:
      // FIXME workaround for Redis 4.x improper OK reply to QUIT
      if(!strcmp(buf, "OK")) {
        resp->type = RESP_SIMPLE_STRING;
        resp->value = xStringCopyOf("OK");
      }
      else if(cp->isEnabled) {
        fprintf(stderr, "WARNING! Redis-X : invalid type '%c' in '%s'\n", buf[0], buf);
        status = REDIS_UNEXPECTED_RESP;
      }
  }

  // Check for errors, and return NULL if there were any.
  if(status) {
    redisxDestroyRESP(resp);
    // If persistent error disable this client so we don't attempt to read from it again...
    if(status == X_NO_SERVICE) rCloseClientAsync(cl);
    if(pStatus) *pStatus = status;
    return NULL;
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
 * @return      X_SUCCESS (0) if successful, or X_NULL if the client is NULL, or another error code
 *              (&lt;0) from redisx.h / xchange.h.
 */
int redisxResetClient(RedisClient *cl) {
  static const char *fn = "redisxResetClient";

  int status;

  prop_error(fn, redisxLockConnected(cl));

  status = redisxSendRequestAsync(cl, "RESET", NULL, NULL, NULL);
  if(!status) {
    RESP *reply = redisxReadReplyAsync(cl, &status);
    if(!status) {
      status = redisxCheckRESP(reply, RESP_SIMPLE_STRING, 0);
      if(!status) if(strcmp("RESET", (char *) reply->value) != 0)
        status = x_error(REDIS_UNEXPECTED_RESP, ENOMSG, fn, "expected 'RESET', got '%s'", (char *) reply->value);
    }
    redisxDestroyRESP(reply);
  }

  redisxUnlockClient(cl);

  prop_error(fn, status);

  return X_SUCCESS;
}
