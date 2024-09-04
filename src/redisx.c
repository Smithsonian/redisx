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
#define SET_PRIORITIES              TRUE        ///< Disable if you want to use gdb to debug...
#endif

#define XPRIO_MIN                   (sched_get_priority_min(SCHED_RR))
#define XPRIO_MAX                   (sched_get_priority_max(SCHED_RR))
#define XPRIO_RANGE                 (XPRIO_MAX - XPRIO_MIN)

#define REDISX_LISTENER_PRIORITY    (XPRIO_MIN + (int) (REDISX_LISTENER_REL_PRIORITY * XPRIO_RANGE))

typedef struct ServerLink {
  Redis *redis;
  struct ServerLink *next;
} ServerLink;

static ServerLink *serverList;
static pthread_mutex_t serverLock = PTHREAD_MUTEX_INITIALIZER;


// The response listener threads...
static pthread_attr_t threadConfig;

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
 * Sets the password to use for authenticating on the Redis server after connection. See the AUTH
 * Redis command for more explanation. Naturally, you need to call this prior to connecting
 * your Redis instance to have the desired effect.
 *
 * @param redis   Pointer to the Redis instance for which to set credentials
 * @param passwd  the password to use for authenticating on the server, or NULL to clear a
 *                previously configured password.
 */
