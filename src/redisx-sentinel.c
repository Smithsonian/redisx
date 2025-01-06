/**
 * @file
 *
 * @date Created  on Jan 6, 2025
 * @author Attila Kovacs
 *
 *  Functions to manage clients to high-availability Redis Sentinel server configurations.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "redisx-priv.h"


/**
 * Attemps to connect to a given Redis sentinel server. If successful, the server is moved to the
 * top of the list of sentinel servers, so it will be the first one tro try for new connections.
 * The Redis instance is assumed to be un-connected at the time of the call.
 *
 * @param redis         A Redis server instance
 * @param serverIndex   the current array index of the server among the sentinels
 * @return              X_SUCCESS (0) if successful, or else an error code &lt;0.
 */
static int rTryConnectSentinel(Redis *redis, int serverIndex) {
  static const char *fn = "rTryConnectSentinel";

  RedisPrivate *p = (RedisPrivate *) redis->priv;
  RedisSentinel *s = p->sentinel;
  RedisServer server = s->servers[serverIndex];  // A copy, not a reference...
  char desc[80];
  int status;

  sprintf(desc, "sentinel server %d", serverIndex);
  prop_error(fn, rSetServerAsync(redis, desc, server.host, server.port));

  status = rConnectClient(redis, REDISX_INTERACTIVE_CHANNEL);
  if(status != X_SUCCESS) return status; // No error propagation. It's OK if server is down...

  // Move server to the top of the list, so next time we try this one first...
  memmove(&s->servers[1], s->servers, serverIndex * sizeof(RedisServer));
  s->servers[0] = server;

  return X_SUCCESS;
}


/// \cond PRIVATE

/**
 * Obtains the current Sentinel master from the given participating node.
 *
 * @param redis     A Redis server instance
 * @return          X_SUCCESS (0) if successful, or else an error code &lt;0.
 */
int rDiscoverSentinel(Redis *redis) {
  static const char *fn = "rConnectSentinel";

  RedisPrivate *p = (RedisPrivate *) redis->priv;
  const RedisSentinel *s = p->sentinel;
  int i, savedTimeout = p->timeoutMillis;

  p->timeoutMillis = s->timeoutMillis > 0 ? s->timeoutMillis : REDISX_DEFAULT_SENTINEL_TIMEOUT_MILLIS;

  for(i = 0; i < s->nServers; i++) if(rTryConnectSentinel(redis, i) == X_SUCCESS) {
    RESP *reply;
    int status;

    // Get the name of the master...
    reply = redisxRequest(redis, "SENTINEL", "get-master-addr-by-name", s->serviceName, NULL, &status);
    rCloseClientAsync(redis->interactive);

    if(status) continue;

    if(redisxCheckDestroyRESP(reply, RESP_ARRAY, 2) == X_SUCCESS) {
      RESP **component = (RESP **) reply->value;
      int port = (int) strtol((char *) component[1]->value, NULL, 10);

      status = rSetServerAsync(redis, "sentinel master", (char *) component[0]->value, port);

      redisxDestroyRESP(reply);
      p->timeoutMillis = savedTimeout;

      prop_error(fn, status);

      // TODO update sentinel server list...

      return X_SUCCESS;
    }
  }

  p->timeoutMillis = savedTimeout;
  return x_error(X_NO_SERVICE, ENOTCONN, fn, "no Sentinel server available");
}

/**
 * Verifies that a given Redis instance is in the master role. User's should always communicate
 * to the master, not to replicas.
 *
 * @param redis     A redis server instance
 * @return          X_SUCCESS (0) if the server is confirmed to be the master, or else an error
 *                  code &lt;0.
 */
int rConfirmMasterRole(Redis *redis) {
  static const char *fn = "rConfirmMasterRole";

  RESP *reply, **component;
  int status;

  // Try ROLE command first (available since Redis 4)
  reply = redisxRequest(redis, "ROLE", NULL, NULL, NULL, &status);
  prop_error(fn, status);

  if(redisxCheckDestroyRESP(reply, RESP_ARRAY, 0) != X_SUCCESS) {
    // Fallback to using INFO replication...
    XLookupTable *info = redisxGetInfo(redis, "replication");
    const XField *role;

    if(!info) return x_trace(fn, NULL, X_FAILURE);

    role = xLookupField(info, "role");
    if(role) status = strcmp("master", (char *) role->value);
    else status = x_trace(fn, NULL, X_FAILURE);

    xDestroyLookup(info);
    return status;
  }

  if(reply->n < 1) {
    redisxDestroyRESP(reply);
    return x_error(X_FAILURE, EBADE, fn, "Got empty array response");
  }

  component = (RESP **) reply->value;
  status = strcmp("master", (char *) component[0]->value);

  redisxDestroyRESP(reply);

  if(status) return x_error(X_FAILURE, EAGAIN, fn, "Replica is not master");
  return X_SUCCESS;
}

/// \endcond


