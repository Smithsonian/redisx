/**
 * @file
 *
 * @date Created  on Aug 26, 2024
 * @author Attila Kovacs
 *
 *   Network layer management functions for the RedisX library.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <sys/utsname.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#if __Lynx__
#  include <socket.h>
#else
#  include <netinet/ip.h>
#  include <sys/socket.h>
#  include <fnmatch.h>
#endif
#include <netdb.h>

#include "redisx-priv.h"

static int rStartPipelineListenerAsync(Redis *redis);
static void rDisconnectClientAsync(RedisClient *cl);
static int rReconnectAsync(Redis *redis, boolean usePipeline);

/// \cond PRIVATE
///
// SHUT_RD is not defined on LynxOS for some reason, even though it should be...
#ifndef SHUT_RD
#  define SHUT_RD 0
#endif

typedef struct ServerLink {
  Redis *redis;
  struct ServerLink *next;
} ServerLink;

/// \endcond

static ServerLink *serverList;
static pthread_mutex_t serverLock = PTHREAD_MUTEX_INITIALIZER;

static int tcpBufSize = REDISX_TCP_BUF_SIZE;


/**
 * Gets an IP address string for a given host name. If more than one IP address is associated with a host name, the first one
 * is returned.
 *
 * \param hostName      The host name, e.g. "localhost"
 * \param ip            Pointer to the string buffer to which to write the corresponding IP.
 *
 * \return              X_SUCESSS       if the name was successfully matched to an IP address.
 *                      X_NAME_INVALID  if the no host is known by the specified name.
 *                      X_NULL          if hostName is NULL or if it is not associated to any valid IP address.
 */
static int hostnameToIP(const char *hostName, char *ip) {
  static const char *fn = "hostnameToIP";

  struct hostent *h;
  struct in_addr **addresses;
  int i;

  *ip = '\0';
  if(hostName == NULL) return x_error(X_NULL, EINVAL, fn, "input hostName is NULL");

  if ((h = gethostbyname((char *) hostName)) == NULL)
    return x_error(X_NAME_INVALID, errno, fn, "host lookup failed for: %s.", hostName);

  addresses = (struct in_addr **) h->h_addr_list;

  for(i = 0; addresses[i] != NULL; i++) {
    //Return the first one;
    strcpy(ip, inet_ntoa(*addresses[i]));
    return X_SUCCESS;
  }

  return x_error(X_NULL, ENODEV, fn, "no valid address for host %s", hostName);
}

static int rSetServerAsync(Redis *redis, const char *desc, const char *hostname, int port) {
  static const char *fn = "rSetServer";

  RedisPrivate *p = (RedisPrivate *) redis;
  char ipAddress[IP_ADDRESS_LENGTH] = {'\0'};
  int status;

  if(!hostname) return x_error(X_NULL, EINVAL, fn, "%s address is NULL", desc);
  if(!hostname[0]) return x_error(X_NAME_INVALID, EINVAL, fn, "%s name is empty", desc);

  status = hostnameToIP(hostname, ipAddress);
  if(status) return x_trace(fn, desc, status);

  p->addr = inet_addr((char *) ipAddress);
  p->port = port > 0 ? port : 0;

  if(redis->id) free(redis->id);
  redis->id = xStringCopyOf(ipAddress);

  return X_SUCCESS;
}

/**
 * Configure the Redis client sockets for optimal performance...
 *
 * \param socket          The socket file descriptor.
 * \param timeoutMillis   [ms] Socket read/write timeout, or &lt;0 to no set.
 * \param lowLatency      TRUE (non-zero) if socket is to be configured for low latency, or else FALSE (0).
 *
 */
static void rConfigSocket(int socket, int timeoutMillis, boolean lowLatency) {
  const boolean enable = TRUE;

  if(timeoutMillis > 0) {
    struct linger linger;
    struct timeval timeout;

    // Set a time limit for sending.
    timeout.tv_sec = timeoutMillis / 1000;
    timeout.tv_usec = 1000 * (timeoutMillis % 1000);
    if(setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, & timeout, sizeof(struct timeval)))
      xvprintf("WARNING! Redix-X: socket send timeout not set: %s", strerror(errno));

    // Redis recommends simply dropping the connection. By turning SO_LINGER off, we'll
    // end up with a 'connection reset' error on Redis, avoiding the TIME_WAIT state.
    // It is faster than the 'proper' handshaking close if the server can handle it properly.
    linger.l_onoff = FALSE;
    linger.l_linger = 0;
    if(setsockopt(socket, SOL_SOCKET, SO_LINGER, & linger, sizeof(struct timeval)))
      xvprintf("WARNING! Redis-X: socket linger not set: %s", strerror(errno));
  }

