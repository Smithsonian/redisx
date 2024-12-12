/**
 * @file
 *
 * @date Created  on Aug 26, 2024
 * @author Attila Kovacs
 *
 *   Table access functions for the RedisX library. These functions can be used both for Redis hash tables
 *   and for global key/value data in a Redis database.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#if !(__Lynx__ && __powerpc__)
#  include <fnmatch.h>
#endif

#include "redisx-priv.h"


#define SCAN_INITIAL_STORE_CAPACITY   256   ///< Number of Redis keys to allocate initially when using SCAN to get list of keywords

/// \cond PRIVATE
#define SCAN_INITIAL_CURSOR         "0"     ///< Initial cursor value for SCAN command.
/// \endcond

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
 * @sa redisxScanTable()
 * @sa redisxDEstroyEntries()
 */
RedisEntry *redisxGetTable(Redis *redis, const char *table, int *n) {
  static const char *fn = "redisxGetTable";
  RedisEntry *entries = NULL;
  RESP *reply;

  if(n == NULL) {
    x_error(0, EINVAL, fn, "parameter 'n' is NULL");
    return NULL;
  }

  if(table == NULL) {
    *n = x_error(X_GROUP_INVALID, EINVAL, fn, "table parameter is NULL");
    return NULL;
  }

  if(!table[0]) {
    *n = x_error(X_GROUP_INVALID, EINVAL, fn, "table parameter is empty");
    return NULL;
  }

  reply = redisxRequest(redis, "HGETALL", table, NULL, NULL, n);
  if(*n) return x_trace_null(fn, NULL);

  // Cast RESP2 array respone to RESP3 map also...
  if(reply && reply->type == RESP_ARRAY) {
    reply->type = RESP3_MAP;
    reply->n /= 2;
  }

  *n = redisxCheckDestroyRESP(reply, RESP3_MAP, 0);
  if(*n) {
    return x_trace_null(fn, NULL);
  }

  *n = reply->n;

  if(*n > 0) {
    RedisMapEntry *dict = (RedisMapEntry *) reply->value;
    entries = (RedisEntry *) calloc(*n, sizeof(RedisEntry));

    if(entries == NULL) {
      fprintf(stderr, "WARNING! Redis-X : alloc %d table entries: %s\n", *n, strerror(errno));
    }
    else {
      int i;

      for(i = 0; i < reply->n; i += 2) {
        RedisEntry *e = &entries[i];
        RedisMapEntry *component = &dict[i];
        e->key = component->key->value;
        e->value = component->value->value;

        // Dereference the key/value so we don't destroy them with the reply.
        component->key->value = NULL;
        component->value->value = NULL;
      }
    }
  }

  // Free the reply container, but not the strings inside, which are returned.
  redisxDestroyRESP(reply);
  return entries;
}

/**
 * Sets a global or hashtable value on Redis.
 *
 * \param redis         Pointer to a Redis instance.
 * \param table         Hash table identifier or NULL if setting a global value.
 * \param key           Redis field name (i.e. variable name).
 * \param value         A proper 0-terminated string value to store.
 * \param confirm       Whether we should get a confirmation from the server (requires a round-trip).
 *
 * \return      X_SUCCESS if the variable was succesfully set, or:
 *                  X_NO_INIT
 *                  X_NAME_INVALID
 *                  X_NULL
 *                  X_NO_SERVICE
 *                  X_FAILURE
 *
 */
