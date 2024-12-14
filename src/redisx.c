/**
 * \file
 *
 * \date   May 4, 2018
 * \author Attila Kovacs
 *
 * \brief
 *      A Redis client library that compiles on older and newer platforms alike, such as LynxOS 3.1.0 PowerPCs.
 *      It is quite full featured, supporting multiple Redis instances, pipelining, user-specified connect and disconnect hooks,
 *      multiple user-specified subscription listeners, and user-specified pipeline processors.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>

#include "redisx-priv.h"

/// \cond PRIVATE

#if DEBUG
#define SET_PRIORITIES              FALSE       ///< Disable if you want to use gdb to debug...
#else
#endif

#define XPRIO_MIN                   (sched_get_priority_min(SCHED_RR))
#define XPRIO_MAX                   (sched_get_priority_max(SCHED_RR))
#define XPRIO_RANGE                 (XPRIO_MAX - XPRIO_MIN)

#define REDISX_LISTENER_PRIORITY    (XPRIO_MIN + (int) (REDISX_LISTENER_REL_PRIORITY * XPRIO_RANGE))

extern int debugTraffic;            ///< Whether to print excerpts of all traffic to/from the Redis server.

/// \endcond


/**
 * Checks that a redis instance is valid.
 *
 * @param redis   The Redis instance
 * @return        X_SUCCESS (0) if the instance is valid, or X_NULL if the argument is NULL,
 *                or else X_NO_INIT if the redis instance is not initialized.
 */
int redisxCheckValid(const Redis *redis) {
  static const char *fn = "rCheckRedis";
  if(!redis) return x_error(X_NULL, EINVAL, fn, "Redis instamce is NULL");
  if(!redis->priv) return x_error(X_NO_INIT, EAGAIN, fn, "Redis instance is not initialized");
  return X_SUCCESS;
}

/// \cond PROTECTED

/**
 * Waits to get exlusive access to configuring the properties of a Redis instance.
 *
 * \param redis         Pointer to a Redis instance.
 *
 */
int rConfigLock(Redis *redis) {
  prop_error("rConfigLock", redisxCheckValid(redis));
  RedisPrivate *p = (RedisPrivate *) redis->priv;
  pthread_mutex_lock(&p->configLock);
  return X_SUCCESS;
}

/**
 * Relinquish exlusive access to configuring the properties of a Redis instance.
 *
 * \param redis         Pointer to a Redis instance.
 *
 */
int rConfigUnlock(Redis *redis) {
  prop_error("rConfigUnlock", redisxCheckValid(redis));
  RedisPrivate *p = (RedisPrivate *) redis->priv;
  pthread_mutex_unlock(&p->configLock);
  return X_SUCCESS;
}

/// \endcond

/**
 * Enable or disable verbose reporting of all Redis operations (and possibly some details of them).
 * Reporting is done on the standard output (stdout). It may be useful when debugging programs
 * that use the redisx interface. Verbose reporting is DISABLED by default.
 *
 * \param value         TRUE to enable verbose reporting, or FALSE to disable.
 *
 * @sa redisxDebugTraffic()
 */
void redisxSetVerbose(boolean value) {
  xSetVerbose(value);
}

/**
 * Checks id verbose reporting is enabled.
 *
 * \return          TRUE if verbose reporting is enabled, otherwise FALSE.
 */
boolean redisxIsVerbose() {
  return xIsVerbose();
}

/**
 * Enable or disable verbose reporting of all Redis bound traffic. It may be useful when debugging
 * programs that use the redisx interface. Verbose reporting is DISABLED by default.
 *
 * \param value         TRUE to enable verbose reporting, or FALSE to disable.
 *
 * @sa redisxSetVerbose()
 */
void redisxDebugTraffic(boolean value) {
  debugTraffic = value ? TRUE : FALSE;
}


/**
 * Sets the user name to use for authenticating on the Redis server after connection. See the
 * `AUTH` Redis command for more explanation. Naturally, you need to call this prior to connecting
 * your Redis instance to have the desired effect.
 *
 * @param redis     Pointer to the Redis instance for which to set credentials
 * @param username  the password to use for authenticating on the server, or NULL to clear a
 *                  previously configured password.
 * @return          X_SUCCESS (0) if successful, X_NULL if the redis argument is NULL, or
 *                  X_ALREADY_OPEN if called after Redis was already connected.
 *
 * @sa redisxSetPassword()
 */
