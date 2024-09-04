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
#include <pthread.h>
#include <sys/utsname.h>
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

/// \cond PRIVATE

// SHUT_RD is not defined on LynxOS for some reason, even though it should be...
#ifndef SHUT_RD
#  define SHUT_RD 0
#endif

/// \endcond

static int tcpBufSize = REDIS_TCP_BUF;

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
  return cp->idx != PIPELINE_CHANNEL;
}

/**
 * Configure the REDIS client sockets for optimal performance...
 *
 * \param socket        The socket file descriptor.
 * \param lowLatency    TRUE (non-zero) if socket is to be configured for low latency, or else FALSE (0).
 *
 */
static void rConfigSocket(int socket, boolean lowLatency) {
  const boolean enable = TRUE;

#if REDIS_TIMEOUT_SECONDS > 0
  {
    struct linger linger;
    struct timeval timeout;

    // Set a time limit for sending.
    timeout.tv_sec = REDIS_TIMEOUT_SECONDS;
    timeout.tv_usec = 0;
    if(setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, & timeout, sizeof(struct timeval)))
      perror("WARNING! Redis-X : socket send timeout not set");

    // Redis recommends simply dropping the connection. By turning SO_LINGER off, we'll
    // end up with a 'connection reset' error on Redis, avoiding the TIME_WAIT state.
    // It is faster than the 'proper' handshaking close if the server can handle it properly.
    linger.l_onoff = FALSE;
    linger.l_linger = 0;
    if(setsockopt(socket, SOL_SOCKET, SO_LINGER, & linger, sizeof(struct timeval)))
      perror("WARNING! Redis-X : socket linger not set");
  }
#endif

#if __linux__
  {
    const int tos = lowLatency ? IPTOS_LOWDELAY : IPTOS_THROUGHPUT;

    // Optimize service for latency or throughput
    // LynxOS 3.1 does not support IP_TOS option...
    if(setsockopt(socket, IPPROTO_IP, IP_TOS, &tos, sizeof(tos)))
      perror("WARNING! Redis-X : socket type-of-service not set.");
  }
#endif

#if !(__Lynx__ && __powerpc__)
    // Send packets immediately even if small...
    if(lowLatency) if(setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, & enable, sizeof(int)))
      perror("WARNING! Redis-X : socket tcpnodelay not enabled.");
#endif

  // Check connection to remote every once in a while to detect if it's down...
  if(setsockopt(socket, SOL_SOCKET, SO_KEEPALIVE, & enable, sizeof(int)))
    perror("WARNING! Redis-X : socket keep-alive not enabled");

  // Allow to reconnect to closed RedisX sockets immediately
  //  if(setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, & enable, sizeof(int)))
  //    perror("WARNING! Redis-X : socket reuse address not enabled");

  // Set the TCP buffer size to use. Larger buffers facilitate more throughput.
  if(tcpBufSize > 0) {
    if(setsockopt(socket, SOL_SOCKET, SO_SNDBUF, &tcpBufSize, sizeof(int)))
      perror("WARNING! Redis-X : socket send buf size not set");

    if(setsockopt(socket, SOL_SOCKET, SO_RCVBUF, &tcpBufSize, sizeof(int)))
      perror("WARNING! Redis-X : socket send buf size not set");
  }
}

/**
 * Authenticates on the Redis server by sending the previously set password via AUTH.
 *
 * @param cl    Pointer to the Redis client to authenticate
 * @return      X_SUCCESS (0) if succssfully authenticated on the server, or else an
 *              appropriate error.
 *
 * @sa redisxSetPassword()
 */