#if __linux__
  {
    const int tos = lowLatency ? IPTOS_LOWDELAY : IPTOS_THROUGHPUT;

    // Optimize service for latency or throughput
    // LynxOS 3.1 does not support IP_TOS option...
    if(setsockopt(socket, IPPROTO_IP, IP_TOS, &tos, sizeof(tos)))
      xvprintf("WARNING! Redis-X: socket type-of-service not set: %s", strerror(errno));
  }
#endif

#if !(__Lynx__ && __powerpc__)
  // Send packets immediately even if small...
  if(lowLatency) if(setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, & enable, sizeof(int)))
    xvprintf("WARNING! Redis-X: socket tcpnodelay not enabled: %s", strerror(errno));
#endif

  // Check connection to remote every once in a while to detect if it's down...
  if(setsockopt(socket, SOL_SOCKET, SO_KEEPALIVE, & enable, sizeof(int)))
    xvprintf("WARNING! Redis-X: socket keep-alive not enabled: %s", strerror(errno));

  // Allow to reconnect to closed RedisX sockets immediately
  //  if(setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, & enable, sizeof(int)))
  //    x_warn(id, "socket reuse address not enabled: %s", strerror(errno));

  // Set the TCP buffer size to use. Larger buffers facilitate more throughput.
  if(tcpBufSize > 0) {
    if(setsockopt(socket, SOL_SOCKET, SO_SNDBUF, &tcpBufSize, sizeof(int)))
      xvprintf("WARNING! Redis-X: socket send buf size not set: %s", strerror(errno));

    if(setsockopt(socket, SOL_SOCKET, SO_RCVBUF, &tcpBufSize, sizeof(int)))
      xvprintf("WARNING! Redis-X: socket send buf size not set: %s", strerror(errno));
  }
}

/**
 * Authenticates on the Redis server by sending the previously set password via AUTH.
 *
 * @param cl    Pointer to the Redis client to authenticate
 * @return      X_SUCCESS (0) if succssfully authenticated on the server, or else an
 *              appropriate error (&lt;0).
 *
 * @sa redisxSetPassword()
 */
static int rAuthAsync(RedisClient *cl) {
  static const char *fn = "rAuthAsync";

  ClientPrivate *cp = (ClientPrivate *) cl->priv;
  const RedisPrivate *p = (RedisPrivate *) cp->redis->priv;
  RESP *reply;

  int status = p->username ? redisxSendRequestAsync(cl, "AUTH", p->username, p->password, NULL) : redisxSendRequestAsync(cl, "AUTH", p->password, NULL, NULL);
  prop_error(fn, status);

  reply = redisxReadReplyAsync(cl);
  status = redisxCheckRESP(reply, RESP_SIMPLE_STRING, -1);
  redisxDestroyRESP(reply);

  prop_error(fn, status);

  return X_SUCCESS;
}

static int rRegisterServer(Redis *redis) {
  ServerLink *l = (ServerLink *) calloc(1, sizeof(ServerLink));
  x_check_alloc(l);
  l->redis = redis;

  pthread_mutex_lock(&serverLock);
  l->next = serverList;
  serverList = l;
  pthread_mutex_unlock(&serverLock);

  return X_SUCCESS;
}

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


static int rDiscoverSentinel(Redis *redis) {
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
    if(status) continue;

    rDisconnectClientAsync(redis->interactive);

    if(redisxCheckDestroyRESP(reply, RESP_ARRAY, 2) == X_SUCCESS) {
      RESP **component = (RESP **) reply->value;
      int port = (int) strtol((char *) component[1]->value, NULL, 10);

      status = rSetServerAsync(redis, "sentinel master", (char *) component[0]->value, port);

      redisxDestroyRESP(reply);
      p->timeoutMillis = savedTimeout;

      prop_error(fn, status);
      return X_SUCCESS;
    }
  }

  p->timeoutMillis = savedTimeout;
  return x_error(X_NO_SERVICE, ENOTCONN, fn, "no Sentinel server available");
}

