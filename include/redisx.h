/**
 * \file
 *
 * \date May 4, 2018
 * \author Attila Kovacs
 * \version 1.0
 *
 *  RedisX is a completely free Redis / Valkey client library, available on Github as:
 *
 *   https://github.com/Smithsonian/redisx
 *
 */

#ifndef REDISX_H_
#define REDISX_H_

#include <xchange.h>
#include <pthread.h>
#include <stdint.h>

// API version constants --------------------------------------------------------->

/// API major version
#define REDISX_MAJOR_VERSION  1

/// API minor version
#define REDISX_MINOR_VERSION  0

/// Integer sub version of the release
#define REDISX_PATCHLEVEL     3

/// Additional release information in version, e.g. "-1", or "-rc1".
#define REDISX_RELEASE_STRING ""



// Configuration constants ------------------------------------------------------->
#ifndef REDISX_TCP_PORT
/// Default TCP/IP port on which Redis server listens to clients.
#  define REDISX_TCP_PORT                 6379
#endif

#ifndef REDISX_TCP_BUF_SIZE
/// (bytes) Default TCP buffer size (send/recv) for Redis clients. Values &lt;= 0 will use system default.
#  define REDISX_TCP_BUF_SIZE             0
#endif

#ifndef REDISX_CMDBUF_SIZE
/// (bytes) Size of many internal arrays, and the max. send chunk size. At least ~16 bytes...
#  define REDISX_CMDBUF_SIZE              8192
#endif

#ifndef REDISX_RCVBUF_SIZE
/// (bytes) Redis receive buffer size (at most that much is read from the socket in a single call).
#  define REDISX_RCVBUF_SIZE              8192
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

#ifndef REDISX_DEFAULT_TIMEOUT_MILLIS
/// [ms] Default socket read/write timeout for Redis clients
#  define REDISX_DEFAULT_TIMEOUT_MILLIS           3000
#endif

#ifndef REDISX_DEFAULT_SENTINEL_TIMEOUT_MILLIS
/// [ms] Default socket read/write timeout for Redis clients
#  define REDISX_DEFAULT_SENTINEL_TIMEOUT_MILLIS   100
#endif

// Various exposed constants ----------------------------------------------------->

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

/**
 * Enumeration of RESP component types. These are the first character IDs for the standard RESP interface
 * implemented by Redis.
 *
 */
enum resp_type {
  // RESP2 types:
  RESP_ARRAY            = '*',    ///< \hideinitializer RESP array type
  RESP_INT              = ':',    ///< \hideinitializer RESP integer type
  RESP_SIMPLE_STRING    = '+',    ///< \hideinitializer RESP simple string type
  RESP_ERROR            = '-',    ///< \hideinitializer RESP error message type
  RESP_BULK_STRING      = '$',    ///< \hideinitializer RESP bulk string type

  // RESP3 types:
  RESP3_NULL            = '_',    ///< \hideinitializer RESP3 null value
  RESP3_DOUBLE          = ',',    ///< \hideinitializer RESP3 floating-point value
  RESP3_BOOLEAN         = '#',    ///< \hideinitializer RESP3 boolean value
  RESP3_BLOB_ERROR      = '!',    ///< \hideinitializer RESP3 blob error
  RESP3_VERBATIM_STRING = '=',    ///< \hideinitializer RESP3 verbatim string (with type)
  RESP3_BIG_NUMBER      = '(',    ///< \hideinitializer RESP3 big integer / decimal
  RESP3_MAP             = '%',    ///< \hideinitializer RESP3 dictionary of key / value
  RESP3_SET             = '~',    ///< \hideinitializer RESP3 unordered set of elements
  RESP3_ATTRIBUTE       = '|',    ///< \hideinitializer RESP3 dictionary of attributes (metadata)
  RESP3_PUSH            = '>',    ///< \hideinitializer RESP3 dictionary of attributes (metadata)
};

#define RESP3_CONTINUED   ';'     ///< \hideinitializer RESP3 dictionary of attributes (metadata)

