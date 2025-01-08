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
 *              or an error (&lt;0) returned by redisxRequest().
 *
 */
int redisxLoadScript(Redis *redis, const char *script, char **sha1) {
  static const char *fn = "redisxLoadScript";
  RESP *reply;
  int status = X_SUCCESS;

  if(script == NULL) return x_error(X_NULL, EINVAL, fn, "input script is NULL");
  if(*script == '\0') return x_error(X_NULL, EINVAL, fn, "input script is empty");

  *sha1 = NULL;

  reply = redisxRequest(redis, "SCRIPT", "LOAD", script, NULL, &status);
  prop_error(fn, status);
  prop_error(fn, redisxCheckDestroyRESP(reply, RESP_BULK_STRING, 0));

  *sha1 = (char *) reply->value;
  redisxDestroyRESP(reply);

  return X_SUCCESS;
}

/**
 * Returns an EVAL argument array for the given Redis script and its keys and parameters.
 *
 * @param sha1        script SHA1 sum
 * @param keys        NULL-terminated array of Redis keys to pass to script, or NULL if the
 *                    script requires no keys.
 * @param params      NULL-terminated array of other parameters to pass to script, or NULL
 *                    if the script requires no parameters
 * @param[out] nargs  number of entries in returned argument array
 * @return            array containing EVAL command and arguments.
 *
 * @sa redisxRunScript()
 * @sa redisxRunScriptAsync()
 */
static const char **rGetScriptArgs(const char *sha1, const char **keys, const char **params, int *nargs) {
  const char **args;
  char sn[20];
  int i = 0,k, nkeys = 0, nparams = 0, n;

  if(keys) while(keys[nkeys]) nkeys++;
  if(params) while(params[nparams]) nparams++;

  n = 3 + nkeys + nparams;
  sprintf(sn, "%d", nkeys);
  args = (const char **) malloc(n * sizeof(char *));
  if(!args) {
    *nargs = x_error(X_FAILURE, errno, "rGetScriptArgs", "alloc() error (%d char *)", n);
    return NULL;
  }

  args[i++] = "EVALSHA";
  args[i++] = (char *) sha1;
  args[i++] = sn;

  for(k = 0; k < nkeys; k++) args[i++] = (char *) keys[k];
  for(k = 0; k < nparams; k++) args[i++] = (char *) params[k];

  *nargs = n;
  return args;
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
 *                  are NULL, or else an error code (&lt;0) from redisxSendArrayRequestAsync().
 *
 * @sa redisxRunScript()
 * @sa redisxLoadScript()
 * @sa redisxLockConnected()
 */

int redisxRunScriptAsync(RedisClient *cl, const char *sha1, const char **keys, const char **params) {
  static const char *fn = "redisxRunScriptAsync";

  const char **args;
  int nargs, status;

  prop_error(fn, rCheckClient(cl));

  if(sha1 == NULL) return x_error(X_NULL, EINVAL, fn, "input script SHA1 sum is NULL");

  args = rGetScriptArgs(sha1, keys, params, &nargs);
  if(!args) return x_trace(fn, NULL, nargs);

  status = redisxSendArrayRequestAsync(cl, (const char **) args, NULL, nargs);
  free(args);

  prop_error(fn, status);
  return X_SUCCESS;
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
 * @param status    Pointer to int in which to return status, or NULL if not required.
 * @return          The response received from the script or the EVALSHA request, or NULL if
 *                  there was an error.
 *
 * @sa redisxRunScriptAsync()
 * @sa redisxLoadScript()
 */
RESP *redisxRunScript(Redis *redis, const char *sha1, const char **keys, const char **params, int *status) {
  static const char *fn = "redisxRunScript";

  const char **args;
  int nargs;
  RESP *reply = NULL;

  if(redisxCheckValid(redis) != X_SUCCESS) return x_trace_null(fn, NULL);

  if(sha1 == NULL) {
    if(status) *status = X_NULL;
    x_error(0, EINVAL, fn, "sha1 parameter is NULL");
    return NULL;
  }

  args = rGetScriptArgs(sha1, keys, params, &nargs);
  if(!args) {
    if(status) *status = nargs;
    return x_trace_null(fn, NULL);
  }

  reply = redisxArrayRequest(redis, args, NULL, nargs, status);
  free(args);

  return reply;
}