void redisxSetPassword(Redis *redis, const char *passwd) {
  RedisPrivate *p;

  if(!redis) return;

  p = (RedisPrivate *) redis->priv;
  if(p->password) free(p->password);
  p->password = xStringCopyOf(passwd);
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
 * Shuts down Redis immediately, including all running Redis instances. It does not obtain
 * excluive locks to server list, configuration settings, or to open channels. As such
 * it should only be called to clean up an otherwise terminated program, e.g.
 * with atexit().
 *
 */
static void rShutdownAsync() {
  ServerLink *l;

  // NOTE: Don't use any locks, as they may deadlock when trying to shut down...

  l = serverList;

  while(l != NULL) {
    ServerLink *next = l->next;
    rShutdownLinkAsync(l->redis);
    free(l);
    l = next;
  }

  serverList = NULL;
}

/**
 *  Initializes the Redis client library, and sets the hostname or IP address for the Redis server.
 *
 *  \param server       Server host name or numeric IP address, e.g. "127.0.0.1"
 *
 *  \return             X_SUCCESS or
 *                      X_FAILURE       if the IP address is invalid.
 *                      X_NULL          if the IP address is NULL.
 */
Redis *redisxInit(const char *server) {
  static int isInitialized = FALSE;

  Redis *redis;
  RedisPrivate *p;
  ServerLink *l;
  int i;
  char ipAddress[IP_ADDRESS_LENGTH];

  if(server == NULL) return NULL;

  if(simpleHostnameToIP(server, ipAddress) < 0) return NULL;

  if(!isInitialized) {
    // Initialize the thread attributes once only to avoid segfaulting...
    pthread_attr_init(&threadConfig);
    atexit(rShutdownAsync);
    isInitialized = TRUE;
  }

  p = (RedisPrivate *) calloc(1, sizeof(RedisPrivate));
  pthread_mutex_init(&p->configLock, NULL);
  pthread_mutex_init(&p->subscriberLock, NULL);
  p->clients = (RedisClient *) calloc(3, sizeof(RedisClient));

  // Initialize the store access mutexes for each client channel.
  for(i=REDISX_CHANNELS; --i >= 0; ) rInitClient(&p->clients[i], i);

  redis = (Redis *) calloc(1, sizeof(Redis));
  redis->priv = p;
  redis->interactive = &p->clients[INTERACTIVE_CHANNEL];
  redis->pipeline = &p->clients[PIPELINE_CHANNEL];
  redis->subscription = &p->clients[SUBSCRIPTION_CHANNEL];
  redis->id = xStringCopyOf(ipAddress);

  for(i=REDISX_CHANNELS; --i >= 0; ) {
    ClientPrivate *cp = (ClientPrivate *) p->clients[i].priv;
    cp->redis = redis;
  }

  p->addr = inet_addr((char *) ipAddress);
  p->port = REDIS_TCP_PORT;

  l = (ServerLink *) calloc(1, sizeof(ServerLink));
  l->redis = redis;

  pthread_mutex_lock(&serverLock);
  l->next = serverList;
  serverList = l;
  pthread_mutex_unlock(&serverLock);

  return redis;
}

/**
 * Removes a Redis instance from the list of tracked instances. This is ormally called only
 * by redisxDestroy()
 *
 * @param redis      Pointer to a Redis instance.
 */
static void rUnregisterServer(const Redis *redis) {
  ServerLink *s, *last = NULL;

  pthread_mutex_lock(&serverLock);

  // remove this server from the open servers...
  for(s = serverList; s != NULL; ) {
    ServerLink *next = s->next;
    if(s->redis == redis) {
      if(last) last->next = s->next;
      else serverList = s->next;
      free(s);
      break;
    }
    last = s;
    s = next;
  }

  pthread_mutex_unlock(&serverLock);
}

/**
 * Destroys a Redis intance, disconnecting any clients that may be connected, and freeing all resources
 * used by that Redis instance.
 *
 * \param redis         Pointer to a Redis instance.
 *
 */
void redisxDestroy(Redis *redis) {
  int i;
  RedisPrivate *p;


  if(redis == NULL) return;

  p = (RedisPrivate *) redis->priv;

  if(redisxIsConnected(redis)) redisxDisconnect(redis);

  for(i=REDISX_CHANNELS; --i >= 0; ) {
    ClientPrivate *cp = (ClientPrivate *) p->clients[i].priv;
    pthread_mutex_destroy(&cp->readLock);
    pthread_mutex_destroy(&cp->writeLock);
    pthread_mutex_destroy(&cp->pendingLock);
    if(cp != NULL) free(cp);
  }
  free(p->clients);
  free(p);

  rUnregisterServer(redis);

  free(redis);
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
 * @sa redisxLockEnabled()
 */
int redisxSelectClientDBAsync(RedisClient *cl, int idx, boolean confirm) {
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
  redisxSelectDB(redis, p->dbIndex, TRUE);
}

/**
 * Switches to another database index on the Redis server. Note that you cannot change the database on an active
 * PUB/SUB channel, hence the call will return X_INCOMPLETE if attempted. You should instead switch DB when there
 * are no active subscriptions.
 *
 * @param redis       Pointer to a Redis instance.
 * @param idx         zero-based database index
 * @param confirm     Whether to wait for confirmation from Redis, and check the response.
 * @return            X_SUCCESS (0) if successful, or
 *                    X_NULL if the redis argument is NULL,
 *                    X_INCOMPLETE if there is an active subscription channel that cannot be switched or
 *                    one of the channels could not confirm the switch, or
 *                    else another error code (&lt;0) from redisx.h / xchange.h.
 *
 * @sa redisxSelectDB()
 * @sa redisxLockEnabled()
 */
int redisxSelectDB(Redis *redis, int idx, boolean confirm) {
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
    int s = redisxLockEnabled(cl);

    // We can't switch unconnected clients or the existing subscription client
    if(s == REDIS_INVALID_CHANNEL || c == SUBSCRIPTION_CHANNEL) {
      if(!s) status = X_INCOMPLETE;
      redisxUnlockClient(cl);
      continue;
    }

    s = redisxSelectClientDBAsync(cl, idx, confirm && c != PIPELINE_CHANNEL);
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

  status = redisxLockEnabled(cl);
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
 * Silently consumes a reply from the specified Redis channel.
 *
 * \param cl    Pointer to a Redis channel.
 *
 * \return      X_SUCCESS if a response was successfully consumed, or
 *              REDIS_NULL if a valid response could not be obtained.
 *
 */
int redisxIgnoreReplyAsync(RedisClient *cl) {
  static const char *funcName = "redisxIgnoreReplyAsync()";
  RESP *resp;

  if(cl == NULL) return redisxError(funcName, X_NULL);

  resp = redisxReadReplyAsync(cl);
  if(resp == NULL) return redisxError(funcName, REDIS_NULL);
  else redisxDestroyRESP(resp);
  return X_SUCCESS;
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
 * The listener function that processes pipelined responses in the background. It is started when Redis
 * is connected with the pipeline enabled.
 *
 * \param pRedis        Pointer to a Redis instance.
 *
 * \return              Always NULL.
 *
 */
static void *RedisPipelineListener(void *pRedis) {
  static int counter, lastError;

  Redis *redis = (Redis *) pRedis;
  RedisPrivate *p;
  RedisClient *cl;
  ClientPrivate *cp;
  RESP *reply = NULL;
  void (*consume)(RESP *response);

  pthread_detach(pthread_self());

  xvprintf("Redis-X> Started processing pipelined responses...\n");

  if(redis == NULL) {
    redisxError("RedisPipelineListener", X_NULL);
    return NULL;
  }

  p = (RedisPrivate *) redis->priv;
  cl = redis->pipeline;
  cp = (ClientPrivate *) cl->priv;

  while(cp->isEnabled && p->isPipelineListenerEnabled && pthread_equal(p->pipelineListenerTID, pthread_self())) {
    // Discard the response from the prior iteration
    if(reply) redisxDestroyRESP(reply);

    // Get a new response...
    reply = redisxReadReplyAsync(cl);

    counter++;

    // If client was disabled while waiting for response, then break out.
    if(!cp->isEnabled) {
      pthread_mutex_lock(&cp->pendingLock);
      if(cp->pendingRequests > 0) xvprintf("WARNING! pipeline disabled with %d requests in queue.\n", cp->pendingRequests);
      pthread_mutex_unlock(&cp->pendingLock);
      break;
    }

    if(reply == NULL) {
      fprintf(stderr, "WARNING! Redis-X: pipeline null response.\n");
      continue;
    }

    if(reply->n < 0) {
      if(reply->n != lastError) fprintf(stderr, "ERROR! Redis-X: pipeline parse error: %d.\n", reply->n);
      lastError = reply->n;
      continue;
    }

    // Skip confirms...
    if(reply->type == RESP_SIMPLE_STRING) continue;

    consume = p->pipelineConsumerFunc;
    if(consume) consume(reply);

#if REDISX_LISTENER_YIELD_COUNT > 0
    // Allow the waiting processes to take control...
    if(counter % REDISX_LISTENER_YIELD_COUNT == 0) sched_yield();
#endif

  } // <-- End of listener loop...

  xvprintf("Redis-X> Stopped processing pipeline responses (%d processed)...\n", counter);

  rConfigLock(redis);
  // If we are the current listener thread, then mark the listener as disabled.
  if(pthread_equal(p->pipelineListenerTID, pthread_self())) p->isPipelineListenerEnabled = FALSE;
  rConfigUnlock(redis);

  if(reply != NULL) redisxDestroyRESP(reply);

  return NULL;
}

/// \cond PRIVATE

/**
 * Starts the PUB/SUB listener thread with the specified thread attributes.
 *
 * \param redis     Pointer to the Redis instance.
 * \param attr      The thread attributes to set for the PUB/SUB listener thread.
 *
 * \return          0 if successful, or -1 if pthread_create() failed.
 *
 */
int rStartSubscriptionListenerAsync(Redis *redis) {
  RedisPrivate *p = (RedisPrivate *) redis->priv;

#if SET_PRIORITIES
  struct sched_param param;
#endif

  p->isSubscriptionListenerEnabled = TRUE;

  if (pthread_create(&p->subscriptionListenerTID, &threadConfig, RedisSubscriptionListener, redis) == -1) {
    perror("ERROR! Redis-X : pthread_create SubscriptionListener");
    p->isSubscriptionListenerEnabled = FALSE;
    return -1;
  }

#if SET_PRIORITIES
  param.sched_priority = REDISX_LISTENER_PRIORITY;
  pthread_attr_setschedparam(&threadConfig, &param);
  pthread_setschedparam(p->subscriptionListenerTID, SCHED_RR, &param);
#endif

  return 0;
}

/**
 * Starts the pipeline listener thread with the specified thread attributes.
 *
 * \param redis     Pointer to the Redis instance.
 * \param attr      The thread attributes to set for the pipeline listener thread.
 *
 * \return          0 if successful, or -1 if pthread_create() failed.
 *
 */
int rStartPipelineListenerAsync(Redis *redis) {
  RedisPrivate *p = (RedisPrivate *) redis->priv;

#if SET_PRIORITIES
  struct sched_param param;
#endif

  p->isPipelineListenerEnabled = TRUE;

  if (pthread_create(&p->pipelineListenerTID, &threadConfig, RedisPipelineListener, redis) == -1) {
    perror("ERROR! Redis-X : pthread_create PipelineListener");
    p->isPipelineListenerEnabled = FALSE;
    return -1;
  }

#if SET_PRIORITIES
  param.sched_priority = REDISX_LISTENER_PRIORITY;
  pthread_attr_setschedparam(&threadConfig, &param);
  pthread_setschedparam(p->pipelineListenerTID, SCHED_RR, &param);
#endif

  return 0;
}

/// \endcond

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