#define REDIS_INVALID_CHANNEL       (-101)  ///< \hideinitializer There is no such channel in the Redis instance.
#define REDIS_NULL                  (-102)  ///< \hideinitializer Redis returned NULL
#define REDIS_ERROR                 (-103)  ///< \hideinitializer Redis returned an error
#define REDIS_INCOMPLETE_TRANSFER   (-104)  ///< \hideinitializer The transfer to/from Redis is incomplete
#define REDIS_UNEXPECTED_RESP       (-105)  ///< \hideinitializer Got a Redis response of a different type than expected
#define REDIS_UNEXPECTED_ARRAY_SIZE (-106)  ///< \hideinitializer Got a Redis response with different number of elements than expected.
#define REDIS_MOVED                 (-107)  ///< \hideinitializer The requested key has moved to another cluster shard.
#define REDIS_MIGRATING             (-108)  ///< \hideinitializer The requested key is importing, and you may query with ASKED on the specified node.

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
 * The RESP protocol to use for a Redis instance. Redis originally used RESP2, but later releases added
 * support for RESP3.
 *
 */
enum redisx_protocol {
  REDISX_RESP2 = 2,                   ///< \hideinitializer RESP2 protocol
  REDISX_RESP3                        ///< \hideinitializer RESP3 protocol (since Redis version 6.0.0)
};

/**
 * \brief Structure that represents a Redis response (RESP format).
 *
 * REFERENCES:
 * <ol>
 * <li>https://github.com/redis/redis-specifications/tree/master/protocol</li>
 * </ol>
 *
 * \sa redisxDestroyRESP()
 * \sa redisxIsScalarType()
 * \sa redisxIsStringType()
 * \sa redisxIsArrayType()
 * \sa redisxIsMapType()
 */
typedef struct RESP {
  enum resp_type type;          ///< RESP type; RESP_ARRAY, RESP_INT ...
  int n;                       ///< Either the integer value of a RESP_INT or a RESP3_BOOLEAN response, or the
                                ///< dimension of the value field.
  void *value;                  ///< Pointer to text (char *) content or to an array of components
                                ///< (RESP**) or (RedisMap *), or else a pointer to a `double`, depending
                                ///< on `type`.
} RESP;

/**
 * Structure that represents a key/value mapping in RESP3.
 *
 * @sa redisxIsMapType()
 * @sa RESP3_MAP
 * @sa RESP3_ATTRIBUTE
 *
 */
typedef struct {
  RESP *key;                    ///< The keyword component
  RESP *value;                  ///< The associated value component
} RedisMap;


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
 * Redis server host and port specification.
 *
 * @sa redisxInitSentinel()
 */
typedef struct RedisServer {
  char *host;                   ///< The hostname or IP address of the server
  int port;                     ///< The port number or &lt;=0 to use the default 6379
} RedisServer;



/**
 * A Redis cluster configuration
 *
 */
typedef struct {
  void *priv;                   ///< Private data not exposed to users.
} RedisCluster;

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
 * User-specified callback function for handling RedisX errors from socket-level read / write calls.
 * It's mainly there for the application to perform any cleanup as necessary or to report the error. However,
 * it can also interacti with the Redis instance in limited ways. The implementation should follow a set of
 * basic rules:
 *
 * <ul>
 * <li>It should not call synchronized functions on the affected client, including attempts to connect,
 * disconnect, or reconnect the client.</li>
 * <li>It may call `Async` functions for the client (the client is in a locked state when the handler is
 * called).</li>
 * <li>It should not attempt to lock or unlock the affected client.</li>
 * <li>It may use `errno` to gather information about the source of the error, and may even change or reset
 * `errno`, to change behavior (e.g. re-setting to `EAGAIN` or `EWOULBLOCK` will keep the client connected
 * and the return status of the failed call will become X_TIMEDOUT; or setting it to any other value will
 * disconnect the client and the failed call will return X_NO_SERVICE instead.).</li>
 * <li>The limitations can by bypassed by placing the error on a queue and let an asynchronous thread
 * take it from there.</li>
 * </ul>
 *
 * @param redis     Pointer to the RedisX instance
 * @param channel   the channel over which the error occurred
 * @param op        the name/ID of the operation where the error occurred.
 */
