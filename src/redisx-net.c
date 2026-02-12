/**
 * @file
 *
 * @date Created  on Aug 26, 2024
 * @author Attila Kovacs
 *
 *   Network layer management functions for the RedisX library.
 *
 *
 * Notes:
 *
 *  - Some older platforms (e.g. LynxOS 3.1.0) do not have proper support for IPv6. Therefore
 *    this source code will assume IPv6 support starting POSIX-1.2001 only. We'll use
 *    _POSIX_C_SOURCE &lt;= 200112L to decide whether or not to build with IPv6 support.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <sys/utsname.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#if __Lynx__ && __powerpc__
#  include <socket.h>
#  include <time.h>
#else
#  include <sys/time.h>
#  include <netinet/ip.h>
#  include <sys/types.h>    // getaddrinfo()
#  include <sys/socket.h>
#endif
#include <netdb.h>

#include "redisx-priv.h"

/// \cond PRIVATE
extern int rDiscoverSentinelAsync(Redis *redis);
extern int rConfirmMasterRoleAsync(Redis *redis);
extern void rDestroySentinel(RedisSentinel *sentinel);
/// \endcond

static int rStartPipelineListenerAsync(Redis *redis);
static int rReconnectAsync(Redis *redis, boolean usePipeline);
static void rDisconnectClientAsync(RedisClient *cl);

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



/**
 * Gets an IP address string for a given host name. If more than one IP address is associated with a host name, the first one
 * is returned.
 *
 * \param hostName      The host name, e.g. "localhost"
 * \param[out] ip       Pointer to the string buffer to which to write the corresponding IP.
 * \param[out] addr     Pointer to sockaddr structure to populate. It should accommodate both
 *                      in_addr and in6_addr data types.
 *
 * \return              AF_INET         The host name was matched to an IPv4 address.
 *                      AF_INET6        The host name was matched to an IPv6 address.
 *                      X_NAME_INVALID  if the no host is known by the specified name.
 *                      X_NULL          if hostName is NULL or if it is not associated to any valid IP address.
 */
static int hostnameToIP(const char *hostName, char *ip, void *addr) {
  static const char *fn = "hostnameToIP";

#if _POSIX_C_SOURCE >= 200112L
  // getaddrinfo() is POSIX-1.2001, so it appears in GCC 3.0, more or less...
  struct addrinfo *infList, *inf;
  int fam;

  *ip = '\0';

  if(hostName == NULL) return x_error(X_NULL, EINVAL, fn, "input hostName is NULL");
  if(!hostName[0]) return x_error(X_NULL, EINVAL, fn, "input hostName is empty");

  if(getaddrinfo(hostName, NULL, NULL, &infList) != 0)
    return x_error(X_NAME_INVALID, errno, fn, "host lookup failed for: %s.", hostName);

  // Use the first IPv4 or IPv6 address
  for(inf = infList; inf != NULL; inf = inf->ai_next)
    if(inf->ai_family == AF_INET || inf->ai_family == AF_INET6) break;

  if(!inf || !inf->ai_addr) {
    freeaddrinfo(infList);
    return x_error(X_NAME_INVALID, errno, fn, "host has no address: %s.", hostName);
  }

  fam = inf->ai_family;
  if(fam == AF_INET6)
    *(struct in6_addr *) addr = ((struct sockaddr_in6 *) inf->ai_addr)->sin6_addr;
  else
    *(struct in_addr *) addr = ((struct sockaddr_in *) inf->ai_addr)->sin_addr;

  freeaddrinfo(infList);
  inet_ntop(fam, addr, ip, IP_ADDRESS_LENGTH);

  return fam;
#else
  // For earlier GCC use gethostbyname() instead.
  const struct hostent  *server;
  struct in_addr **addresses;

  server = gethostbyname((char *) hostName);
  if (server == NULL)
    return x_error(X_NAME_INVALID, errno, fn, "host lookup failed for: %s.", hostName);

  addresses = (struct in_addr **) server->h_addr_list;

  if(!addresses || !addresses[0])
  return x_error(X_NULL, ENODEV, fn, "no valid address for host %s", hostName);

  // Return the first one...
  *(struct in_addr *) addr = *addresses[0];
  strcpy(ip, inet_ntoa(*addresses[0]));

  return AF_INET;
#endif
}

