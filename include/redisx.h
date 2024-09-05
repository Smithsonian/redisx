/**
 * \file
 *
 * \date May 4, 2018
 * \author Attila Kovacs
 *
 *   RedisX public constant, data types and function prototypes.
 */

#ifndef REDISX_H_
#define REDISX_H_

#include <pthread.h>
#include <stdint.h>

#include "xchange.h"

// Configuration constants ------------------------------------------------------->
#ifndef REDISX_TCP_PORT
/// Default TCP/IP port on which Redis server listens to clients.
#  define REDISX_TCP_PORT                  6379
#endif

#ifndef REDISX_TCP_BUF_SIZE
/// (bytes) Default TCP buffer size (send/recv) for Redis clients. Values &lt;= 0 will use system default.
#  define REDISX_TCP_BUF_SIZE                   0
#endif

#ifndef REDISX_CMDBUF_SIZE
/// (bytes) Size of many internal arrays, and the max. send chunk size. At least ~16 bytes...
#  define REDISX_CMDBUF_SIZE               8192
#endif

#ifndef REDISX_RCVBUF_SIZE
/// (bytes) Redis receive buffer size (at most that much is read from the socket in a single call).
#  define REDISX_RCVBUF_SIZE            8192
#endif

#ifndef REDISX_SET_LISTENER_PRIORITY
/// Whether to explicitly set listener thread priorities
#  define REDISX_SET_LISTENER_PRIORITY    FALSE
#endif

#ifndef REDISX_LISTENER_REL_PRIORITY
/// [0.0:1.0] Listener priority as fraction of available range
/// You may want to set it quite high to ensure that the receive buffer is promptly cleared.
#  define REDISX_LISTENER_REL_PRIORITY    (0.5)
#endif

// Various exposed constants ----------------------------------------------------->

/// API major version
#define REDISX_MAJOR_VERSION  0

/// API minor version
#define REDISX_MINOR_VERSION  9

/// Integer sub version of the release
#define REDISX_PATCHLEVEL     0

/// Additional release information in version, e.g. "-1", or "-rc1".
#define REDISX_RELEASE_STRING "-devel"

/// \cond PRIVATE

#ifdef str_2
#  undef str_2
#endif

/// Stringify level 2 macro
#define str_2(s) str_1(s)

#ifdef str_1
#  undef str_1
#endif

/// Stringify level 1 macro
#define str_1(s) #s

/// \endcond

/// The version string for this library
/// \hideinitializer
#define REDISX_VERSION_STRING str_2(REDISX_MAJOR_VERSION) "." str_2(REDISX_MINOR_VERSION) \
                                  "." str_2(REDISX_PATCHLEVEL) REDISX_RELEASE_STRING

// These are the first character ID's for the standard RESP interface implemented by Redis.
#define RESP_ARRAY              '*'     ///< \hideinitializer RESP array type
#define RESP_INT                ':'     ///< \hideinitializer RESP integer type
#define RESP_SIMPLE_STRING      '+'     ///< \hideinitializer RESP simple string type
#define RESP_ERROR              '-'     ///< \hideinitializer RESP error message type
#define RESP_BULK_STRING        '$'     ///< \hideinitializer RESP bulk string type
#define RESP_PONG               'P'     ///< \hideinitializer RESP PONG response type

#define REDIS_INVALID_CHANNEL       (-101)  ///< \hideinitializer There is no such channel in the Redis instance.
#define REDIS_NULL                  (-102)  ///< \hideinitializer Redis returned NULL
#define REDIS_ERROR                 (-103)  ///< \hideinitializer Redis returned an error
#define REDIS_INCOMPLETE_TRANSFER   (-104)  ///< \hideinitializer The transfer to/from Redis is incomplete
#define REDIS_UNEXPECTED_RESP       (-105)  ///< \hideinitializer Got a Redis response of a different type than expected
#define REDIS_UNEXPECTED_ARRAY_SIZE (-106)  ///< \hideinitializer Got a Redis response with different number of elements than expected.

/**
 * RedisX channel IDs. RedisX uses up to three separate connections to the server: (1) an interactive client, in which
 * each query is a full round trip, (2) a pipeline clinet, in which queries are submitted in bulk, and responses
 * arrive asynchronously, and (3) a substription client devoted to PUB/SUB requests and push messages. Not all clients
 * are typically initialized at start. The interactive channel is always connected; the pipeline client can be selected
 * when connecting to the server; and the subscription client is connected as needed to process PUB/SUB requests.
 */
enum redisx_channel {
  REDISX_INTERACTIVE_CHANNEL = 0,     ///< \hideinitializer Redis channel number for interactive queries
  REDISX_PIPELINE_CHANNEL,            ///< \hideinitializer Redis channel number for pipelined transfers
  REDISX_SUBSCRIPTION_CHANNEL         ///< \hideinitializer Redis channel number for PUB/SUB messages
};

#define REDISX_CHANNELS     (REDISX_SUBSCRIPTION_CHANNEL + 1)  ///< \hideinitializer The number of channels a Redis instance has.


/**
 * \brief Structure that represents a Redis response (RESP format).
 *
 * \sa redisxDestroyRESP()
 */