int redisxSetUser(Redis *redis, const char *username) {
  static const char *fn = "redisxSetUser";

  int status = X_SUCCESS;

  prop_error(fn, rConfigLock(redis));
  if(redisxIsConnected(redis)) status = x_error(X_ALREADY_OPEN, EALREADY, fn, "already connected");
  else {
    RedisPrivate *p = (RedisPrivate *) redis->priv;
    if(p->username) free(p->username);
    p->username = xStringCopyOf(username);
  }
  rConfigUnlock(redis);

  return status;
}

/**
 * Sets the password to use for authenticating on the Redis server after connection. See the AUTH
 * Redis command for more explanation. Naturally, you need to call this prior to connecting
 * your Redis instance to have the desired effect.
 *
 * @param redis     Pointer to the Redis instance for which to set credentials
 * @param passwd    the password to use for authenticating on the server, or NULL to clear a
 *                  previously configured password.
 * @return          X_SUCCESS (0) if successful, X_NULL if the redis argument is NULL, or
 *                  X_ALREADY_OPEN if called after Redis was already connected.
 *
 * @sa redisxSetUser()
 */
int redisxSetPassword(Redis *redis, const char *passwd) {
  static const char *fn = "redisxSetPassword";

  int status = X_SUCCESS;

  prop_error(fn, rConfigLock(redis));
  if(redisxIsConnected(redis)) status = x_error(X_ALREADY_OPEN, EALREADY, fn, "already connected");
  else {
    RedisPrivate *p = (RedisPrivate *) redis->priv;
    if(p->password) free(p->password);
    p->password = xStringCopyOf(passwd);
  }
  rConfigUnlock(redis);

  return status;
}


/**
 * Sets the RESP prorocol version to use for future client connections. The protocol is set with the
 * HELLO command, which was introduced in Redis 6.0.0 only. For older Redis server instances, the
 * protocol will default to RESP2. Calling this function will enable using HELLO to handshake with
 * the server.
 *
 * @param redis       The Redis server instance
 * @param protocol    REDISX_RESP2 or REDISX_RESP3.
 * @return            X_SUCCESS (0) if successful, or X_NULL if the redis argument in NULL, X_NO_INIT
 *                    if the redis instance was not initialized.
 *
 * @sa redisxGetProtocol()
 * @sa redisxGetHelloReply()
 */
int redisxSetProtocol(Redis *redis, enum redisx_protocol protocol) {
  static const char *fn = "redisxSetProtocol";

  RedisPrivate *p;

  prop_error(fn, rConfigLock(redis));
  p = (RedisPrivate *) redis->priv;
  p->hello = TRUE;
  p->protocol = protocol;
  rConfigUnlock(redis);

  return X_SUCCESS;
}

/**
 * Returns the actual protocol used with the Redis server. If HELLO was used during connection it will
 * be the protocol that was confirmed in the response of HELLO (and which hopefully matches the
 * protocol requested). Otherwise, RedisX will default to RESP2.
 *
 * @param redis     The Redis server instance
 * @return          REDISX_RESP2 or REDISX_RESP3, or else an error code, such as X_NULL if the
 *                  argument is NULL, or X_NO_INIT if the Redis server instance was not initialized.
 *
 * @sa redisxSetProtocol()
 */
enum redisx_protocol redisxGetProtocol(Redis *redis) {
  static const char *fn = "redisxGetProtocol";

  const RedisPrivate *p;
  int protocol;

  prop_error(fn, rConfigLock(redis));
  p = (RedisPrivate *) redis->priv;
  protocol = p->protocol;
  rConfigUnlock(redis);

  return protocol;
}

/**
 * Sets a user-defined callback for additioan custom configuring of client sockets
 *
 *
 * @param redis     The Redis server instance
 * @param func      The user-defined callback function, which performs the additional socket configuration
 * @return          X_SUCCESS (0) if successful, or or X_NULL if the redis argument in NULL, X_NO_INIT
 *                  if the redis instance was not initialized.
 *
 * @sa redisxSetSocketErrorHandler()
 */
int redisxSetSocketConfigurator(Redis *redis, RedisSocketConfigurator func) {
  static const char *fn = "redisxSetSocketConfigurator";

  RedisPrivate *p;

  prop_error(fn, rConfigLock(redis));
  p = (RedisPrivate *) redis->priv;
  p->hello = TRUE;
  p->socketConf = func;
  rConfigUnlock(redis);

  return X_SUCCESS;
}