typedef void (*RedisErrorHandler)(Redis *redis, enum redisx_channel channel, const char *op);

/**
 * A user-defined function for consuming responses from a Redis pipeline connection. The implementation
 * should follow a set of simple rules:
 *
 * <ul>
 * <li>the implementation should not destroy the RESP data. The RESP will be destroyed automatically
 * after the call returns. However, the call may retain any data from the RESP itself, provided
 * the data is de-referenced from the RESP before return.<li>
 * <li>The implementation should not block (aside from maybe a quick mutex unlock) and return quickly,
 * so as to not block the client for long periods</li>
 * <li>If extensive processing or blocking calls are required to process the message, it is best to
 * simply place a copy of the RESP on a queue and then return quickly, and then process the message
 * asynchronously in a background thread.</li>
 * </ul>
 *
 * @param response     A response received from the pipeline client.
 *
 * @sa redisxSetPipelineConsumer()
 */
typedef void (*RedisPipelineProcessor)(RESP *response);

/**
 * A user-defined function for consuming push messages from a Redis client. The implementation
 * should follow a set of simple rules:
 *
 * <ul>
 * <li>the implementation should not destroy the RESP data. The RESP will be destroyed automatically
 * after the call returns. However, the call may retain any data from the RESP itself, provided
 * that such data is de-referenced from the RESP before the return.<li>
 * <li>The implementation should not block (aside from maybe a quick mutex unlock) and return quickly,
 * so as to not block the client for long periods</li>
 * <li>If extensive processing or blocking calls are required to process the message, it is best to
 * simply place a copy of the RESP on a queue and then return quickly, and then process the message
 * asynchronously in a background thread.</li>
 * <li>The client on which the push notification originated will be locked when this function is
 * called, waiting for a response to an earlier query. Thus the implementation should not attempt to
 * lock the client again or release the lock. It may send asynchronous requests on the client, e.g. via
 * redisxSendRequestAsync(), but it should not try to read a response (given that the client is
 * blocked for another read operation). If more flexible client access is needed, the implementation
 * should make a copy of the RESP and place it on a queue for asynchronous processing by another thread.
 * </li>
 * </ul>
 *
 * @param cl          The Redis client that sent the push. The client is locked for exlusive
 *                    access when this function is called.
 * @param message     The RESP3 message that was pushed by the client
 * @param ptr         Additional data passed along.
 *
 * @sa redisxSetPushProcessor()
 */
typedef void (*RedisPushProcessor)(RedisClient *cl, RESP *message, void *ptr);

/**
 * User callback function allowing additional customization of the client socket before connection.
 *
 * @param socket      The socket descriptor.
 * @param channel     REDISX_INTERACTIVE_CHANNEL, REDISX_PIPELINE_CHANNEL, REDISX_SUBSCRIPTION_CHANNEL
 * @return            X_SUCCESS (0) if the socket may be used as is after the return. Any other value
 *                    will indicate that the socket should not be used and that the caller itself should
 *                    fail with an error.
 */
typedef int (*RedisSocketConfigurator)(int socket, enum redisx_channel channel);

void redisxSetVerbose(boolean value);
boolean redisxIsVerbose();
void redisxDebugTraffic(boolean value);

int redisxSetReplyTimeout(Redis *redis, int timeoutMillis);
int redisxSetSocketTimeout(Redis *redis, int millis);
int redisxSetTcpBuf(Redis *redis, int size);
int redisxSetSentinelTimeout(Redis *redis, int millis);
int redisxSetSocketConfigurator(Redis *redis, RedisSocketConfigurator func);
int redisxSetSocketErrorHandler(Redis *redis, RedisErrorHandler f);

int redisxSetHostname(Redis *redis, const char *host);
int redisxSetPort(Redis *redis, int port);
int redisxSetUser(Redis *redis, const char *username);
int redisxSetPassword(Redis *redis, const char *passwd);
int redisxSelectDB(Redis *redis, int idx);
int redisxSetProtocol(Redis *redis, enum redisx_protocol protocol);

