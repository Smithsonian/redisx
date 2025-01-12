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
 * Attemps to connect to a given Redis sentinel server. The Redis instance is assumed to be
 * unconnected at the time of the call, and the caller must have an exclusive lock on the
 * Redis configuration mutex.
 *
 * @param redis         A Redis server instance
 * @param serverIndex   the current array index of the server among the sentinels
 * @return              X_SUCCESS (0) if successful, or else an error code &lt;0.
 */
static int rTryConnectSentinelAsync(Redis *redis, int serverIndex) {
  static const char *fn = "rTryConnectSentinel";

  RedisPrivate *p = (RedisPrivate *) redis->priv;
  RedisSentinel *s = p->sentinel;
  RedisServer server = s->servers[serverIndex];  // A copy, not a reference...
  char desc[80];
  int status;

  sprintf(desc, "sentinel server %d", serverIndex);
  xvprintf("Redis-X> Connect to %s.\n", desc);

  prop_error(fn, rSetServerAsync(redis, desc, server.host, server.port));

  status = rConnectClientAsync(redis, REDISX_INTERACTIVE_CHANNEL);
  if(status != X_SUCCESS) return status; // No error propagation. It's OK if server is down...

  return X_SUCCESS;
}

/// \cond PRIVATE

/**
 * Moves a server from its current position in the Sentinel server list to the top.
 * the caller must have an exclusive lock on the Redis configuration mutex.
 *
 * @param s     The Redis Sentinel configuration
 * @param idx   The current zero-based index of the server in the list
 * @return
 */
static int rSetTopSentinelAsync(RedisSentinel *s, int idx) {
  RedisServer server;

  if(idx == 0) return X_SUCCESS;

  xvprintf("Redis-X> Moving server %d to top of the list.\n", idx);

  // Make a local copy of the server's data.
  server = s->servers[idx];

  // Bump the preceding elements down by one (this overwrites the slot at idx).
  memmove(&s->servers[1], s->servers, idx * sizeof(RedisServer));

  // Set the top to the saved local copu.
  s->servers[0] = server;

  return X_SUCCESS;
}

/**
 * Moves or adds the master server, specified by host name and port number, to the top of
 * the list of known Sentinel servers, so next time we try it first when connecting.
 * The caller must have an exclusive lock on the Redis configuration mutex.
 *
 * @param s         Redis Sentinel configuration
 * @param hostname  Host name or IP address of the master node
 * @param port      Port number of master server on node
 * @return          X_SUCCESS (0) if successful, or else an error code &lt;0.
 */
static int rIncludeMasterAsync(RedisSentinel *s, char *hostname, int port) {
  int i;
  void *old = s->servers;

  for(i = 0; i < s->nServers; i++) {
    RedisServer *server = &s->servers[i];
    int sport = server->port > 0 ? server->port : REDISX_TCP_PORT;
    if(sport == port && strcmp(server->host, hostname) == 0) {
      // Found master on server list, move it to the top...
      rSetTopSentinelAsync(s, i);
      return X_SUCCESS;
    }
  }

  xvprintf("Redis-X> Adding master at %s:%d to top of Sentinels.\n", hostname, port);

  // Current master does not seem to be on our list, so let's add it to the top.
  s->servers = (RedisServer *) realloc(s->servers, (s->nServers + 1) * sizeof(RedisServer));
  if(!s->servers) {
    // Ouch realloc error, go on with the old server list...
    s->servers = old;
    return X_FAILURE;
  }
  else {
    // Bump the existing list, and add the new master on top
    memmove(&s->servers[1], s->servers, s->nServers * sizeof(RedisServer));
    s->servers[0].host = hostname;
    s->servers[0].port = port;
    s->nServers++;
  }

  return X_SUCCESS;
}

/**
 * Verifies that a given Redis instance is in the master role. Users should always communicate
 * to the master, not to replicas. It assumes that we have a live interactive connection to the
 * server already, and the caller must have an exclusive lock on the Redis configuration mutex.
 *
 * Upon successful return the current master is added or moved to the top of the sentinel server
 * list, so it is the first to try next time.
 *
 * @param redis     A Redis server instance, with the interactive client connected.
 * @return          X_SUCCESS (0) if the server is confirmed to be the master, or else an error
 *                  code &lt;0.
 */