/**
 * Sets the user-specific error handler to call if a socket level trasmit error occurs.
 * It replaces any prior handlers set earlier.
 *
 * \param redis     The Redis instance to configure.
 * \param f         The error handler function, which is called with the pointer to the redis
 *                  instance that had the errror, the redis channel index
 *                  (e.g. REDIS_INTERACTIVE_CHANNEL) and the operation (e.g. 'send' or 'read')
 *                  that failed. Note, that the call may be made with the affected Redis
 *                  channel being in a locked state. As such the handler should not directly
 *                  attempt to change the connection state of the Redis instance. Any calls
 *                  that require exlusive access to the affected channel should instead be
 *                  spawn off into a separate thread, which can obtain the necessary lock
 *                  when it is released.
 *
 * \return          X_SUCCESS if the handler was successfully configured, or X_NULL if the
 *                  Redis instance is NULL.
 *
 * @sa redisxSetSocketConfigurator()
 */
int redisxSetSocketErrorHandler(Redis *redis, RedisErrorHandler f) {
  static const char *fn = "redisxSetSocketErrorHandler";

  RedisPrivate *p;

  prop_error(fn, rConfigLock(redis));
  p = (RedisPrivate *) redis->priv;
  p->transmitErrorFunc = f;
  rConfigUnlock(redis);

  return X_SUCCESS;
}


/**
 * Returns the current time on the Redis server instance.
 *
 * @param redis     Pointer to a Redis instance.
 * @param[out] t    Pointer to a timespec structure in which to return the server time.
 * @return          X_SUCCESS (0) if successful, or X_NULL if either argument is NULL, or X_PARSE_ERROR
 *                  if could not parse the response, or another error returned by redisxCheckRESP().
 */
int redisxGetTime(Redis *redis, struct timespec *t) {
  static const char *fn = "redisxGetTime";

  RESP *reply, **components;
  int status = X_SUCCESS;
  char *tail;

  if(!t) return x_error(X_NULL, EINVAL, fn, "output timespec is NULL");

  memset(t, 0, sizeof(*t));

  reply = redisxRequest(redis, "TIME", NULL, NULL, NULL, &status);
  prop_error(fn, status);

  status = redisxCheckDestroyRESP(reply, RESP_ARRAY, 2);
  prop_error(fn, status);

  components = (RESP **) reply->value;
  status = redisxCheckRESP(components[0], RESP_BULK_STRING, 0);
  if(status) {
    redisxDestroyRESP(reply);
    return x_trace(fn, NULL, status);
  }

  // [1] seconds.
  errno = 0;
  t->tv_sec = strtol((char *) components[0]->value, &tail, 10);
  if(errno) {
    redisxDestroyRESP(reply);
    return x_error(X_PARSE_ERROR, errno, fn, "tv_sec parse error: '%s'", (char *) components[0]->value);
  }

  status = redisxCheckRESP(components[0], RESP_BULK_STRING, 0);
  if(status) {
    redisxDestroyRESP(reply);
    return x_trace(fn, NULL, status);
  }

  // [2] microseconds.
  errno = 0;
  t->tv_nsec = 1000 * strtol((char *) components[1]->value, &tail, 10);

  if(errno)
    status = x_error(X_PARSE_ERROR, errno, fn, "tv_nsec parse error: '%s'", (char *) components[1]->value);
  else status = X_SUCCESS;

  redisxDestroyRESP(reply);

  return status;
}

/**
 * Pings the Redis server (see the Redis `PING` command), and checks the response.
 *
 * @param redis     Pointer to a Redis instance.
 * @param message   Optional message , or NULL for `PING` without an argument.
 * @return          X_SUCCESS (0) if successful, or else an error code (&lt;0) from redisx.h / xchange.h.
 *
 */
int redisxPing(Redis *redis, const char *message) {
  static const char *fn = "redisxPing";

  int status = X_SUCCESS;

  RESP *reply = redisxRequest(redis, "PING", message, NULL, NULL, &status);
  prop_error(fn, status);
  prop_error(fn, redisxCheckDestroyRESP(reply, message ? RESP_BULK_STRING : RESP_SIMPLE_STRING, 0));

  if(strcmp(message ? message : "PONG", (char *) reply->value) != 0)
    status = x_error(REDIS_UNEXPECTED_RESP, ENOMSG, fn, "expected 'PONG', got '%s'", (char *) reply->value);

  redisxDestroyRESP(reply);

  return status;
}