int redisxSetValue(Redis *redis, const char *table, const char *key, const char *value, boolean confirm) {
  static const char *fn = "redisxSetValue";

  int status = X_SUCCESS;

  prop_error(fn, redisxCheckValid(redis));
  prop_error(fn, redisxLockConnected(redis->interactive));

  status = redisxSetValueAsync(redis->interactive, table, key, value, confirm);
  redisxUnlockClient(redis->interactive);

  prop_error(fn, status);
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
 * \return          X_SUCCESS (0)   if successful, or
 *                  X_NULL          if the client or value is NULL
 *                  X_NAME_INVALID  if key is invalid,
 *                  or an error (&lt;0) returned by redisxSendRequestAsync().
 */
int redisxSetValueAsync(RedisClient *cl, const char *table, const char *key, const char *value, boolean confirm) {
  static const char *fn = "redisxSetValueAsync";
  int status;

  if(cl == NULL) return x_error(X_NULL, EINVAL, fn, "client is NULL");
  if(key == NULL) return x_error(X_NAME_INVALID, EINVAL, fn, "'key' parameter is NULL");
  if(!key[0]) return x_error(X_NAME_INVALID, EINVAL, fn, "'key' parameter is empty");
  if(value == NULL) return x_error(X_NULL, EINVAL, fn, "'value' parameter is NULL");

  // No need for response. Just set value.
  if(!confirm) {
    prop_error(fn, redisxSkipReplyAsync(cl));
  }

  if(table == NULL) status = redisxSendRequestAsync(cl, "SET", key, value, NULL);
  else status = redisxSendRequestAsync(cl, "HSET", table, key, value);

  prop_error(fn, status);

  xvprintf("Redis-X> set %s = %s on %s\n", key, value, table);

  if(confirm) {
    RESP *reply = redisxReadReplyAsync(cl);
    status = redisxCheckRESP(reply, RESP_INT, 0);
    redisxDestroyRESP(reply);
    prop_error(fn, status);
  }

  return X_SUCCESS;
}

/**
 * Retrieve a variable from Redis (as an undigested RESP), through the interactive connection. This is not the highest
 * throughput mode (that would be sending asynchronous pipeline request, and then asynchronously collecting the
 * results such as with redisxSendRequestAsync() / redisxReadReplyAsync()), because it requires separate network
 * roundtrips for each and every request. But, it is simple and perfectly good method when one needs to retrieve only
 * a few (&lt;1000) variables per second...
 *
 * The call effectively implements a Redis GET (if the table argument is NULL) or HGET call.
 *
 * \param[in]  redis     Pointer to a Redis instance.
 * \param[in]  table     Hashtable from which to retrieve a value or NULL if to use the global table.
 * \param[in]  key       Field name (i.e. variable name).
 * \param[out] status    (optional) pointer to the return error status, which is either X_SUCCESS on success or else
 *                       the error code set by redisxArrayRequest(). It may be NULL if not required.
 *
 * \return          A freshly allocated RESP containing the Redis response, or NULL if no valid
 *                  response could be obtained. Values are returned as RESP_BULK_STRING (count = 1),
 *                  or else type RESP_ERROR or RESP_NULL if Redis responded with an error or null, respectively.
 *
 * \sa redisxGetStringValue()
 */
RESP *redisxGetValue(Redis *redis, const char *table, const char *key, int *status) {
  static const char *fn = "redisxGetValue";

  RESP *reply;
  int s = X_SUCCESS;

  if(table && !table[0]) {
    x_error(X_GROUP_INVALID, EINVAL, fn, "'table' parameter is empty");
    if(status) *status = X_GROUP_INVALID;
    return NULL;
  }

  if(key == NULL) {
    x_error(X_NAME_INVALID, EINVAL, fn, "'key' parameter is NULL");
    if(status) *status = X_NAME_INVALID;
    return NULL;
  }

  if(!key[0]) {
    x_error(X_NAME_INVALID, EINVAL, fn, "'key' parameter is empty");
    if(status) *status = X_NAME_INVALID;
    return NULL;
  }

  if(table == NULL) reply = redisxRequest(redis, "GET", key, NULL, NULL, &s);
  else reply = redisxRequest(redis, "HGET", table, key, NULL, &s);

  if(status) *status = s;
  if(s) x_trace_null(fn, NULL);

  return reply;
}

/**
 * Retrieve a variable from Redis as a string (or byte array), through the interactive connection. This is not the
 * highest throughput mode (that would be sending asynchronous pipeline request, and then asynchronously collecting
 * the results such as with redisxSendRequestAsync() / redisxReadReplyAsync()), because it requires separate network
 * roundtrips for each and every request. But, it is simple and perfectly good method when one needs to retrieve only
 * a few (&lt;1000) variables per second...
 *
 * The call effectively implements a Redis GET (if the table argument is NULL) or HGET call.
 *
 * \param[in]  redis     Pointer to a Redis instance.
 * \param[in]  table     Hashtable from which to retrieve a value or NULL if to use the global table.
 * \param[in]  key       Field name (i.e. variable name).
 * \param[out] len       (optional) pointer in which to return the length (&gt;=0) of the value or else
 *                       an error code (&lt;0) defined in xchange.h / redisx.h
 *
 * \return          A freshly allocated RESP array containing the Redis response, or NULL if no valid
 *                  response could be obtained.
 *
 * \sa redisxGetValue()
 */
char *redisxGetStringValue(Redis *redis, const char *table, const char *key, int *len) {
  RESP *reply;
  char *str = NULL;
  int status;

  reply = redisxGetValue(redis, table, key, len);
  status = redisxCheckRESP(reply, RESP_BULK_STRING, 0);

  if(status == X_SUCCESS) {
    str = (char *) reply->value;
    reply->value = NULL;
    if(len) *len = reply->n;
  }
  else if(len) *len = status;

  redisxDestroyRESP(reply);

  if(status) x_trace_null("redisxGetStringValue", NULL);

  return str;
}

/**
 * Sets multiple key/value pairs in a given hash table. This function should be called with exclusive
 * access to the client.
 *
 * \param cl            A Redis client to which we have exclusive access.
 * \param table         Hashtable from which to retrieve a value.
 * \param entries       Pointer to an array of key/value pairs.
 * \param n             Number of entries.
 * \param confirm       Whether we should get a confirmation from the server (requires a round-trip).
 *
 * \return              X_SUCCESS (0) on success or an error code (&lt;0) from redisx.h / xchange.h.
 *
 * @sa redisxMultiSet()
 * @sa redisxLockClient()
 */
int redisxMultiSetAsync(RedisClient *cl, const char *table, const RedisEntry *entries, int n, boolean confirm) {
  static const char *fn = "redisxMultiSetAsync";
  int i, *L, N, status = X_SUCCESS;
  char **req;

  if(cl == NULL) return  x_error(X_NULL, EINVAL, fn, "redis is NULL");
  if(table == NULL) return  x_error(X_GROUP_INVALID, EINVAL, fn, "table parameter is NULL");
  if(!table[0]) return  x_error(X_GROUP_INVALID, EINVAL, fn, "table parameter is empty");
  if(entries == NULL) return  x_error(X_NULL, EINVAL, fn, "'entries' parameter is NULL");
  if(n < 1) return  x_error(X_SIZE_INVALID, EINVAL, fn, "invalid size: %d", n);

  N = (n<<1)+2;

  req = (char **) malloc(N * sizeof(char *));
  if(!req) {
    fprintf(stderr, "WARNING! Redis-X : alloc %d request components: %s\n", N, strerror(errno));
    return x_trace(fn, NULL, X_FAILURE);
  }

  L = (int *) calloc(N, sizeof(int));
  if(!L) {
    fprintf(stderr, "WARNING! Redis-X : alloc %d request sizes: %s\n", N, strerror(errno));
    free(req);
    return x_trace(fn, NULL, X_FAILURE);
  }

  req[0] = "HMSET"; // TODO, as of Redis 4.0.0, just use HSET...
  req[1] = (char *) table;

  for(i=0; i<n; i++) {
    int m = 2 + (i<<1);
    req[m] = (char *) entries[i].key;
    req[m+1] = (char *) entries[i].value;
    L[m+1] = entries[i].length;
  }

  if(!confirm) status = redisxSkipReplyAsync(cl);
  if(!status) status = redisxSendArrayRequestAsync(cl, req, L, N);

  free(req);
  free(L);

  prop_error(fn, status);

  return X_SUCCESS;
}

/**
 * Sets multiple key/value pairs in a given hash table.
 *
 * \param redis         Pointer to a Redis instance.
 * \param table         Hashtable from which to retrieve a value.
 * \param entries       Pointer to an array of key/value pairs.
 * \param n             Number of entries.
 * \param confirm       Whether we should get a confirmation from the server (requires a round-trip).
 *
 * \return              X_SUCCESS (0) on success or an error code (&lt;0) from redisx.h / xchange.h.
 *
 */
int redisxMultiSet(Redis *redis, const char *table, const RedisEntry *entries, int n, boolean confirm) {
  static const char *fn = "redisxMultiSet";
  int status;

  prop_error(fn, redisxCheckValid(redis));

  if(table == NULL) return x_error(X_GROUP_INVALID, EINVAL, fn, "table parameter is NULL");
  if(!table[0]) return x_error(X_GROUP_INVALID, EINVAL, fn, "table parameter is empty");
  if(entries == NULL) return x_error(X_NULL, EINVAL, fn, "'entries' parameter is NULL");
  if(n < 1) return x_error(X_SIZE_INVALID, EINVAL, fn, "invalid size: %d", n);

  prop_error(fn, redisxLockConnected(redis->interactive));
  status = redisxMultiSetAsync(redis->interactive, table, entries, n, confirm);
  if(status == X_SUCCESS && confirm) {
    RESP *reply = redisxReadReplyAsync(redis->interactive);
    status = redisxCheckRESP(reply, RESP_SIMPLE_STRING, 0);
    if(!status) if(strcmp(reply->value, "OK")) status = REDIS_ERROR;
    redisxDestroyRESP(reply);
  }
  redisxUnlockClient(redis->interactive);

  prop_error(fn, status);

  return X_SUCCESS;
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
 * \return               An array with pointers to key names from this table or NULL if there was an error
 *                       (see parameter n for an error status from redisx.h / xchange.h).
 *
 * @sa redisxScanKeys()
 * @sa redisxDestroyKeys()
 */
char **redisxGetKeys(Redis *redis, const char *table, int *n) {
  static const char *fn = "redisxGetKeys";
  RESP *reply;
  char **names = NULL;

  if(n == NULL) {
    x_error(X_NULL, EINVAL, fn, "parameter 'n' is NULL");
    return NULL;
  }

  if(table && !table[0]) {
    *n = x_error(X_NULL, EINVAL, fn, "'table' parameter is empty");
    return NULL;
  }

  reply = redisxRequest(redis, table ? "HKEYS" : "KEYS", table ? table : "*", NULL, NULL, n);
  if(*n) return x_trace_null(fn, NULL);

  *n = redisxCheckDestroyRESP(reply, RESP_ARRAY, 0);
  if(*n) return x_trace_null(fn, NULL);

  *n = reply->n;
  if(reply->n > 0) {
    names = (char **) calloc(reply->n, sizeof(char *));

    if(names == NULL) fprintf(stderr, "WARNING! Redis-X : alloc pointers for %d keys: %s\n", reply->n, strerror(errno));
    else {
      int i;
      for(i = 0; i < reply->n; i++) {
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
int redisxSetScanCount(Redis *redis, int count) {
  RedisPrivate *p;

  prop_error("redisxSetScanCount", rConfigLock(redis));
  p = (RedisPrivate *) redis->priv;
  p->scanCount = count;
  rConfigUnlock(redis);

  return X_SUCCESS;
}

/**
 * Returns the COUNT parameter currently set to be used with Redis SCAN-type commands
 *
 * @param redis     Pointer to a Redis instance.
 * @return          The current COUNT to use for SCAN-type commands, or &lt;0 in case
 *                  of an error.
 *
 * @sa redisxGetScanCount()
 * @sa redisxScanKeys()
 * @sa redisxScanTable()
 */
int redisxGetScanCount(Redis *redis) {
  const RedisPrivate *p;
  int count;

  prop_error("redisxGetScanCount", rConfigLock(redis));
  p = (RedisPrivate *) redis->priv;
  count = p->scanCount;
  rConfigUnlock(redis);

  return count;
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
 * @sa redisxDestroyKeys()
 */
char **redisxScanKeys(Redis *redis, const char *pattern, int *n, int *status) {
  static const char *fn = "redisxScanKeys";

  RESP *reply = NULL;
  char *cmd[6] = {NULL};
  char **pCursor;
  char **names = NULL;
  char countArg[20];
  int capacity = SCAN_INITIAL_STORE_CAPACITY;
  int args = 0, i, j;

  if(n == NULL) {
    x_error(X_NULL, EINVAL, fn, "parameter 'n' is NULL");
    if(status) *status = X_NULL;
    return NULL;
  }

  if(status == NULL) {
    x_error(0, EINVAL, fn, "'status' parameter is NULL");
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

  // Clean up.
  redisxDestroyRESP(reply);
  free(*pCursor);

  // Check for errors
  if(*status) x_trace(fn, NULL, *status);

  if(!names) return NULL;

  // Sort alphabetically.
  qsort(names, *n, sizeof(char *), compare_strings);

  // Remove duplicates
  for(i = *n; --i > 0; ) if(!strcmp(names[i], names[i-1])) {
    free(names[i]);
    names[i] = NULL;
  }

  // Compact...
  for(i = 0, j = 0; i < *n; i++) if(names[i]) {
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
 * @sa redisxDestroyEntries()
 */
RedisEntry *redisxScanTable(Redis *redis, const char *table, const char *pattern, int *n, int *status) {
  static const char *fn = "redisxScanTable";

  RESP *reply = NULL;
  RedisEntry *entries = NULL;
  char *cmd[7] = {NULL}, countArg[20];
  char **pCursor;
  int capacity = SCAN_INITIAL_STORE_CAPACITY;
  int args= 0, i, j;

  if(n == NULL) {
    x_error(X_NULL, EINVAL, fn, "parameter 'n' is NULL");
    if(status) *status = X_NULL;
    return NULL;
  }

  if(status == NULL) {
    x_error(0, EINVAL, fn, "'status' parameter is NULL");
    return NULL;
  }

  if(table == NULL) {
    x_error(X_GROUP_INVALID, EINVAL, fn, "'table' parameter is NULL");
    *status = X_NULL;
    return NULL;
  }

  if(!table[0]) {
    x_error(X_GROUP_INVALID, EINVAL, fn, "'table' parameter is empty");
    *status = X_NULL;
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

  // Clean up.
  redisxDestroyRESP(reply);
  free(*pCursor);

  // Check for errors
  if(*status) x_trace(fn, NULL, *status);

  if(!entries) return NULL;

  // Sort alphabetically.
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
 * Destroy a RedisEntry array with dynamically allocate keys/values, such as returned e.g. by
 * redisxScanTable().
 *
 * IMPORTANT:
 *
 * You should not use this function to destroy RedisEntry[] arrays, which contain static
 * string references (keys or values). If the table contains only static references you can simply
 * call free() on the table. Otherwise, you will have to first free only the dynamically sized
 * string fields within before calling free() on the table itself.
 *
 *
 * @param entries   Pointer to the entries array (or single entry data). It may be NULL, in which
 *                  case this call will return immediately.
 * @param count     The number of elements contained in the array
 *
 * @sa redisxScanTable()
 * @sa redisxGetTable()
 */
void redisxDestroyEntries(RedisEntry *entries, int count) {
  if(!entries) return;

  while(--count >= 0) {
    RedisEntry *e = &entries[count];
    if(e->key) free(e->key);
    if(e->value) free(e->value);
  }

  free(entries);
}

/**
 * Destroy an array of keywords (i.e. an array of string pointers), such as returned e.g. by
 * redisxScanKeys().
 *
 * @param keys    An array of string pointers
 * @param count   The number of strings contained in the array. It may be NULL., in which case
 *                this call will return immediately.
 *
 * @sa redisxScanKeys()
 * @sa redisxGetKeys()
 */
void redisxDestroyKeys(char **keys, int count) {
  if(!keys) return;
  while(--count >= 0) if(keys[count]) free(keys[count]);
  free(keys);
}

// The following is not available on prior to the POSIX.1-2008 standard
// We'll use the __STDC_VERSION__ constant as a proxy to see if fnmatch is available
#if __STDC_VERSION__ > 201112L

/**
 * Removes all Redis entries that match the specified table:field name pattern.
 *
 * \param redis     Pointer to the Redis instance.
 * \param pattern   Glob pattern of aggregate table:field IDs to delete
 * \return          The number of tables + fields that were matched and deleted, or else an xchange error code (&lt;0).
 */
int redisxDeleteEntries(Redis *redis, const char *pattern) {
  static const char *fn = "redisxDeleteEntries";

  char *root, *key;
  char **keys;
  int i, n = 0, found = 0, status;

  if(!pattern) return x_error(X_NULL, EINVAL, fn, "'pattern' is NULL");
  if(!pattern[0]) return x_error(X_NULL, EINVAL, fn, "'pattern' is empty");

  // Separate the top-level component
  root = xStringCopyOf(pattern);
  xSplitID(root, &key);

  if(redisxIsGlobPattern(root)) {
    keys = redisxScanKeys(redis, root, &n, &status);
    if(status || !keys) {
      free(root);
      prop_error(fn, status);
      return x_trace(fn, NULL, X_NULL);
    }
  }
  else {
    keys = (char **) calloc(1, sizeof(char *));
    x_check_alloc(keys);
    keys[0] = xStringCopyOf(root);
    n = 1;
  }

  for(i = 0; i < n ; i++) {
    const char *table = keys[i];
    RedisEntry *entries;
    int nEntries;

    // If the table itself matches, delete it wholesale...
    if(fnmatch(root, table, 0) == 0) {
      RESP *reply = redisxRequest(redis, "DEL", table, NULL, NULL, &status);
      if(status == X_SUCCESS) if(redisxCheckRESP(reply, RESP_INT, 1) == X_SUCCESS) found++;
      redisxDestroyRESP(reply);
      continue;
    }

    // Otherwise check the table entries...
    entries = redisxScanTable(redis, table, key, &nEntries, &status);
    if(status == X_SUCCESS) {
      int k;
      for(k = 0; k < nEntries; k++) {
        const RedisEntry *e = &entries[k];
        char *id = xGetAggregateID(table, e->key);

        if(id) {
          if(fnmatch(key, id, 0) == 0) {
            RESP *reply = redisxRequest(redis, "HDEL", table, e->key, NULL, &status);
            if(status == X_SUCCESS) if(redisxCheckRESP(reply, RESP_INT, 1) == X_SUCCESS) found++;
            redisxDestroyRESP(reply);
          }
          free(id);
        }

        if(e->key) free(e->key);
        if(e->value) free(e->value);
      }
    }

    if(entries) free(entries);
  }

  redisxDestroyKeys(keys, n);
  free(root);


  return found;
}

#endif