int rConfirmMasterRoleAsync(Redis *redis) {
  static const char *fn = "rConfirmMasterRole";

  RESP *reply, **component;
  int status;

  // Try ROLE command first (available since Redis 4)
  status = redisxSendRequestAsync(redis->interactive, "ROLE", NULL, NULL, NULL);
  prop_error(fn, status);

  reply = redisxReadReplyAsync(redis->interactive, &status);
  prop_error(fn, status);

  if(redisxCheckDestroyRESP(reply, RESP_ARRAY, 0) != X_SUCCESS) {
    // Fallback to using INFO replication...
    XLookupTable *info;
    const XField *role;

    status = redisxSendRequestAsync(redis->interactive, "INFO", "replication", NULL, NULL);
    prop_error(fn, status);

    reply = redisxReadReplyAsync(redis->interactive, &status);
    prop_error(fn, status);

    info = rConsumeInfoReply(reply);
    if(!info) return x_trace(fn, NULL, X_FAILURE);

    role = xLookupField(info, "role");
    if(role) status = strcmp("master", (char *) role->value);
    else status = x_trace(fn, NULL, X_FAILURE);

    xDestroyLookup(info);
    return status;
  }

  if(reply->n < 1) {
    redisxDestroyRESP(reply);
    return x_error(X_FAILURE, EBADMSG, fn, "Got empty array response");
  }

  component = (RESP **) reply->value;
  status = strcmp("master", (char *) component[0]->value);

  redisxDestroyRESP(reply);

  if(status == X_SUCCESS) {
    RedisPrivate *p = (RedisPrivate *) redis->priv;
    xvprintf("Redis-X> Confirmed master at %s:%d.\n", p->hostname, p->port);
    if(p->sentinel) rIncludeMasterAsync(p->sentinel, p->hostname, p->port);
    return X_SUCCESS;
  }

  return x_error(X_FAILURE, EAGAIN, fn, "server is a replica, not the master");
}

/**
 * Configures the current Sentinel master from the given participating node. The caller
 * should have exclusive access to the configuration of the Redis instance.
 *
 * @param redis     A Redis server instance
 * @return          X_SUCCESS (0) if successful, or else an error code &lt;0.
 */
int rDiscoverSentinelAsync(Redis *redis) {
  static const char *fn = "rConnectSentinel";

  RedisPrivate *p = (RedisPrivate *) redis->priv;
  RedisConfig *config = &p->config;
  RedisSentinel *s = p->sentinel;
  int i, savedTimeout = config->timeoutMillis;

  config->timeoutMillis = s->timeoutMillis > 0 ? s->timeoutMillis : REDISX_DEFAULT_SENTINEL_TIMEOUT_MILLIS;

  xvprintf("Redis-X> Looking for the Sentinel master...\n");

  for(i = 0; i < s->nServers; i++) if(rTryConnectSentinelAsync(redis, i) == X_SUCCESS) {
    RESP *reply;
    int status;

    // Check if this is master...
    if(rConfirmMasterRoleAsync(redis)) goto success; // @suppress("Goto statement used")

    // Get the name of the master...
    status = redisxSendRequestAsync(redis->interactive, "SENTINEL", "get-master-addr-by-name", s->serviceName, NULL);
    if(status) continue;

    reply = redisxReadReplyAsync(redis->interactive, &status);
    if(status) continue;

    if(redisxCheckDestroyRESP(reply, RESP_ARRAY, 2) == X_SUCCESS) {
      RESP **component = (RESP **) reply->value;
      int port = (int) strtol((char *) component[1]->value, NULL, 10);

      status = rSetServerAsync(redis, "sentinel master", (char *) component[0]->value, port);
      redisxDestroyRESP(reply);
      prop_error(fn, status);

      if(i > 0) rSetTopSentinelAsync(s, i);

      // TODO update sentinel server list?...

      goto success; // @suppress("Goto statement used")
    }
  }

  config->timeoutMillis = savedTimeout;
  return x_error(X_NO_SERVICE, ENOTCONN, fn, "no Sentinel server available");

  // --------------------------------------------------------------------------------------------------
  success:

  config->timeoutMillis = savedTimeout;
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

