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
  RedisPrivate *p;

  if(!redis) return X_NULL;
  if(redisxIsConnected(redis)) return redisxError("redisxSetUser()", X_ALREADY_OPEN);

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
  RedisPrivate *p;

  if(!redis) return X_NULL;
   if(redisxIsConnected(redis)) return redisxError("redisxSetPassword()", X_ALREADY_OPEN);

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

  if(!redis) return X_NULL;

  rConfigLock(redis);
  p = (RedisPrivate *) redis->priv;
  p->transmitErrorFunc = f;
  rConfigUnlock(redis);

  return X_SUCCESS;
}

/**
 * Loads a LUA script into Redis, returning it's SHA1 hash to use as it's call ID.
 *
 * \param[in]  redis         Pointer to a Redis instance.
 * \param[in]  script        String containing the full LUA script.
 * \param[out] sha1          Buffer into which SHA1 key returned by Redis to use as call ID.
 *                           (It must be at least 41 bytes, and will be string terminated).
 *                           By default it will return an empty string.
 *
 * \return      X_SUCCESS (0)           if the script has been successfully loaded into Redis, or
 *              X_NULL                  if the Redis instance is NULL
 *              X_NAME_INVALID          if the script is NULL or empty.
 *              REDIS_UNEXPECTED_RESP   if received a Redis reponse of the wrong type,
 *
 *              ot an error returned by redisxRequest().
 *
 */
int redisxLoadScript(Redis *redis, const char *script, char **sha1) {
  static const char *funcName = "redisxLoadScript()";
  RESP *reply;
  int status;

  if(redis == NULL) return redisxError(funcName, X_NULL);
  if(script == NULL) return redisxError(funcName, X_NAME_INVALID);
  if(*script == '\0') return redisxError(funcName, X_NAME_INVALID);

  *sha1 = NULL;

  reply = redisxRequest(redis, "SCRIPT", "LOAD", script, NULL, &status);

  if(!status) {
    redisxDestroyRESP(reply);
    return redisxError(funcName, status);
  }

  status = redisxCheckDestroyRESP(reply, RESP_BULK_STRING, 0);
  if(status) return redisxError(funcName, status);

  *sha1 = (char *) reply->value;
  redisxDestroyRESP(reply);

  return X_SUCCESS;
}

/**
 * Returns the current time on the Redis server instance.
 *
 * @param redis     Pointer to a Redis instance.
 * @param t         Pointer to a timespec structure in which to return the server time.
 * @return          X_SUCCESS (0) if successful, or X_NULL if either argument is NULL, or X_PARSE_ERROR
 *                  if could not parse the response, or another error returned by redisxCheckRESP().
 */
int redisxGetTime(Redis *redis, struct timespec *t) {
  static const char *funcName = "redisxGetTime()";

  RESP *reply, **components;
  int status = X_SUCCESS;
  char *tail;

  if(!redis || !t) return redisxError(funcName, X_NULL);

  memset(t, 0, sizeof(*t));

  reply = redisxRequest(redis, "TIME", NULL, NULL, NULL, &status);
  if(status) {
    redisxDestroyRESP(reply);
    return redisxError(funcName, status);
  }

  status = redisxCheckDestroyRESP(reply, RESP_ARRAY, 2);
  if(status) return redisxError(funcName, status);

  components = (RESP **) reply->value;
  status = redisxCheckRESP(components[0], RESP_BULK_STRING, 0);
  if(status) {
    redisxDestroyRESP(reply);
    return redisxError(funcName, status);
  }

  // [1] seconds.
  t->tv_sec = strtol((char *) components[0]->value, &tail, 10);
  if(tail == components[0]->value || errno == ERANGE) {
    redisxDestroyRESP(reply);
    return redisxError(funcName, X_PARSE_ERROR);
  }

  status = redisxCheckRESP(components[0], RESP_BULK_STRING, 0);
  if(status) {
    redisxDestroyRESP(reply);
    return redisxError(funcName, status);
  }

  // [2] microseconds.
  t->tv_nsec = 1000 * strtol((char *) components[1]->value, &tail, 10);

  if(tail == components[1]->value || errno == ERANGE) status = redisxError(funcName, X_PARSE_ERROR);
  else status = X_SUCCESS;

  redisxDestroyRESP(reply);

  return status;
}

/**
 * Pings the Redis server (see the Redis `PING` command), and check the response.
 *
 * @param redis     Pointer to a Redis instance.
 * @param message   Optional message , or NULL for `PING` without an argument.
 * @return          X_SUCCESS (0) if successful, or else an error code (&lt;0) from redisx.h / xchange.h.
 *
 */