/**
 * Validates a Sentinel configuration.
 *
 * @param serviceName   The service name as registered in the Sentinel server configuration.
 * @param serverList    An set of Sentinel servers to use to dynamically find the current master.
 * @param nServers      The number of servers in the list
 * @return              X_SUCCESS (0) if successful, or X_NAME_INVALID if the serviceName is
 *                      NULL or empty, or X_NULL if the serverList is NULL, or X_SIZE_INVALID
 *                      if nServers is 0 or negative, or else X_GROUP_INVALID if the first server
 *                      has a NULL or empty host name.
 */
int redisxValidateSentinel(const char *serviceName, const RedisServer *serverList, int nServers) {
  static const char *fn = "redisxValidateSentinel";

  if(!serviceName) return x_error(X_NAME_INVALID, EINVAL, fn, "service name is NULL");
  if(!serviceName[0]) return x_error(X_NAME_INVALID, EINVAL, fn, "service name is empty");
  if(!serverList) return x_error(X_NULL, EINVAL, fn, "server list is NULL");
  if(nServers < 1) return x_error(X_SIZE_INVALID, EINVAL, fn, "invalid number of servers: %d", nServers);
  if(serverList[0].host == NULL) return x_error(X_GROUP_INVALID, EINVAL, fn, "first server address is NULL");
  if(!serverList[0].host[0]) return x_error(X_GROUP_INVALID, EINVAL, fn, "first server address is empty");

  return X_SUCCESS;
}

/**
 * Initializes a Redis client with a Sentinel configuration of alternate servers, and the default
 * sentinel node connection timeout.
 *
 * @param serviceName     The service name as registered in the Sentinel server configuration.
 *                        The supplied name will be copied, not referenced, so that the value
 *                        passed may be freely destroyed after the call.
 * @param serverList      An set of Sentinel servers to use to dynamically find the current master.
 *                        The list itself and its contents are not referenced. Instead a deep copy
 *                        will be made of it, so the list that was pased can be freely destroyed
 *                        after the call.
 * @param nServers        The number of servers in the list
 * @return                X_SUCCESS (0) if successful, or else an error code &lt;0.
 *
 * @sa redisxSetSentinelTimeout()
 * @sa redisxInit()
 * @sa redisxConnect()
 */
Redis *redisxInitSentinel(const char *serviceName, const RedisServer *serverList, int nServers) {
  static const char *fn = "redisxInitSentinel";

  Redis *redis;
  RedisPrivate *p;
  RedisSentinel *s;

  if(redisxValidateSentinel(serviceName, serverList, nServers) != X_SUCCESS) return x_trace_null(fn, NULL);

  redis = redisxInit(serverList[0].host);
  if(!redis) return x_trace_null(fn, NULL);

  p = (RedisPrivate *) redis->priv;
  s = (RedisSentinel *) calloc(1, sizeof(RedisSentinel));
  x_check_alloc(s);

  s->servers = (RedisServer *) calloc(nServers, sizeof(RedisServer));
  if(!s->servers) {
    x_error(0, errno, fn, "alloc error (%d RedisServer)", nServers);
    free(s);
    return NULL;
  }
  memcpy(s->servers, serverList, nServers * sizeof(RedisServer));

  s->nServers = nServers;
  s->serviceName = xStringCopyOf(serviceName);
  s->timeoutMillis = REDISX_DEFAULT_SENTINEL_TIMEOUT_MILLIS;

  p->sentinel = s;

  return redis;
}

/**
 * Changes the connection timeout for Sentinel server instances in the discovery phase. This is different
 * from the timeout that is used for the master server, once it is discovered.
 *
 * @param redis     The Redis instance, which was initialized for Sentinel via redisxInitSentinel().
 * @param millis    [ms] The new connection timeout or &lt;=0 to use the default value.
 * @return          X_SUCCESS (0) if successfully set sentinel connection timeout, or else X_NULL if the
 *                  redis instance is NULL, or X_NO_INIT if the redis instance is not initialized for
 *                  Sentinel.
 *
 * @sa redisxSetSocketTimeout()
 * @sa redisxInitSentinel()
 */
int redisxSetSentinelTimeout(Redis *redis, int millis) {
  static const char *fn = "redisxSetSentinelTimeout";

  RedisPrivate *p;
  int status = X_SUCCESS;

  prop_error(fn, rConfigLock(redis));
  p = (RedisPrivate *) redis->priv;
  if(p->sentinel) p->sentinel->timeoutMillis = millis > 0 ? millis : REDISX_DEFAULT_SENTINEL_TIMEOUT_MILLIS;
  else status = x_error(X_NO_INIT, EAGAIN, fn, "Redis was not initialized for Sentinel");
  rConfigUnlock(redis);

  return status;
}

/// \cond PRIVATE
void rDestroySentinel(RedisSentinel *sentinel) {
  if(!sentinel) return;

  while(--sentinel->nServers >= 0) {
    RedisServer *server = &sentinel->servers[sentinel->nServers];
    if(server->host) free(server->host);
  }
  if(sentinel->serviceName) free(sentinel->serviceName);
}
/// \endcond