typedef struct RESP {
  char type;                    ///< RESP_ARRAY, RESP_INT ...
  int n;                        ///< Either the integer value of a RESP_INT response, or the dimension of
                                ///< the value field.
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
 * A type of function that handles Redis PUB/SUB messages. These functions should follow a set of
 * basic rules:
 *
 * <ul>
 * <li>The call should return promptly and never block for significant periors. If it has blocking
 * calls or if extended processing is required, the function should simply place a copy of the
 * necessary information on a queue and process queued entries in a separate thread. (The call
 * arguments will not persist beyond the scope of the call, so don't attempt to place them
 * directly in a queue.)</li>
 *
 * <li>The subscriber call should not attempt to modify or free() the strings it is called with.
 * The same strings maybe used by other subscribers, and thus modifying their content would
 * produce unpredictable results with those subscribers.</li>
 *
 * <li>If the call needs to manipulate the supplied string arguments, it should operate on copies
 * (e.g. obtained via
 * xStringCopy()).</li>
 *
 * <li>The caller should free up any temporary resources it allocates, including copies of the
 * argument strings, before returning. However, it should never call free() on the supplied
 * arguments directly.</li>
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
typedef void (*RedisSubscriberCall)(const char *pattern, const char *channel, const char *msg, long length);


/**
 * User-specified callback function for handling RedisX errors.
 *
 * @param redis     Pointer to the RedisX instance
 * @param channel   the channel over which the error occurred
 * @param op        the name/ID of the operation where the error occurred.
 */
typedef void (*RedisErrorHandler)(Redis *redis, enum redisx_channel channel, const char *op);

void redisxSetVerbose(boolean value);
boolean redisxIsVerbose();

void redisxSetTcpBuf(int size);
int redisxSetTransmitErrorHandler(Redis *redis, RedisErrorHandler f);

void redisxSetPort(Redis *redis, int port);
int redisxSetUser(Redis *redis, const char *username);
int redisxSetPassword(Redis *redis, const char *passwd);
int redisxSelectDB(Redis *redis, int idx);

Redis *redisxInit(const char *server);
void redisxDestroy(Redis *redis);
int redisxConnect(Redis *redis, boolean usePipeline);
void redisxDisconnect(Redis *redis);
int redisxReconnect(Redis *redis, boolean usePipeline);
int redisxPing(Redis *redis, const char *message);

boolean redisxIsConnected(Redis *redis);
boolean redisxHasPipeline(Redis *redis);

RedisClient *redisxGetClient(Redis *redis, enum redisx_channel channel);
RedisClient *redisxGetLockedConnectedClient(Redis *redis, enum redisx_channel channel);

void redisxAddConnectHook(Redis *redis, void (*setupCall)(Redis *));
void redisxRemoveConnectHook(Redis *redis, void (*setupCall)(Redis *));
void redisxClearConnectHooks(Redis *redis);

void redisxAddDisconnectHook(Redis *redis, void (*cleanupCall)(Redis *));
void redisxRemoveDisconnectHook(Redis *redis, void (*cleanupCall)(Redis *));
void redisxClearDisconnectHooks(Redis *redis);

RESP *redisxRequest(Redis *redis, const char *command, const char *arg1, const char *arg2, const char *arg3, int *status);
RESP *redisxArrayRequest(Redis *redis, char *args[], int length[], int n, int *status);
int redisxSetValue(Redis *redis, const char *table, const char *key, const char *value, boolean confirm);
RESP *redisxGetValue(Redis*redis, const char *table, const char *key, int *status);
char *redisxGetStringValue(Redis *redis, const char *table, const char *key, int *len);
RedisEntry *redisxGetTable(Redis *redis, const char *table, int *n);
RedisEntry *redisxScanTable(Redis *redis, const char *table, const char *pattern, int *n, int *status);
int redisxMultiSet(Redis *redis, const char *table, const RedisEntry *entries, int n, boolean confirm);
char **redisxGetKeys(Redis *redis, const char *table, int *n);
char **redisxScanKeys(Redis *redis, const char *pattern, int *n, int *status);
void redisxSetScanCount(Redis *redis, int count);
int redisxGetScanCount(Redis *redis);
void redisxDestroyEntries(RedisEntry *entries, int count);
void redisxDestroyKeys(char **keys, int count);

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
int redisxLockConnected(RedisClient *cl);
int redisxUnlockClient(RedisClient *cl);

// Asynchronous access routines (use within redisxLockClient()/ redisxUnlockClient() blocks)...
int redisxSendRequestAsync(RedisClient *cl, const char *command, const char *arg1, const char *arg2, const char *arg3);
int redisxSendArrayRequestAsync(RedisClient *cl, char *args[], int length[], int n);
int redisxSetValueAsync(RedisClient *cl, const char *table, const char *key, const char *value, boolean confirm);
int redisxMultiSetAsync(RedisClient *cl, const char *table, const RedisEntry *entries, int n, boolean confirm);
RESP *redisxReadReplyAsync(RedisClient *cl);
int redisxIgnoreReplyAsync(RedisClient *cl);
int redisxSkipReplyAsync(RedisClient *cl);
int redisxPublishAsync(Redis *redis, const char *channel, const char *data, int length);

// Error generation with stderr message...
int redisxError(const char *func, int errorCode);
const char* redisxErrorDescription(int code);

// The following is not available on prior to the POSIX.1-2008 standard
// We'll use the __STDC_VERSION__ constant as a proxy to see if fnmatch is available
#if __STDC_VERSION__ > 201112L
int redisxDeleteEntries(Redis *redis, const char *pattern);
#endif

#endif /* REDISX_H_ */
