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
  static const char *funcName = "redisxGetTable()";
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
  static const char *funcName = "redisxSetValue()";

  int status = X_SUCCESS;
  RedisClient *cl;

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

  status = redisxSetValueAsync(cl, table, key, value, !isPipelined);
  redisxUnlockClient(cl);

  return status;
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
int redisxSetValueAsync(RedisClient *cl, const char *table, const char *key, const char *value, boolean confirm) {
  static const char *funcName = "redisxSetValueAsync()";
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

  if(confirm) {
    RESP *reply = redisxReadReplyAsync(cl);
    status = redisxCheckRESP(reply, RESP_INT, 0);
    redisxDestroyRESP(reply);
    if(status) return redisxError(funcName, status);
  }

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
  static const char *funcName = "redisxGetValue()";

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
  static const char *funcName = "redisxMultiSet()";
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
 * @sa redisxDestroyKeys()
 */
char **redisxGetKeys(Redis *redis, const char *table, int *n) {
  static const char *funcName = "redisxGetKeys()";
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
 * @sa redisxDestroyKeys()
 */
char **redisxScanKeys(Redis *redis, const char *pattern, int *n, int *status) {
  static const char *funcName = "redisxScanKeys()";

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
 * @sa redisxDestroyEntries()
 */
RedisEntry *redisxScanTable(Redis *redis, const char *table, const char *pattern, int *n, int *status) {
  static const char *funcName = "redisxScanTable()";

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
 * Destroy a RedisEntry array, such as returned e.g. by redisxScanTable()
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
  char **keys;
  int i, n = 0, status;

  if(!pattern) return X_NAME_INVALID;

  keys = redisxScanKeys(redis, pattern, &n, &status);
  if(status) return status;
  if(!keys) return X_NULL;

  for(i = 0; i < n ; i++) {
    char *root, *key;
    const char *table = keys[i];
    RedisEntry *entries;
    int nEntries;

    // If the table itself matches, delete it wholesale...
    if(fnmatch(pattern, table, 0) == 0) {
      RESP *reply = redisxRequest(redis, "DEL", table, NULL, NULL, &status);
      if(redisxCheckDestroyRESP(reply, RESP_INT, 1) == X_SUCCESS) n++;
      continue;
    }

    // Look for table:key style patterns
    root = xStringCopyOf(pattern);
    xSplitID(root, &key);

    // Otherwise check the table entries...
    entries = redisxScanTable(redis, table, root, &nEntries, &status);
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

    free(root);

    if(entries) free(entries);
  }
  return n;
}

#endif
