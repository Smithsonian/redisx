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
#define SET_PRIORITIES              REDIS_SET_LISTENER_PRIORITIES        ///< Whether to actually set listener priorities
#endif

#define XPRIO_MIN                   (sched_get_priority_min(SCHED_RR))
#define XPRIO_MAX                   (sched_get_priority_max(SCHED_RR))
#define XPRIO_RANGE                 (XPRIO_MAX - XPRIO_MIN)

#define REDISX_LISTENER_PRIORITY    (XPRIO_MIN + (int) (REDISX_LISTENER_REL_PRIORITY * XPRIO_RANGE))

/// \endcond

/// \cond PRIVATE


/**
 * Waits to get exlusive access to configuring the properties of a Redis instance.
 *
 * \param redis         Pointer to a Redis instance.
 *
 */
void rConfigLock(Redis *redis) {
  RedisPrivate *p = (RedisPrivate *) redis->priv;
  pthread_mutex_lock(&p->configLock);
}

/**
 * Relinquish exlusive access to configuring the properties of a Redis instance.
 *
 * \param redis         Pointer to a Redis instance.
 *
 */
void rConfigUnlock(Redis *redis) {
  RedisPrivate *p = (RedisPrivate *) redis->priv;
  pthread_mutex_unlock(&p->configLock);
}

/// \endcond

/**
 * Enable or disable verbose reporting of all Redis operations (and possibly some details of them).
 * Reporting is done on the standard output (stdout). It may be useful when debugging programs
 * that use the redisx interface. Verbose reporting is DISABLED by default.
 *
 * \param value         TRUE to enable verbose reporting, or FALSE to disable.
 *
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

  RedisPrivate *p;

  if(!redis) return x_error(X_NULL, EINVAL, fn, "redis is NULL");
  if(redisxIsConnected(redis)) return x_error(X_ALREADY_OPEN, EALREADY, fn, "already connected");

  p = (RedisPrivate *) redis->priv;
  if(p->username) free(p->username);
  p->username = xStringCopyOf(username);

  return X_SUCCESS;
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

  RedisPrivate *p;

  if(!redis) return x_error(X_NULL, EINVAL, fn, "redis is NULL");
  if(redisxIsConnected(redis)) return x_error(X_ALREADY_OPEN, EALREADY, fn, "already connected");

  p = (RedisPrivate *) redis->priv;
  if(p->password) free(p->password);
  p->password = xStringCopyOf(passwd);

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
 */
int redisxSetTransmitErrorHandler(Redis *redis, RedisErrorHandler f) {
  RedisPrivate *p;

  if(!redis) return x_error(X_NULL, EINVAL, "redisxSetTransmitErrorHandler", "redis is NULL");

  rConfigLock(redis);
  p = (RedisPrivate *) redis->priv;
  p->transmitErrorFunc = f;
  rConfigUnlock(redis);

  return X_SUCCESS;
}


/**
 * Returns the current time on the Redis server instance.
 *
 * @param redis     Pointer to a Redis instance.
 * @param[out] t         Pointer to a timespec structure in which to return the server time.
 * @return          X_SUCCESS (0) if successful, or X_NULL if either argument is NULL, or X_PARSE_ERROR
 *                  if could not parse the response, or another error returned by redisxCheckRESP().
 */
