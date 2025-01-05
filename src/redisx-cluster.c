/**
 * @file
 *
 * @date Created  on Jan 2, 2025
 * @author Attila Kovacs
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>

#include "redisx-priv.h"

/// \cond PRIVATE

#define HASH_MASK   (16384 - 1);

/**
 * A shard in a Redis cluster, serving a specific range of hashes.
 *
 */
typedef struct {
  Redis **redis;                ///< The connection instance to the server.
  int n_servers;                ///< Number of servers (master + replicas)
  int start;                    ///< Shard hash range start (inclusive)
  int end;                      ///< Shard hash range end (inclusive)
} RedisShard;

/**
 * Private cluster configuration data, not exposed to users
 *
 */
typedef struct {
  pthread_mutex_t mutex;        ///< mutex for exclusive access to the cluster configuration
  int n_shards;                 ///< the number of shards in the cluster
  RedisShard *shard;            ///< array containing information on each shard
  boolean usePipeline;          ///< Whether shards should have dedicated pipeline connections
  boolean reconfiguring;        ///< Whether the cluster is currently being reconfigured.
} ClusterPrivate;

/// \endcond

/**
 * ZMODEM CRC-16 lookup table
 *
 * https://crccalc.com/?crc=0&method=CRC-16/XMODEM&datatype=ascii&outtype=hex
 *
 */
static const uint16_t crc_tab[] = { //
        0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50a5, 0x60c6, 0x70e7, //
        0x8108, 0x9129, 0xa14a, 0xb16b, 0xc18c, 0xd1ad, 0xe1ce, 0xf1ef, //
        0x1231, 0x0210, 0x3273, 0x2252, 0x52b5, 0x4294, 0x72f7, 0x62d6, //
        0x9339, 0x8318, 0xb37b, 0xa35a, 0xd3bd, 0xc39c, 0xf3ff, 0xe3de, //
        0x2462, 0x3443, 0x0420, 0x1401, 0x64e6, 0x74c7, 0x44a4, 0x5485, //
        0xa56a, 0xb54b, 0x8528, 0x9509, 0xe5ee, 0xf5cf, 0xc5ac, 0xd58d, //
        0x3653, 0x2672, 0x1611, 0x0630, 0x76d7, 0x66f6, 0x5695, 0x46b4, //
        0xb75b, 0xa77a, 0x9719, 0x8738, 0xf7df, 0xe7fe, 0xd79d, 0xc7bc, //
        0x48c4, 0x58e5, 0x6886, 0x78a7, 0x0840, 0x1861, 0x2802, 0x3823, //
        0xc9cc, 0xd9ed, 0xe98e, 0xf9af, 0x8948, 0x9969, 0xa90a, 0xb92b, //
        0x5af5, 0x4ad4, 0x7ab7, 0x6a96, 0x1a71, 0x0a50, 0x3a33, 0x2a12, //
        0xdbfd, 0xcbdc, 0xfbbf, 0xeb9e, 0x9b79, 0x8b58, 0xbb3b, 0xab1a, //
        0x6ca6, 0x7c87, 0x4ce4, 0x5cc5, 0x2c22, 0x3c03, 0x0c60, 0x1c41, //
        0xedae, 0xfd8f, 0xcdec, 0xddcd, 0xad2a, 0xbd0b, 0x8d68, 0x9d49, //
        0x7e97, 0x6eb6, 0x5ed5, 0x4ef4, 0x3e13, 0x2e32, 0x1e51, 0x0e70, //
        0xff9f, 0xefbe, 0xdfdd, 0xcffc, 0xbf1b, 0xaf3a, 0x9f59, 0x8f78, //
        0x9188, 0x81a9, 0xb1ca, 0xa1eb, 0xd10c, 0xc12d, 0xf14e, 0xe16f, //
        0x1080, 0x00a1, 0x30c2, 0x20e3, 0x5004, 0x4025, 0x7046, 0x6067, //
        0x83b9, 0x9398, 0xa3fb, 0xb3da, 0xc33d, 0xd31c, 0xe37f, 0xf35e, //
        0x02b1, 0x1290, 0x22f3, 0x32d2, 0x4235, 0x5214, 0x6277, 0x7256, //
        0xb5ea, 0xa5cb, 0x95a8, 0x8589, 0xf56e, 0xe54f, 0xd52c, 0xc50d, //
        0x34e2, 0x24c3, 0x14a0, 0x0481, 0x7466, 0x6447, 0x5424, 0x4405, //
        0xa7db, 0xb7fa, 0x8799, 0x97b8, 0xe75f, 0xf77e, 0xc71d, 0xd73c, //
        0x26d3, 0x36f2, 0x0691, 0x16b0, 0x6657, 0x7676, 0x4615, 0x5634, //
        0xd94c, 0xc96d, 0xf90e, 0xe92f, 0x99c8, 0x89e9, 0xb98a, 0xa9ab, //
        0x5844, 0x4865, 0x7806, 0x6827, 0x18c0, 0x08e1, 0x3882, 0x28a3, //
        0xcb7d, 0xdb5c, 0xeb3f, 0xfb1e, 0x8bf9, 0x9bd8, 0xabbb, 0xbb9a, //
        0x4a75, 0x5a54, 0x6a37, 0x7a16, 0x0af1, 0x1ad0, 0x2ab3, 0x3a92, //
        0xfd2e, 0xed0f, 0xdd6c, 0xcd4d, 0xbdaa, 0xad8b, 0x9de8, 0x8dc9, //
        0x7c26, 0x6c07, 0x5c64, 0x4c45, 0x3ca2, 0x2c83, 0x1ce0, 0x0cc1, //
        0xef1f, 0xff3e, 0xcf5d, 0xdf7c, 0xaf9b, 0xbfba, 0x8fd9, 0x9ff8, //
        0x6e17, 0x7e36, 0x4e55, 0x5e74, 0x2e93, 0x3eb2, 0x0ed1, 0x1ef0, //
};