/**
 * Siwtches to another database. This version should be called with an exclusive lock on the affected
 * client.
 *
 * @param cl          the redis client
 * @param idx         zero-based database index
 * @param confirm     Whether to wait for confirmation from Redis, and check the response.
 * @return            X_SUCCESS (0) if successful, or else an error code (&lt;0) from redisx.h / xchange.h.
 *
 * @sa redisxSelectDB()
 * @sa redisxLockConnected()
 */
static int redisxSelectDBAsync(RedisClient *cl, int idx, boolean confirm) {
  static const char *fn = "redisxSelectClientDBAsync";

  char sval[20];

  if(!confirm) prop_error(fn, redisxSkipReplyAsync(cl));

  sprintf(sval, "%d", idx);
  prop_error(fn, redisxSendRequestAsync(cl, "SELECT", sval, NULL, NULL));

  if(confirm) {
    RESP *reply = redisxReadReplyAsync(cl);
    int status = redisxCheckRESP(reply, RESP_SIMPLE_STRING, 0);
    if(!status) if(strcmp("OK", (char *) reply->value) != 0)
      status = x_error(REDIS_UNEXPECTED_RESP, ENOMSG, fn, "expected 'OK', got '%s'", (char *) reply->value);
    redisxDestroyRESP(reply);
    prop_error(fn, status);
  }

  return X_SUCCESS;
}

static void rAffirmDB(Redis *redis) {
  const RedisPrivate *p = (RedisPrivate *) redis->priv;
  redisxSelectDB(redis, p->dbIndex);
}

/**
 * Switches to another database index on the Redis server. Note that you cannot change the database on an active
 * PUB/SUB channel, hence the call will return X_INCOMPLETE if attempted. You should instead switch DB when there
 * are no active subscriptions.
 *
 * @param redis       Pointer to a Redis instance.
 * @param idx         zero-based database index
 * @return            X_SUCCESS (0) if successful, or
 *                    X_NULL if the redis argument is NULL,
 *                    X_INCOMPLETE if there is an active subscription channel that cannot be switched or
 *                    one of the channels could not confirm the switch, or
 *                    else another error code (&lt;0) from redisx.h / xchange.h.
 *
 * @sa redisxSelectDB()
 * @sa redisxLockConnected()
 */
int redisxSelectDB(Redis *redis, int idx) {
  static const char *fn = "redisxSelectDB";

  const RedisPrivate *p;
  enum redisx_channel c;
  int dbIdx, status = X_SUCCESS;

  prop_error(fn, rConfigLock(redis));
  p = (RedisPrivate *) redis->priv;
  dbIdx = p->dbIndex;
  rConfigUnlock(redis);

  if(dbIdx == idx) return X_SUCCESS;

  if(idx) redisxAddConnectHook(redis, rAffirmDB);
  else redisxRemoveConnectHook(redis, rAffirmDB);

  if(!redisxIsConnected(redis)) return X_SUCCESS;

  for(c = 0; c < REDISX_CHANNELS; c++) {
    RedisClient *cl = redisxGetClient(redis, c);
    int s = redisxLockConnected(cl);

    // We can't switch unconnected clients or the existing subscription client
    if(s == REDIS_INVALID_CHANNEL || c == REDISX_SUBSCRIPTION_CHANNEL) {
      if(!s) status = X_INCOMPLETE;
      redisxUnlockClient(cl);
      continue;
    }

    s = redisxSelectDBAsync(cl, idx, c != REDISX_PIPELINE_CHANNEL);
    redisxUnlockClient(cl);

    if(s) {
      char str[20];
      sprintf(str, "%d", idx);

      status = X_INCOMPLETE;
      x_trace(fn, str, status);
    }
  }

  return status;
}


/**
 * Prints a descriptive error message to stderr, and returns the error code.
 *
 * \param func      A string that describes the function or location where the error occurred.
 * \param errorCode The error code that describes the failure.
 *
 * \return          the error code.
 */
int redisxError(const char *func, int errorCode) {
  if(!errorCode) return errorCode;

  if(errorCode == REDIS_INCOMPLETE_TRANSFER) errno = EBADMSG;

  if(xDebug) {
    static int errorCount;
    fprintf(stderr, "DEBUG-X> %4d (%s) in %s.\n", errorCode, redisxErrorDescription(errorCode), func);
    if(errorCount > MAX_DEBUG_ERROR_COUNT) {
      fprintf(stderr, "Redis-X> Reached max debug count. Exiting program with %d.\n", errorCode);
      exit(errorCode);
    }
  }

  return errorCode;
}