int redisxGetTime(Redis *redis, struct timespec *t) {
  static const char *fn = "redisxGetTime";

  RESP *reply, **components;
  int status = X_SUCCESS;
  char *tail;

  if(!redis) return x_error(X_NULL, EINVAL, fn, "redis is NULL");
  if(!t) return x_error(X_NULL, EINVAL, fn, "output timespec is NULL");

  memset(t, 0, sizeof(*t));

  reply = redisxRequest(redis, "TIME", NULL, NULL, NULL, &status);
  if(status) {
    redisxDestroyRESP(reply);
    return x_trace(fn, NULL, status);
  }

  status = redisxCheckDestroyRESP(reply, RESP_ARRAY, 2);
  prop_error(fn, status);

  components = (RESP **) reply->value;
  status = redisxCheckRESP(components[0], RESP_BULK_STRING, 0);
  if(status) {
    redisxDestroyRESP(reply);
    return x_trace(fn, NULL, status);
  }

  // [1] seconds.
  t->tv_sec = strtol((char *) components[0]->value, &tail, 10);
  if(tail == components[0]->value || errno == ERANGE) {
    redisxDestroyRESP(reply);
    return x_error(X_PARSE_ERROR, errno, fn, "tv_sec parse error: '%s'", (char *) components[0]->value);
  }

  status = redisxCheckRESP(components[0], RESP_BULK_STRING, 0);
  if(status) {
    redisxDestroyRESP(reply);
    return x_trace(fn, NULL, status);
  }

  // [2] microseconds.
  t->tv_nsec = 1000 * strtol((char *) components[1]->value, &tail, 10);

  if(tail == components[1]->value || errno == ERANGE)
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

  if(!redis) return x_error(X_NULL, EINVAL, fn, "redis is NULL");

  RESP *reply = redisxRequest(redis, "PING", message, NULL, NULL, &status);

  if(!status) {
    if(!reply) status = x_error(X_NULL, errno, fn, "reply was NULL");
    else {
      status = redisxCheckRESP(reply, message ? RESP_BULK_STRING : RESP_SIMPLE_STRING, 0);
      if(!status) if(strcmp(message ? message : "PONG", (char *)reply->value) != 0)
        status = x_error(REDIS_UNEXPECTED_RESP, EBADE, fn, "expected 'PONG', got '%s'", (char *)reply->value);
    }
  }

  redisxDestroyRESP(reply);
  prop_error(fn, status);

  return X_SUCCESS;
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

  if(!confirm) {
    prop_error(fn, redisxSkipReplyAsync(cl));
  }

  sprintf(sval, "%d", idx);
  prop_error(fn, redisxSendRequestAsync(cl, "SELECT", sval, NULL, NULL));

  if(confirm) {
    RESP *reply = redisxReadReplyAsync(cl);
    int status = redisxCheckRESP(reply, RESP_SIMPLE_STRING, 0);
    if(!status) if(strcmp("OK", (char *) reply->value) != 0)
      status = x_error(REDIS_UNEXPECTED_RESP, EBADE, fn, "expected 'OK', got '%s'", (char *) reply->value);
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

  RedisPrivate *p;
  enum redisx_channel c;
  int status = X_SUCCESS;

  if(!redis) return x_error(X_NULL, EINVAL, fn, "redis is NULL");

  p = (RedisPrivate *) redis->priv;
  if(p->dbIndex == idx) return X_SUCCESS;

  p->dbIndex = idx;

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
 * Frees up the resources used by a RESP structure that was dynamically allocated.
 * The call will segfault if the same RESP is destroyed twice or if the argument
 * is a static allocation.
 *
 * \param resp      Pointer to the RESP structure to be destroyed, which may be NULL (no action taken).
 */
void redisxDestroyRESP(RESP *resp) {
  if(resp == NULL) return;
  if(resp->type == RESP_ARRAY) while(--resp->n >= 0) {
    RESP **component = (RESP **) resp->value;
    redisxDestroyRESP(component[resp->n]);
  }
  if(resp->value != NULL) free(resp->value);
  free(resp);
}


/**
 * Checks a Redis RESP for NULL values or unexpected values.
 *
 * \param resp              Pointer to the RESP structure from Redis.
 * \param expectedType      The RESP type expected (e.g. RESP_ARRAY) or 0 if not checking type.
 * \param expectedSize      The expected size of the RESP (array or bytes) or <=0 to skip checking
 *
 * \return      X_SUCCESS (0)                   if the RESP passes the tests, or
 *              X_PARSE_ERROR                   if the RESP is NULL (garbled response).
 *              REDIS_NULL                      if Redis returned (nil),
 *              REDIS_UNEXPECTED_TYPE           if got a reply of a different type than expected
 *              REDIS_UNEXPECTED_ARRAY_SIZE     if got a reply of different size than expected.
 *
 *              or the error returned in resp->n.
 *
 */
int redisxCheckRESP(const RESP *resp, char expectedType, int expectedSize) {
  if(resp == NULL) return X_PARSE_ERROR;
  if(resp->type != RESP_INT) {
    if(resp->n < 0) return resp->n;
    if(resp->value == NULL) if(resp->n) return REDIS_NULL;
  }
  if(expectedType) if(resp->type != expectedType) return REDIS_UNEXPECTED_RESP;
  if(expectedSize > 0) if(resp->n != expectedSize) return REDIS_UNEXPECTED_ARRAY_SIZE;
  return X_SUCCESS;
}

/**
 * Like redisxCheckRESP(), but it also destroys the RESP in case of an error.
 *
 * \param resp              Pointer to the RESP structure from Redis.
 * \param expectedType      The RESP type expected (e.g. RESP_ARRAY) or 0 if not checking type.
 * \param expectedSize      The expected size of the RESP (array or bytes) or <=0 to skip checking
 *
 * \return      The return value of redisxCheckRESP().
 *
 * \sa redisxCheckRESP()
 *
 */
int redisxCheckDestroyRESP(RESP *resp, char expectedType, int expectedSize) {
  int status = redisxCheckRESP(resp, expectedType, expectedSize);
  if(status) redisxDestroyRESP(resp);
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
  const ClientPrivate *pp;

  if(redis == NULL) return FALSE;
  pp = (ClientPrivate *) redis->pipeline->priv;
  return pp->isEnabled;
}

/**
 * Sets the function processing valid pipeline responses.
 *
 * \param redis             Pointer to a Redis instance.
 * \param f    T            he function that processes a single argument of type RESP pointer.
 *
 * \return      X_SUCCESS (0)   if successful, or
 *              X_NULL          if the Redis instance is NULL.
 */
int redisxSetPipelineConsumer(Redis *redis, void (*f)(RESP *)) {
  RedisPrivate *p;

  if(redis == NULL) return x_error(X_NULL, EINVAL, "redisxSetPipelineConsumer", "redis is NULL");

  p = (RedisPrivate *) redis->priv;
  rConfigLock(redis);
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
 *                  response could be obtained.
 *
 * @sa redisxArrayRequest()
 * @sa redisxSendRequestAsync()
 * @sa redisxReadReplyAsync()
 */
RESP *redisxRequest(Redis *redis, const char *command, const char *arg1, const char *arg2, const char *arg3, int *status) {
  static const char *fn = "redisxRequest";

  RESP *reply;
  const char *args[] = { command, arg1, arg2, arg3 };
  int n, s;


  if(redis == NULL) x_error(X_NULL, EINVAL, fn, "redis is NULL");

  if(command == NULL) n = 0;
  else if(arg1 == NULL) n = 1;
  else if(arg2 == NULL) n = 2;
  else if(arg3 == NULL) n = 3;
  else n = 4;

  reply = redisxArrayRequest(redis, (char **) args, NULL, n, &s);

  if(status) *status = s;
  if(s) x_trace_null(fn, NULL);

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
RESP *redisxArrayRequest(Redis *redis, char *args[], int lengths[], int n, int *status) {
  static const char *fn = "redisxArrayRequest";
  RESP *reply = NULL;
  RedisClient *cl;


  if(redis == NULL || args == NULL || n < 1 || status == NULL) {
    x_error(0, EINVAL, fn, "invalid parameter: redis=%p, args=%p, n=%d, status=%p", redis, args, n, status);
    if(status) *status = X_NULL;
    return NULL;
  }
  else *status = X_SUCCESS;

  xvprintf("Redis-X> request %s... [%d].\n", args[0], n);

  cl = redis->interactive;
  *status = redisxLockConnected(cl);
  if(*status) return x_trace_null(fn, NULL);

  *status = redisxSendArrayRequestAsync(cl, args, lengths, n);
  if(!(*status)) reply = redisxReadReplyAsync(cl);
  redisxUnlockClient(cl);

  if(*status) x_trace_null(fn, NULL);

  return reply;
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