static uint16_t crc16(const uint8_t *buf, size_t len) {
  uint16_t crc = 0;
  while (len-- > 0) crc = (crc << 8) ^ crc_tab[((crc >> 8) ^ *(buf++)) & 0x00FF];
  return crc;
}

/// \cond PRIVATE
/**
 * Returns the hash value using the same ZMODEM / ACORN CRC-16 algorithm that Redis
 * uses internally.
 *
 * @param key
 * @return
 */
uint16_t rCalcHash(const char *key) {
  const char *from = strchr(key, '{');

  if(from) {
    const char *to = strchr(++from, '}');
    if(to > from) return crc16((uint8_t *) from, to - from) & HASH_MASK;
  }

  return crc16((uint8_t *) key, strlen(key)) & HASH_MASK;
}
/// \endcond

static void rDiscardShardsAsync(RedisShard *shards, int n_shards) {
  int i;

  if(!shards) return;


  for(i = 0; i < n_shards; i++) {
    RedisShard *s = &shards[i];
    int m;

    for(m = 0; m < s->n_servers; m++) {
      Redis *r = s->redis[m];
      redisxDisconnect(r);
      redisxDestroy(r);
    }
    free(shards);
  }
}

/**
 * Returns the current cluster configuration obtained from the specified node
 *
 * @param cluster           Pointer to cluster to set as the parent to the discovered shards
 * @param redis             The node to use for discovery. It need not be in a connected state.
 * @param[out] n_shards     Pointer to integer in which to return the number of shards discovered
 *                          or else an error code &lt;0.
 * @return                  Array containing the discovered shards or NULL if there was an error.
 */
