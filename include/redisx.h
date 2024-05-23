/**
 * \file
 *
 * \date May 4, 2018
 * \author Attila Kovacs
 */

#ifndef REDISX_H_
#define REDISX_H_

#include <pthread.h>
#include <stdint.h>

#include "xchange.h"

#define REDIS_TCP_PORT          6379    ///< TCP/IP port on which Redis server listens to clients.
#define REDIS_TCP_BUF              0    ///< (bytes), <= 0 to use system default.

// These are serve as array indices. The order does not matter, but they must span 0-2.
#define INTERACTIVE_CHANNEL         0   ///< \hideinitializer Redis channel number for interactive queries
#define PIPELINE_CHANNEL            1   ///< \hideinitializer Redis channel number for pipelined transfers
#define SUBSCRIPTION_CHANNEL        2   ///< \hideinitializer Redis channel number for PUB/SUB messages
#define REDIS_CHANNELS              3   ///< \hideinitializer The number of channels a Redis instance has.

// These are the first character ID's for the standard RESP interface implemented by Redis.
#define RESP_ARRAY              '*'     ///< \hideinitializer RESP array type
#define RESP_INT                ':'     ///< \hideinitializer RESP integer type
#define RESP_SIMPLE_STRING      '+'     ///< \hideinitializer RESP simple string type
#define RESP_ERROR              '-'     ///< \hideinitializer RESP error message type
#define RESP_BULK_STRING        '$'     ///< \hideinitializer RESP bulk string type
#define RESP_PONG               'P'     ///< \hideinitializer RESP PONG response type

#define REDIS_INVALID_CHANNEL       (-101)  ///< \hideinitializer There is no such channel in the Redis intance.
#define REDIS_NULL                  (-102)  ///< \hideinitializer Redis returned NULL
#define REDIS_ERROR                 (-103)  ///< \hideinitializer Redis returned an error
#define REDIS_INCOMPLETE_TRANSFER   (-104)  ///< \hideinitializer The transfer to/from Redis is incomplete
#define REDIS_UNEXPECTED_RESP       (-105)  ///< \hideinitializer Got a Redis response of a different type than expected
#define REDIS_UNEXPECTED_ARRAY_SIZE (-106)  ///< \hideinitializer Got a Redis response with different number of elements than expected.

#define REDIS_TIMEOUT_SECONDS           3   ///< (seconds) Abort with an error if cannot send before this timeout (<=0 for not timeout)
#define REDIS_SIMPLE_STRING_SIZE      256   ///< Only store up to this many characters from Redis confirms and errors.
#define REDIS_CMDBUF_SIZE            8192   ///< Size of many internal arrays, and the max. send size. At least ~16 bytes...
#define REDIS_RCV_CHUNK_SIZE         8192   ///< (bytes) Redis receive buffer size.

#define SCAN_INITIAL_STORE_CAPACITY   256   ///< Number of Redis keys to allocate initially when using SCAN to get list of keywords

/**
 * \brief Structure that represents a Redis response (RESP format).
 *
 * \sa redisxDestroyRESP()
 */
typedef struct RESP {
  char type;                    ///< RESP_ARRAY, RESP_INT ...
  int n;                        ///< Either the integer value of a RESP_INT response, or the dimension of the value field.
  void *value;                  ///< Pointer to text (char *) content to an array of components (RESP**)...
} RESP;


/**
 * \brief A single key / value entry, or field, in the Redis database.
 *
 */
typedef struct RedisEntry {
  char *key;                    ///< The Redis key or field name
  char *value;                  ///< The string value stored for that field.
  int length;                   ///< Bytes in value.
} RedisEntry;


/**
 * \brief Structure that represents a single Redis client connection instance.
 *
 */
typedef struct RedisClient {
  /// \cond PRIVATE
  void *priv;                   ///< Private data for this Redis client not exposed by the public API
  /// \endcond
} RedisClient;


/**
 * \brief Structure that represents a Redis database instance, with one or more RedisClient connections.
 *
 * \sa redisxInit()
 * \sa redisxDestroy()
 */
typedef struct Redis {
  char *id;                     ///< The string ID of the Redis server. Default is IP, e.g. "127.0.0.1"

  RedisClient *interactive;     ///< Pointer to the interactive client connection
  RedisClient *pipeline;        ///< Pointer to the pipeline client connection
  RedisClient *subscription;    ///< Pointer to the subscription client connection

  /// \cond PROTECTED
  void *priv;                   ///< Private data for this Redis instance not exposed by the public API
  /// \endcond
} Redis;

/**
 * A type of function that handles Redis PUB/SUB messages. These functions should follow a set of basic rules:
 *
 * <ul>
 * <li>The call should return promptly and never block. If it has blocking calls or if extended processing is
 * required, the function should simply place a copy of the necessary information on a queue and process queued
 * entries in a separate thread. (The call arguments will not persist beyond the scope of the call, so don't
 * attempt to place them directly in a queue.)</li>
 *
 * <li>The subscriber call should not attempt to modify or free() the strings it is called with. The same strings
 * maybe used by other subscribers, and thus modifying their content would produce unpredictable results with those
 * subscribers.</li>
 *
 * <li>If the call needs to manipulate the supplied string arguments, it should operate on copies (e.g. obtained via
 * xStringCopy()).</li>
 *
 * <li>The caller should free up any temporary resources it allocates, including copies of the argument strings,
 * before returning. However, it should never call free() on the supplied arguments directly.</li>
 * </ul>
 *
 * @param pattern  The subscription pattern for which this notification came for
 *                 or NULL if not a pattern match.
 *
 * @param channel  The PUB/SUB channel on which the message arrived.
 *
 * @param msg      A pointer to the message content received.
 *                 The message buffer itself is not expected to last beyond the
 *                 call, so the function f() should make a copy if it for
 *                 any persistent use.
 *
 * @param length   The number of bytes in the message. Since Redis messages can
 *                 be binary a '\0' termination should no be assumed. Instead, the
 *                 length of the message is specified explicitly.
 */