/**
 * Checks if a Redis instance has the pipeline connection enabled.
 *
 * \param redis         Pointer to a Redis instance.
 *
 * \return      TRUE (1) if the pipeline client is enabled on the Redis intance, or FALSE (0) otherwise.
 */
boolean redisxHasPipeline(Redis *redis) {
  static const char *fn = "redisxHasPipeline";

  const ClientPrivate *pp;

  boolean isEnabled;

  prop_error(fn, redisxCheckValid(redis));
  prop_error(fn, redisxLockClient(redis->pipeline));
  pp = (ClientPrivate *) redis->pipeline->priv;
  isEnabled = pp->isEnabled;
  redisxUnlockClient(redis->pipeline);

  return isEnabled;
}

/**
 * Sets the function processing valid pipeline responses. The implementation should follow a
 * simple set of rules:
 *
 * <ul>
 * <li>the implementation should not destroy the RESP data. The RESP will be destroyed automatically
 * after the call returns. However, the call may retain any data from the RESP itself, provided
 * the data is de-referenced from the RESP before return.<li>
 * <li>The implementation should not block (aside from maybe a quick mutex unlock) and return quickly,
 * so as to not block the client for long periods</li>
 * <li>If extensive processing or blocking calls are required to process the message, it is best to
 * simply place a copy of the RESP on a queue and then return quickly, and then process the message
 * asynchronously in a background thread.</li>
 * </ul>
 *
 * \param redis             Pointer to a Redis instance.
 * \param f                 The function that processes a single argument of type RESP pointer.
 *
 * \return      X_SUCCESS (0)   if successful, or
 *              X_NULL          if the Redis instance is NULL.
 */
int redisxSetPipelineConsumer(Redis *redis, RedisPipelineProcessor f) {
  RedisPrivate *p;

  prop_error("redisxSetPipelineConsumer", rConfigLock(redis));
  p = (RedisPrivate *) redis->priv;
  p->pipelineConsumerFunc = f;
  rConfigUnlock(redis);

  return X_SUCCESS;
}

/**
 * Returns the result of a Redis command with up to 3 regularly terminated string arguments. This is not the highest
 * throughput mode (that would be sending asynchronous pipeline request, and then asynchronously collecting the results
 * such as with redisxSendRequestAsync() / redisxReadReplyAsync(), because it requires separate network roundtrips for each
 * and every request. But, it is simple and perfectly good method when one needs to retrieve only a few (<1000)
 * variables per second...
 *
 * To make Redis calls with binary (non-string) data, you can use redisxArrayRequest() instead, where you can
 * set the number of bytes for each argument explicitly.
 *
 * \param redis     Pointer to a Redis instance.
 * \param command   Redis command, e.g. "HGET"
 * \param arg1      First terminated string argument or NULL.
 * \param arg2      Second terminated string argument or NULL.
 * \param arg3      Third terminated string argument or NULL.
 * \param status    Pointer to the return error status, which is either X_SUCCESS on success or else
 *                  the error code set by redisxArrayRequest().
 *
 * \return          A freshly allocated RESP array containing the Redis response, or NULL if no valid
 *                  response could be obtained or status is not X_SUCCESS.
 *
 * @sa redisxArrayRequest()
 * @sa redisxSendRequestAsync()
 * @sa redisxReadReplyAsync()
 */
RESP *redisxRequest(Redis *redis, const char *command, const char *arg1, const char *arg2, const char *arg3, int *status) {
  RESP *reply;
  const char *args[] = { command, arg1, arg2, arg3 };
  int n, s = X_SUCCESS;

  if(command == NULL) n = 0;
  else if(arg1 == NULL) n = 1;
  else if(arg2 == NULL) n = 2;
  else if(arg3 == NULL) n = 3;
  else n = 4;

  reply = redisxArrayRequest(redis, args, NULL, n, &s);
  if(status) *status = s;

  if(s) {
    redisxDestroyRESP(reply);
    return x_trace_null("redisxRequest", NULL);
  }

  return reply;
}