static RedisShard *rClusterDiscoverAsync(const RedisCluster *cluster, Redis *redis, int *n_shards) {
  static const char *fn = "rClusterDiscoverAsync";

  RESP *reply;
  RedisShard *shards = NULL;
  int isConnected;

  isConnected = redisxIsConnected(redis);

  if(!isConnected) {
    *n_shards = redisxConnect(redis, FALSE);
    if(*n_shards) return x_trace_null(fn, NULL);
  }

  reply = redisxRequest(redis, "CLUSTER", "SLOTS", NULL, NULL, n_shards);
  if(*n_shards) {
    redisxDestroyRESP(reply);
    return x_trace_null(fn, NULL);
  }

  if(redisxCheckRESP(reply, RESP_ARRAY, 0) == X_SUCCESS) {
    RESP **array = (RESP **) reply->value;
    int k;

    if(reply->n > 0) {
      shards = (RedisShard *) calloc(reply->n, sizeof(RedisShard));
      if(!shards) *n_shards = x_error(X_FAILURE, errno, fn, "alloc error (%d shards)", reply->n);
      else *n_shards = reply->n;
    }

    for(k = 0; k < reply->n; k++) {
      RESP **desc = (RESP **) array[k]->value;
      RedisShard *s = &shards[k];
      int m;

      s->start = desc[0]->n;
      s->end = desc[1]->n;
      s->n_servers = array[k]->n - 2;
      s->redis = (Redis **) calloc(s->n_servers, sizeof(Redis *));

      if(!s->redis) {
        s->n_servers = 0;
        x_error(0, errno, fn, "alloc error (%d servers)\n", s->n_servers);
        rDiscardShardsAsync(shards, *n_shards);
        *n_shards = X_FAILURE;
        return NULL;
      }

      for(m = 0; m < s->n_servers; s++) {
        RESP **node = (RESP **) desc[2]->value;
        RedisPrivate *p = (RedisPrivate *) s->redis[m]->priv;

        s->redis[m] = redisxInit((char *) node[0]->value);
        redisxSetPort(s->redis[m], node[1]->n);
        p->cluster = (RedisCluster *) cluster;

        rCopyConfig(&((RedisPrivate *) redis->priv)->config, s->redis[m]);
      }
    }

    redisxDestroyRESP(reply);
  }

  if(!isConnected) redisxDisconnect(redis);

  if(*n_shards < 0) x_trace(fn, NULL, *n_shards);

  return shards;
}

/// \cond PRIVATE

/**
 * Thread to reload a changed cluster configuration. It should be called with the cluster mutex
 * already locked. The mutex will be released once the reconfiguration is complete.
 *
 * @param pCluster
 */
void *ClusterRefreshThread(void *pCluster) {
  RedisCluster *cluster = (RedisCluster *) pCluster;
  ClusterPrivate *p = (ClusterPrivate *) cluster->priv;

  for(int i = 0; i < p->n_shards; i++) {
    const RedisShard *s = &p->shard[i];
    int m;

    for(m = 0; m < s->n_servers; m++) {
      int n_shards = 0;
      RedisShard *shard = rClusterDiscoverAsync(cluster, s->redis[m], &n_shards);

      if(n_shards >= 0) {
        rDiscardShardsAsync(p->shard, p->n_shards);
        p->shard = shard;
        p->n_shards = n_shards;
        break;
      }
    }
  }

  p->reconfiguring = FALSE;

  pthread_mutex_unlock(&p->mutex);

  return NULL;
}

/**
 * Initiates the reloading of the cluster configuration in a separate background thread.
 *
 * @param cluster   Pointer to a Redis cluster configuration
 * @return          X_SUCCESS (0) if the reconfiguration thread was successfully launched
 *                  or else an error code &lt;0 (with errno also indicating the type of
 *                  error).
 */
int rClusterRefresh(RedisCluster *cluster) {
  static const char *fn = "rClusterRefresh";

  ClusterPrivate *p;
  pthread_t tid;

  if(!cluster) return x_error(X_NULL, EINVAL, fn, "cluster is NULL");

  p = (ClusterPrivate *) cluster->priv;
  if(!p) return x_error(X_NO_INIT, ENXIO, fn, "cluster is not initialized");

  // Return immediately if the cluster is being reconfigured at present.
  // This is important so we may process all pending MOVED responses while
  // the reconfiguration takes place.
  if(p->reconfiguring) return X_SUCCESS;

  pthread_mutex_lock(&p->mutex);

  // After obtaining the exclusive lock, check again that no other thread has
  // begun reconfiguration.
  if(p->reconfiguring) {
    pthread_mutex_unlock(&p->mutex);
    return X_SUCCESS;
  }

  // We are now officially in charge of reconfiguring the cluster...
  p->reconfiguring = TRUE;

  errno = 0;
  if(pthread_create(&tid, NULL, ClusterRefreshThread, (void *) cluster) != 0) {
    pthread_mutex_unlock(&p->mutex);
    return x_error(X_FAILURE, errno, fn, "failed to start refresher thread");
  }

  return X_SUCCESS;
}

/// \endcond

