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
  enum redisx_channel idx;      ///< e.g. INTERACTIVE_CHANNEL, PIPELINE_CHANNEL, or SUBSCRIPTION_CHANNEL
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
  int port;                     ///< port number (usually 6379)
  int dbIndex;                  ///< the zero-based database index
  char *username;               ///< REdis user name (if any)
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
  RedisErrorHandler transmitErrorFunc;

  pthread_mutex_t subscriberLock;
  MessageConsumer *subscriberList;

} RedisPrivate;


// in redisx-sub.c ------------------------>
void rConfigLock(Redis *redis);
void rConfigUnlock(Redis *redis);
int rStartPipelineListenerAsync(Redis *redis);
int rStartSubscriptionListenerAsync(Redis *redis);

// in redisx-sub.c ------------------------>
void *RedisSubscriptionListener(void *pRedis);

// in redisx-net.c ------------------------>
void rInitClient(RedisClient *cl, enum redisx_channel channel);
int rConnectClient(Redis *redis, enum redisx_channel channel);
void rShutdownLinkAsync(Redis *redis);
void rCloseClient(RedisClient *cl);
boolean rIsLowLatency(const ClientPrivate *cp);

/// \endcond

#endif /* REDISX_PRIV_H_ */
