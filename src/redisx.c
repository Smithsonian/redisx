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
#include <errno.h>
#include <unistd.h>
#include <netdb.h>



#include "redisx.h"

/// \cond PRIVATE
///

#if DEBUG
#define SET_PRIORITIES              FALSE       ///< Disable if you want to use gdb to debug...
#else
#define SET_PRIORITIES              TRUE        ///< Disable if you want to use gdb to debug...
#endif

#if __Lynx__
#  define MIN_PRIORITY              17          ///< Interactive priority on LynxOS...
#else
#  define MIN_PRIORITY              1           ///< Minimum sched_priority on Linux...
#endif

#define LISTENER_PRIORITY           (MIN_PRIORITY + 24)  /// Listener priority. POSIX requires supporting at least 32 levels.
#define LISTENER_YIELD_COUNT        10          ///< yield after this many processed listener messages, <= 0 to disable yielding

#if (__Lynx__ && __powerpc__)
#  define SEND_YIELD_COUNT          1           ///< Yield after this many send() calls, <= 0 to disable yielding
#else
#  define SEND_YIELD_COUNT          (-1)        ///< Yield after this many send() calls, <= 0 to disable yielding
#endif

#define SCAN_INITIAL_CURSOR         "0"         ///< Initial cursor value for SCAN command.

#define IP_ADDRESS_LENGTH           40          ///< IPv6: 39 chars + termination.

// SHUT_RD is not defined on LynxOS for some reason, even though it should be...
#ifndef SHUT_RD
#define SHUT_RD 0
#endif

typedef struct MessageConsumer {
  Redis *redis;
  char *channelStem;          ///< channels stem that incoming channels must begin with to meet for this notification to be activated.
  RedisSubscriberCall func;
  struct MessageConsumer *next;
} MessageConsumer;


typedef struct Hook {
  void (*call)(void);
  struct Hook *next;
} Hook;


typedef struct {
  Redis *redis;                 ///< Pointer to the enclosing Redis instance
  int idx;                      ///< e.g. INTERACTIVE_CHANNEL, PIPELINE_CHANNEL, or SUBSCRIPTION_CHANNEL
  volatile boolean isEnabled;   ///< Whether the client is currecntly enabled for sending/receiving data
  pthread_mutex_t writeLock;    ///< A lock for writing and requests through this channel...
  pthread_mutex_t readLock;     ///< A lock for reading from the channel...
  pthread_mutex_t pendingLock;  ///< A lock for updating pendingRequests...
  char in[REDIS_RCV_CHUNK_SIZE];      ///< Local input buffer
  int available;                ///< Number of bytes available in the buffer.
  int next;                     ///< Index of next unconsumed byte in buffer.
  int socket;                   ///< Changing the socket should require both locks!
  int pendingRequests;          ///< Number of request sent and not yet answered...
} ClientPrivate;


typedef struct {
  uint32_t addr;                ///< The 32-bit inet address
  char *password;               ///< Redis password (if any)

  RedisClient *clients;

  pthread_mutex_t configLock;

  int scanCount;                ///< Count argument to use in SCAN commands, or <= 0 for default

  pthread_t pipelineListenerTID;
  pthread_t subscriptionListenerTID;

  boolean isPipelineListenerEnabled;
  boolean isSubscriptionListenerEnabled;

  Hook *firstCleanupCall;
  Hook *firstConnectCall;

  void (*pipelineConsumerFunc)(RESP *response);
  void (*transmitErrorFunc)(Redis *redis, int channel, const char *op);

  pthread_mutex_t subscriberLock;
  MessageConsumer *subscriberList;

} RedisPrivate;


typedef struct ServerLink {
  Redis *redis;
  struct ServerLink *next;
} ServerLink;
/// \endcond

static ServerLink *serverList;
static pthread_mutex_t serverLock = PTHREAD_MUTEX_INITIALIZER;

static int tcpBufSize = REDIS_TCP_BUF;

// Local prototypes ------------------------------------------------------------->

static void rConfigLock(Redis *redis);
static void rConfigUnlock(Redis *redis);

static int rConnectAsync(Redis *redis, boolean usePipeline);
static void rDisconnectAsync(Redis *redis);
static int rReconnectAsync(Redis *redis, boolean usePipeline);

static void rShutdownAsync();
static void rShutdownLinkAsync(Redis *redis);

static void rCloseClient(RedisClient *cl);
static void rCloseClientAsync(RedisClient *cl);
static int rConnectClient(Redis *redis, int channel);
static int rConnectSubscriptionClientAsync(Redis *redis);
static void rEndSubscriptionAsync(Redis *redis);
static void rNotifyConsumers(Redis *redis, char *pattern, char *channel, char *msg, int length);
static int rIsGlobPattern(const char *str);

static void rResetClientAsync(RedisClient *cl);
static void rConfigSocket(int socket, boolean lowLatency);
static boolean rIsLowLatency(const ClientPrivate *cp);
static int rTransmitError(ClientPrivate *cp, const char *op);

static int rSendBytesAsync(ClientPrivate *cp, const char *buf, int l, boolean isLast);
static int rReadToken(ClientPrivate *cp, char *buf, int length);
static int rReadBytes(ClientPrivate *cp, char *buf, int length);

static int rStartPipelineListenerAsync(Redis *redis, pthread_attr_t *attr);
static int rStartSubscriptionListenerAsync(Redis *redis, pthread_attr_t *attr);

static void *RedisPipelineListener(void *pRedis);
static void *RedisSubscriptionListener(void *pRedis);


static char *hostName;

// The response listener threads...
static pthread_attr_t threadConfig;

/**
 * Waits to get exlusive access to configuring the properties of a Redis instance.
 *
 * \param redis         Pointer to a Redis instance.
 *
 */
static void rConfigLock(Redis *redis) {
  RedisPrivate *p = (RedisPrivate *) redis->priv;
  pthread_mutex_lock(&p->configLock);
}


/**
 * Relinquish exlusive access to configuring the properties of a Redis instance.
 *
 * \param redis         Pointer to a Redis instance.
 *
 */
static void rConfigUnlock(Redis *redis) {
  RedisPrivate *p = (RedisPrivate *) redis->priv;
  pthread_mutex_unlock(&p->configLock);
}


/**
 * Waits to get exlusive access to Redis scubscriber calls.
 *
 * \param redis         Pointer to a Redis instance.
 *
 */
static void rSubscriberLock(Redis *redis) {
  RedisPrivate *p = (RedisPrivate *) redis->priv;
  pthread_mutex_lock(&p->subscriberLock);
}


/**
 * Relinquish exlusive access to Redis scubscriber calls.
 *
 * \param redis         Pointer to a Redis instance.
 *
 */