/**
 * Returns the Redis server in a cluster which is to be used for queries relating to the
 * specified Redis keyword. In Redis cluster configurations, the database is distributed in
 * a way that each cluster node serves only a subset of the Redis keys. Thus, this function
 * allows to identify the node that serves a given key. The function supports Redish hashtags
 * according to the specification.
 *
 * @param cluster     Pointer to a Redis cluster configuration
 * @param key         The Redis keyword of interest. It may use hashtags (i.e., if the keyword
 *                    contains a segment enclosed in {} brackets, then the hash will be
 *                    calculated on the bracketed segment only. E.g. `{user:1000}.name` and
 *                    `{user:1000}.address` will both return the same hash for `user:1000` only.
 * @return            A connected Redis server (cluster shard), which can be used for
 *                    queries on the given keyword, or NULL if either input pointer is NULL
 *                    (errno = EINVAL), or the cluster has not been initialized (errno = ENXIO),
 *                    or if no node could be connected to serve queries for the given key
 *                    (errno = EAGAIN).
 *
 * @sa redisxClusterInit()
 */
Redis *redisxClusterGetShard(RedisCluster *cluster, const char *key) {
  static const char *fn = "redisxClusterGetShard";

  ClusterPrivate *p;
  uint16_t hash;
  int i;

  if(!cluster) {
    x_error(X_NULL, EINVAL, fn, "cluster is NULL");
    return NULL;
  }

  if(!key) {
    x_error(X_NAME_INVALID, EINVAL, fn, "key is NULL");
    return NULL;
  }

  if(!key[0]) {
    x_error(X_NAME_INVALID, EINVAL, fn, "key is empty");
    return NULL;
  }

  p = (ClusterPrivate *) cluster->priv;
  if(!p) {
    x_error(X_NO_INIT, ENXIO, fn, "cluster is not initialized");
    return NULL;
  }

  hash = rCalcHash(key);

  pthread_mutex_lock(&p->mutex);

  for(i = 0; i < p->n_shards; i++) {
    const RedisShard *s = &p->shard[i];
    if(hash >= s->start && hash <= s->end) {
      int m;

      for(m = 0; m < s->n_servers; m++) {
        Redis *r = s->redis[m];
        if(!redisxIsConnected(r)) if(redisxConnect(r, p->usePipeline) != X_SUCCESS) continue;
        pthread_mutex_unlock(&p->mutex);
        return r;
      }
    }
  }

  pthread_mutex_unlock(&p->mutex);

  x_error(0, EAGAIN, fn, "no server found for hash %hu", hash);
  return NULL;
}

/**
 * Initializes a Redis cluster configuration using a known cluster node. The call will connect to
 * the specified node (if not already connected), and will query the cluster configuration from it.
 * On return the input node's connection state remains what it was prior to the call.
 *
 * The caller may try multiple nodes from a list of known cluster nodes, until a valid (non-NULL)
 * configuration is returned.
 *
 * The cluster will inherit configuration from the node, including user authentication, socket
 * configuration, connection / disconnection hooks, and asynchronous processing functions. Thus
 * you may configure the node as usual prior to this call, knowing that the nodes in the cluster
 * will be configured the same way also.
 *
 * @param node      A known cluster node (connected or not). It's configuration will be used
 *                  for all cluster nodes discovered also.
 * @return          The Redis cluster configuration obtained from the node, or else NULL if
 *                  there was an error (errno may indicate the type of error).
 *
 * @sa redisxClusterGetShard()
 * @sa redisxClusterDestroy()
 * @sa redisxClusterConnect()
 */
RedisCluster *redisxClusterInit(Redis *node) {
  static const char *fn = "redisxClusterInit";

  RedisCluster *cluster;
  ClusterPrivate *p;
  boolean isConnected;

  isConnected = redisxIsConnected(node);
  if(!isConnected) if(redisxConnect(node, FALSE) != X_SUCCESS) return x_trace_null(fn, NULL);

  if(!rConfigLock(node)) {
    if(!isConnected) redisxDisconnect(node);
    return x_trace_null(fn, NULL);
  }

  cluster = (RedisCluster *) calloc(1, sizeof(RedisCluster));
  x_check_alloc(cluster);

  p = (ClusterPrivate *) calloc(1, sizeof(ClusterPrivate));
  x_check_alloc(p);

  cluster->priv = p;
  p->usePipeline = redisxHasPipeline(node);

  pthread_mutex_init(&p->mutex, NULL);

  p->shard = rClusterDiscoverAsync(cluster, node, &p->n_shards);
  if(p->n_shards <= 0) {
    redisxClusterDestroy(cluster);
    return x_trace_null(fn, NULL);
  }

  rConfigUnlock(node);

  if(!isConnected) redisxDisconnect(node);

  return cluster;
}