int redisxPing(Redis *redis, const char *message) {
  static const char *funcName = "redisxPing()";
  int status = X_SUCCESS;

  RESP *reply = redisxRequest(redis, "PING", message, NULL, NULL, &status);

  if(!reply) return redisxError(funcName, X_NULL);
  if(!status) {
    status = redisxCheckRESP(reply, message ? RESP_BULK_STRING : RESP_SIMPLE_STRING, 0);
    if(!status) if(strcmp(message ? message : "PONG", (char *)reply->value) != 0) status = REDIS_UNEXPECTED_RESP;
  }

  redisxDestroyRESP(reply);
  if(status) return redisxError(funcName, status);

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
static int redisxSelectClientDBAsync(RedisClient *cl, int idx, boolean confirm) {
  static const char *funcName = "redisxSelectClientDBAsync()";

  char sval[20];
  int status;

  if(cl == NULL) return redisxError(funcName, X_NULL);

  if(!confirm) {
    status = redisxSkipReplyAsync(cl);
    if(status) return redisxError(funcName, status);
  }

  sprintf(sval, "%d", idx);
  status = redisxSendRequestAsync(cl, "SELECT", sval, NULL, NULL);
  if(status) return redisxError(funcName, status);

  if(confirm) {
    RESP *reply = redisxReadReplyAsync(cl);
    status = redisxCheckRESP(reply, RESP_SIMPLE_STRING, 0);
    if(!status) if(strcmp("OK", (char *) reply->value) != 0) status = REDIS_UNEXPECTED_RESP;
    redisxDestroyRESP(reply);
    if(status) return redisxError(funcName, status);
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
  RedisPrivate *p;
  enum redisx_channel c;
  int status = X_SUCCESS;

  if(!redis) return redisxError("redisxSelectDB()", X_NULL);

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

    s = redisxSelectClientDBAsync(cl, idx, c != REDISX_PIPELINE_CHANNEL);
    redisxUnlockClient(cl);

    if(s) status = X_INCOMPLETE;
  }

  return status;
}


/**
 * Sends a `RESET` request to the specified Redis client. The server will perform a reset as if the
 * client disconnected and reconnected again.
 *
 * @param cl    The Redis client
 * @return      X_SUCCESS (0) if successful, or else an error code (&lt;0) from redisx.h / xchange.h.
 */
int redisxResetClient(RedisClient *cl) {
  static const char *funcName = "redisxResetClient()";

  int status = X_SUCCESS;

  if(cl == NULL) return redisxError(funcName, X_NULL);

  status = redisxLockConnected(cl);
  if(status) return redisxError(funcName, status);

  status = redisxSendRequestAsync(cl, "RESET", NULL, NULL, NULL);
  if(!status) {
    RESP *reply = redisxReadReplyAsync(cl);
    status = redisxCheckRESP(reply, RESP_SIMPLE_STRING, 0);
    if(!status) if(strcmp("RESET", (char *) reply->value) != 0) status = REDIS_UNEXPECTED_RESP;
    redisxDestroyRESP(reply);
  }

  redisxUnlockClient(cl);

  if(status) return redisxError(funcName, status);

  return X_SUCCESS;
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

  if(redis == NULL) return redisxError("redisxSetPipelineConsumer()", X_NULL);

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
  const char *args[] = { command, arg1, arg2, arg3 };
  int n;

  if(redis == NULL) return NULL;

  if(command == NULL) n = 0;
  else if(arg1 == NULL) n = 1;
  else if(arg2 == NULL) n = 2;
  else if(arg3 == NULL) n = 3;
  else n = 4;

  return redisxArrayRequest(redis, (char **) args, NULL, n, status);
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
  static const char *funcName = "redisxArrayRequest()";
  RESP *reply = NULL;
  RedisClient *cl;

  if(redis == NULL || args == NULL || n < 1) *status = X_NULL;
  else *status = X_SUCCESS;

  if(*status) {
    redisxError(funcName, *status);
    return NULL;
  }

  xvprintf("Redis-X> request %s... [%d].\n", args[0], n);

  cl = redis->interactive;
  *status = redisxLockConnected(cl);
  if(*status) {
    redisxError(funcName, *status);
    return NULL;
  }

  *status = redisxSendArrayRequestAsync(cl, args, lengths, n);
  if(!(*status)) reply = redisxReadReplyAsync(cl);
  redisxUnlockClient(cl);

  if(*status) redisxError(funcName, *status);

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