/**
 * Configures a new server by name or IP address and port number for a given Redis instance
 *
 * @param redis       A Redis instance
 * @param desc        The type of server, e.g. "master", "replica", "sentinel-18"
 * @param hostname    The new host name or IP address
 * @param port        The new port number, or &lt=0 to use the default Redis port.
 * @return            X_SUCCESS (0) if successful or else an error code &lt;0.
 */
int rSetServerAsync(Redis *redis, const char *desc, const char *hostname, int port) {
  static const char *fn = "rSetServer";

  RedisPrivate *p = (RedisPrivate *) redis->priv;
  char ipAddress[IP_ADDRESS_LENGTH] = {'\0'};

  if(!hostname) return x_error(X_NULL, EINVAL, fn, "%s address is NULL", desc);
  if(!hostname[0]) return x_error(X_NAME_INVALID, EINVAL, fn, "%s name is empty", desc);

  p->in_family = hostnameToIP(hostname, ipAddress, (void *) &p->addr);
  if(p->in_family < 0) return x_trace(fn, desc, p->in_family);

  p->hostname = xStringCopyOf(hostname);
  p->port = port > 0 ? port : REDISX_TCP_PORT;

  if(redis->id) free(redis->id);
  redis->id = xStringCopyOf(ipAddress);

  return X_SUCCESS;
}

/**
 * Configure the Redis client sockets for optimal performance...
 *
 * \param socket          The socket file descriptor.
 * \param timeoutMillis   [ms] Socket read/write timeout, or &lt;=0 to no set.
 * \param tcpBufSize      [bytes] Socket read / write buffer sizes, or &lt;=0 to not set;
 * \param lowLatency      TRUE (non-zero) if socket is to be configured for low latency, or else FALSE (0).
 *
 */
static void rConfigSocket(int socket, int timeoutMillis, int tcpBufSize, boolean lowLatency) {
  static const char *fn = "RedisX";

  const boolean enable = TRUE;
  struct linger linger;

  // Redis recommends simply dropping the connection. By turning SO_LINGER off, we'll
  // end up with a 'connection reset' error on Redis, avoiding the TIME_WAIT state.
  // It is faster than the 'proper' handshaking close if the server can handle it properly.
  linger.l_onoff = FALSE;
  linger.l_linger = 0;
  if(setsockopt(socket, SOL_SOCKET, SO_LINGER, & linger, sizeof(struct linger)))
    x_warn(fn, "socket linger not set: %s", strerror(errno));

  if(timeoutMillis > 0) {
    struct timeval timeout;

    // Set a time limit for sending.
    timeout.tv_sec = timeoutMillis / 1000;
    timeout.tv_usec = 1000 * (timeoutMillis % 1000);
    if(setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, & timeout, sizeof(struct timeval)))
      x_warn(fn, "socket send timeout not set: %s", strerror(errno));
  }

#if __linux__
  {
    const int tos = lowLatency ? IPTOS_LOWDELAY : IPTOS_THROUGHPUT;

    // Optimize service for latency or throughput
    // LynxOS 3.1 does not support IP_TOS option...
    if(setsockopt(socket, IPPROTO_IP, IP_TOS, &tos, sizeof(tos)))
      x_warn(fn, "socket type-of-service not set: %s", strerror(errno));
  }
#endif

#if !(__Lynx__ && __powerpc__)
  // Send packets immediately even if small...
  if(lowLatency) if(setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, & enable, sizeof(int)))
    x_warn(fn, "socket tcpnodelay not enabled: %s", strerror(errno));