/**
 * Destroys a Redis cluster configuration, freeing up all resources used, but not before
 * disconnecting from all shards that may be in a connected state.
 *
 * @param cluster     Pointer to a Redis cluster configuration.
 *
 * @sa redisxClusterInit()
 */
void redisxClusterDestroy(RedisCluster *cluster) {
  ClusterPrivate *p;

  if(!cluster) return;

  redisxClusterDisconnect(cluster);

  p = (ClusterPrivate *) cluster->priv;
  if(p) {
    pthread_mutex_lock(&p->mutex);

    rDiscardShardsAsync(p->shard, p->n_shards);

    pthread_mutex_unlock(&p->mutex);
    pthread_mutex_destroy(&p->mutex);

    free(p);
  }
  free(cluster);
}

/**
 * Connects all shards of a Redis cluster. Shards normally get connected on demand. Thus,
 * this function is only necessary if the user wants to ensure that all shards are connected
 * before using the cluster.
 *
 * Note, that if the cluster configuration changes while connected, the automatically reconfigured
 * cluster will not automatically reconnect to the new shards during the reconfiguration. However,
 * the new shards will still connect on demand when accessed via redisClusterGetShard().
 *
 * @param cluster   Pointer to a Redis cluster configuration
 * @return          X_SUCCESS (0) if successful, or else a RedisX error code &lt;0 (errno
 *                  will also indicate the type of error).
 *
 * @sa redisxClusterInit()
 * @sa redisxClusterConnect()
 * @sa redisxClusterGetShard()
 */
int redisxClusterConnect(RedisCluster *cluster) {
  static const char *fn = "redisxConnectCluster";

  ClusterPrivate *p;
  int i, status = X_SUCCESS;

  if(!cluster) return x_error(X_NULL, EINVAL, fn, "cluster is NULL");

  p = (ClusterPrivate *) cluster->priv;
  if(!p) return x_error(X_NO_INIT, ENXIO, fn, "cluster is not initialized");

  pthread_mutex_lock(&p->mutex);

#if WITH_OMP
#  pragma omp parallel for
#endif
  for(i = 0; i < p->n_shards; i++) {
    int m = p->shard[i].n_servers;
    while(--m >= 0) {
      int s = redisxConnect(p->shard[i].redis[m], p->usePipeline);
      if(s) {
        if(!status) status = s;
        x_trace(fn, NULL, status);
      }
    }
  }

  pthread_mutex_unlock(&p->mutex);

  return status;
}

/**
 * Disconnects from all shards of a Redis cluster. Note, that a cluster can still be used even
 * after it is disconnected, since each call to redisxClusterGetShard() will automatically
 * reconnect the requested shard on demand as needed.
 *
 * @param cluster   Pointer to a Redis cluster configuration
 * @return          X_SUCCESS (0) if successful, or else a RedisX error code &lt;0 (errno
 *                  will also indicate the type of error).
 *
 * @sa redisxClusterInit()
 * @sa redisxClusterConnect()
 */
int redisxClusterDisconnect(RedisCluster *cluster) {
  static const char *fn = "redisxDisconnectCluster";

  ClusterPrivate *p;
  int i;

  if(!cluster) return x_error(X_NULL, EINVAL, fn, "cluster is NULL");

  p = (ClusterPrivate *) cluster->priv;
  if(!p) return x_error(X_NO_INIT, ENXIO, fn, "cluster is not initialized");

  pthread_mutex_lock(&p->mutex);

#if WITH_OMP
#  pragma omp parallel for
#endif
  for(i = 0; i < p->n_shards; i++) {
    int m = p->shard[i].n_servers;
    while(--m >= 0) redisxDisconnect(p->shard[i].redis[m]);
  }

  pthread_mutex_unlock(&p->mutex);

  return X_SUCCESS;
}