static int rAuthAsync(RedisClient *cl) {
  ClientPrivate *cp = (ClientPrivate *) cl->priv;
  const RedisPrivate *p = (RedisPrivate *) cp->redis->priv;
  RESP *reply;

  int status = redisxSendRequestAsync(cl, "AUTH", p->password, NULL, NULL);
  if(status) return redisxError("rAuthAsync()", status);

  reply = redisxReadReplyAsync(cl);
  status = redisxCheckRESP(reply, RESP_SIMPLE_STRING, -1);
  redisxDestroyRESP(reply);

  return status;
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
 *              X_NO_SERVICE        if there was an error connecting to Redis
 *
 *              or else an error returned by rConnectClientAsync().
 *
 * \sa rConnectClientAsync()
 *
 */
static int rConnectAsync(Redis *redis, boolean usePipeline) {
  int status = X_SUCCESS;
  RedisPrivate *p = (RedisPrivate *) redis->priv;
  const ClientPrivate *ip = (ClientPrivate *) redis->interactive->priv;
  const ClientPrivate *pp = (ClientPrivate *) redis->pipeline->priv;
  Hook *f;

  if(redisxIsConnected(redis)) {
    fprintf(stderr, "WARNING! Redis-X : already connected.\n");
    return X_ALREADY_OPEN;
  }

  if(!ip->isEnabled) {
    static int warnedInteractive;

    xvprintf("Redis-X> Connect interactive client.\n");
    status = rConnectClient(redis, INTERACTIVE_CHANNEL);

    if(status) {
      if(!warnedInteractive) {
        fprintf(stderr, "ERROR! Redis-X : interactive client connection failed: %s\n", redisxErrorDescription(status));
        warnedInteractive = TRUE;
      }
      return X_NO_SERVICE;
    }
    warnedInteractive = FALSE;
  }

  if(usePipeline) {
    if(!pp->isEnabled) {
      static int warnedPipeline;

      xvprintf("Redis-X> Connect pipeline client.\n");
      status = rConnectClient(redis, PIPELINE_CHANNEL);

      if(status) {
        if(!warnedPipeline) {
          fprintf(stderr, "ERROR! Redis-X : pipeline client connection failed: %s\n", redisxErrorDescription(status));
          warnedPipeline = TRUE;
        }
        return X_NO_SERVICE;
      }
      warnedPipeline = FALSE;
    }

    status = rStartPipelineListenerAsync(redis);
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

  if(status) xvprintf("WARNING! Redis-X: client %d close socket error %d.\n", cp->idx, status);
}

/**
 * Resets the client properties for the specified REDIS client.
 *
 * \param client        Pointer to the REDIS client that is to be reset/initialized.
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
 * Closes the REDIS client on the specified communication channel. It is assused the caller
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
 * Closes the REDIS client on the specified communication channel. It is assused the caller
 * has an exclusive lock on the Redis configuration to which the client belongs.
 *
 * \param cl        Pointer to the Redis client instance.
 *
 */
void rCloseClient(RedisClient *cl) {
  redisxLockClient(cl);
  rCloseClientAsync(cl);
  redisxUnlockClient(cl);
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
  return rConnectAsync(redis, usePipeline);
}

/**
 * Disconnect all clients from the REDIS server.
 *
 * \param redis         Pointer to a Redis instance.
 *
 */
void redisxDisconnect(Redis *redis) {
  if(redis == NULL) return;

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
 *              or else an error as would be returned by redisxConnect().
 *
 */
int redisxReconnect(Redis *redis, boolean usePipeline) {
  int status;

  if(redis == NULL) return redisxError("redisxReconnect()", X_NULL);

  rConfigLock(redis);
  status = rReconnectAsync(redis, usePipeline);
  rConfigUnlock(redis);

  return status;
}

/**
 * Shuts down the Redis connection immediately. It does not obtain excluive locks
 * to either configuration settings or to open channels. As such it should only
 * be called to clean up an otherwise terminated program.
 *
 * @param redis   Pointer to the Redis intance to shut down.
 */
void rShutdownLinkAsync(Redis *redis) {
  RedisPrivate *p = (RedisPrivate *) redis->priv;
  int i;

  // NOTE: Don't use client locks, as they may deadlock when trying to shut down...
  for(i=0; i<REDISX_CHANNELS; i++) rDisconnectClientAsync(&p->clients[i]);
}

/// \cond PRIVATE

/**
 * Initializes a redis client structure for the specified communication channel
 *
 * @param cl
 * @param idx
 */
void rInitClient(RedisClient *cl, enum redisx_channel idx) {
  ClientPrivate *cp;

  cl->priv = calloc(1, sizeof(ClientPrivate));
  cp = (ClientPrivate *) cl->priv;
  cp->idx = idx;
  pthread_mutex_init(&cp->readLock, NULL);
  pthread_mutex_init(&cp->writeLock, NULL);
  pthread_mutex_init(&cp->pendingLock, NULL);
  rResetClientAsync(cl);
}

/**
 * Connects the specified REDIS client to the REDIS server.
 *
 * \param redis         Pointer to a Redis instance.
 * \param channel       INTERACTIVE_CHANNEL, PIPELINE_CHANNEL, or SUBSCRIPTION_CHANNEL
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
  static boolean warnedFailed;

  struct sockaddr_in serverAddress;
  const RedisPrivate *p;
  RedisClient *cl;
  ClientPrivate *cp;

  const char *channelID;
  char host[200], *id;
  int status;
  int sock;

  cl = redisxGetClient(redis, channel);

  p = (RedisPrivate *) redis->priv;
  cp = (ClientPrivate *) cl->priv;

  serverAddress.sin_family      = AF_INET;
  serverAddress.sin_port        = htons(REDIS_TCP_PORT);
  serverAddress.sin_addr.s_addr = p->addr;
  memset(serverAddress.sin_zero, '\0', sizeof(serverAddress.sin_zero));

  if((sock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0) {
    fprintf(stderr, "ERROR! Redis-X : client %d socket creation failed.\n", channel);
    return X_NO_SERVICE;
  }

  rConfigSocket(sock, rIsLowLatency(cp));

  while(connect(sock, (struct sockaddr *) &serverAddress, sizeof(serverAddress)) != 0) {
    if(!warnedFailed) {
      perror("ERROR! Redis-X: connect failed");
      warnedFailed = TRUE;
    }
    close(sock);
    return X_NO_INIT;
  }

  warnedFailed = FALSE;

  xvprintf("Redis-X> client %d assigned socket fd %d.\n", channel, sock);

  redisxLockClient(cl);

  cp->socket = sock;
  cp->isEnabled = TRUE;

  if(p->password) {
    status = rAuthAsync(cl);
    if(status) {
      rCloseClientAsync(cl);
      redisxUnlockClient(cl);
      return status;
    }
  }

  // Set the client name in Redis.
  gethostname(host, sizeof(host));

  id = (char *) malloc(strlen(host) + 100);      // <host>:pid-<pid>:<channel> + termination;
  switch(cp->idx) {
    case INTERACTIVE_CHANNEL: channelID = "interactive"; break;
    case PIPELINE_CHANNEL: channelID = "pipeline"; break;
    case SUBSCRIPTION_CHANNEL: channelID = "subscription"; break;
    default: channelID = "unknown";
  }

  sprintf(id, "%s:pid-%d:%s", host, (int) getppid(), channelID);

  status = redisxSkipReplyAsync(cl);
  if(!status) status = redisxSendRequestAsync(cl, "CLIENT", "SETNAME", id, NULL);

  free(id);

  if(status) {
    rCloseClientAsync(cl);
    redisxUnlockClient(cl);
    return X_NO_INIT;
  }

  redisxUnlockClient(cl);

  return X_SUCCESS;
}

/// \endcond

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
 * Returns the current TCP/IP buffer size (send and receive) to be used for future client connections.
 *
 * @return      (bytes) future TCP/IP buffer size, 0 if system default.
 */
int redisxGetTcpBuf() {
  return tcpBufSize > 0 ? tcpBufSize : 0;
}

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
int simpleHostnameToIP(const char *hostName, char *ip) {
  struct hostent *h;
  struct in_addr **addresses;
  int i;

  *ip = '\0';
  if(hostName == NULL) return X_NULL;

  if ((h = gethostbyname((char *) hostName)) == NULL) {
    fprintf(stderr, "ERROR! Host lookup failed for: %s.\n", hostName);
    return X_NAME_INVALID;
  }

  addresses = (struct in_addr **) h->h_addr_list;

  for(i = 0; addresses[i] != NULL; i++) {
    //Return the first one;
    strcpy(ip, inet_ntoa(*addresses[i]));
    return X_SUCCESS;
  }

  return X_NULL;
}



/**
 * Sets a non-standard TCP port number to use for the Redis server, prior to calling
 * `redisxConnect()`.
 *
 * @param redis   Pointer to a Redis instance.
 * @param port    The TCP port number to use.
 *
 * @sa redisxConnect();
 */
void redisxSetPort(Redis *redis, int port) {
  RedisPrivate *p;

  if(redis == NULL) {
    redisxError("redisxSetPort()", X_NULL);
    return;
  }

  p = (RedisPrivate *) redis->priv;
  p->port = port;
}

/**
 * Connects to a REDIS server.
 *
 * \param redis         Pointer to a Redis instance.
 * \param usePipeline   TRUE (non-zero) if Redis should be connected with a pipeline client also, or
 *                      FALSE (0) if only the interactive client is needed.
 *
 * \return              X_SUCCESS (0)      if successfully connected to the REDIS server.
 *                      X_NO_INIT          if library was not initialized via initRedis().
 *                      X_ALREADY_OPEN     if already connected.
 *                      X_NO_SERVICE       if the connection failed.
 *                      X_NULL             if the redis argument is NULL.
 *
 * \sa redisxInit()
 * \sa redisxSetPort()
 * \sa redisxSetTcpBuf()
 * \sa redisxDisconnect()
 */
int redisxConnect(Redis *redis, boolean usePipeline) {
  static const char *funcName = "redisxConnect()";
  int status;

  if(redis == NULL) return redisxError(funcName, X_NULL);

  rConfigLock(redis);

  status = rConnectAsync(redis, usePipeline);

  rConfigUnlock(redis);

  if(status) return redisxError(funcName, status);

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

  if(redis == NULL) return FALSE;

  ip = (ClientPrivate *) redis->interactive->priv;
  return ip->isEnabled;
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
 * Returns the redis client for a given connection type in a Redis instance.
 *
 * \param redis         Pointer to a Redis instance.
 * \param channel       INTERACTIVE_CHANNEL, PIPELINE_CHANNEL, or SUBSCRIPTION_CHANNEL
 *
 * \return      Pointer to the matching Redis client, or NULL if the channel argument is invalid.
 *
 */
RedisClient *redisxGetClient(Redis *redis, enum redisx_channel channel) {
  RedisPrivate *p;

  if(redis == NULL) return NULL;

  p = (RedisPrivate *) redis->priv;
  if(channel < 0 || channel >= REDISX_CHANNELS) return NULL;
  return &p->clients[channel];
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
 * @sa redisxLockEnabled()
 * @sa redisxUnlockClient()
 */
int redisxLockClient(RedisClient *cl) {
  static const char *funcName = "redisLockClient()";
  ClientPrivate *cp;
  int status;

  if(cl == NULL) return redisxError(funcName, X_NULL);
  cp = (ClientPrivate *) cl->priv;

  status = pthread_mutex_lock(&cp->writeLock);
  if(status) {
    fprintf(stderr, "WARNING! Redis-X : %s failed with code: %d.\n", funcName, status);
    return redisxError(funcName, X_FAILURE);
  }

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
 *               REDIS_INVALID_CHANNEL  if the channel is enabled/connected.
 *
 * @sa redisxLockClient()
 * @sa redisxUnlockClient()
 */
int redisxLockEnabled(RedisClient *cl) {
  static const char *funcName = "redisxLockEnabled()";
  const ClientPrivate *cp;
  int status = redisxLockClient(cl);
  if(status) return redisxError(funcName, status);

  cp = (ClientPrivate *) cl->priv;
  if(!cp->isEnabled) {
    redisxUnlockClient(cl);
    return redisxError(funcName, REDIS_INVALID_CHANNEL);
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
 * @sa redisxLockEnabled()
 */
int redisxUnlockClient(RedisClient *cl) {
  static const char *funcName = "redisxUnlockClient()";
  ClientPrivate *cp;
  int status;

  if(cl == NULL) return redisxError(funcName, X_NULL);
  cp = (ClientPrivate *) cl->priv;

  status = pthread_mutex_unlock(&cp->writeLock);
  if(status) fprintf(stderr, "WARNING! Redis-X : %s failed with code: %d.\n", funcName, status);

  return status ? redisxError(funcName, X_FAILURE) : X_SUCCESS;
}

