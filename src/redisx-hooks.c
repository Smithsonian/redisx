/**
 * @file
 *
 * @date Created  on Aug 26, 2024
 * @author Attila Kovacs
 *
 *  A set of functions to manage callback hooks for the RedisX library.
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#include "redisx-priv.h"

static Hook *createHook(Redis *redis, void (*f)(Redis *)) {
  Hook *h = (Hook *) calloc(1, sizeof(Hook));
  h->call = f;
  h->arg = redis;
  return h;
}

/**
 * Adds a connect call hook, provided it is not already part of the setup routine.
 *
 * \param redis         Pointer to a Redis instance.
 * \param setupCall     User-specified callback routine to be called after the Redis instance has been connected.
 *                      It will be passed a pointer to the Redis instance, which triggered the call by
 *                      having established connection.
 * @return  X_SUCCESS (0) if successful or else X_NULL if either of the arguments is NULL.
 *
 */
// cppcheck-suppress constParameter
// cppcheck-suppress constParameterPointer
int redisxAddConnectHook(Redis *redis, void (*setupCall)(Redis *)) {
  static const char *fn = "redisxAddConnectHook";
  RedisPrivate *p;

  if(redis == NULL) return x_error(X_NULL, EINVAL, fn, "redis is NULL");
  if(setupCall == NULL) return x_error(X_NULL, EINVAL, fn, "setupCall is NULL");

  xvprintf("Redis-X> Adding a connect callback.\n");

  p = (RedisPrivate *) redis->priv;

  rConfigLock(redis);
  if(p->firstConnectCall == NULL) p->firstConnectCall = createHook(redis, setupCall);
  else {
    // Check if the specified hook is already added...
    Hook *k = p->firstConnectCall;
    while(k != NULL) {
      if(k->call == setupCall) break;
      if(k->next == NULL) k->next = createHook(redis, setupCall);
      k = k->next;
    }
  }
  rConfigUnlock(redis);

  return X_SUCCESS;
}

/**
 * Removes a connect call hook.
 *
 * \param redis         Pointer to a Redis instance.
 * \param setupCall     User-specified callback routine to be called after the Redis instance has been connected.
 *
 * @return  X_SUCCESS (0) if successful or else X_NULL if either of the arguments is NULL.
 */
// cppcheck-suppress constParameter
// cppcheck-suppress constParameterPointer
int redisxRemoveConnectHook(Redis *redis, void (*setupCall)(Redis *)) {
  static const char *fn = "redisxRemoveConnectHook";

  RedisPrivate *p;
  Hook *c, *last = NULL;

  if(redis == NULL) x_error(X_NULL, EINVAL, fn, "redis is NULL");
  if(setupCall == NULL) x_error(X_NULL, EINVAL, fn, "setupCall is NULL");

  xvprintf("Redis-X> Removing a connect callback.\n");

  p = (RedisPrivate *) redis->priv;

  rConfigLock(redis);
  c = p->firstConnectCall;

  while(c != NULL) {
    Hook *next = c->next;

    if(c->call == setupCall) {
      if(last == NULL) p->firstConnectCall = next;
      else last->next = next;
      free(c);
    }
    else last = c;
    c = next;
  }
  rConfigUnlock(redis);

  return X_SUCCESS;
}

/**
 * Removes all connect hooks, that is no user callbacks will be made when the specifed
 * Redis instance is connected.
 *
 * \param redis         Pointer to a Redis instance.
 */
void redisxClearConnectHooks(Redis *redis) {
  RedisPrivate *p;
  Hook *c;

  if(redis == NULL) return;

  xvprintf("Redis-X> Clearing all connect callbacks.\n");

  p = (RedisPrivate *) redis->priv;

  rConfigLock(redis);
  c = p->firstConnectCall;

  while(c != NULL) {
    Hook *next = c->next;
    free(c);
    c = next;
  }
  rConfigUnlock(redis);
}

/**
 * Adds a cleanup call, provided it is not already part of the cleanup routine, for when the
 * specified Redis instance is disconnected.
 *
 * \param redis         Pointer to a Redis instance.
 * \param cleanupCall   User specified function to call when Redis is disconnected. It will be passed
 *                      a pointer to the Redis instance, which triggered the call by having
 *                      disconnected from the Redis server.
 *
 * @return  X_SUCCESS (0) if successful or else X_NULL if either of the arguments is NULL.
 */
// cppcheck-suppress constParameter
// cppcheck-suppress constParameterPointer
int redisxAddDisconnectHook(Redis *redis, void (*cleanupCall)(Redis *)) {
  static const char *fn = "redisxAddDisconnectHook";

  RedisPrivate *p;

  if(redis == NULL) return x_error(X_NULL, EINVAL, fn, "redis is NULL");
  if(cleanupCall == NULL) return x_error(X_NULL, EINVAL, fn, "cleanupCall is NULL");

  xvprintf("Redis-X> Adding a disconnect callback.\n");

  p = (RedisPrivate *) redis->priv;

  rConfigLock(redis);
  if(p->firstCleanupCall == NULL) p->firstCleanupCall = createHook(redis, cleanupCall);
  else {
    // Check if the specified hook is already added...
    Hook *k = p->firstCleanupCall;
    while(k != NULL) {
      if(k->call == cleanupCall) break;
      if(k->next == NULL) k->next = createHook(redis, cleanupCall);
      k = k->next;
    }
  }
  rConfigUnlock(redis);

  return X_SUCCESS;
}

/**
 * Removes a cleanup call hook for when the Redis instance is disconnected.
 *
 * \param redis         Pointer to a Redis instance.
 * \param cleanupCall   User specified function to call when Redis is disconnected.
 *
 * @return  X_SUCCESS (0) if successful or else X_NULL if the argument is NULL.
 */
// cppcheck-suppress constParameter
// cppcheck-suppress constParameterPointer
int redisxRemoveDisconnectHook(Redis *redis, void (*cleanupCall)(Redis *)) {
  static const char *fn = "redisxRemoveDisconnectHook";

  RedisPrivate *p;
  Hook *c, *last = NULL;

  if(redis == NULL) return x_error(X_NULL, EINVAL, fn, "redis is NULL");
  if(cleanupCall == NULL) return x_error(X_NULL, EINVAL, fn, "cleanupCall is NULL");

  xvprintf("Redis-X> Removing a disconnect callback.\n");

  p = (RedisPrivate *) redis->priv;

  rConfigLock(redis);
  c = p->firstCleanupCall;

  while(c != NULL) {
    Hook *next = c->next;

    if(c->call == cleanupCall) {
      if(last == NULL) p->firstCleanupCall = next;
      else last->next = next;
      free(c);
    }
    else last = c;
    c = next;
  }
  rConfigUnlock(redis);

  return X_SUCCESS;
}

/**
 * Removes all disconnect hooks, that is no user-specified callbacks will be made when the
 * specified Redis instance is disconnected.
 *
 * \param redis         Pointer to a Redis instance.
 */
void redisxClearDisconnectHooks(Redis *redis) {
  RedisPrivate *p;
  Hook *c;

  if(redis == NULL) return;

  xvprintf("Redis-X> Clearing all disconnect callbacks.\n");

  p = (RedisPrivate *) redis->priv;

  rConfigLock(redis);
  c = p->firstCleanupCall;

  while(c != NULL) {
    Hook *next = c->next;
    free(c);
    c = next;
  }
  rConfigUnlock(redis);
}