#endif

  // Check connection to remote every once in a while to detect if it's down...
  if(setsockopt(socket, SOL_SOCKET, SO_KEEPALIVE, & enable, sizeof(int)))
    x_warn(fn, "socket keep-alive not enabled: %s", strerror(errno));

  // Allow to reconnect to closed RedisX sockets immediately
  //  if(setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, & enable, sizeof(int)))
  //    x_warn(id, "socket reuse address not enabled: %s", strerror(errno));

  // Set the TCP buffer size to use. Larger buffers facilitate more throughput.
  if(tcpBufSize > 0) {
    if(setsockopt(socket, SOL_SOCKET, SO_SNDBUF, &tcpBufSize, sizeof(int)))
      x_warn(fn, "socket send buf size not set: %s", strerror(errno));

    if(setsockopt(socket, SOL_SOCKET, SO_RCVBUF, &tcpBufSize, sizeof(int)))
      x_warn(fn, "socket send buf size not set: %s", strerror(errno));
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

  int status = p->config.username ?
          redisxSendRequestAsync(cl, "AUTH", p->config.username, p->config.password, NULL) :
          redisxSendRequestAsync(cl, "AUTH", p->config.password, NULL, NULL);
  prop_error(fn, status);

  reply = redisxReadReplyAsync(cl, &status);
  prop_error(fn, status);

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

/**
 * Same as rConnectClient() but called with the client's mutex already locked.
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
 * \sa rConnectClient()
 *
 */
int rConnectAsync(Redis *redis, boolean usePipeline) {
  static const char *fn = "rConnectAsync";

  int status = X_SUCCESS;
  const RedisPrivate *p = (RedisPrivate *) redis->priv;
  const ClientPrivate *ip = (ClientPrivate *) redis->interactive->priv;
  const ClientPrivate *pp = (ClientPrivate *) redis->pipeline->priv;
  Hook *f;

  if(redisxIsConnected(redis)) {
    x_warn(fn, "WARNING! Redis-X: already connected to %s\n", redis->id);
    return X_ALREADY_OPEN;
  }

  if(p->sentinel) prop_error(fn, rDiscoverSentinelAsync(redis));

  if(!ip->isEnabled) {
    static int warnedInteractive;

    xvprintf("Redis-X> Connect interactive client.\n");
    status = rConnectClientAsync(redis, REDISX_INTERACTIVE_CHANNEL);

    if(status) {
      if(!warnedInteractive) {
        x_warn("RedisX", "interactive client connection failed: %s\n", redisxErrorDescription(status));
        warnedInteractive = TRUE;
      }
      return x_trace(fn, "interactive", X_NO_SERVICE);
    }
    warnedInteractive = FALSE;
  }

  if(p->sentinel) {
    if(rConfirmMasterRoleAsync(redis) != X_SUCCESS) prop_error(fn, rReconnectAsync(redis, usePipeline));
  }

  if(usePipeline) {
    if(!pp->isEnabled) {
      static int warnedPipeline;

      xvprintf("Redis-X> Connect pipeline client.\n");
      status = rConnectClientAsync(redis, REDISX_PIPELINE_CHANNEL);

      if(status) {
        if(!warnedPipeline) {
          x_warn("RedisX", "pipeline client connection failed: %s\n", redisxErrorDescription(status));
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
  for(f = p->config.firstConnectCall; f != NULL; f = f->next) f->call(redis);

  xvprintf("Redis-X> connect complete.\n");

  return status;
}

static void rShutdownClientAsync(RedisClient *cl) {
  ClientPrivate *cp = (ClientPrivate *) cl->priv;
  const int sock = cp->socket;      // Local copy of socket fd that won't possibly change mid-call.

  cp->isEnabled = FALSE;            // No new synchronized requests or async reads.

  if(sock < 0) return;

  shutdown(sock, SHUT_RD);
}

static void rDisconnectClientAsync(RedisClient *cl) {
  ClientPrivate *cp = (ClientPrivate *) cl->priv;
  const int sock = cp->socket;      // Local copy of socket fd that won't possibly change mid-call.

  rShutdownClientAsync(cl);

  if(sock >= 0) {
    int status;

    cp->socket = -1;                  // Reset the channel's socket descriptor to 'unassigned'

    status = close(sock);
    if(status) x_warn("rDisconnectClientAsync", "client %d close() error: %s.\n", (int) cp->idx, strerror(errno));
  }
}

/**
 * Resets the client properties for the specified Redis client.
 *
 * \param client        Pointer to the Redis client that is to be reset/initialized.
 *
 */
static void rResetClientAsync(RedisClient *cl) {
#if(WITH_TLS)
  extern void rDestroyClientTLS(ClientPrivate *cp);
#endif

  ClientPrivate *cp = (ClientPrivate *) cl->priv;

  pthread_mutex_lock(&cp->pendingLock);
  cp->pendingRequests = 0;
  pthread_mutex_unlock(&cp->pendingLock);

  cp->isEnabled = FALSE;
  cp->available = 0;
  cp->next = 0;

#if(WITH_TLS)
  rDestroyClientTLS(cp);
#endif

  cp->socket = -1;                  // Reset the channel's socket descriptor to 'unassigned'
}

/**
 * Closes the Redis client on the specified communication channel. This call assumes that
 * the caller has an exlusive lock on the client.
 *
 * \param cl        Pointer to the Redis client instance.
 *
 */
void rCloseClientAsync(RedisClient *cl) {
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
 * Same as redisxDisconnect() except without the exclusive configuration locking of the
 * Redis instance.
 *
 * \param redis         Pointer to a Redis instance.
 *
 */
void rDisconnectAsync(Redis *redis) {
  RedisPrivate *p = (RedisPrivate *) redis->priv;
  Hook *f;

  // Disable pipeline listener...
  p->isPipelineListenerEnabled = FALSE;

  // Gracefully end subscriptions..,.
  p->isSubscriptionListenerEnabled = FALSE;

  // Stop reading from clients immediately.
  // (stops background threads, releases read locks)
  // Shut down clients immediately
  rShutdownClientAsync(redis->subscription);
  rShutdownClientAsync(redis->pipeline);
  rShutdownClientAsync(redis->interactive);

  // Close clients after obtaining exclusive locks on them...
  rCloseClient(redis->subscription);
  rCloseClient(redis->pipeline);
  rCloseClient(redis->interactive);

  // Call the cleanup hooks...
  for(f = p->config.firstCleanupCall; f != NULL; f = f->next) f->call(redis);

  xvprintf("Redis-X> disconnect complete.\n");
}

/**
 * Same as redisxReconnect() except without the exclusive locking mechanism.
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
 * Shuts down and destroys all Redis clients immediately. It does not obtain
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
    redisxDestroy(l->redis);
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
static void rInitClient(Redis *redis, enum redisx_channel idx) {
  RedisPrivate *p = (RedisPrivate *) redis->priv;
  RedisClient *cl = &p->clients[idx];
  ClientPrivate *cp;

  cp = calloc(1, sizeof(ClientPrivate));
  x_check_alloc(cp);

  cp->redis = redis;
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

static int rHelloAsync(RedisClient *cl, const char *clientID) {
  ClientPrivate *cp = (ClientPrivate *) cl->priv;
  RedisPrivate *p = (RedisPrivate *) cp->redis->priv;
  RedisConfig *config = &p->config;
  RESP *reply;
  char proto[20];
  const char *args[6];
  int status, k = 0;

  args[k++] = "HELLO";

  // Try HELLO and see what we get back...
  sprintf(proto, "%d", (int) config->protocol);
  args[k++] = proto;

  if(p->config.password) {
    args[k++] = "AUTH";
    args[k++] = config->username ? config->username : "default";
    args[k++] = config->password;
  }

  args[k++] = "SETNAME";
  args[k++] = clientID;

  status = redisxSendArrayRequestAsync(cl, args, NULL, k);
  if(status != X_SUCCESS) return status;

  reply = redisxReadReplyAsync(cl, &status);
  if(!status) status = redisxCheckRESP(reply, RESP3_MAP, 0);

  if(status == X_SUCCESS) {
    RedisMap *e = redisxGetKeywordEntry(reply, "proto");
    if(e && e->value->type == RESP_INT) {
      config->protocol = e->value->n;
      xvprintf("Confirmed protocol %d\n", config->protocol);
    }

    redisxDestroyRESP(p->helloData);
    p->helloData = reply;
  }
  else xvprintf("! Redis-X: HELLO failed: %s\n", redisxErrorDescription(status));

  return status;
}


/**
 * Connects the specified Redis client to the Redis server. It should be called with the the configuration
 * mutex of the Redis instance locked.
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
 *
 * @sa rConfigLock()
 */
int rConnectClientAsync(Redis *redis, enum redisx_channel channel) {
  static const char *fn = "rConnectClient";

#if WITH_TLS
  extern int rConnectTLSClientAsync(ClientPrivate *cp, const TLSConfig *tls);
#endif

  union {
    struct sockaddr_in v4;
#if _POSIX_C_SOURCE >= 200112L
    struct sockaddr_in6 v6;
#endif
  } serverAddress = {};
  int addrlen = sizeof(struct sockaddr_in);

  struct utsname u;
  RedisPrivate *p;
  RedisClient *cl;
  ClientPrivate *cp;
  RedisConfig *config;

  const char *channelID;
  char host[200], *id;
  int status = X_SUCCESS;
  uint16_t port;
  int sock;

  cl = redisxGetClient(redis, channel);

  p = (RedisPrivate *) redis->priv;
  cp = (ClientPrivate *) cl->priv;
  config = &p->config;

  port = p->port > 0 ? p->port : REDISX_TCP_PORT;

#if _POSIX_C_SOURCE >= 200112L
  if(p->in_family == AF_INET6) {
    serverAddress.v6.sin6_family = AF_INET6;
    serverAddress.v6.sin6_port   = htons(port);
    serverAddress.v6.sin6_addr   = p->addr.v6;
    addrlen = sizeof(struct sockaddr_in6);
  }
  else {
#endif
    serverAddress.v4.sin_family = AF_INET;
    serverAddress.v4.sin_port   = htons(port);
    serverAddress.v4.sin_addr   = p->addr.v4;
    memset(&serverAddress.v4.sin_zero, 0, sizeof(serverAddress.v4.sin_zero));
#if _POSIX_C_SOURCE >= 200112L
  }
#endif

  sock = socket(p->in_family, SOCK_STREAM, IPPROTO_TCP);
  if(sock < 0)
    return x_error(X_NO_SERVICE, errno, fn, "client %d socket creation failed", channel);

  rConfigSocket(sock, config->timeoutMillis, config->tcpBufSize, rIsLowLatency(cp));

  if(config->socketConf) {
    prop_error(fn, config->socketConf(sock, channel));
  }

#if WITH_TLS
  if(config->tls.enabled && rConnectTLSClientAsync(cp, &config->tls) != X_SUCCESS) {
    close(sock);
    return x_error(X_NO_INIT, errno, fn, "failed to connect (with TLS) to %s:%hu: %s", redis->id, port, strerror(errno));
  }
  else
#endif


  if(connect(sock, (struct sockaddr *) &serverAddress, addrlen) < 0) {
    close(sock);
    return x_error(X_NO_INIT, errno, fn, "failed to connect to %s:%hu: %s", redis->id, port, strerror(errno));
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

  if(config->hello) status = rHelloAsync(cl, id);

  if(status != X_SUCCESS) {
    status = X_SUCCESS;
    config->hello = FALSE;

    // No HELLO, go the old way...
    config->protocol = REDISX_RESP2;
    if(config->password) status = rAuthAsync(cl);

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
 *  \param server       Server host name or numeric IP address, e.g. "127.0.0.1". The string will
 *                      be copied, not referenced, for the internal configuration, such that the
 *                      string passed may be destroyed freely after the call.
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
  RedisConfig *config;
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
  if(rSetServerAsync(redis, "server", server, 0) != X_SUCCESS) {
    free(redis->priv);
    free(redis);
    return x_trace_null(fn, NULL);
  }

  // Initialize mutexes
  pthread_mutex_init(&p->configLock, NULL);
  pthread_mutex_init(&p->subscriberLock, NULL);

  config = &p->config;
  config->protocol = REDISX_RESP2;     // Default
  config->timeoutMillis = REDISX_DEFAULT_TIMEOUT_MILLIS;
  config->tcpBufSize = REDISX_TCP_BUF_SIZE;

  // Create clients...
  p->clients = (RedisClient *) calloc(3, sizeof(RedisClient));
  x_check_alloc(p->clients);

  // Initialize clients.
  for(i = REDISX_CHANNELS; --i >= 0; ) rInitClient(redis, i);

  // Alias clients
  redis->interactive = &p->clients[REDISX_INTERACTIVE_CHANNEL];
  redis->pipeline = &p->clients[REDISX_PIPELINE_CHANNEL];
  redis->subscription = &p->clients[REDISX_SUBSCRIPTION_CHANNEL];

  rRegisterServer(redis);

  return redis;
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


  redisxDestroyRESP(p->helloData);
  redisxClearSubscribers(redis);
  rDestroySentinel(p->sentinel);
  rClearConfig(&p->config);

  if(p->clients) free(p->clients);

  free(p);

  rUnregisterServer(redis);

  if(redis->id) free(redis->id);

  free(redis);
}

/**
 * Set the size of the TCP/IP buffers (send and receive) for future client connections.
 *
 * @param redis     Pointer to a Redis instance.
 * @param size      (bytes) requested buffer size, or <= 0 to use default value
 * @return          X_SUCCESS (0) if successful, or else X_NULL if the redis instance is NULL,
 *                  or X_NO_INIT if the redis instance is not initialized, or X_FAILURE
 *                  if Redis was initialized in Sentinel configuration.
 */
int redisxSetTcpBuf(Redis *redis, int size) {
  RedisPrivate *p;

  prop_error("redisxSetTcpBuf", rConfigLock(redis));
  p = (RedisPrivate *) redis->priv;
  p->config.tcpBufSize = size;
  rConfigUnlock(redis);

  return X_SUCCESS;
}

/**
 * Changes the host name for the Redis server, prior to calling `redisxConnect()`.
 *
 * @param redis   Pointer to a Redis instance.
 * @param host    New host name or IP address to use.
 *
 * @return                X_SUCCESS (0) if successful, or else X_NULL if the redis instance
 *                        or the host name is NULL, or X_NO_INIT if the redis instance is not
 *                        initialized, X_ALREADY_OPEN if the redis instance is currently
 *                        in a connected state, or X_FAILURE if Redis was initialized in Sentinel
 *                        configuration.
 *
 * @sa redisxSetPort()
 * @sa redisxConnect()
 */
int redisxSetHostname(Redis *redis, const char *host) {
  static const char *fn = "redisxSetPort";

  const RedisPrivate *p;
  int status = X_SUCCESS;

  if(!host) return x_error(X_NULL, EINVAL, fn, "host name is NULL");
  if(!host[0]) return x_error(X_NULL, EINVAL, fn, "host name is empty");

  prop_error(fn, rConfigLock(redis));
  p = (RedisPrivate *) redis->priv;

  if(redisxIsConnected(redis)) {
    rConfigUnlock(redis);
    return x_error(X_ALREADY_OPEN, EALREADY, fn, "already connected to %s:%d", p->hostname, p->port);
  }

  if(p->sentinel) status = x_error(X_FAILURE, EAGAIN, fn, "redis is in Sentinel configuration");
  else status = rSetServerAsync(redis, "server", host, p->port);
  rConfigUnlock(redis);

  return status;
}

/**
 * Sets a non-standard TCP port number to use for the Redis server, prior to calling
 * `redisxConnect()`.
 *
 * @param redis   Pointer to a Redis instance.
 * @param port    The TCP port number to use.
 *
 * @return                X_SUCCESS (0) if successful, or else X_NULL if the redis instance is NULL,
 *                        or X_NO_INIT if the redis instance is not initialized, X_ALREADY_OPEN
 *                        if the Redis instance is lready connected to a server, or X_FAILURE
 *                        if Redis was initialized in Sentinel configuration.
 *
 * @sa redisxSetHostname()
 * @sa redisxConnect()
 */
int redisxSetPort(Redis *redis, int port) {
  static const char *fn = "redisxSetPort";

  RedisPrivate *p;
  int status = X_SUCCESS;

  prop_error(fn, rConfigLock(redis));
  p = (RedisPrivate *) redis->priv;

  if(redisxIsConnected(redis)) {
    rConfigUnlock(redis);
    return x_error(X_ALREADY_OPEN, EALREADY, fn, "already connected to %s:%d", p->hostname, p->port);
  }

  if(p->sentinel) status = x_error(X_FAILURE, EAGAIN, fn, "redis is in Sentinel configuration");
  else p->port = port;
  rConfigUnlock(redis);

  return status;
}


/**
 * Sets a socket timeout for future client connections on a Redis instance. Effectively this is a timeout for
 * send() only. The timeout for interatvice replies is controlled separately via redisxSetReplyTimoeut().
 *
 * If not set (or set to zero or a negative value), then the timeout will not be configured for sockets, and
 * the system default timeout values will apply.
 *
 * @param redis      The Redis instance
 * @param millis     [ms] The desired socket read/write timeout, or &lt;0 for socket default.
 * @return           X_SUCCESS (0) if successful, or else X_NULL if the redis instance is NULL,
 *                   or X_NO_INIT if the redis instance is not initialized.
 *
 * @sa redisxSetReplyTimeout()
 */
int redisxSetSocketTimeout(Redis *redis, int millis) {
  RedisPrivate *p;

  prop_error("redisxSetPort", rConfigLock(redis));
  p = (RedisPrivate *) redis->priv;
  p->config.timeoutMillis = millis > 0 ? millis : REDISX_DEFAULT_TIMEOUT_MILLIS;
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
  static long counter, lastError;

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
    reply = redisxReadReplyAsync(cl, NULL);
    if(!reply) continue;

    counter++;

    if(reply->n < 0) {
      if(reply->n != lastError) fprintf(stderr, "ERROR! Redis-X: pipeline parse error: %d.\n", reply->n);
      lastError = reply->n;
      continue;
    }

    // Skip confirms...
    if(reply->type == RESP_SIMPLE_STRING) continue;

    consume = p->config.pipelineConsumerFunc;
    if(consume) consume(reply);

#if REDISX_LISTENER_YIELD_COUNT > 0
    // Allow the waiting processes to take control...
    if(counter % REDISX_LISTENER_YIELD_COUNT == 0) sched_yield();
#endif

  } // <-- End of listener loop...

  xvprintf("Redis-X> Stopped processing pipeline responses (%ld processed)...\n", counter);

  pthread_mutex_lock(&cp->pendingLock);
  if(cp->pendingRequests > 0) x_warn("RedisX", "pipeline disabled with %d pending requests in queue.\n", cp->pendingRequests);
  pthread_mutex_unlock(&cp->pendingLock);

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