/**
 * Returns the result of the most generic type of Redis request with any number of arguments. This is not the
 * highest throughput mode (that would be sending asynchronous pipeline request, and then asynchronously collecting
 * the results such as with redisxSendArrayRequestAsync() / redisxReadReplyAsync(), because it requires separate network
 * roundtrips for each and every request. But, it is simple and perfectly good method when one needs to retrieve
 * only a few (<1000) variables per second...
 *
 * \param redis     Pointer to a Redis instance.
 * \param args      An array of strings to send to Redis, corresponding to a single query.
 *                  If you have an `char **` array, you may need to cast to `(const char **)` to avoid
 *                  compiler warnings.
 * \param lengths   Array indicating the number of bytes to send from each string argument. Zero
 *                  values can be used to determine the string length automatically using strlen(),
 *                  and the length argument itself may be NULL to determine the lengths of all
 *                  string arguments automatically.
 * \param n         Number of string arguments.
 * \param status    Pointer to the return error status, which is either
 *
 *                      X_SUCCESS       on success.
 *                      X_NO_INIT       if the Redis client librarywas not initialized via initRedis.
 *                      X_NULL          if the argument is NULL or n<1.
 *                      X_NO_SERVICE    if not connected to Redis.
 *                      X_FAILURE       If there was a socket level error.
 *
 *
 * \return          A freshly allocated RESP array containing the Redis response, or NULL if no valid
 *                  response could be obtained.
 *
 * @sa redisxRequest()
 * @sa redisxSendArrayRequestAsync()
 * @sa redisxReadReplyAsync()
 */
RESP *redisxArrayRequest(Redis *redis, const char **args, const int *lengths, int n, int *status) {
  static const char *fn = "redisxArrayRequest";
  RESP *reply = NULL;
  RedisClient *cl;

  if(redisxCheckValid(redis) != X_SUCCESS) return x_trace_null(fn, NULL);

  if(args == NULL || n < 1 || status == NULL) {
    x_error(0, EINVAL, fn, "invalid parameter: args=%p, n=%d, status=%p", args, n, status);
    if(status) *status = X_NULL;
    return NULL;
  }
  else *status = X_SUCCESS;

  cl = redis->interactive;
  *status = redisxLockConnected(cl);
  if(*status) return x_trace_null(fn, NULL);

  redisxClearAttributesAsync(cl);

  *status = redisxSendArrayRequestAsync(cl, args, lengths, n);
  if(!(*status)) reply = redisxReadReplyAsync(cl);
  redisxUnlockClient(cl);

  if(*status) x_trace_null(fn, NULL);

  return reply;
}


/**
 * Returns a copy of the attributes sent along with the last interative request. The user should
 * destroy the returned RESP after using it by calling redisxDestroyRESP().
 *
 * @param redis     Pointer to a Redis instance.
 * @return          The attributes (if any) that were sent along with the last response on the
 *                  interactive client.
 *
 * @sa redisxGetAttributeAsync()
 * @sa redisxRequest()
 * @sa redisxArrayRequest()
 * @sa redisxDestroyRESP()
 */
RESP *redisxGetAttributes(Redis *redis) {
  static const char *fn = "redisxGetAttributes";
  RESP *attr;

  if(redisxCheckValid(redis) != X_SUCCESS) return x_trace_null(fn, NULL);
  if(redisxLockConnected(redis->interactive) != X_SUCCESS) return x_trace_null(fn, NULL);

  attr = redisxCopyOfRESP(redisxGetAttributesAsync(redis->interactive));
  redisxUnlockClient(redis->interactive);

  return attr;
}

/**
 * Sets a user-defined function to process push messages for a specific Redis instance. The function's
 * implementation must follow a simple set of rules:
 *
 * <ul>
 * <li>the implementation should not destroy the RESP data. The RESP will be destroyed automatically
 * after the call returns. However, the call may retain any data from the RESP itself, provided
 * the data is de-referenced from the RESP before return.<li>
 * <li>The call will have exclusive access to the client. As such it should not try to obtain a
 * lock or release the lock itself.</li>
 * <li>The implementation should not block (aside from maybe a quick mutex unlock) and return quickly,
 * so as to not block the client for long periods</li>
 * <li>If extensive processing or blocking calls are required to process the message, it is best to
 * simply place a copy of the RESP on a queue and then return quickly, and then process the message
 * asynchronously in a background thread.</li>
 * <li>The client on which the push is originated will be locked, thus the implementation should
 * avoid getting explusive access to the client</li>
 * </ul>
 *
 * @param redis   Redis instance
 * @param func    Function to use for processing push messages from the given Redis instance, or NULL
 *                to ignore push messages.
 * @param arg     (optional) User-defined pointer argument to pass along to the processing function.
 * @return        X_SUCCESS (0) if successful, or else X_NULL (errno set to EINVAL) if the client
 *                argument is NULL, or X_NO_INIT (errno set to EAGAIN) if redis is uninitialized.
 */