static void rSubscriberUnlock(Redis *redis) {
  RedisPrivate *p = (RedisPrivate *) redis->priv;
  pthread_mutex_unlock(&p->subscriberLock);
}

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
int redisxSetTransmitErrorHandler(Redis *redis, void (*f)(Redis *redis, int channel, const char *op)) {
  RedisPrivate *p;

  if(!redis) return X_NULL;

  rConfigLock(redis);
  p = (RedisPrivate *) redis->priv;
  p->transmitErrorFunc = f;
  rConfigUnlock(redis);

  return X_SUCCESS;
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
 * Returns the current TCP/IP buffer size (send and receive) to be used for future client connections.
 *
 * @return      (bytes) future TCP/IP buffer size, 0 if system default.
 */
int redisxGetTcpBuf() {
  return tcpBufSize > 0 ? tcpBufSize : 0;
}

/**
 * Get exclusive write access to the specified REDIS channel.
 *
 * \param cl        Pointer to the Redis client instance.
 *
 * \return          X_SUCCESS           if the exclusive lock for the channel was successfully obtained
 *                  X_FAILURE           if pthread_mutex_lock() returned an error
 *                  X_NULL              if the client is NULL.
 */
int redisxLockClient(RedisClient *cl) {
  const char *funcName = "redisLockChannel()";
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
 * Relinquish exclusive write access to the specified REDIS channel
 *
 * \param cl        Pointer to the Redis client instance
 *
 * \return          X_SUCCESS           if the exclusive lock for the channel was successfully obtained
 *                  X_FAILURE           if pthread_mutex_lock() returned an error
 *                  X_NULL              if the client is NULL
 */
int redisxUnlockClient(RedisClient *cl) {
  const char *funcName = "redisxUnlockClient()";
  ClientPrivate *cp;
  int status;

  if(cl == NULL) return redisxError(funcName, X_NULL);
  cp = (ClientPrivate *) cl->priv;

  status = pthread_mutex_unlock(&cp->writeLock);
  if(status) fprintf(stderr, "WARNING! Redis-X : %s failed with code: %d.\n", funcName, status);

  return status ? redisxError(funcName, X_FAILURE) : X_SUCCESS;
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
 */
int redisxLockEnabled(RedisClient *cl) {
  const char *funcName = "redisxLockEnabled()";
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
 * Returns the redis client for a given connection type in a Redis instance.
 *
 * \param redis         Pointer to a Redis instance.
 * \param channel       INTERACTIVE_CHANNEL, PIPELINE_CHANNEL, or SUBSCRIPTION_CHANNEL
 *
 * \return      Pointer to the matching Redis client, or NULL if the channel argument is invalid.
 *
 */
RedisClient *redisxGetClient(Redis *redis, int channel) {
  RedisPrivate *p;

  if(redis == NULL) return NULL;

  p = (RedisPrivate *) redis->priv;
  if(channel < 0 || channel >= REDIS_CHANNELS) return NULL;
  return &p->clients[channel];
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
int hostnameToIP(const char *hostName, char *ip) {
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
 * Returns the host name on which this program is running. It returns a reference to the same
 * static variable every time. As such you should never call free() on the returned value.
 * Note, that only the leading part of the host name is returned, so for a host
 * that is registered as 'somenode.somedomain' only 'somenode' is returned.
 *
 * \return      The host name string (leading part only).
 *
 * \sa redisxSetHostName()
 *
 */
char *redisxGetHostName() {
  if(hostName == NULL) {
    struct utsname u;
    int i;
    uname(&u);

    // Keep only the leading part only...
    for(i=0; u.nodename[i]; i++) if(u.nodename[i] == '.') {
      u.nodename[i] = '\0';
      break;
    }

    hostName = xStringCopyOf(u.nodename);
  }
  return hostName;
}

/**
 * Changes the host name to the user-specified value instead of the default (leading component
 * of the value returned by gethostname()). Subsequent calls to redisxGetHostName() will return
 * the newly set value. An argument of NULL resets to the default.
 *
 * @param name      the host name to use, or NULL to revert to the default (leading component
 *                  of gethostname()).
 *
 * @sa redisxGetHostName()
 */
void redisxSetHostName(const char *name) {
  char *oldName = hostName;
  hostName = xStringCopyOf(name);
  if(oldName) free(oldName);
}

static Hook *createHook(void (*f)(void)) {
  Hook *h = (Hook *) calloc(1, sizeof(Hook));
  h->call = f;
  return h;
}

/**
 * Adds a connect call hook, provided it is not already part of the setup routine.
 *
 * \param redis         Pointer to a Redis instance.
 * \param setupCall     User-specified callback routine to be called after the Redis instance has been connected.
 *
 */
void redisxAddConnectHook(Redis *redis, const void (*setupCall)(void)) {
  RedisPrivate *p;

  if(redis == NULL) return;
  if(setupCall == NULL) return;

  xvprintf("Redis-X> Adding a connect callback.\n");

  p = (RedisPrivate *) redis->priv;

  rConfigLock(redis);
  if(p->firstConnectCall == NULL) p->firstConnectCall = createHook(setupCall);
  else {
    // Check if the specified hook is already added...
    Hook *k = p->firstConnectCall;
    while(k != NULL) {
      if(k->call == setupCall) break;
      if(k->next == NULL) k->next = createHook(setupCall);
      k = k->next;
    }
  }
  rConfigUnlock(redis);
}

/**
 * Removes a connect call hook.
 *
 * \param redis         Pointer to a Redis instance.
 * \param setupCall     User-specified callback routine to be called after the Redis instance has been connected.
 *
 */
void redisxRemoveConnectHook(Redis *redis, const void (*setupCall)(void)) {
  RedisPrivate *p;
  Hook *c, *last = NULL;

  if(redis == NULL) return;
  if(setupCall == NULL) return;

  xvprintf("Redis-X> Removing a connect callback.\n");

  p = (RedisPrivate *) redis->priv;

  rConfigLock(redis);
  c = p->firstConnectCall;

  while(c != NULL) {
    Hook *next = c->next;

    if(c->call == setupCall) {
      if(last == NULL) p->firstConnectCall = next;
      else last->next = next;
      free(c);
    }
    else last = c;
    c = next;
  }
  rConfigUnlock(redis);
}


/**
 * Removes all connect hooks, that is no user callbacks will be made when the specifed
 * Redis instance is connected.
 *
 * \param redis         Pointer to a Redis instance.
 *
 */
void redisxClearConnectHooks(Redis *redis) {
  RedisPrivate *p;
  Hook *c;

  if(redis == NULL) return;

  xvprintf("Redis-X> Clearing all connect callbacks.\n");

  p = (RedisPrivate *) redis->priv;

  rConfigLock(redis);
  c = p->firstConnectCall;

  while(c != NULL) {
    Hook *next = c->next;
    free(c);
    c = next;
  }
  rConfigUnlock(redis);
}



/**
 * Adds a cleanup call, provided it is not already part of the cleanup routine, for when the
 * specified Redis instance is disconnected.
 *
 * \param redis         Pointer to a Redis instance.
 * \param cleanupCall   User specified function to call when Redis is disconnected.
 *
 */
void redisxAddDisconnectHook(Redis *redis, const void (*cleanupCall)(void)) {
  RedisPrivate *p;

  if(redis == NULL) return;
  if(cleanupCall == NULL) return;

  xvprintf("Redis-X> Adding a disconnect callback.\n");

  p = (RedisPrivate *) redis->priv;

  rConfigLock(redis);
  if(p->firstCleanupCall == NULL) p->firstCleanupCall = createHook(cleanupCall);
  else {
    // Check if the specified hook is already added...
    Hook *k = p->firstCleanupCall;
    while(k != NULL) {
      if(k->call == cleanupCall) break;
      if(k->next == NULL) k->next = createHook(cleanupCall);
      k = k->next;
    }
  }
  rConfigUnlock(redis);
}

/**
 * Removes a cleanup call hook for when the Redis instance is disconnected.
 *
 * \param redis         Pointer to a Redis instance.
 * \param cleanupCall   User specified function to call when Redis is disconnected.
 *
 */
void redisxRemoveDisconnectHook(Redis *redis, const void (*cleanupCall)(void)) {
  RedisPrivate *p;
  Hook *c, *last = NULL;

  if(redis == NULL) return;
  if(cleanupCall == NULL) return;

  xvprintf("Redis-X> Removing a disconnect callback.\n");

  p = (RedisPrivate *) redis->priv;

  rConfigLock(redis);
  c = p->firstCleanupCall;

  while(c != NULL) {
    Hook *next = c->next;

    if(c->call == cleanupCall) {
      if(last == NULL) p->firstCleanupCall = next;
      else last->next = next;
      free(c);
    }
    else last = c;
    c = next;
  }
  rConfigUnlock(redis);
}


/**
 * Removes all disconnect hooks, that is no user-specified callbacks will be made when the
 * specified Redis instance is disconnected.
 *
 * \param redis         Pointer to a Redis instance.
 *
 */
void redisxClearDisconnectHooks(Redis *redis) {
  RedisPrivate *p;
  Hook *c;

  if(redis == NULL) return;

  xvprintf("Redis-X> Clearing all disconnect callbacks.\n");

  p = (RedisPrivate *) redis->priv;

  rConfigLock(redis);
  c = p->firstCleanupCall;

  while(c != NULL) {
    Hook *next = c->next;
    free(c);
    c = next;
  }
  rConfigUnlock(redis);
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

  if(hostnameToIP(server, ipAddress) < 0) return NULL;

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
  for(i=REDIS_CHANNELS; --i >= 0; ) {
    RedisClient *cl = &p->clients[i];
    ClientPrivate *cp;

    cl->priv = calloc(1, sizeof(ClientPrivate));
    cp = (ClientPrivate *) cl->priv;
    cp->idx = i;
    pthread_mutex_init(&cp->readLock, NULL);
    pthread_mutex_init(&cp->writeLock, NULL);
    pthread_mutex_init(&cp->pendingLock, NULL);
    rResetClientAsync(cl);
  }

  redis = (Redis *) calloc(1, sizeof(Redis));
  redis->priv = p;
  redis->interactive = &p->clients[INTERACTIVE_CHANNEL];
  redis->pipeline = &p->clients[PIPELINE_CHANNEL];
  redis->subscription = &p->clients[SUBSCRIPTION_CHANNEL];
  redis->id = xStringCopyOf(ipAddress);

  for(i=REDIS_CHANNELS; --i >= 0; ) {
    ClientPrivate *cp = (ClientPrivate *) p->clients[i].priv;
    cp->redis = redis;
  }

  p->addr = inet_addr((char *) ipAddress);

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

  for(i=REDIS_CHANNELS; --i >= 0; ) {
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
 */
int redisxConnect(Redis *redis, boolean usePipeline) {
  int status;

  if(redis == NULL) return redisxError("redisxConnect()", X_NULL);

  rConfigLock(redis);

  status = rConnectAsync(redis, usePipeline);

  rConfigUnlock(redis);

  if(status) return redisxError("redisxConnect()", status);

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

    status = rStartPipelineListenerAsync(redis, &threadConfig);
  }

  xvprintf("Redis-X> socket(s) online.\n");

  // Call the connect hooks...
  for(f = p->firstConnectCall; f != NULL; f = f->next) f->call();

  xvprintf("Redis-X> connect complete.\n");

  return status;
}

/**
 * Connects the subscription client/channel to the REDIS server, for sending and receiving
 * PUB/SUB commands and messages, and starts the SubscriptionListener thread for
 * consuming incoming PUB/SUB messages in the background.
 *
 * \param redis         Pointer to a Redis instance.
 *
 * \return      X_SUCCESS on success, or X_NO_SERVICE if the connection failed.
 */
static int rConnectSubscriptionClientAsync(Redis *redis) {
  int status;
  const ClientPrivate *sp = (ClientPrivate *) redis->subscription->priv;

  if(sp->isEnabled) {
    fprintf(stderr, "WARNING! Redis-X : pub/sub client is already connected.\n");
    return X_SUCCESS;
  }

  xvprintf("Redis-X> Connect pub/sub client.\n");
  status = rConnectClient(redis, SUBSCRIPTION_CHANNEL);

  if(status) {
    fprintf(stderr, "ERROR! Redis-X : pub/sub client connection failed. code: %d\n", status);
    return X_NO_SERVICE;
  }

  return X_SUCCESS;
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
static int rConnectClient(Redis *redis, int channel) {
  static boolean warnedFailed;

  struct sockaddr_in serverAddress;
  const RedisPrivate *p;
  RedisClient *cl;
  ClientPrivate *cp;

  const char *host, *channelID;
  char *id;
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
  host = redisxGetHostName(redis);
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
  rEndSubscriptionAsync(redis);
  rCloseClient(redis->pipeline);
  rCloseClient(redis->interactive);

  xvprintf("Redis-X> sockets closed.\n");

  // Call the cleanup hooks...
  for(f = p->firstCleanupCall; f != NULL; f = f->next) f->call();

  xvprintf("Redis-X> disconnect complete.\n");
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
 * Same as reconnectRedis() except without the exlusive locking mechanism.
 */
static int rReconnectAsync(Redis *redis, boolean usePipeline) {
  xvprintf("Redis-X> reconnecting to server...\n");
  rDisconnectAsync(redis);
  return rConnectAsync(redis, usePipeline);
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
  for(i=0; i<REDIS_CHANNELS; i++) rDisconnectClientAsync(&p->clients[i]);
}


/**
 * Closes the REDIS client on the specified communication channel. It ia assused the caller
 * has an exclusive lock on the Redis configuration to which the client belongs.
 *
 * \param cl        Pointer to the Redis client instance.
 *
 */
static void rCloseClient(RedisClient *cl) {
  redisxLockClient(cl);
  rCloseClientAsync(cl);
  redisxUnlockClient(cl);
  return;
}


/**
 * Closes the REDIS client on the specified communication channel. It ia assused the caller
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
 * Checks if a client was configured with a low-latency socket connection.
 *
 * \param cp        Pointer to the private data of a Redis client.
 *
 * \return          TRUE (1) if the client is low latency, or else FALSE (0).
 *
 */
static boolean rIsLowLatency(const ClientPrivate *cp) {
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

#if ! (__Lynx__ &&  __powerpc__)
  {
    const int tos = lowLatency ? IPTOS_LOWDELAY : IPTOS_THROUGHPUT;

    // Optimize service for latency or throughput
    // LynxOS 3.1 does not support IP_TOS option...
    if(setsockopt(socket, IPPROTO_IP, IP_TOS, &tos, sizeof(tos)))
      perror("WARNING! Redis-X : socket type-of-service not set.");

    // Send packets immediately even if small...
    if(lowLatency) if(setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, & enable, sizeof(int)))
      perror("WARNING! Redis-X : socket tcpnodelay not enabled.");
  }
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
 * Instructs Redis to skip sending a reply for the next command.
 *
 * Sends <code>CLIENT REPLY SKIP</code>
 *
 * \param cl            Pointer to the Redis client to use.
 *
 * \return              X_SUCCESS (0) on success or an error code on failure, is either X_NO_SERVICE
 *                      (if not connected to the REDIS server on the requested channel)
 *                      or the errno set by send().
 *
 *                          X_NULL      if the client is NULL.
 */
int redisxSkipReplyAsync(RedisClient *cl) {
  const char *funcName = "redisSkipReplyAsync()";
  static char cmd[] = "*3\r\n$6\r\nCLIENT\r\n$5\r\nREPLY\r\n$4\r\nSKIP\r\n";

  int status;

  if(cl == NULL) return redisxError(funcName, X_NULL);

  status = rSendBytesAsync((ClientPrivate *) cl->priv, cmd, sizeof(cmd) - 1, TRUE);
  if(status) return redisxError(funcName, status);

  return X_SUCCESS;
}



/**
 * Starts an atomic Redis transaction block, by sending <code>MULTI</code> on the specified client connection.
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
 *                  X_NULL          if the Redis client is NULL
 *
 *                  or else the error set by send().
 *
 * @sa redisxExecBlockAsync()
 * @sa redisxAbortBlockAsync()
 *
 */
int redisxStartBlockAsync(RedisClient *cl) {
  const char *funcName = "redisxStartBlockAsync()";
  static char cmd[] = "*1\r\n$5\r\nMULTI\r\n";

  int status;

  if(cl == NULL) return redisxError(funcName, X_NULL);

  status = rSendBytesAsync((ClientPrivate *) cl->priv, cmd, sizeof(cmd) - 1, TRUE);
  if(status) return redisxError(funcName, status);

  return X_SUCCESS;
}


/**
 * Abort an atomic transaction block. It sends <code>DISCARD</code>.
 *
 * \param cl    Pointer to a Redis client
 *
 * \return      X_SUCCESS (0) if successful or else an error code from send() (see errno.h).
 *
 * @sa redisxStartBlockAsync()
 *
 */
int redisxAbortBlockAsync(RedisClient *cl) {
  const char *funcName = "redisxAbortBlockAsync()";
  static char cmd[] = "*1\r\n$7\r\nDISCARD\r\n";

  int status;

  if(cl == NULL) return redisxError(funcName, X_NULL);

  status = rSendBytesAsync((ClientPrivate *) cl->priv, cmd, sizeof(cmd) - 1, TRUE);
  if(status) return redisxError(funcName, status);

  redisxIgnoreReplyAsync(cl);

  return X_SUCCESS;
}

/**
 * Finish and execute an atomic transaction block. It sends <code>EXEC</code>, skips through all
 * <code>OK</code> and <code>QUEUED</code> acknowledgements, and returns the reply to the transaction
 * block itself.
 *
 * \param cl    Pointer to a Redis client
 *
 * \return      The array RESP returned by EXEC, or NULL if there was an error.
 *
 * @sa redisxStartBlockAsync()
 * @sa redisxAbortBlockAsync()
 *
 */
RESP *redisxExecBlockAsync(RedisClient *cl) {
  const char *funcName = "redisxExecBlockAsync()";
  static char cmd[] = "*1\r\n$4\r\nEXEC\r\n";

  int status;

  if(cl == NULL) {
    redisxError(funcName, X_NULL);
    return NULL;
  }

  status = redisxSkipReplyAsync(cl);
  if(status) {
    redisxError(funcName, status);
    return NULL;
  }

  status = rSendBytesAsync((ClientPrivate *) cl->priv, cmd, sizeof(cmd) - 1, TRUE);
  if(status) {
    redisxError(funcName, status);
    return NULL;
  }

  for(;;) {
    RESP *reply = redisxReadReplyAsync(cl);
    if(!reply) {
      redisxError(funcName, REDIS_NULL);
      return NULL;
    }
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
  const char *funcName = "redisxLoadScript()";
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
 * Send a command (with up to 3 arguments) to the REDIS server. The arguments supplied
 * will be used up to the first non-NULL value. E.g. to send the command 'HLEN acc3'
 * on the regular REDIS command connection (INTERACTIVE_CHANNEL), you would call:
 *
 *   sendRequest(INTERACTIVE_CHANNEL, "HLEN", "acc3", NULL, NULL);
 *
 * \param cl            Pointer to the Redis client instance.
 * \param command       REDIS command string.
 * \param arg1          Optional first string argument or NULL.
 * \param arg2          Optional second string argument or NULL.
 * \param arg3          Optional third string argument or NULL.
 *
 * \return              0 on success or an error code on failure, is either X_NO_SERVICE
 *                      (if not connected to the REDIS server on the requested channel)
 *                      or the errno set by send().
 */
int redisxSendRequestAsync(RedisClient *cl, const char *command, const char *arg1, const char *arg2, const char *arg3) {
  const char *args[] = { command, arg1, arg2, arg3 };
  int n;
  const ClientPrivate *cp;

  if(cl == NULL) return X_NULL;

  cp = (ClientPrivate *) cl->priv;
  if(!cp->isEnabled) return X_NO_INIT;
  if(command == NULL) return X_NAME_INVALID;

  // Count the non-null arguments...
  if(arg1 == NULL) n = 1;
  else if(arg2 == NULL) n = 2;
  else if(arg3 == NULL) n = 3;
  else n = 4;

  return redisxSendArrayRequestAsync(cl, (char **) args, NULL, n);
}


/**
 * Send a Redis request with an arbitrary number of arguments.
 *
 * \param cl            Pointer to the Redis client.
 * \param args          The array of string arguments to send.
 * \param lengths       Array indicating the number of bytes to send from each string argument. Zero
 *                      or negative values can be used to determine the string length automatically
 *                      using strlen(), and the length argument itself may be NULL to determine the
 *                      lengths of all string arguments automatically.
 * \param n             The number of arguments to send.
 *
 * \return              0 on success or an error code on failure, is either X_NO_SERVICE
 *                      (if not connected to the REDIS server on the requested channel)
 *                      or the errno set by send().
 */
int redisxSendArrayRequestAsync(RedisClient *cl, char *args[], int lengths[], int n) {
  const char *funcName = "redisxSendArrayRequestAsync()";
  char buf[REDIS_CMDBUF_SIZE];
  int status, i, L;
  ClientPrivate *cp;

  if(cl == NULL) return redisxError(funcName, X_NULL);
  cp = (ClientPrivate *) cl->priv;

  // Send the number of string elements in the command...
  L = sprintf(buf, "*%d\r\n", n);

  for(i=0; i<n; i++) {
    int l, L1;

    if(!lengths) l = strlen(args[i]);
    else l = lengths[i] > 0 ? lengths[i] : strlen(args[i]);

    L += sprintf(buf + L, "$%d\r\n", l);

    // length of next RESP the bulk string component including \r\n\0 termination.
    L1 = l + 3;

    if((L + L1) > REDIS_CMDBUF_SIZE) {
      // If buf cannot include the next argument, then flush the buffer...
      status = rSendBytesAsync(cp, buf, L, FALSE);
      if(status) return redisxError(funcName, status);

      L = 0;

      // If the next argument does not fit into the buffer, then send it piecemeal
      if(L1 > REDIS_CMDBUF_SIZE) {
        status = rSendBytesAsync(cp, args[i], l, FALSE);
        if(status) return redisxError(funcName, status);

        status = rSendBytesAsync(cp, "\r\n", 2, i == (n-1));
        if(status) return redisxError(funcName, status);
      }
      else {
        memcpy(buf, args[i], l);            // Copy argument into buffer.
        L = l + sprintf(buf+l, "\r\n");     // Add \r\n\0...
      }
    }
    else {
      memcpy(buf+L, args[i], l);            // Copy argument into buffer.
      L += l;
      L += sprintf(buf+L, "\r\n");          // Add \r\n\0
    }
  }

  // flush the remaining bits in the buffer...
  if(L > 0) {
    status = rSendBytesAsync(cp, buf, L, TRUE);
    if(status) return redisxError(funcName, status);
  }

  pthread_mutex_lock(&cp->pendingLock);
  cp->pendingRequests++;
  pthread_mutex_unlock(&cp->pendingLock);

  return X_SUCCESS;
}


/**
 * Sends a regular string terminated Redis PUB/SUB message on the specified channel.
 * Same as redisxPublish() with the length argument set to the length of the
 * string message.
 * Redis must be connected before attempting to send messages.
 *
 * \param redis         Pointer to a Redis instance.
 * \param channel       Redis PUB/SUB channel on which to notify
 * \param message       Message to send.
 *
 *
 * \return      X_SUCCESS       if the message was successfullt sent.
 *              X_NO_INIT       if the Redis library was not initialized via initRedis().
 *              X_NO_SERVICE    if there was a connection problem.
 *              PARSE_ERROR     if the Redis response could not be confirmed.
 *              or the errno returned by send();
 *
 * @sa redisxPublish()
 * @sa redisxPublishAsync()
 * @sa redisxSubscribe()
 *
 */
int redisxNotify(Redis *redis, const char *channel, const char *message) {
  return redisxPublish(redis, channel, message, 0);
}



/**
 * Sends a generic Redis PUB/SUB message on the specified channel.
 * Redis must be connected before attempting to send messages. It will send the
 * message over the pipeline client if it is avaiable, or else over
 * the interactive client.
 *
 * \param redis         Pointer to a Redis instance.
 * \param channel       Redis PUB/SUB channel on which to notify
 * \param data          Data to send.
 * \param length        Bytes of data to send, or 0 to determine automatically with strlen().
 *
 *
 * \return      X_SUCCESS       if the message was successfullt sent.
 *              X_NO_INIT       if the Redis library was not initialized via initRedis().
 *              X_NO_SERVICE    if there was a connection problem.
 *              PARSE_ERROR     if the Redis response could not be confirmed.
 *              or the errno returned by send().
 *
 * @sa redisxNotify()
 * @sa redisxPublishAsync()
 * @sa redisxSubscribe()
 */
int redisxPublish(Redis *redis, const char *channel, const char *data, int length) {
  const char *funcName = "redisxNotifyBin()";

  int status = 0;

  if(redis == NULL) return redisxError(funcName, X_NULL);

  status = redisxLockEnabled(redis->interactive);
  if(status) return redisxError(funcName, status);

  // Now send the message
  status = redisxPublishAsync(redis, channel, data, length);

  // Clean up...
  redisxUnlockClient(redis->interactive);

  if(status) return redisxError(funcName, status);

  xvprintf("Redis-X> sent message to %s, channel %s: '%s'.\n", redis->id, channel, data);

  return X_SUCCESS;
}


/**
 * Sends a Redis notification asynchronously using the Redis "PUBLISH" command.
 * The caller should have an exclusive lock on the interactive Redis channel before calling this.
 *
 * \param redis         Pointer to a Redis instance.
 * \param channel       Redis PUB/SUB channel on which to notify
 * \param data          Message body data.
 * \param length        Bytes of message data to send, ot 0 to determine automatically with strlen().
 *
 * \return      X_SUCCESS (0)   if successful, or else
 *              X_NULL          if the redis instance is NULL
 *              X_NAME_INVALID  if the PUB/SUB channel is null or empty
 *
 *              or an error code returned by redisxSendArrayRequestAsync().
 *
 * @sa redisxPublish()
 * @sa redisxNotify()
 */
int redisxPublishAsync(Redis *redis, const char *channel, const char *data, int length) {
  const char *funcName = "redisSEndInteractiveNotifyAsync()";

  char *args[3];
  int status, L[3] = {0};

  if(redis == NULL) return redisxError(funcName, X_NULL);
  if(channel == NULL) return redisxError(funcName, X_NAME_INVALID);
  if(*channel == '\0') return redisxError(funcName, X_NAME_INVALID);

  args[0] = "PUBLISH";
  args[1] = (char *) channel;
  args[2] = (char *) data;

  if(length <= 0) length = strlen(data);
  L[2] = length;

  status = redisxSkipReplyAsync(redis->interactive);
  if(status) return redisxError(funcName, status);

  status = redisxSendArrayRequestAsync(redis->interactive, args, L, 3);
  if(status) return redisxError(funcName, status);

  return X_SUCCESS;
}


/**
 * Sets a global or hashtable value on Redis.
 *
 * \param redis         Pointer to a Redis instance.
 * \param table         Hash table identifier or NULL if setting a global value.
 * \param key           Redis field name (i.e. variable name).
 * \param value         A proper 0-terminated string value to store.
 * \param isPipelined   If TRUE the call will be sent on the pipeline channel and no response
 *                      will be requested. Otherwise, the request will be sent on the interactive
 *                      channel, and checked for confirmation.
 *
 * \return      X_SUCCESS if the variable was succesfully set, or:
 *
 *                  X_NO_INIT
 *                  X_NAME_INVALID
 *                  X_NULL
 *                  X_NO_SERVICE
 *                  X_FAILURE
 *
 */
int redisxSetValue(Redis *redis, const char *table, const char *key, const char *value, boolean isPipelined) {
  const char *funcName = "setRedisValue()";

  int status = X_SUCCESS;
  RedisClient *cl;
  RESP *reply = NULL;

  if(redis == NULL) return redisxError(funcName, X_NULL);

  if(isPipelined) {
    cl = redis->pipeline;
    if(redisxLockEnabled(cl) != X_SUCCESS) isPipelined = FALSE;
  }

  // Not pipelined or pipeline is not available...
  if(!isPipelined) {
    cl = redis->interactive;
    status = redisxLockEnabled(cl);
    if(status) return redisxError(funcName, status);
  }

  if(isPipelined) status = redisxSkipReplyAsync(cl);
  if(!status) {
    status = redisxSendValueAsync(cl, table, key, value, !isPipelined);
    if(!isPipelined) if(!status) reply = redisxReadReplyAsync(cl);
  }

  redisxUnlockClient(cl);

  if(status) return redisxError(funcName, status);

  if(!isPipelined) {
    status = redisxCheckRESP(reply, RESP_INT, 0);
    redisxDestroyRESP(reply);
    if(status) return redisxError(funcName, status);
  }

  return X_SUCCESS;
}

/**
 * Sends a request for setting a table value, using the Redis "SET" or "HSET" command.
 *
 * \param cl        Pointer to a Redis channel.
 * \param table     Hashtable from which to retrieve a value or NULL if to use the global table.
 * \param key       Field name (i.e. variable name).
 * \param value     The string value to set (assumes normal string termination).'
 * \param confirm   Whether confirmation is required from Redis to acknowledge.
 *
 * \return           X_SUCCESS (0)   if successful, or
 *                  X_NULL          if the client or value is NULL
 *                  X_NAME_INVALID  if key is invalid,
 *
 *                  or an error returned by redisxSendRequestAsync().
 */
int redisxSendValueAsync(RedisClient *cl, const char *table, const char *key, const char *value, boolean confirm) {
  const char *funcName = "redisxSendValueAsync()";
  int status;

  if(cl == NULL) return redisxError(funcName, X_NULL);
  if(key == NULL) return redisxError(funcName, X_NAME_INVALID);
  if(value == NULL) return redisxError(funcName, X_NULL);

  // No need for response. Just set value.
  if(!confirm) {
    status = redisxSkipReplyAsync(cl);
    if(status) return redisxError(funcName, status);
  }

  if(table == NULL) status = redisxSendRequestAsync(cl, "SET", key, value, NULL);
  else status = redisxSendRequestAsync(cl, "HSET", table, key, value);

  if(status) return redisxError(funcName, status);

  xvprintf("Redis-X> set %s = %s on %s\n", key, value, table);

  return X_SUCCESS;
}

/**
 * Retrieve a variable from Redis, through the interactive connection. This is not the highest throughput mode
 * (that would be sending asynchronous pipeline request, and then asynchronously collecting the results such as
 * with redisxSendRequestAsync() / redisxReadReplyAsync()), because it requires separate network roundtrips for each
 * and every request. But, it is simple and perfectly good method when one needs to retrieve only a few (<1000)
 * variables per second...
 *
 * The call is effectively implements a Redis GET (if the tale argument is NULL) or HGET call.
 *
 * \param[in]  redis     Pointer to a Redis instance.
 * \param[in]  table     Hashtable from which to retrieve a value or NULL if to use the global table.
 * \param[in]  key       Field name (i.e. variable name).
 * \param[out] status    Pointer to the return error status, which is eithe X_SUCCESS on success or else
 *                       the error code set by redisxArrayRequest().
 *
 * \return          A freshly allocated RESP array containing the Redis response, or NULL if no valid
 *                  response could be obtained.
 */
RESP *redisxGetValue(Redis *redis, const char *table, const char *key, int *status) {
  const char *funcName = "redisxGetValue()";

  RESP *reply;

  if(redis == NULL) return NULL;

  if(key == NULL) {
    *status = redisxError(funcName, X_NAME_INVALID);
    return NULL;
  }

  if(table == NULL) reply = redisxRequest(redis, "GET", key, NULL, NULL, status);
  else reply = redisxRequest(redis, "HGET", table, key, NULL, status);

  if(*status) redisxError(funcName, *status);

  return reply;
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
  const char *funcName = "redisxGetTime()";

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
 * Returns all the key/value pairs stored in a given hash table
 *
 * \param[in]  redis     Pointer to a Redis instance.
 * \param[in]  table     Hashtable from which to retrieve a value or NULL if to use the global table.
 * \param[out] n         Pointer to the integer in which the number of elements or an error (<0) is returned.
 *                       It may return an error value from redisxRequest(), or:
 *
 *                         REDIS_NULL          If got a null or empty response from Redis
 *                         UNEXPECTED_RESP     If the response from Redis was not the expected array type
 *
 * \return               A table of all entries (key/value pairs) from this table or NULL if there was an error (see parameter n).
 *
 */
RedisEntry *redisxGetTable(Redis *redis, const char *table, int *n) {
  const char *funcName = "redisxGetAll()";
  RedisEntry *entries = NULL;
  RESP *reply;

  if(n == NULL) return NULL;

  if(redis == NULL) {
    *n = X_NO_INIT;
    return NULL;
  }

  if(table == NULL) {
    *n = X_GROUP_INVALID;
    return NULL;
  }

  reply = redisxRequest(redis, "HGETALL", table, NULL, NULL, n);

  if(*n) {
    redisxDestroyRESP(reply);
    redisxError(funcName, *n);
    return NULL;
  }

  *n = redisxCheckDestroyRESP(reply, RESP_ARRAY, 0);
  if(*n) {
    redisxError(funcName, *n);
    return NULL;
  }

  *n = reply->n / 2;

  if(*n > 0) {
    entries = (RedisEntry *) calloc(*n, sizeof(RedisEntry));

    if(entries == NULL) fprintf(stderr, "WARNING! Redis-X : alloc %d table entries: %s\n", *n, strerror(errno));
    else {
      int i;

      for(i=0; i<reply->n; i+=2) {
        RedisEntry *e = &entries[i>>1];
        RESP **component = (RESP **) reply->value;

        e->key = (char *) component[i]->value;
        e->value = (char *) component[i+1]->value;
        e->length = component[i+1]->n;

        // Dereference the values from the RESP
        component[i]->value = NULL;
        component[i+1]->value = NULL;
      }
    }
  }

  // Free the Reply container, but not the strings inside, which are returned.
  redisxDestroyRESP(reply);
  return entries;
}


/**
 * Sets multiple key/value pairs in a given hash table.
 *
 * \param redis         Pointer to a Redis instance.
 * \param table         Hashtable from which to retrieve a value or NULL if to use the global table.
 * \param entries       Pointer to an array of key/value pairs.
 * \param n             Number of entries.
 * \param isPipelined   If TRUE the call will be sent on the pipeline channel and no response
 *                      will be requested. Otherwise, the request will be sent on the interactive
 *                      channel, and checked for confirmation.
 *
 * \return              X_SUCCESS (0) on success or an error code.
 *
 */
int redisxMultiSet(Redis *redis, const char *table, const RedisEntry *entries, int n, boolean isPipelined) {
  const char *funcName = "redisSetAll()";
  int i, *L, N, status;
  char **req;

  if(redis == NULL) return X_NO_INIT;
  if(table == NULL) return X_GROUP_INVALID;
  if(entries == NULL) return X_NULL;
  if(n < 1) return X_SIZE_INVALID;

  N = (n<<1)+2;

  req = (char **) malloc(N * sizeof(char *));
  if(!req) {
    fprintf(stderr, "WARNING! Redis-X : alloc %d request components: %s\n", N, strerror(errno));
    return redisxError(funcName, X_FAILURE);
  }

  L = (int *) calloc(N, sizeof(int));
  if(!L) {
    fprintf(stderr, "WARNING! Redis-X : alloc %d request sizes: %s\n", N, strerror(errno));
    free(req);
    return redisxError(funcName, X_FAILURE);
  }

  req[0] = "HMSET";
  req[1] = (char *) table;

  for(i=0; i<n; i++) {
    int m = 2 + (n<<1);
    req[m] = (char *) entries[n].key;
    req[m+1] = (char *) entries[n].value;
    L[m+1] = entries[n].length;
  }

  isPipelined &= redisxHasPipeline(redis);

  if(isPipelined) {
    status = redisxLockEnabled(redis->pipeline);
    if(!status) {
      status = redisxSkipReplyAsync(redis->pipeline);
      if(!status) status = redisxSendArrayRequestAsync(redis->pipeline, req, L, N);
      redisxUnlockClient(redis->pipeline);
    }

    free(req);
    free(L);
  }

  else {
    RESP *reply;

    reply = redisxArrayRequest(redis, req, L, N, &status);
    free(req);
    free(L);

    if(!status) status = redisxCheckDestroyRESP(reply, RESP_SIMPLE_STRING, 0);
    if(!status) if(strcmp(reply->value, "OK")) status = REDIS_ERROR;

    redisxDestroyRESP(reply);
  }

  return status ? redisxError(funcName, status) : X_SUCCESS;
}


/**
 * Returns all the key names stored in a given hash table
 *
 * \param[in]  redis         Pointer to a Redis instance.
 * \param[in]  table     The hashtable from which to retrieve a value or NULL if to use the global table.
 * \param[out] n         Pointer to the integer in which the number of elements or an error (<0) is returned.
 *                       It may return an error value from redisxRequest(), or:
 *
 *                          REDIS_NULL          If got a null or empty response from Redis
 *                          UNEXPECTED_RESP     If the response from Redis was not the expected array type
 *
 * \return               An array with pointers to key names from this table or NULL if there was an error (see parameter n).
 *
 * @sa redisxScanKeys()
 */
char **redisxGetKeys(Redis *redis, const char *table, int *n) {
  const char *funcName = "redisxGetKeys()";
  RESP *reply;
  char **names = NULL;

  if(redis == NULL) return NULL;

  reply = redisxRequest(redis, "HKEYS", table, NULL, NULL, n);

  if(*n) {
    redisxDestroyRESP(reply);
    redisxError(funcName, *n);
    return NULL;
  }

  *n = redisxCheckDestroyRESP(reply, RESP_ARRAY, 0);
  if(*n) {
    redisxError(funcName, *n);
    return NULL;
  }

  *n = reply->n;
  if(reply->n > 0) {
    names = (char **) calloc(reply->n, sizeof(char *));

    if(names == NULL) fprintf(stderr, "WARNING! Redis-X : alloc pointers for %d keys: %s\n", reply->n, strerror(errno));
    else {
      int i;
      for(i=0; i<reply->n; i++) {
        RESP **component = (RESP **) reply->value;
        names[i] = (char *) component[i]->value;

        // de-reference name from RESP.
        component[i]->value = NULL;
      }
    }
  }

  redisxDestroyRESP(reply);
  return names;
}

/**
 * Sets the COUNT parameter to use with Redis SCAN type commands. COUNT specifies how much work
 * Redis should do in a single scan iteration. 0 (or negative) values can be used to scan with
 * defaults (without the COUNT option), which is usually equivalent to COUNT=10. When scanning
 * large datasets, it may take many scan calls to go through all the data. When networking has
 * limited bandwidth, or large latencies it may be desirable to do more work per call on the
 * server side to reduce traffic. However, the cost of larger COUNT values is that it may increase
 * server latencies for other queries.
 *
 * @param redis     Pointer to a Redis instance.
 * @param count     The new COUNT to use for SCAN-type commands or <0 to use default.
 *
 * @sa redisxGetScanCount()
 * @sa redisxScanKeys()
 * @sa redisxScanTable()
 *
 */
void redisxSetScanCount(Redis *redis, int count) {
  RedisPrivate *p = (RedisPrivate *) redis->priv;
  rConfigLock(redis);
  p->scanCount = count;
  rConfigUnlock(redis);
}

/**
 * Returns the COUNT parameter currently set to be used with Redis SCAN-type commands
 *
 * @param redis     Pointer to a Redis instance.
 * @return          The current COUNT to use for SCAN-type commands or <0 to use default.
 *
 * @sa redisxGetScanCount()
 * @sa redisxScanKeys()
 * @sa redisxScanTable()
 */
int redisxGetScanCount(Redis *redis) {
  const RedisPrivate *p = (RedisPrivate *) redis->priv;
  return p->scanCount;
}

static int compare_strings(const void *a, const void *b) {
  return strcmp((char *) a, (char *)b);
}

/**
 * Returns an alphabetical list of the Redis keys using the Redis SCAN command.
 * Because it uses the scan command, it is guaranteed to not hog the database for excessive periods, and
 * hence it is preferable to redisxGetKeys(table=NULL).
 *
 * Some data may be returned even if there was an error, and the caller is responsible
 * for cleaning up the returned srotage elements.
 *
 * The caller may adjust the amount of work performed in each scan call via the redisxSetScanCount()
 * function, prior to calling this.
 *
 * \param[in]  redis     Pointer to a Redis instance.
 * \param[in]  pattern   keyword pattern to match, or NULL for all keys.
 * \param[out] n         Pointer to the integer in which the number of elements
 * \param[out] status    integer in which to return the status, which is X_SUCCESS (0) if successful, or may
 *                       an error value from redisxRequest(), or:
 *
 *                          X_NULL              If one of the arguments is NULL
 *                          REDIS_NULL          If got a null or empty response from Redis
 *                          UNEXPECTED_RESP     If the response from Redis was not the expected array type
 *
 * \return               An array with pointers to key names from this table or NULL.
 *
 * @sa redisxGetKeys()
 * @sa redisxSetScanCount()
 */
char **redisxScanKeys(Redis *redis, const char *pattern, int *n, int *status) {
  const char *funcName = "redisxScanKeys()";

  RESP *reply = NULL;
  char *cmd[6] = {NULL};
  char **pCursor;
  char **names = NULL;
  char countArg[20];
  int args = 0, i, j, capacity = SCAN_INITIAL_STORE_CAPACITY;

  if(redis == NULL || n == NULL || status == NULL) {
    if(status != NULL) *status = redisxError(funcName, X_NULL);
    return NULL;
  }

  *status = X_SUCCESS;
  *n = 0;

  cmd[args++] = "SCAN";

  pCursor = &cmd[args];
  cmd[args++] = xStringCopyOf(SCAN_INITIAL_CURSOR);

  if(pattern) {
    cmd[args++] = "MATCH";
    cmd[args++] = (char *) pattern;
  }

  i = redisxGetScanCount(redis);
  if(i > 0) {
    sprintf(countArg, "%d", i);
    cmd[args++] = "COUNT";
    cmd[args++] = countArg;
  }

  xdprintf("Redis-X> Calling SCAN (MATCH %s)\n", pattern);

  do {
    int count;
    RESP **components;

    if(reply) redisxDestroyRESP(reply);
    reply = redisxArrayRequest(redis, cmd, NULL, args, status);

    if(*status) break;

    // We expect an array of 2 elements { cursor, { names } }
    *status = redisxCheckRESP(reply, RESP_ARRAY, 2);
    if(*status) break;

    components = (RESP **) reply->value;

    *status = redisxCheckRESP(components[0], RESP_BULK_STRING, 0);
    if(*status) break;

    *status = redisxCheckRESP(components[1], RESP_ARRAY, 0);
    if(*status) break;

    // Discard previously received cursor...
    free(*pCursor);
    *pCursor = (char *) components[0]->value;
    components[0]->value = NULL;        // de-reference name from RESP.

    count = components[1]->n;
    components = (RESP **) components[1]->value;

    // OK, we got a reasonable response, now make sure we have storage space for it.
    if(!names) {
      names = (char **) calloc(capacity, sizeof(char *));
      if(!names) {
        fprintf(stderr, "WARNING! Redis-X : alloc pointers for up to %d keys: %s\n", capacity, strerror(errno));
        break;
      }
    }
    else if(*n + count > capacity) {
      char **old = names;

      capacity <<= 1;
      names = (char **) realloc(names, capacity * sizeof(char *));
      if(!names) {
        fprintf(stderr, "WARNING! Redis-X : realloc pointers for up to %d keys: %s\n", capacity, strerror(errno));
        free(old);
        *n = 0;
        break;
      }
    }

    // Store the names we got...
    for(i=0; i<count; i++) {
      *status = redisxCheckRESP(components[i], RESP_BULK_STRING, 0);
      if(*status) break;
      names[(*n)++] = (char *) components[i]->value;
      components[i]->value = NULL;      // de-reference name from RESP.
    }

  } while (strcmp(*pCursor, SCAN_INITIAL_CURSOR));  // Done when cursor is back to 0...

  // Check for errors
  if(*status) redisxError(funcName, *status);

  // Clean up.
  redisxDestroyRESP(reply);
  free(*pCursor);

  if(!names) return NULL;

  // Sort alphabetically.
  xdprintf("Redis-X> Sorting %d scanned table entries.\n", *n);
  qsort(names, *n, sizeof(char *), compare_strings);

  // Remove duplicates
  for(i=*n; --i > 0; ) if(!strcmp(names[i], names[i-1])) {
    free(names[i]);
    names[i] = NULL;
  }

  // Compact...
  for(i=0, j=0; i < *n; i++) if(names[i]) {
    if(i != j) names[j] = names[i];
    j++;
  }

  *n = j;

  xvprintf("Redis-X> Returning %d scanned / sorted keys (after removing dupes).\n", *n);

  return names;
}

static int compare_entries(const void *a, const void *b) {
  const RedisEntry *A = (RedisEntry *) a;
  const RedisEntry *B = (RedisEntry *) b;
  return strcmp(A->key, B->key);
}


/**
 * Returns an alphabetical list of the Redis hash table data using the Redis HSCAN command.
 * Because it uses the scan command, it is guaranteed to not hog the database for excessive periods, and
 * hence it is preferable to redisxGetTable().
 *
 * Some data may be returned even if there was an error, and the caller is responsible
 * for cleaning up the returned srotage elements.
 *
 * The caller may adjust the amount of work performed in each scan call via the redisxSetScanCount()
 * function, prior to calling this.
 *
 * \param[in]  redis     Pointer to a Redis instance.
 * \param[in]  table     Name of Redis hash table to scan data from
 * \param[in]  pattern   keyword pattern to match, or NULL for all keys.
 * \param[out] n         Pointer to the integer in which the number of elements
 * \param[out] status    integer in which to return the status, which is X_SUCCESS (0) if successful, or may
 *                       an error value from redisxRequest(), or:
 *
 *                          X_NULL              If one of the arguments is NULL
 *                          REDIS_NULL          If got a null or empty response from Redis
 *                          UNEXPECTED_RESP     If the response from Redis was not the expected array type
 *
 * \return               A RedisEntry[] array or NULL.
 *
 * @sa redisxGetKeys()
 * @sa redisxSetScanCount()
 */
RedisEntry *redisxScanTable(Redis *redis, const char *table, const char *pattern, int *n, int *status) {
  const char *funcName = "redisxScanKeys()";

  RESP *reply = NULL;
  RedisEntry *entries = NULL;
  char *cmd[7] = {NULL}, countArg[20];
  char **pCursor;
  int args= 0, i, j, capacity = SCAN_INITIAL_STORE_CAPACITY;

  if(redis == NULL || table == NULL || n == NULL || status == NULL) {
    redisxError(funcName, X_NULL);
    return NULL;
  }

  *status = X_SUCCESS;
  *n = 0;

  cmd[args++] = "HSCAN";
  cmd[args++] = (char *) table;

  pCursor = &cmd[args];
  cmd[args++] = xStringCopyOf(SCAN_INITIAL_CURSOR);

  if(pattern) {
    cmd[args++] = "MATCH";
    cmd[args++] = (char *) pattern;
  }

  i = redisxGetScanCount(redis);
  if(i > 0) {
    sprintf(countArg, "%d", i);
    cmd[args++] = "COUNT";
    cmd[args++] = countArg;
  }

  xdprintf("Redis-X> Calling HSCAN %s (MATCH %s)\n", table, pattern);

  do {
    int count;
    RESP **components;

    if(reply) redisxDestroyRESP(reply);
    reply = redisxArrayRequest(redis, cmd, NULL, args, status);
    if(*status) break;

    // We expect an array of 2 elements { cursor, { key , value ... } }
    *status = redisxCheckRESP(reply, RESP_ARRAY, 2);
    if(*status) break;

    components = (RESP **) reply->value;

    *status = redisxCheckRESP(components[0], RESP_BULK_STRING, 0);
    if(*status) break;

    *status = redisxCheckRESP(components[1], RESP_ARRAY, 0);
    if(*status) break;

    // Discard previously received cursor...
    free(*pCursor);
    *pCursor = (char *) components[0]->value;
    components[0]->value = NULL;        // de-reference name from RESP.

    count = components[1]->n >> 1;
    components = (RESP **) components[1]->value;

    // OK, we got a reasonable response, now make sure we have storage space for it.
    if(!entries) {
      entries = (RedisEntry *) calloc(capacity, sizeof(RedisEntry));
      if(!entries) {
        fprintf(stderr, "WARNING! Redis-X : alloc up to %d table entries: %s\n", capacity, strerror(errno));
        break;
      }
    }
    else if(*n + count > capacity) {
      RedisEntry *old = entries;

      capacity <<= 1;
      entries = (RedisEntry *) realloc(entries, capacity * sizeof(RedisEntry));
      if(!entries) {
        fprintf(stderr, "WARNING! Redis-X : realloc up to %d table entries: %s\n", capacity, strerror(errno));
        free(old);
        *n = 0;
        break;
      }
    }

    // Store the names we got...
    for(i=0; i<count; i++) {
      RedisEntry *e = &entries[*n];
      int k = i << 1;

      *status = redisxCheckRESP(components[k], RESP_BULK_STRING, 0);
      if(*status) break;

      *status = redisxCheckRESP(components[k+1], RESP_BULK_STRING, 0);
      if(*status) break;

      (*n)++;

      e->key = (char *) components[k]->value;
      e->value = (char *) components[k+1]->value;
      e->length = components[k+1]->n;

      components[k]->value = NULL;          // de-reference name from RESP.
      components[k+1]->value = NULL;        // de-reference value from RESP.
    }

  } while(strcmp(*pCursor, SCAN_INITIAL_CURSOR));  // Done when cursor is back to 0...

  // Check for errors
  if(*status) redisxError(funcName, *status);

  // Clean up.
  redisxDestroyRESP(reply);
  free(*pCursor);

  if(!entries) return NULL;

  // Sort alphabetically.
  xdprintf("Redis-X> Sorting %d scanned table entries.\n", *n);
  qsort(entries, *n, sizeof(RedisEntry), compare_entries);

  // Remove duplicates
  for(i=*n; --i > 0; ) if(!compare_entries(&entries[i], &entries[i-1])) {
    if(entries[i].key) free(entries[i].key);
    if(entries[i].value) free(entries[i].value);
    memset(&entries[i], 0, sizeof(RedisEntry));
  }

  // Compact...
  for(i=0, j=0; i < *n; i++) if(entries[i].key) {
    if(i != j) entries[j] = entries[i];
    j++;
  }

  *n = j;

  xvprintf("Redis-X> Returning %d scanned / sorted table entries (after removing dupes).\n", *n);

  return entries;
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
  const char *funcName = "getRedisValue()";
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
  *status = redisxLockEnabled(cl);
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
 * Sends a sequence of bytes to the desired socket.
 *
 * \param sock          The socket file descriptor.
 * \param buf           Pointer to the buffer containing the data to be sent.
 * \param length        The number of bytes that should be sent from the buffer.
 * \param isLast        TRUE if this is the last component of a longer message, or FALSE
 *                      if more data will follow imminently.
 * \return              0 if the data was successfully sent, otherwise the errno set by send().
 *
 */
static int rSendBytesAsync(ClientPrivate *cp, const char *buf, int length, boolean isLast) {
#if SEND_YIELD_COUNT > 0
  static int count;   // Total bytes sent;
#endif

  const int sock = cp->socket;      // Local copy of socket fd that won't possibly change mid-call.
  char *from = (char *) buf;                 // Pointer to the next byte to send from buf...

  if(!buf) return X_NULL;

  xdprintf(" >>> '%s'\n", buf);

  if(!cp->isEnabled) return X_NO_INIT;
  if(sock < 0) return X_NO_INIT;

  while(length > 0) {
    int n;

#if __Lynx__
    // LynxOS PPCs do not have MSG_MORE, and MSG_EOR behaves differently -- to the point where it
    // can produce kernel panics. Stay safe and send messages with no flag, same as write()
    // On LynxOS write() has wider implementation than send(), including UNIX sockets...
    n = send(sock, from, length, 0);
#else
    // Linux supports flagging outgoing messages to inform it whether or not more
    // imminent data is on its way
    n = send(sock, from, length, isLast ? (rIsLowLatency(cp) ? MSG_EOR : 0) : MSG_MORE);
#endif

    if(n < 0) return rTransmitError(cp, "send");

    from += n;
    length -= n;

#if SEND_YIELD_COUNT > 0
    if(++count % SEND_YIELD_COUNT == 0) sched_yield();
#endif
  }

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
 * Reads a response from Redis and returns it.
 *
 * \param cl    Pointer to a Redis channel
 *
 * \return      The RESP structure for the reponse received from Redis, or NULL if an error was encountered
 *              (errno will be set to describe the error, which may either be an errno produced by recv()
 *              or EBADMSG if the message was corrupted and/or unparseable.
 */
RESP *redisxReadReplyAsync(RedisClient *cl) {
  const char *funcName = "redisxReadReplyAsync()";

  ClientPrivate *cp;
  RESP *resp = NULL;
  char buf[REDIS_SIMPLE_STRING_SIZE+2];   // +<string>\0
  int size = 0;
  int status = X_SUCCESS;

  if(cl == NULL) return NULL;
  cp = (ClientPrivate *) cl->priv;

  if(!cp->isEnabled) return NULL;

  size = rReadToken(cp, buf, REDIS_SIMPLE_STRING_SIZE+1);
  if(size < 0) {
    // Either read/recv had an error, or we got garbage...
    if(cp->isEnabled) redisxError(funcName, size);
    cp->isEnabled = FALSE;  // Disable this client so we don't attempt to read from it again...
    return NULL;
  }

  resp = (RESP *) calloc(1, sizeof(RESP));
  resp->type = buf[0];

  // Get the integer / size value...
  if(resp->type == RESP_ARRAY || resp->type == RESP_INT || resp->type == RESP_BULK_STRING) {
    char *tail;
    resp->n = strtol(&buf[1], &tail, 10);
    if(errno == ERANGE || tail == &buf[1]) {
      fprintf(stderr, "WARNING! Redis-X : unparseable dimension '%s'\n", &buf[1]);
      status = X_PARSE_ERROR;
    }
  }

  // Now get the body of the response...
  if(!status) switch(resp->type) {

    case RESP_ARRAY: {
      RESP **component;
      int i;

      if(resp->n <= 0) break;

      resp->value = (RESP **) malloc(resp->n * sizeof(RESP *));
      if(resp->value == NULL) {
        fprintf(stderr, "WARNING! Redis-X : alloc %d RESP arrays: %s\n", resp->n, strerror(errno));
        status = X_FAILURE;
        // We should get the data from the input even if we have nowhere to store...
      }

      component = (RESP **) resp->value;

      for(i=0; i<resp->n; i++) {
        RESP* r = redisxReadReplyAsync(cl);     // Always read RESP even if we don't have storage for it...
        if(resp->value) component[i] = r;
      }

      // Consistency check. Discard response if incomplete (because of read errors...)
      if(resp->value) for(i=0; i<resp->n; i++) if(component[i] == NULL) {
        fprintf(stderr, "WARNING! Redis-X : incomplete array received (index %d of %d).\n", (i+1), resp->n);
        if(!status) status = REDIS_INCOMPLETE_TRANSFER;
        break;
      }

      break;
    }

    case RESP_BULK_STRING:
      if(resp->n < 0) break;                          // no string token following!

      resp->value = malloc(resp->n + 2);              // <string>\r\n
      if(resp->value == NULL) {
        fprintf(stderr, "WARNING! Redis-X : alloc string of %d bytes: %s\n", resp->n + 2, strerror(errno));
        status = X_FAILURE;
        // We still want to consume the bytes from the input...
      }

      size = rReadBytes(cp, (char *) resp->value, resp->n + 2);

      if(size < 0) {
        if(!status) status = size;
      }
      else if(resp->value) {
        ((char *) resp->value)[resp->n] = '\0';
        xdprintf("\"%s\"\n", (char *) resp->value);
      }

      break;

    case RESP_SIMPLE_STRING:
    case RESP_ERROR:
      resp->value = malloc(size);

      if(resp->value == NULL) {
        fprintf(stderr, "WARNING! Redis-X : alloc simple string of %d bytes: %s\n", size, strerror(errno));
        status = X_FAILURE;
        break;
      }

      memcpy(resp->value, &buf[1], size-1);
      resp->n = size-1;
      ((char *)resp->value)[resp->n] = '\0';

      break;

    case RESP_INT:          // Nothing left to do for INT type response.
      break;

    case RESP_PONG:
      if(strcmp(buf, "PONG\r\n")) {
        fprintf(stderr, "WARNING! Redis-X : garbled PONG?\n");
        status = X_PARSE_ERROR;
      }
      break;

    default:
      // FIXME workaround for Redis 4.x improper OK reply to QUIT
      if(!strcmp(buf, "OK")) {
        resp->type = RESP_SIMPLE_STRING;
        resp->value = strdup("OK");
      }
      else if(cp->isEnabled) {
        fprintf(stderr, "WARNING! Redis-X : invalid type '%c' in '%s'\n", buf[0], buf);
        status = REDIS_UNEXPECTED_RESP;
      }
  }

  // Check for errors, and return NULL if there were any.
  if(status) {
    redisxDestroyRESP(resp);
    redisxError(funcName, status);
    return NULL;
  }

  pthread_mutex_lock(&cp->pendingLock);
  cp->pendingRequests--;
  pthread_mutex_unlock(&cp->pendingLock);

  return resp;
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
  const char *funcName = "redisxIgnoreReplyAsync()";
  RESP *resp;

  if(cl == NULL) return redisxError(funcName, X_NULL);

  resp = redisxReadReplyAsync(cl);
  if(resp == NULL) return redisxError(funcName, REDIS_NULL);
  else redisxDestroyRESP(resp);
  return X_SUCCESS;
}

/**
 * Reads a chunk of data into the client's receive holding buffer.
 *
 * @param cp        Pointer to the private data of the client.
 * @return          X_SUCCESS (0) if successful, or else an appropriate error (see xchange.h).
 */
static int rReadChunkAsync(ClientPrivate *cp) {
  const int sock = cp->socket;      // Local copy of socket fd that won't possibly change mid-call.

  if(sock < 0) return X_NO_INIT;

  cp->next = 0;
  cp->available = recv(sock, cp->in, REDIS_RCV_CHUNK_SIZE, 0);
  xdprintf(" ... read %d bytes from client %d socket.\n", cp->available, cp->idx);
  if(cp->available <= 0) {
    if(cp->available == 0) errno = ECONNRESET;        // 0 return is remote cleared connection. So set ECONNRESET...
    return rTransmitError(cp, "read");
  }
  return X_SUCCESS;
}

/**
 * Tries to read a REDIS "\r\n" terminated token into the specified buffer, using up to the specified amount
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
  const char *funcName = "rReadToken()";
  int foundTerms = 0, L;

  length--; // leave room for termination in incomplete tokens...

  pthread_mutex_lock(&cp->readLock);

  if(!cp->isEnabled) {
    pthread_mutex_unlock(&cp->readLock);
    return redisxError(funcName, X_NO_SERVICE);
  }

  for(L=0; cp->isEnabled && foundTerms < 2; L++) {
    char c;

    // Read a chunk of available data from the socket...
    if(cp->next >= cp->available) {
      int status = rReadChunkAsync(cp);
      if(status) {
        pthread_mutex_unlock(&cp->readLock);
        return redisxError(funcName, status);
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
    return X_NO_SERVICE;
  }

  // From here on L is the number of characters actually buffered.
  if(L > length) L = length-1;

  // Discard "\r\n"
  if(foundTerms == 2) L -= 2;

  // Terminate string in buffer
  buf[L] = '\0';

  xdprintf("[%s]\n", buf);

  if(*buf == RESP_ERROR) {
    fprintf(stderr, "Redis-X> error message: %s\n", &buf[1]);
    return L;
  }

  return foundTerms==2 ? L : redisxError(funcName, REDIS_INCOMPLETE_TRANSFER);
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
  const char *funcName = "rReadBytes()";
  int L;

  pthread_mutex_lock(&cp->readLock);

  if(!cp->isEnabled) {
    pthread_mutex_unlock(&cp->readLock);
    return redisxError(funcName, X_NO_SERVICE);
  }

  for(L=0; cp->isEnabled && L<length; L++) {
    // Read a chunk of available data from the socket...
    if(cp->next >= cp->available) {
      int status = rReadChunkAsync(cp);
      if(status) {
        pthread_mutex_unlock(&cp->readLock);
        return redisxError(funcName, status);
      }
    }

    if(buf) buf[L] = cp->in[cp->next++];
  }

  pthread_mutex_unlock(&cp->readLock);

  return L;
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
 * Specific call for dealing with socket level transmit (send/rcv) errors. It prints a descriptive message to
 * sdterr, and calls the configured user transmit error handler routine, and either exists the program
 * (if redisExitOnConnectionError() is set), or returns X_NO_SERVICE.
 *
 * @param cp    Pointer to the client's private data structure on which the error occurred.
 * @param op    The operation that failed, e.g. 'send' or 'read'.
 * @return      X_NO_SERVICE
 *
 * @sa redisxSetTransmitErrorHandler()
 */
static int rTransmitError(ClientPrivate *cp, const char *op) {
  if(cp->isEnabled) {
    RedisPrivate *p = (RedisPrivate *) cp->redis->priv;
    // Let the handler disconnect, if it wants to....
    fprintf(stderr, "RedisX : WARNING! %s failed on %s channel %d: %s\n", op, cp->redis->id, cp->idx, strerror(errno));
    if(p->transmitErrorFunc) p->transmitErrorFunc(cp->redis, cp->idx, op);
  }
  return X_NO_SERVICE;
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
 * Add a targeted subscriber processing function to the list of functions that process
 * Redis PUB/SUB responses. You will still have to subscribe the relevant PUB/SUB messages
 * from redis separately, using redisxSubscribe() before any messages are delivered to
 * this client. If the subscriber with the same callback function and channel stem
 * is already added, this call simply return and will NOT create a duplicate enry.
 * However, the same callback may be added multiple times with different channel stems
 * (which pre-filter what messages each of the callbacks may get).
 *
 * \param redis         Pointer to a Redis instance.
 * \param channelStem   If NULL, the consumer will receive all Redis messages published to the given channel.
 *                      Otherwise, the consumer will be notified only if the incoming channel begins with the
 *                      specified stem.
 * \param f             A function that consumes subscription messages.
 *
 * @sa redisxRemoveSubscribers()
 * @sa redisxSubscribe()
 *
 */
void redisxAddSubscriber(Redis *redis, const char *channelStem, RedisSubscriberCall f) {
  MessageConsumer *c;
  RedisPrivate *p;

  if(redis == NULL) return;

  p = (RedisPrivate *) redis->priv;

  rSubscriberLock(redis);

  // Check if the subscriber is already listed with the same stem. If so, nothing to do...
  for(c = p->subscriberList; c != NULL; c = c->next) {
    if(f != c->func) continue;

    if(channelStem) {
      if(strcmp(channelStem, c->channelStem)) continue;
    }
    else if(c->channelStem) continue;

    // This subscriber is already listed. Nothing to do.
    xdprintf("DEBUG-X> Matching subscriber callback for stem %s already listed. Nothing to do.\n", channelStem);
    rSubscriberUnlock(redis);
    return;
  }

  c = (MessageConsumer *) calloc(1, sizeof(MessageConsumer));

  c->func = f;
  c->channelStem = xStringCopyOf(channelStem);
  c->next = p->subscriberList;
  p->subscriberList = c;

  rSubscriberUnlock(redis);

  xvprintf("Redis-X> Added new subscriber callback for stem %s.\n", channelStem);
}


/**
 * Removes all instances of a subscribe consumer function from the current list of consumers.
 * This calls only deactivates the specified processing callback function(s), without
 * stopping the delivery of associated messages. To stop Redis sending messages that are
 * no longer being processed, you should also call redisxUnsubscribe() as appropriate.
 *
 * \param redis     Pointer to a Redis instance.
 * \param f         The consumer function to remove from the list of active subscribers.
 *
 * \return  The number of instances of f() that have been removed from the list of subscribers.
 *
 * \sa redisxAddSubscriber()
 * @sa redisxClearSubscribers()
 * @sa redisxUnsubscrive()
 */
int redisxRemoveSubscribers(Redis *redis, RedisSubscriberCall f) {
  const char *funcName = "redisxRemoveSubscribers()";

  RedisPrivate *p;
  MessageConsumer *c, *last = NULL;
  int removed = 0;

  if(!f) return redisxError(funcName, X_NULL);
  if(redis == NULL) return redisxError(funcName, X_NULL);

  p = (RedisPrivate *) redis->priv;

  rSubscriberLock(redis);

  for(c = p->subscriberList; c != NULL; ) {
    MessageConsumer *next = c->next;

    if(c->func == f) {
      if(last) last->next = next;
      else p->subscriberList = next;

      if(c->channelStem) free(c->channelStem);
      free(c);

      removed++;
    }
    else last = c;

    c = next;
  }

  rSubscriberUnlock(redis);

  xvprintf("Redis-X> Removed %d subscriber callbacks.\n", removed);

  return removed;
}

/**
 * Stops the custom consumption of PUB/SUB messages from Redis.
 *
 * \param redis         Pointer to a Redis instance.
 *
 * \return              X_SUCCESS (0)   if successful, or
 *                      X_NULL          if the redis instance is NULL.
 *
 */
int redisxClearSubscribers(Redis *redis) {
  RedisPrivate *p;
  MessageConsumer *c;
  int n = 0;

  if(redis == NULL) return redisxError("redisxClearSubscribers()", X_NULL);

  p = (RedisPrivate *) redis->priv;

  rSubscriberLock(redis);
  c = p->subscriberList;
  p->subscriberList = NULL;
  rSubscriberUnlock(redis);

  while(c != NULL) {
    MessageConsumer *next = c->next;
    free(c);
    c = next;
    n++;
  }

  xvprintf("Redis-X> Cleared all (%d) subscriber callbacks.\n", n);

  return n;
}


/**
 * Checks if a given string is a glob-style pattern.
 *
 * \param str       The string to check.
 *
 * \return          TRUE if it is a glob pattern (e.g. has '*', '?' or '['), otherwise FALSE.
 *
 */
static int rIsGlobPattern(const char *str) {
  for(; *str; str++) switch(*str) {
    case '*':
    case '?':
    case '[': return TRUE;
  }
  return FALSE;
}


/**
 * Subscribe to a specific Redis channel. The call will also start the subscription listener
 * thread to processing incoming subscription messages. Subscribing only enabled the delivery
 * of the messages to this client without any actions on these messages. In order to process
 * the messages for your subscriptons, you will also want to call redisxAddSubscriber() to add
 * your custom processor function(s).
 *
 * \param redis         Pointer to a Redis instance.
 * \param pattern       The Channel pattern to subscribe to, e.g. 'acc1', or 'acc*'...
 *
 * \return      X_SUCCESS       if successfully subscribed to the Redis distribution channel.
 *              X_NO_SERVICE    if there is no active connection to the Redis server.
 *              X_NULL          if the channel argument is NULL
 *
 * @sa redisxAddSubscriber()
 * @sa redisxUnsubscribe()
 * @sa redisxNotify()
 * @sa redisxPublish()
 * @sa redisxPublishAsync()
 */
int redisxSubscribe(Redis *redis, const char *pattern) {
  const char *funcName = "redisxSubscribe()";

  const ClientPrivate *cp;
  int status = 0;

  if(redis == NULL || pattern == NULL) return redisxError(funcName, X_NULL);

  // connect subscription client as needed.
  rConfigLock(redis);
  cp = (ClientPrivate *) redis->subscription->priv;
  if(!cp->isEnabled) status = rConnectSubscriptionClientAsync(redis);
  if(!status) {
    // start processing subscriber responses if not already...
    const RedisPrivate *p = (RedisPrivate *) redis->priv;
    if(!p->isSubscriptionListenerEnabled) rStartSubscriptionListenerAsync(redis, &threadConfig);
  }
  rConfigUnlock(redis);

  if(status) return redisxError(funcName, status);

  status = redisxLockEnabled(redis->subscription);
  if(status) return redisxError(funcName, status);

  status = redisxSendRequestAsync(redis->subscription, rIsGlobPattern(pattern) ? "PSUBSCRIBE" : "SUBSCRIBE", pattern, NULL, NULL);
  redisxUnlockClient(redis->subscription);

  if(status) return redisxError(funcName, status);

  return X_SUCCESS;
}

/**
 * Unsubscribe from one or all Redis PUB/SUB channel(s). If there are no active subscriptions
 * when Redis confirms the unsubscrive command, the subscription listener thread will also conclude
 * automatically. Unsubscribing will stop delivery of mesasages for the affected channels but
 * any associated processing callbacks remain registered, until redisxRemovesubscribers() is
 * called to deactive them as appropriate.
 *
 * \param redis         Pointer to a Redis instance.
 * \param pattern       The channel pattern, or NULL to unsubscribe all channels and patterns.
 *
 * \return     X_SUCCESS       if successfully subscribed to the Redis distribution channel.
 *             X_NO_SERVICE    if there is no active connection to the Redis server.
 *
 * @sa redisxSubscribe()
 * @sa redisxEndSubscribe()
 * @sa redisxRemoveSubscribers()
 */
int redisxUnsubscribe(Redis *redis, const char *pattern) {
  const char *funcName = "redisxUnsubscribe()";

  int status;

  if(redis == NULL) return redisxError(funcName, X_NULL);

  status = redisxLockEnabled(redis->subscription);
  if(status) return redisxError(funcName, status);

  if(pattern) {
    status = redisxSendRequestAsync(redis->subscription, rIsGlobPattern(pattern) ? "PUNSUBSCRIBE" : "UNSUBSCRIBE", pattern, NULL, NULL);
  }
  else {
    status = redisxSendRequestAsync(redis->subscription, "UNSUBSCRIBE", NULL, NULL, NULL);
    if(!status) status = redisxSendRequestAsync(redis->subscription, "PUNSUBSCRIBE", NULL, NULL, NULL);
  }

  redisxUnlockClient(redis->subscription);

  if(status) return redisxError(funcName, status);

  return X_SUCCESS;
}


/**
 * Unsubscribes from all channels, stops the subscription listener thread, and closes the subscription
 * client connection. This call assumes the caller has an exlusive lock on the configuration properties
 * of the affected Redis instance.
 *
 * @param redis     Pointer to a Redis instance.
 *
 * @sa redisxEndSubscribe()
 * @sa redisxUnsubscribe()
 */
static void rEndSubscriptionAsync(Redis *redis) {
  RedisPrivate *p;

  xvprintf("Redis-X> End all subscriptions, and quit listener.\n");

  p = (RedisPrivate *) redis->priv;

  redisxUnsubscribe(redis, NULL);

  p->isSubscriptionListenerEnabled = FALSE;
  rCloseClient(redis->subscription);
}

/**
 * Unsubscribes from all channels, stops the subscription listener thread, and closes the subscription
 * client connection.
 *
 * @param redis     Pointer to a Redis instance.
 *
 * @sa redisxUnsubscribe()
 */
void redisxEndSubscription(Redis *redis) {
  rConfigLock(redis);
  rEndSubscriptionAsync(redis);
  rConfigUnlock(redis);
}



/**
 * This is the subscription client thread listener routine. It is started by redisScubscribe(), and stopped
 * when Redis confirms that there are no active subscriptions following an 'unsubscribe' request.
 *
 * \param pRedis        Pointer to a Redis instance.
 *
 * \return              Always NULL.
 *
 */
static void *RedisSubscriptionListener(void *pRedis) {
  static int counter, lastError;

  Redis *redis = (Redis *) pRedis;
  RedisPrivate *p;
  RedisClient *cl;
  const ClientPrivate *cp;
  RESP *reply = NULL, **component;
  int i;

  pthread_detach(pthread_self());

  xvprintf("Redis-X> Started processing subsciptions...\n");

  if(redis == NULL) {
    redisxError("RedisSubscriptionListener", X_NULL);
    return NULL;
  }

  p = (RedisPrivate *) redis->priv;
  cl = redis->subscription;
  cp = (ClientPrivate *) cl->priv;

  while(cp->isEnabled && p->isSubscriptionListenerEnabled && pthread_equal(p->subscriptionListenerTID, pthread_self())) {
    // Discard the response from the prior iteration
    if(reply) redisxDestroyRESP(reply);

    // Get the new response...
    reply = redisxReadReplyAsync(cl);

    counter++;

    // In case client was closed while waiting for response, break out...
    if(!cp->isEnabled) break;

    if(reply == NULL) {
      fprintf(stderr, "WARNING! Redis-X : invalid subscriber response, errno = %d.\n", errno);
      continue;
    }

    if(reply->n < 0) {
      if(reply->n != lastError) fprintf(stderr, "ERROR! Redis-X : subscriber parse error: %d.\n", reply->n);
      lastError = reply->n;
      continue;
    }

    if(reply->type != RESP_ARRAY) {
      fprintf(stderr, "WARNING! Redis-X : unexpected subscriber response type: '%c'.\n", reply->type);
      continue;
    }

    component = (RESP **) reply->value;

    lastError = 0;

    // Check that the response has a valid array dimension
    if(reply->n < 3 || reply->n > 4) {
      fprintf(stderr, "WARNING! Redis-X: unexpected number of components in subscriber response: %d.\n", reply->n);
      continue;
    }

    // Check for NULL component (all except last -- the payload -- which may be NULL)
    for(i=reply->n-1; --i >= 0; ) if(component[i] == NULL) {
      fprintf(stderr, "WARNING! Redis-X : subscriber NULL in component %d.\n", (i+1));
      continue;
    }

    if(!strcmp("message", (char *) component[0]->value)) {
      // Send the message to the matching subscribers or warn if invalid....
      if(reply->n == 3)
        rNotifyConsumers(redis, NULL, (char *) component[1]->value, (char *) component[2]->value, component[2]->n);
      else fprintf(stderr, "WARNING! Redis-X: unexpected subscriber message dimension: %d.\n", reply->n);
    }

    else if(!strcmp("pmessage", (char *) component[0]->value)) {
      // Send the message to the matching subscribers or warn if invalid....
      if(reply->n == 4)
        rNotifyConsumers(redis, (char *) component[1]->value, (char *) component[2]->value, (char *) component[3]->value, component[3]->n);
      else fprintf(stderr, "WARNING! Redis-X: unexpected subscriber pmessage dimension: %d.\n", reply->n);
    }

    //    else if(!strcmp("unsubscribe", (char *) component[0]->value)) if(component[2]->n <= 0) {
    //      xvprintf("Redis-X> no more subscriptions.\n");
    //
    //      // if unsubscribe, and no subscribers left, then exit the listener...
    //      // !!! THIS IS DANGEROUS !!! -- because we can have a concurrent subscribe that will think
    //      // listener is alive, just as we are about to kill it. It's safer to keep it running...
    //      //break;
    //    }

  } // <-- End of listener loop

  rConfigLock(redis);
  // If we are the current listener thread, then mark the listener as disabled.
  if(pthread_equal(p->subscriptionListenerTID, pthread_self())) p->isSubscriptionListenerEnabled = FALSE;
  rConfigUnlock(redis);

  xvprintf("Redis-X> Stopped processing subscriptions (%d processed)...\n", counter);

  redisxDestroyRESP(reply);

  return NULL;
}

static void rNotifyConsumers(Redis *redis, char *pattern, char *channel, char *msg, int length) {
#if LISTENER_YIELD_COUNT > 0
  static int count;
#endif

  MessageConsumer *c;
  RedisPrivate *p;
  RedisSubscriberCall *f = NULL;
  int n = 0;

  xdprintf("NOTIFY: %s | %s\n", channel, msg);

  p = (RedisPrivate *) redis->priv;

  rSubscriberLock(redis);

  // Count how many matching subscribers there are...
  for(c = p->subscriberList ; c != NULL; c = c->next) {
    if(c->channelStem != NULL) if(strncmp(c->channelStem, channel, strlen(c->channelStem))) continue;
    n++;
  }

  if(n) {
    // Put matching subscribers into an array...
    f = (RedisSubscriberCall *) calloc(n, sizeof(*f));
    n = 0;
    for(c = p->subscriberList ; c != NULL; c = c->next) {
      if(c->channelStem != NULL) if(strncmp(c->channelStem, channel, strlen(c->channelStem))) continue;
      f[n++] = c->func;
    }
  }

  rSubscriberUnlock(redis);

  // If there were matching subscribers, call them...
  if(n) {
    int i;
    for(i=0; i<n; i++) f[i](pattern, channel, msg, length);
    free(f);
  }


#if LISTENER_YIELD_COUNT > 0
  // Allow the waiting processes to take control...
  if(++count % LISTENER_YIELD_COUNT == 0) sched_yield();
#endif
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
      fprintf(stderr, "WARNING! SMA-X: pipeline null response.\n");
      continue;
    }

    if(reply->n < 0) {
      if(reply->n != lastError) fprintf(stderr, "ERROR! SMA-X: pipeline parse error: %d.\n", reply->n);
      lastError = reply->n;
      continue;
    }

    // Skip confirms...
    if(reply->type == RESP_SIMPLE_STRING) continue;

    consume = p->pipelineConsumerFunc;
    if(consume) consume(reply);


#if LISTENER_YIELD_COUNT > 0
    // Allow the waiting processes to take control...
    if(counter % LISTENER_YIELD_COUNT == 0) sched_yield();
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
 * Starts the PUB/SUB listener thread with the specified thread attributes.
 *
 * \param redis     Pointer to the Redis instance.
 * \param attr      The thread attributes to set for the PUB/SUB listener thread.
 *
 * \return          0 if successful, or -1 if pthread_create() failed.
 *
 */
static int rStartSubscriptionListenerAsync(Redis *redis, pthread_attr_t *attr) {
  RedisPrivate *p = (RedisPrivate *) redis->priv;

#if SET_PRIORITIES
  struct sched_param param;
#endif

  p->isSubscriptionListenerEnabled = TRUE;

  if (pthread_create(&p->subscriptionListenerTID, attr, RedisSubscriptionListener, redis) == -1) {
    perror("ERROR! Redis-X : pthread_create SubscriptionListener");
    p->isSubscriptionListenerEnabled = FALSE;
    return -1;
  }

#if SET_PRIORITIES
  param.sched_priority = LISTENER_PRIORITY;
  pthread_attr_setschedparam(attr, &param);
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
static int rStartPipelineListenerAsync(Redis *redis, pthread_attr_t *attr) {
  RedisPrivate *p = (RedisPrivate *) redis->priv;

#if SET_PRIORITIES
  struct sched_param param;
#endif

  p->isPipelineListenerEnabled = TRUE;

  if (pthread_create(&p->pipelineListenerTID, attr, RedisPipelineListener, redis) == -1) {
    perror("ERROR! Redis-X : pthread_create PipelineListener");
    p->isPipelineListenerEnabled = FALSE;
    return -1;
  }

#if SET_PRIORITIES
  param.sched_priority = LISTENER_PRIORITY;
  pthread_attr_setschedparam(attr, &param);
  pthread_setschedparam(p->pipelineListenerTID, SCHED_RR, &param);
#endif

  return 0;
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

#if !(__Lynx__ && __powerpc__)

/**
 * Removes all Redis entries that match the specified table:field name pattern.
 *
 * \param redis     Pointer to the Redis instance.
 * \param pattern   Glob pattern of aggregate table:field IDs to delete
 * \return          The number of tables + fields that were matched and deleted, or else an xchange error code (&lt;0).
 */
int redisxDeleteEntries(Redis *redis, const char *pattern) {
  char **keys;
  int i, n = 0, status;

  if(!pattern) return X_NAME_INVALID;

  keys = redisxScanKeys(redis, pattern, &n, &status);
  if(status) return status;
  if(!keys) return X_NULL;

  for(i = 0; i < n ; i++) {
    const char *table = keys[i];
    RedisEntry *entries;
    int nEntries;

    // If the table itself matches, delete it wholesale...
    if(fnmatch(pattern, table, 0) == 0) {
      RESP *reply = redisxRequest(redis, "DEL", table, NULL, NULL, &status);
      if(redisxCheckDestroyRESP(reply, RESP_INT, 1) == X_SUCCESS) n++;
      continue;
    }

    // Otherwise check the table entries...
    entries = redisxScanTable(redis, table, pattern, &nEntries, &status);
    if(status == X_SUCCESS) {
      int k;
      for(k = 0; k < nEntries; k++) {
        const RedisEntry *e = &entries[k];
        char *id = xGetAggregateID(table, e->key);

        if(id) {
          if(fnmatch(pattern, id, 0) == 0) {
            RESP *reply = redisxRequest(redis, "HDEL", table, e->key, NULL, &status);
            if(redisxCheckDestroyRESP(reply, RESP_INT, 1) == X_SUCCESS) n++;
          }
          free(id);
        }

        if(e->key) free(e->key);
        if(e->value) free(e->value);
      }
    }

    if(entries) free(entries);
  }
  return n;
}

#endif