Redis *redisxInit(const char *server);
Redis *redisxInitSentinel(const char *serviceName, const RedisServer *serverList, int nServers);
int redisxValidateSentinel(const char *serviceName, const RedisServer *serverList, int nServers);
int redisxCheckValid(const Redis *redis);
void redisxDestroy(Redis *redis);
int redisxConnect(Redis *redis, boolean usePipeline);
void redisxDisconnect(Redis *redis);
int redisxReconnect(Redis *redis, boolean usePipeline);

int redisxSetTLS(Redis *redis, const char *ca_path, const char *ca_file);
int redisxSetMutualTLS(Redis *redis, const char *cert_file, const char *key_file);
int redisxSetTLSCiphers(Redis *redis, const char *cipher_list);
int redisxSetTLSCipherSuites(Redis *redis, const char *list);
int redisxSetDHCipherParams(Redis *redis, const char *dh_params_file);
int redisxSetTLSServerName(Redis *redis, const char *host);
int redisxSetTLSVerify(Redis *redis, boolean value);

RedisCluster *redisxClusterInit(Redis *node);
Redis *redisxClusterGetShard(RedisCluster *cluster, const char *key);
boolean redisxClusterIsRedirected(const RESP *reply);
boolean redisxClusterMoved(const RESP *reply);
boolean redisxClusterIsMigrating(const RESP *reply);
int redisxClusterConnect(RedisCluster *cluster);
int redisxClusterDisconnect(RedisCluster *cluster);
void redisxClusterDestroy(RedisCluster *cluster);
Redis *redisxClusterGetRedirection(RedisCluster *cluster, const RESP *redirect, boolean refresh);
RESP *redisxClusterAskMigrating(Redis *redis, const char **args, const int *lengths, int n, int *status);

int redisxPing(Redis *redis, const char *message);
enum redisx_protocol redisxGetProtocol(Redis *redis);
XLookupTable *redisxGetInfo(Redis *redis, const char *parameter);
RESP *redisxGetHelloData(Redis *redis);

boolean redisxIsConnected(Redis *redis);
boolean redisxHasPipeline(Redis *redis);

RedisClient *redisxGetClient(Redis *redis, enum redisx_channel channel);
RedisClient *redisxGetLockedConnectedClient(Redis *redis, enum redisx_channel channel);

int redisxAddConnectHook(Redis *redis, void (*setupCall)(Redis *));
int redisxRemoveConnectHook(Redis *redis, void (*setupCall)(Redis *));
void redisxClearConnectHooks(Redis *redis);

int redisxAddDisconnectHook(Redis *redis, void (*cleanupCall)(Redis *));
int redisxRemoveDisconnectHook(Redis *redis, void (*cleanupCall)(Redis *));
void redisxClearDisconnectHooks(Redis *redis);

RESP *redisxRequest(Redis *redis, const char *command, const char *arg1, const char *arg2, const char *arg3, int *status);
RESP *redisxArrayRequest(Redis *redis, const char **args, const int *length, int n, int *status);
RESP *redisxGetAttributes(Redis *redis);
int redisxGetAvailable(RedisClient *cl);
int redisxSetValue(Redis *redis, const char *table, const char *key, const char *value, boolean confirm);
RESP *redisxGetValue(Redis*redis, const char *table, const char *key, int *status);
char *redisxGetStringValue(Redis *redis, const char *table, const char *key, int *len);
RedisEntry *redisxGetTable(Redis *redis, const char *table, int *n);
RedisEntry *redisxScanTable(Redis *redis, const char *table, const char *pattern, int *n);
int redisxMultiSet(Redis *redis, const char *table, const RedisEntry *entries, int n, boolean confirm);
char **redisxGetKeys(Redis *redis, const char *table, int *n);
char **redisxScanKeys(Redis *redis, const char *pattern, int *n);
int redisxSetScanCount(Redis *redis, int count);
int redisxGetScanCount(Redis *redis);
void redisxDestroyEntries(RedisEntry *entries, int count);
void redisxDestroyKeys(char **keys, int count);