static int rConfirmMasterRole(Redis *redis) {
  static const char *fn = "rConfirmMasterRole";

  RESP *reply, **component;
  int status;

  // Try ROLE command first (available since Redis 4)
  reply = redisxRequest(redis, "ROLE", NULL, NULL, NULL, &status);
  prop_error(fn, status);

  if(redisxCheckDestroyRESP(reply, RESP_ARRAY, 0) != X_SUCCESS) {
    // Fallback to using INFO replication...
    char *str;

    reply = redisxRequest(redis, "INFO", "replication", NULL, NULL, &status);
    prop_error(fn, status);
    prop_error(fn, redisxCheckDestroyRESP(reply, RESP_BULK_STRING, 0));

    // Go line by line...
    str = strtok((char *) reply->value, "\n");

    while(str) {
      const char *tok = strtok(str, ":");

      if(strcmp("role", tok) == 0) {
        tok = strtok(NULL, "\n");

        status = strcmp("master", tok);
        redisxDestroyRESP(reply);

        if(status) return x_error(X_FAILURE, EAGAIN, fn, "Replica is not master");
        return X_SUCCESS;
      }

      str = strtok(NULL, "\n");
    }

    return x_error(X_FAILURE, EBADE, fn, "Got empty array response");
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

/**
 * Same as connectRedis() except without the exlusive locking mechanism...
 *
 * \param redis         Pointer to a Redis instance.
 * \param usePipeline   TRUE (non-zero) if a pipeline client should be connected also, or FALSE to create an interactive
 *                      connection only.
 *
 * \return      X_SUCCESS (0)       if successful, or
 *              X_ALREADY_OPEN      if the Redis instance is alreast connected.
 *              X_NO_SERVICE        if there was an error connecting to Redis,
 *              or else an error (&lt;0) returned by rConnectClientAsync().
 *
 * \sa rConnectClientAsync()
 *
 */
static int rConnectAsync(Redis *redis, boolean usePipeline) {
  static const char *fn = "rConnectAsync";

  int status = X_SUCCESS;
  RedisPrivate *p = (RedisPrivate *) redis->priv;
  const ClientPrivate *ip = (ClientPrivate *) redis->interactive->priv;
  const ClientPrivate *pp = (ClientPrivate *) redis->pipeline->priv;
  Hook *f;

  if(redisxIsConnected(redis)) {
    x_warn(fn, "WARNING! Redis-X: already connected to %s\n", redis->id);
    return X_ALREADY_OPEN;
  }

  if(p->sentinel) {
    prop_error(fn, rDiscoverSentinel(redis));
    // TODO update sentinel server list...
  }

  if(!ip->isEnabled) {
    static int warnedInteractive;

    xvprintf("Redis-X> Connect interactive client.\n");
    status = rConnectClient(redis, REDISX_INTERACTIVE_CHANNEL);

    if(status) {
      if(!warnedInteractive) {
        fprintf(stderr, "ERROR! Redis-X : interactive client connection failed: %s\n", redisxErrorDescription(status));
        warnedInteractive = TRUE;
      }
      return x_trace(fn, "interactive", X_NO_SERVICE);
    }
    warnedInteractive = FALSE;
  }

  if(p->sentinel) {
    if(rConfirmMasterRole(redis) != X_SUCCESS) prop_error(fn, rReconnectAsync(redis, usePipeline));
  }

  if(usePipeline) {
    if(!pp->isEnabled) {
      static int warnedPipeline;

      xvprintf("Redis-X> Connect pipeline client.\n");
      status = rConnectClient(redis, REDISX_PIPELINE_CHANNEL);

      if(status) {
        if(!warnedPipeline) {
          fprintf(stderr, "ERROR! Redis-X : pipeline client connection failed: %s\n", redisxErrorDescription(status));
          warnedPipeline = TRUE;
        }
        return x_trace(fn, "pipeline", X_NO_SERVICE);
      }
      warnedPipeline = FALSE;
    }

    status = rStartPipelineListenerAsync(redis);
    prop_error(fn, status);
  }

  xvprintf("Redis-X> socket(s) online.\n");

  // Call the connect hooks...
  for(f = p->firstConnectCall; f != NULL; f = f->next) f->call(redis);

  xvprintf("Redis-X> connect complete.\n");

  return status;
}

static void rDisconnectClientAsync(RedisClient *cl) {
  ClientPrivate *cp = (ClientPrivate *) cl->priv;
  const int sock = cp->socket;      // Local copy of socket fd that won't possibly change mid-call.
  int status;

  if(sock < 0) return;

  cp->isEnabled = FALSE;            // No new synchronized requests or async reads.
  cp->socket = -1;                  // Reset the channel's socket descriptor to 'unassigned'

  shutdown(sock, SHUT_RD);
  status = close(sock);

  if(status) fprintf(stderr, "WARNING! Redis-X: client %d close socket error %d.\n", cp->idx, status);
}

/**
 * Resets the client properties for the specified Redis client.
 *
 * \param client        Pointer to the Redis client that is to be reset/initialized.
 *
 */
static void rResetClientAsync(RedisClient *cl) {
  ClientPrivate *cp = (ClientPrivate *) cl->priv;

  pthread_mutex_lock(&cp->pendingLock);
  cp->pendingRequests = 0;
  pthread_mutex_unlock(&cp->pendingLock);

  cp->isEnabled = FALSE;
  cp->available = 0;
  cp->next = 0;
  cp->socket = -1;                  // Reset the channel's socket descriptor to 'unassigned'
}

/**
 * Closes the Redis client on the specified communication channel. It is assused the caller
 * has an exclusive lock on the Redis configuration to which the client belongs. This
 * call assumes that the caller has an exlusive lock on the client's configuration settings.
 *
 * \param cl        Pointer to the Redis client instance.
 *
 */
static void rCloseClientAsync(RedisClient *cl) {
  rDisconnectClientAsync(cl);
  rResetClientAsync(cl);
  return;
}

/// \cond PRIVATE

/**
 * Closes the Redis client on the specified communication channel. It is assused the caller
 * has an exclusive lock on the Redis configuration to which the client belongs.
 *
 * \param cl        Pointer to the Redis client instance.
 *
 */
void rCloseClient(RedisClient *cl) {
  if(redisxLockClient(cl) == X_SUCCESS) {
    rCloseClientAsync(cl);
    redisxUnlockClient(cl);
  }
  return;
}

/// \endcond

/**
 * Same as disconnectRedis() except without the exlusive locking mechanism...
 *
 * \param redis         Pointer to a Redis instance.
 *
 */
static void rDisconnectAsync(Redis *redis) {
  RedisPrivate *p = (RedisPrivate *) redis->priv;
  Hook *f;

  // Disable pipeline listener...
  p->isPipelineListenerEnabled = FALSE;

  // Gracefully end subscriptions and close subscription client
  rCloseClient(redis->pipeline);
  rCloseClient(redis->interactive);

  xvprintf("Redis-X> sockets closed.\n");

  // Call the cleanup hooks...
  for(f = p->firstCleanupCall; f != NULL; f = f->next) f->call(redis);

  xvprintf("Redis-X> disconnect complete.\n");
}

/**
 * Same as reconnectRedis() except without the exlusive locking mechanism.
 */
static int rReconnectAsync(Redis *redis, boolean usePipeline) {
  xvprintf("Redis-X> reconnecting to server...\n");
  rDisconnectAsync(redis);
  prop_error("rReconnectAsync", rConnectAsync(redis, usePipeline));
  return X_SUCCESS;
}

/**
 * Disconnect all clients from the Redis server.
 *
 * \param redis         Pointer to a Redis instance.
 *
 */
void redisxDisconnect(Redis *redis) {
  if(redisxCheckValid(redis) != X_SUCCESS) return;

  rConfigLock(redis);
  rDisconnectAsync(redis);
  rConfigUnlock(redis);
}

/**
 * Disconnects from Redis, and then connects again...
 *
 * \param redis         Pointer to a Redis instance.
 * \param usePipeline   Whether to reconnect in pipelined mode.
 *
 * \return      X_SUCCESS (0)   if successful
 *              X_NULL          if the Redis instance is NULL
 *
 *              or else an error (&lt;0) as would be returned by redisxConnect().
 *
 */
int redisxReconnect(Redis *redis, boolean usePipeline) {
  static const char *fn = "redisxReconnect";

  int status;

  prop_error(fn, rConfigLock(redis));
  status = rReconnectAsync(redis, usePipeline);
  rConfigUnlock(redis);
  prop_error(fn, status);

  return X_SUCCESS;
}

/**
 * Shuts down the Redis connection immediately. It does not obtain excluive locks
 * to either configuration settings or to open channels. As such it should only
 * be called to clean up an otherwise terminated program.
 *
 * @param redis   Pointer to the Redis intance to shut down.
 */
static void rShutdownLinkAsync(Redis *redis) {
  RedisPrivate *p = (RedisPrivate *) redis->priv;
  int i;

  // NOTE: Don't use client locks, as they may deadlock when trying to shut down...
  for(i=0; i<REDISX_CHANNELS; i++) rDisconnectClientAsync(&p->clients[i]);
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
 * Initializes a redis client structure for the specified communication channel
 *
 * @param cl
 * @param idx
 */
static void rInitClient(RedisClient *cl, enum redisx_channel idx) {
  ClientPrivate *cp;

  cp = calloc(1, sizeof(ClientPrivate));
  x_check_alloc(cp);

  cp->idx = idx;
  pthread_mutex_init(&cp->readLock, NULL);
  pthread_mutex_init(&cp->writeLock, NULL);
  pthread_mutex_init(&cp->pendingLock, NULL);

  cl->priv = cp;

  rResetClientAsync(cl);
}

/// \cond PRIVATE

/**
 * Checks if a client was configured with a low-latency socket connection.
 *
 * \param cp        Pointer to the private data of a Redis client.
 *
 * \return          TRUE (1) if the client is low latency, or else FALSE (0).
 *
 */
boolean rIsLowLatency(const ClientPrivate *cp) {
  if(cp == NULL) return FALSE;
  return cp->idx != REDISX_PIPELINE_CHANNEL;
}

static int rHelloAsync(RedisClient *cl, char *clientID) {
  ClientPrivate *cp = (ClientPrivate *) cl->priv;
  RedisPrivate *p = (RedisPrivate *) cp->redis->priv;
  RESP *reply;
  char proto[20];
  char *args[6];
  int status, k = 0;

  args[k++] = "HELLO";

  // Try HELLO and see what we get back...
  sprintf(proto, "%d", (int) p->protocol);
  args[k++] = proto;

  if(p->password) {
    args[k++] = "AUTH";
    args[k++] = p->username ? p->username : "default";
    args[k++] = p->password;
  }

  args[k++] = "SETNAME";
  args[k++] = clientID;

  status = redisxSendArrayRequestAsync(cl, args, NULL, k);
  if(status != X_SUCCESS) return status;

  reply = redisxReadReplyAsync(cl);
  status = redisxCheckRESP(reply, RESP3_MAP, 0);
  if(status == X_SUCCESS) {
    RedisMapEntry *e = redisxGetKeywordEntry(reply, "proto");
    if(e && e->value->type == RESP_INT) {
      p->protocol = e->value->n;
      xvprintf("Confirmed protocol %d\n", p->protocol);
    }

    redisxDestroyRESP(p->helloData);
    p->helloData = reply;
  }
  else xvprintf("! Redis-X: HELLO failed: %s\n", redisxErrorDescription(status));

  return status;
}


/**
 * Connects the specified Redis client to the Redis server.
 *
 * \param redis         Pointer to a Redis instance.
 * \param channel       REDISX_INTERACTIVE_CHANNEL, REDISX_PIPELINE_CHANNEL, or REDISX_SUBSCRIPTION_CHANNEL
 *
 * \return              X_SUCCESS (0) if successful, or else:
 *
 *                          X_NO_INIT          if the library was not initialized
 *                          INVALID_CHANNEL    if the channel argument is out of range
 *                          X_NAME_INVALID     if the redis server address is invalid.
 *                          X_ALREADY_OPEN     if the client on that channels is already connected.
 *                          X_NO_SERVICE       if the socket or connection could not be opened.
 */
int rConnectClient(Redis *redis, enum redisx_channel channel) {
  static const char *fn = "rConnectClient";

  struct sockaddr_in serverAddress;
  struct utsname u;
  RedisPrivate *p;
  RedisClient *cl;
  ClientPrivate *cp;

  const char *channelID;
  char host[200], *id;
  int status = X_SUCCESS;
  int sock;

  cl = redisxGetClient(redis, channel);

  p = (RedisPrivate *) redis->priv;
  cp = (ClientPrivate *) cl->priv;

  serverAddress.sin_family      = AF_INET;
  serverAddress.sin_port        = htons(p->port > 0 ? p->port : REDISX_TCP_PORT);
  serverAddress.sin_addr.s_addr = p->addr;
  memset(serverAddress.sin_zero, '\0', sizeof(serverAddress.sin_zero));

  if((sock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0)
    return x_error(X_NO_SERVICE, errno, fn, "client %d socket creation failed", channel);

  rConfigSocket(sock, p->timeoutMillis, rIsLowLatency(cp));

  while(connect(sock, (struct sockaddr *) &serverAddress, sizeof(serverAddress)) != 0) {
    close(sock);
    return x_error(X_NO_INIT, errno, fn, "failed to connect");
  }

  xvprintf("Redis-X> client %d assigned socket fd %d.\n", channel, sock);

  // Set the client name in Redis.
  uname(&u);
  strncpy(host, u.nodename, sizeof(host) - 1);

  id = (char *) malloc(strlen(host) + 100);      // <host>:pid-<pid>:<channel> + termination;
  switch(cp->idx) {
    case REDISX_INTERACTIVE_CHANNEL: channelID = "interactive"; break;
    case REDISX_PIPELINE_CHANNEL: channelID = "pipeline"; break;
    case REDISX_SUBSCRIPTION_CHANNEL: channelID = "subscription"; break;
    default: channelID = "unknown";
  }
  sprintf(id, "%s:pid-%d:%s", host, (int) getppid(), channelID);

  redisxLockClient(cl);

  cp->socket = sock;
  cp->isEnabled = TRUE;

  if(p->hello) status = rHelloAsync(cl, id);

  if(status != X_SUCCESS) {
    status = X_SUCCESS;
    p->hello = FALSE;

    // No HELLO, go the old way...
    p->protocol = REDISX_RESP2;
    if(p->password) status = rAuthAsync(cl);

    if(!status) status = redisxSkipReplyAsync(cl);
    if(!status) status = redisxSendRequestAsync(cl, "CLIENT", "SETNAME", id, NULL);
  }

  free(id);

  if(status) {
    rCloseClientAsync(cl);
    redisxUnlockClient(cl);
    return x_trace(fn, NULL, X_NO_INIT);
  }

  redisxUnlockClient(cl);

  return X_SUCCESS;
}

/// \endcond

/**
 *  Initializes the Redis client library, and sets the hostname or IP address for the Redis server.
 *
 *  \param server       Server host name or numeric IP address, e.g. "127.0.0.1"
 *
 *  \return             X_SUCCESS or
 *                      X_FAILURE       if the IP address is invalid.
 *                      X_NULL          if the IP address is NULL.
 *
 *  @sa redisxInitSentinel()
 */
Redis *redisxInit(const char *server) {
  static const char *fn = "redisxInit";
  static int isInitialized = FALSE;

  Redis *redis;
  RedisPrivate *p;
  int i;

  if(server == NULL) {
    x_error(0, EINVAL, fn, "server name is NULL");
    return NULL;
  }

  if(!isInitialized) {
    // Initialize the thread attributes once only to avoid segfaulting...
    atexit(rShutdownAsync);
    isInitialized = TRUE;
  }

  // Allocate Redis, including private data...
  p = (RedisPrivate *) calloc(1, sizeof(RedisPrivate));
  x_check_alloc(p);

  redis = (Redis *) calloc(1, sizeof(Redis));
  x_check_alloc(redis);

  redis->priv = p;

  // Try set server...
  i = rSetServerAsync(redis, "server", server, 0);
  if(i) {
    free(redis->priv);
    free(redis);
    return x_trace_null(fn, NULL);
  }

  // Initialize mutexes
  pthread_mutex_init(&p->configLock, NULL);
  pthread_mutex_init(&p->subscriberLock, NULL);

  p->protocol = REDISX_RESP2;     // Default
  p->timeoutMillis = REDISX_DEFAULT_TIMEOUT_MILLIS;

  // Create clients...
  p->clients = (RedisClient *) calloc(3, sizeof(RedisClient));
  x_check_alloc(p->clients);

  // Initialize clients.
  for(i = REDISX_CHANNELS; --i >= 0; ) {
    ClientPrivate *cp = (ClientPrivate *) p->clients[i].priv;
    rInitClient(&p->clients[i], i);
    cp->redis = redis;
  }

  // Alias clients
  redis->interactive = &p->clients[REDISX_INTERACTIVE_CHANNEL];
  redis->pipeline = &p->clients[REDISX_PIPELINE_CHANNEL];
  redis->subscription = &p->clients[REDISX_SUBSCRIPTION_CHANNEL];

  rRegisterServer(redis);

  return redis;
}

/**
 * Initializes a Redis client with a Sentinel configuration of alternate servers, and the default
 * sentinel node connection timeout.
 *
 * @param serviceName     The service name as registered in the Sentinel server configuration.
 * @param serverList      An set of Sentinel servers to use to dynamically find the current master. A
 *                        copy of the supplied name will be used, so the argument passed can be
 *                        freely destroyed after the call.
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

  if(!serviceName) {
    x_error(0, EINVAL, fn, "input serviceName is NULL");
    return NULL;
  }

  if(!serviceName[0]) {
    x_error(0, EINVAL, fn, "input serviceName is empty");
    return NULL;
  }

  if(!serverList) {
    x_error(0, EINVAL, fn, "input serverList is NULL");
    return NULL;
  }
  if(nServers < 1) {
    x_error(0, EINVAL, fn, "invalid nServers: %d", nServers);
    return NULL;
  }

  if(serverList[0].host == NULL) {
    x_error(0, EINVAL, fn, "first server address is NULL");
    return NULL;
  }
  if(!serverList[0].host[0]) {
    x_error(0, EINVAL, fn, "first server address is empty");
    return NULL;
  }

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

static void rDestroySentinel(RedisSentinel *sentinel) {
  if(!sentinel) return;

  while(--sentinel->nServers >= 0) {
    RedisServer *server = &sentinel->servers[sentinel->nServers];
    if(server->host) free(server->host);
  }
  if(sentinel->serviceName) free(sentinel->serviceName);
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

  for(i = REDISX_CHANNELS; --i >= 0; ) {
    ClientPrivate *cp = (ClientPrivate *) p->clients[i].priv;
    if(!cp) continue;

    redisxDestroyRESP(cp->attributes);

    pthread_mutex_destroy(&cp->readLock);
    pthread_mutex_destroy(&cp->writeLock);
    pthread_mutex_destroy(&cp->pendingLock);

    free(cp);
  }

  rDestroySentinel(p->sentinel);
  redisxDestroyRESP(p->helloData);
  redisxClearConnectHooks(redis);
  redisxClearSubscribers(redis);

  if(p->username) free(p->username);
  if(p->password) free(p->password);
  if(p->clients) free(p->clients);

  free(p);

  rUnregisterServer(redis);

  if(redis->id) free(redis->id);

  free(redis);
}

/**
 * Set the size of the TCP/IP buffers (send and receive) for future client connections.
 *
 * @param size      (bytes) requested buffer size, or <= 0 to use default value
 */
void redisxSetTcpBuf(int size) {
  xvprintf("Redis-X> Setting TCP buffer to %d\n.", size);
  tcpBufSize = size;
}

/**
 * Sets a non-standard TCP port number to use for the Redis server, prior to calling
 * `redisxConnect()`.
 *
 * @param redis   Pointer to a Redis instance.
 * @param port    The TCP port number to use.
 *
 * @return                X_SUCCESS (0) if successful, or else X_NULL if the redis instance is NULL,
 *                        or X_NO_INIT if the redis instance is not initialized.
 *
 * @sa redisxConnect();
 */
int redisxSetPort(Redis *redis, int port) {
  RedisPrivate *p;

  prop_error("redisxSetPort", rConfigLock(redis));
  p = (RedisPrivate *) redis->priv;
  p->port = port;
  rConfigUnlock(redis);

  return X_SUCCESS;
}


/**
 * Sets a socket timeout for future client connections on a Redis instance. If not set (or set to zero
 * or a negative value), then the timeout will not be configured for sockets, and the system default
 * timeout values will apply.
 *
 * @param redis      The Redis instance
 * @param millis     [ms] The desired socket read/write timeout, or &lt;0 for socket default.
 * @return           X_SUCCESS (0) if successful, or else X_NULL if the redis instance is NULL,
 *                   or X_NO_INIT if the redis instance is not initialized.
 */
int redisxSetSocketTimeout(Redis *redis, int millis) {
  RedisPrivate *p;

  prop_error("redisxSetPort", rConfigLock(redis));
  p = (RedisPrivate *) redis->priv;
  p->timeoutMillis = millis > 0 ? millis : REDISX_DEFAULT_TIMEOUT_MILLIS;
  rConfigUnlock(redis);

  return X_SUCCESS;
}

/**
 * Connects to a Redis server.
 *
 * \param redis         Pointer to a Redis instance.
 * \param usePipeline   TRUE (non-zero) if Redis should be connected with a pipeline client also, or
 *                      FALSE (0) if only the interactive client is needed.
 *
 * \return              X_SUCCESS (0)      if successfully connected to the Redis server.
 *                      X_NO_INIT          if library was not initialized via initRedis().
 *                      X_ALREADY_OPEN     if already connected.
 *                      X_NO_SERVICE       if the connection failed.
 *                      X_NULL             if the redis argument is NULL.
 *
 * \sa redisxInit()
 * \sa redisxSetPort()
 * \sa redisxSetUser()
 * \sa redisxSetPassword()
 * \sa redisxSetTcpBuf()
 * \sa redisxSelectDB()
 * \sa redisxDisconnect()
 */
int redisxConnect(Redis *redis, boolean usePipeline) {
  static const char *fn = "redisxConnect";
  int status;

  prop_error(fn, rConfigLock(redis));
  status = rConnectAsync(redis, usePipeline);
  rConfigUnlock(redis);
  prop_error(fn, status);

  return X_SUCCESS;
}

/**
 * Checks if a Redis instance is connected.
 *
 * \param redis         Pointer to a Redis instance.
 *
 * \return      TRUE (1) if the Redis instance is connected, or FALSE (0) otherwise.
 *
 */
boolean redisxIsConnected(Redis *redis) {
  const ClientPrivate *ip;

  if(redisxCheckValid(redis) != X_SUCCESS) return FALSE;
  ip = (ClientPrivate *) redis->interactive->priv;
  return ip->isEnabled;
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
void *RedisPipelineListener(void *pRedis) {
  static int counter, lastError;

  Redis *redis = (Redis *) pRedis;
  RedisPrivate *p;
  RedisClient *cl;
  ClientPrivate *cp;
  RESP *reply = NULL;
  void (*consume)(RESP *response);

  pthread_detach(pthread_self());

  xvprintf("Redis-X> Started processing pipelined responses...\n");

  if(redisxCheckValid(redis) != X_SUCCESS) return x_trace_null("RedisPipelineListener", NULL);

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
      if(cp->pendingRequests > 0) xvprintf("WARNING! pipeline disabled with %d pending requests in queue.\n", cp->pendingRequests);
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

/**
 * Starts the pipeline listener thread with the specified thread attributes.
 *
 * \param redis     Pointer to the Redis instance.
 * \param attr      The thread attributes to set for the pipeline listener thread.
 *
 * \return          0 if successful, or -1 if pthread_create() failed.
 *
 */
static int rStartPipelineListenerAsync(Redis *redis) {
  RedisPrivate *p = (RedisPrivate *) redis->priv;

#if SET_PRIORITIES
  struct sched_param param;
#endif

  p->isPipelineListenerEnabled = TRUE;

  if (pthread_create(&p->pipelineListenerTID, NULL, RedisPipelineListener, redis) == -1) {
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
