/**
 *
 * @date Created  on Aug 26, 2024
 * @author Attila Kovacs
 *
 *   Private header for internal use within the RedisX library only. This header should only be
 *   visible to the redisx library itself. Applications using redisx should not be exposed to its contents.
 *   You should never place this file into a shared location such as `/usr/include` or similar.
 */

#ifndef REDISX_PRIV_H_
#define REDISX_PRIV_H_

/// \cond PRIVATE

#include <pthread.h>
#include <stdint.h>
#if WITH_TLS
#  include <openssl/ssl.h>
#endif

#define __XCHANGE_INTERNAL_API__
#include "redisx.h"

#define IP_ADDRESS_LENGTH             40  ///< IPv6: 39 chars + termination.

#define REDISX_LISTENER_YIELD_COUNT   10  ///< yield after this many processed listener messages, <= 0 to disable yielding

typedef struct MessageConsumer {
  Redis *redis;
  char *channelStem;          ///< channels stem that incoming channels must begin with to meet for this notification to be activated.
  RedisSubscriberCall func;
  struct MessageConsumer *next;
} MessageConsumer;


typedef struct Hook {
  void (*call)(Redis *);
  void *arg;
  struct Hook *next;
} Hook;


typedef struct {
  Redis *redis;                 ///< Pointer to the enclosing Redis instance
  enum redisx_channel idx;      ///< e.g. REDISX_INTERACTIVE_CHANNEL, REDISX_PIPELINE_CHANNEL, or REDISX_SUBSCRIPTION_CHANNEL
  volatile boolean isEnabled;   ///< Whether the client is currecntly enabled for sending/receiving data
  pthread_mutex_t writeLock;    ///< A lock for writing and requests through this channel...
  pthread_mutex_t readLock;     ///< A lock for reading from the channel...
  pthread_mutex_t pendingLock;  ///< A lock for updating pendingRequests...
  char in[REDISX_RCVBUF_SIZE];  ///< Local input buffer
  int available;                ///< Number of bytes available in the buffer.
  int next;                     ///< Index of next unconsumed byte in buffer.
  int socket;                   ///< Changing the socket should require both locks!
#if WITH_TLS
  SSL_CTX *ctx;
  SSL *ssl;
#endif
  int pendingRequests;          ///< Number of request sent and not yet answered...
  RESP *attributes;             ///< Attributes from the last packet received.
} ClientPrivate;

typedef struct {
  RedisServer *servers;         ///< List of sentinel servers.
  int nServers;                 ///< number of servers in list
  char *serviceName;            ///< Name of service (for Sentinel).
  int timeoutMillis;            ///< [ms] Connection timeout for sentinel nodes.
} RedisSentinel;

#if WITH_TLS
typedef struct {
  boolean enabled;        ///< Whether TLS is enabled.
  char *certificate;      ///< Certificate (mutual TLS only)
  char *key;              ///< Private key (mutual TLS only)
  char *ca_certificate;   ///< CA sertificate
  char *dh_params;        ///< (optional) parameter file for DH based cyphers
} TLSConfig;
#endif

/**
 * A set of configuration settings for a Redis server connection.
 *
 */
typedef struct {
  int dbIndex;                  ///< the zero-based database index
  char *username;               ///< Redis user name (if any)
  char *password;               ///< Redis password (if any)
  int timeoutMillis;            ///< [ms] Socket read/write timeout
  int tcpBufSize;               ///< [bytes] TCP read/write buffer sizes to use
  int protocol;                 ///< RESP version to use
  boolean hello;                ///< whether to use HELLO (introduced in Redis 6.0.0 only)
  RedisSocketConfigurator socketConf;   ///< Additional user configuration of client sockets

#if WITH_TLS
  TLSConfig tls;                ///< TLS configuration settings
#endif

  Hook *firstCleanupCall;       ///< Linked list of cleanup calls
  Hook *firstConnectCall;       ///< Linked list of connection calls

  void (*pipelineConsumerFunc)(RESP *response);   ///< Callback function to process pipelined responses
  RedisErrorHandler transmitErrorFunc;            ///< Callback function to process socket-level errors

  RedisPushProcessor pushConsumer; ///< User-defined function to consume RESP3 push messages.
  void *pushArg;                ///< User-defined argument to pass along with push messages.
} RedisConfig;

typedef struct {
  char *hostname;               ///< Server host name or IP address
  int port;                     ///< port number (usually 6379)

  RedisSentinel *sentinel;      ///< Sentinel (high-availability) server configuration.
  RedisCluster *cluster;        ///< Cluster, in which this instance is a member
  uint32_t addr;                ///< The 32-bit inet address

  RedisConfig config;
  pthread_mutex_t configLock;

  RESP *helloData;              ///< RESP data received from server during last connection.

  RedisClient *clients;
  int scanCount;                ///< Count argument to use in SCAN commands, or <= 0 for default

  pthread_t pipelineListenerTID;
  pthread_t subscriptionListenerTID;

  boolean isPipelineListenerEnabled;
  boolean isSubscriptionListenerEnabled;

  pthread_mutex_t subscriberLock;
  MessageConsumer *subscriberList;

} RedisPrivate;

// in redisx.c ---------------------------->
int rCopyConfig(const RedisConfig *src, Redis *dst);
void rClearConfig(RedisConfig *config);

// in redisx-sub.c ------------------------>
int rConfigLock(Redis *redis);
int rConfigUnlock(Redis *redis);

// in redisx-net.c ------------------------>
int rConnectClient(Redis *redis, enum redisx_channel channel);
void rCloseClient(RedisClient *cl);
void rCloseClientAsync(RedisClient *cl);
boolean rIsLowLatency(const ClientPrivate *cp);
int rCheckClient(const RedisClient *cl);
int rSetServerAsync(Redis *redis, const char *desc, const char *hostname, int port);


// in redisx-hooks.c ---------------------->
Hook *rCopyHooks(const Hook *list, Redis *owner);
void rClearHooks(Hook *first);

// in redisx-cluster.c -------------------->
int rClusterRefresh(RedisCluster *cluster);
uint16_t rCalcHash(const char *key);

// in resp.c ------------------------------>
int redisxAppendRESP(RESP *resp, RESP *part);

/// \endcond

#endif /* REDISX_PRIV_H_ */