typedef void (*RedisSubscriberCall)(const char *pattern, const char *channel, const char *msg, int length);

int hostnameToIP(const char *hostName, char *ip);
char *redisxGetHostName();
void redisxSetHostName(const char *name);

void redisxSetPassword(Redis *redis, const char *passwd);

void redisxSetVerbose(boolean value);
boolean redisxIsVerbose();

void redisxSetTcpBuf(int size);
int redisxGetTcpBuf();

int redisxSetTransmitErrorHandler(Redis *redis, void (*f)(Redis *redis, int channel, const char *op));

Redis *redisxInit(const char *server);
void redisxDestroy(Redis *redis);
int redisxConnect(Redis *redis, boolean usePipeline);
void redisxDisconnect(Redis *redis);
int redisxReconnect(Redis *redis, boolean usePipeline);
boolean redisxIsConnected(Redis *redis);
boolean redisxHasPipeline(Redis *redis);

RedisClient *redisxGetClient(Redis *redis, int channel);

void redisxAddConnectHook(Redis *redis, const void (*setupCall)(void));
void redisxRemoveConnectHook(Redis *redis, const void (*setupCall)(void));
void redisxClearConnectHooks(Redis *redis);

void redisxAddDisconnectHook(Redis *redis, const void (*cleanupCall)(void));
void redisxRemoveDisconnectHook(Redis *redis, const void (*cleanupCall)(void));
void redisxClearDisconnectHooks(Redis *redis);

RESP *redisxRequest(Redis *redis, const char *command, const char *arg1, const char *arg2, const char *arg3, int *status);
RESP *redisxArrayRequest(Redis *redis, char *args[], int length[], int n, int *status);
int redisxSetValue(Redis *redis, const char *table, const char *key, const char *value, boolean isPipelined);
RESP *redisxGetValue(Redis*redis, const char *table, const char *key, int *status);
RedisEntry *redisxGetTable(Redis *redis, const char *table, int *n);
RedisEntry *redisxScanTable(Redis *redis, const char *table, const char *pattern, int *n, int *status);
int redisxMultiSet(Redis *redis, const char *table, const RedisEntry *entries, int n, boolean isPipelined);
char **redisxGetKeys(Redis *redis, const char *table, int *n);
char **redisxScanKeys(Redis *redis, const char *pattern, int *n, int *status);
void redisxSetScanCount(Redis *redis, int count);
int redisxGetScanCount(Redis *redis);

int redisxSetPipelineConsumer(Redis *redis, void (*f)(RESP *));

int redisxPublish(Redis *redis, const char *channel, const char *message, int length);
int redisxNotify(Redis *redis, const char *channel, const char *message);
int redisxSubscribe(Redis *redis, const char *channel);
int redisxUnsubscribe(Redis *redis, const char *channel);
void redisxAddSubscriber(Redis *redis, const char *channelStem, RedisSubscriberCall f);
int redisxRemoveSubscribers(Redis *redis, RedisSubscriberCall f);
int redisxClearSubscribers(Redis *redis);
void redisxEndSubscription(Redis *redis);

int redisxStartBlockAsync(RedisClient *cl);
int redisxAbortBlockAsync(RedisClient *cl);
RESP *redisxExecBlockAsync(RedisClient *cl);
int redisxLoadScript(Redis *redis, const char *script, char **sha1);

int redisxGetTime(Redis *redis, struct timespec *t);

int redisxCheckRESP(const RESP *resp, char expectedType, int expectedSize);
int redisxCheckDestroyRESP(RESP *resp, char expectedType, int expectedSize);
void redisxDestroyRESP(RESP *resp);

// Locks for async calls
int redisxLockClient(RedisClient *cl);
int redisxLockEnabled(RedisClient *cl);
RedisClient *redisxGetLockedClient(Redis *redis);
int redisxUnlockClient(RedisClient *cl);

// Asynchronous access routines (use within redisxLockClient()/ redisxUnlockClient() blocks)...
int redisxSendRequestAsync(RedisClient *cl, const char *command, const char *arg1, const char *arg2, const char *arg3);
int redisxSendArrayRequestAsync(RedisClient *cl, char *args[], int length[], int n);
int redisxSendValueAsync(RedisClient *cl, const char *table, const char *key, const char *value, boolean confirm);
RESP *redisxReadReplyAsync(RedisClient *cl);
int redisxIgnoreReplyAsync(RedisClient *cl);
int redisxSkipReplyAsync(RedisClient *cl);
int redisxPublishAsync(Redis *redis, const char *channel, const char *data, int length);

// Error generation with stderr message...
int redisxError(const char *func, int errorCode);
const char* redisxErrorDescription(int code);

// The following is not available on Lynx, since it needs fnmatch...
#if !(__Lynx__ && __powerpc__)
int redisxDeleteEntries(Redis *redis, const char *pattern);
#endif
#endif /* REDISX_H_ */
