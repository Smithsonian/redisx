/**
 * @file
 *
 * @date Created  on Sep 5, 2024
 * @author Attila Kovacs
 *
 *   Supporting functions for loading and LUA scripts on a Redis server.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "redisx-priv.h"

/**
 * Loads a LUA script into Redis, returning its SHA1 hash to use as it's call ID.
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
  static const char *funcName = "redisxLoadScript()";
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
 * Send a request to runs a LUA script that has been loaded into the Redis database. This function should
 * be called with the connected client's mutex locked. The call returns as soon as the request has been
 * sent, without waiting for a response to come back.
 *
 * @param cl        The Redis client channel on which to send the request to run the script
 * @param sha1      The SHA1 sum of the script that was previously loaded into the Redis DB.
 * @param keys      A NULL-terminated array of Redis keywords, or NULL if the script does not take
 *                  any keyword argument.
 * @param params    A NULL-terminated array of additional parameters to pass onto the script, or
 *                  NULL if the script does not take any parameters.
 * @return          X_SUCCESS (0) if successful or else X_NULL if the `redis` or `sha1` parameters
 *                  are NULL, or else an error code from redisxSendArrayRequestAsync().
 *
 * @sa redisxRunScript()
 * @sa redisxLoadScript()
 * @sa redisxLockConnected()
 */

int redisxRunScriptAsync(RedisClient *cl, const char *sha1, const char **keys, const char **params) {
  int i = 0, k, nkeys = 0, nparams = 0, nargs;
  char sn[20], **args;

  if(cl == NULL || sha1 == NULL) return redisxError("redisxRunScriptAsync()", X_NULL);

  if(keys) while(keys[nkeys]) nkeys++;
  if(params) while(params[nparams]) nparams++;

  nargs = 3 + nkeys + nparams;
  sprintf(sn, "%d", nkeys);
  args = (char **) malloc(nargs * sizeof(char *));

  args[i++] = "EVALSHA";
  args[i++] = (char *) sha1;
  args[i++] = sn;

  for(k = 0; k < nkeys; k++) args[i++] = (char *) keys[k];
  for(k = 0; k <nparams; i++) args[i++] = (char *) params[k];

  i = redisxSendArrayRequestAsync(cl, args, NULL, nargs);
  free(args);

  return i;
}

/**
 * Runs a LUA script that has been loaded into the Redis database, returning the response received, or
 * NULL if there was an error.
 *
 * @param redis     The Redis instance
 * @param sha1      The SHA1 sum of the script that was previously loaded into the Redis DB.
 * @param keys      A NULL-terminated array of Redis keywords, or NULL if the script does not take
 *                  any keyword argument.
 * @param params    A NULL-terminated array of additional parameters to pass onto the script, or
 *                  NULL if the script does not take any parameters.
 * @return          The response received from the script or the EVALSHA request, or NULL if
 *                  there was an error.
 *
 * @sa redisxRunScriptAsync()
 * @sa redisxLoadScript()
 */
RESP *redisxRunScript(Redis *redis, const char *sha1, const char **keys, const char **params) {
  RESP *reply = NULL;
  int status;

  if(redis == NULL || sha1 == NULL) return NULL;

  status = redisxLockConnected(redis->interactive);
  if (status) return NULL;

  status = redisxRunScriptAsync(redis->interactive, sha1, keys, params);
  if (status == X_SUCCESS) reply = redisxReadReplyAsync(redis->interactive);

  redisxUnlockClient(redis->interactive);
  return reply;
}