int redisxSetPipelineConsumer(Redis *redis, RedisPipelineProcessor f);
int redisxSetPushProcessor(Redis *redis, RedisPushProcessor func, void *arg);

int redisxPublish(Redis *redis, const char *channel, const char *message, int length);
int redisxNotify(Redis *redis, const char *channel, const char *message);
int redisxSubscribe(Redis *redis, const char *channel);
int redisxUnsubscribe(Redis *redis, const char *channel);
int redisxAddSubscriber(Redis *redis, const char *channelStem, RedisSubscriberCall f);
int redisxRemoveSubscribers(Redis *redis, RedisSubscriberCall f);
int redisxClearSubscribers(Redis *redis);
int redisxEndSubscription(Redis *redis);

int redisxStartBlockAsync(RedisClient *cl);
int redisxAbortBlockAsync(RedisClient *cl);
RESP *redisxExecBlockAsync(RedisClient *cl, int *pStatus);
int redisxLoadScript(Redis *redis, const char *script, char **sha1);
RESP *redisxRunScript(Redis *redis, const char *sha1, const char **keys, const char **params, int *status);

int redisxGetTime(Redis *redis, struct timespec *t);
int redisxIsGlobPattern(const char *str);

RESP *redisxCopyOfRESP(const RESP *resp);
int redisxCheckRESP(const RESP *resp, enum resp_type expectedType, int expectedSize);
int redisxCheckDestroyRESP(RESP *resp, enum resp_type, int expectedSize);
void redisxDestroyRESP(RESP *resp);
boolean redisxIsScalarType(const RESP *r);
boolean redisxIsStringType(const RESP *r);
boolean redisxIsArrayType(const RESP *r);
boolean redisxIsMapType(const RESP *r);
boolean redisxHasComponents(const RESP *r);
boolean redisxIsEqualRESP(const RESP *a, const RESP *b);
int redisxSplitText(RESP *resp, char **text);
XField *redisxRESP2XField(const char *name, const RESP *resp);
char *redisxRESP2JSON(const char *name, const RESP *resp);
int redisxPrintRESP(const RESP *resp);
int redisxPrintJSON(const char *name, const RESP *resp);
void redisxPrintDelimited(const RESP *resp, const char *delim, const char *groupPrefix);

RedisMap *redisxGetMapEntry(const RESP *map, const RESP *key);
RedisMap *redisxGetKeywordEntry(const RESP *map, const char *key);

// Locks for async calls
int redisxLockClient(RedisClient *cl);
int redisxLockConnected(RedisClient *cl);
int redisxUnlockClient(RedisClient *cl);

// Asynchronous access routines (use within redisxLockClient()/ redisxUnlockClient() blocks)...
int redisxSendRequestAsync(RedisClient *cl, const char *command, const char *arg1, const char *arg2, const char *arg3);
int redisxSendArrayRequestAsync(RedisClient *cl, const char **args, const int *length, int n);
int redisxClusterAskMigratingAsync(RedisClient *cl, const char **args, const int *lengths, int n);
int redisxSetValueAsync(RedisClient *cl, const char *table, const char *key, const char *value, boolean confirm);
int redisxMultiSetAsync(RedisClient *cl, const char *table, const RedisEntry *entries, int n, boolean confirm);
int redisxGetAvailableAsync(RedisClient *cl);
RESP *redisxReadReplyAsync(RedisClient *cl, int *pStatus);
int redisxClearAttributesAsync(RedisClient *cl);
const RESP *redisxGetAttributesAsync(const RedisClient *cl);
int redisxIgnoreReplyAsync(RedisClient *cl);
int redisxSkipReplyAsync(RedisClient *cl);
int redisxPublishAsync(Redis *redis, const char *channel, const char *data, int length);


// Error generation with stderr message...
int redisxError(const char *func, int errorCode);
const char* redisxErrorDescription(int code);

// The following is not available on prior to the POSIX.1-2001 standard
#if FNMATCH || _POSIX_C_SOURCE >= 200112L
int redisxDeleteEntries(Redis *redis, const char *pattern);
#endif

#endif /* REDISX_H_ */