int redisxSetPushProcessor(Redis *redis, RedisPushProcessor func, void *arg) {
  static const char *fn = "redisxSetPushProcessor";

  RedisPrivate *p;

  prop_error(fn, rConfigLock(redis));
  p = redis->priv;
  p->pushConsumer = func;
  p->pushArg = arg;
  rConfigUnlock(redis);

  return X_SUCCESS;
}

/**
 * Returns a copy of the RESP map that the Redis server has sent us as a response to HELLO on the
 * last client connection, or NULL if HELLO was not used or available.
 *
 * @param redis   The redis instance
 * @return        A copy of the response sent by HELLO on the last client connection, or NULL.
 *
 * @sa redisxSetProtocol()
 * @sa redisxGetInfo()
 */
RESP *redisxGetHelloData(Redis *redis) {
  const RedisPrivate *p;
  RESP *data;

  int status = rConfigLock(redis);
  if(status) return x_trace_null("redisxGetHelloData", NULL);
  p = (RedisPrivate *) redis->priv;
  data = redisxCopyOfRESP(p->helloData);
  rConfigUnlock(redis);

  return data;
}

/**
 * Returns the result of an INFO query (with the optional parameter) as a lookup table
 * of keywords and string values.
 *
 * @param redis       Pointer to Redis instance
 * @param parameter   Optional parameter to pass with INFO, or NULL.
 * @return            a newly created lookup table with the string key/value pairs of the
 *                    response from the Redis server, or NULL if there was an error.
 *                    The caller should destroy the lookup table after using it.
 *
 * @sa redisxGetHelloData()
 */
XLookupTable *redisxGetInfo(Redis *redis, const char *parameter) {
  static const char *fn = "redisxGetInfo";

  XStructure *s;
  XLookupTable *lookup;
  RESP *reply;
  const char *line;
  int status;

  reply = redisxRequest(redis, "INFO", parameter, NULL, NULL, &status);
  if(status) return x_trace_null(fn, NULL);

  if(redisxCheckDestroyRESP(reply, RESP_BULK_STRING, 0) != 0) return x_trace_null(fn, NULL);

  s = xCreateStruct();

  // Go line by line...
  line = strtok((char *) reply->value, "\n");

  // Parse key:value lines into a structure.
  while(line) {
    char *sep = strchr(line, ':');
    if(sep) {
      *sep = '\0';
      xSetField(s, xCreateStringField(line, xStringCopyOf(sep + 1)));
    }
    line = strtok(NULL, "\n");
  }

  redisxDestroyRESP(reply);

  lookup = xCreateLookup(s, FALSE);
  free(s);

  return lookup;
}

  /**
   * Checks if a given string is a glob-style pattern.
   *
   * \param str       The string to check.
   *
   * \return          TRUE if it is a glob pattern (e.g. has '*', '?' or '['), otherwise FALSE.
   *
   */
  int redisxIsGlobPattern(const char *str) {
    for(; *str; str++) switch(*str) {
      case '*':
      case '?':
      case '[': return TRUE;
    }
    return FALSE;
  }

  /**
   * Returns a string description for one of the RM error codes.
   *
   * \param code      One of the error codes defined in 'rm.h' or in 'redisrm.h' (e.g. X_NO_PIPELINE)
   *
   * \return      A constant string with the error description.
   *
   */
  const char *redisxErrorDescription(int code) {
    switch(code) {
      case REDIS_INVALID_CHANNEL: return "invalid Redis channel";
      case REDIS_NULL: return "Redis returned null";
      case REDIS_ERROR: return "Redis returned an error";
      case REDIS_INCOMPLETE_TRANSFER: return "incomplete Redis transfer";
      case REDIS_UNEXPECTED_RESP: return "unexpected Redis response type";
      case REDIS_UNEXPECTED_ARRAY_SIZE: return "unexpected Redis array size";
    }
    return xErrorDescription(code);
  }

