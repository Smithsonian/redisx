/**
 * @file
 *
 * @date Created  on Aug 26, 2024
 * @author Attila Kovacs
 *
 *  PUB/SUB functions for the RedisX library.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>

#include "redisx-priv.h"

/**
 * Waits to get exlusive access to Redis scubscriber calls.
 *
 * \param redis         Pointer to a Redis instance.
 *
 */
static void rSubscriberLock(Redis *redis) {
  RedisPrivate *p = (RedisPrivate *) redis->priv;
  pthread_mutex_lock(&p->subscriberLock);
}

/**
 * Relinquish exlusive access to Redis scubscriber calls.
 *
 * \param redis         Pointer to a Redis instance.
 *
 */
static void rSubscriberUnlock(Redis *redis) {
  RedisPrivate *p = (RedisPrivate *) redis->priv;
  pthread_mutex_unlock(&p->subscriberLock);
}

/**
 * Connects the subscription client/channel to the REDIS server, for sending and receiving
 * PUB/SUB commands and messages, and starts the SubscriptionListener thread for
 * consuming incoming PUB/SUB messages in the background.
 *
 * \param redis         Pointer to a Redis instance.
 *
 * \return      X_SUCCESS on success, or X_NO_SERVICE if the connection failed.
 */
static int rConnectSubscriptionClientAsync(Redis *redis) {
  int status;
  const ClientPrivate *sp = (ClientPrivate *) redis->subscription->priv;

  if(sp->isEnabled) {
    fprintf(stderr, "WARNING! Redis-X : pub/sub client is already connected.\n");
    return X_SUCCESS;
  }

  xvprintf("Redis-X> Connect pub/sub client.\n");
  status = rConnectClient(redis, SUBSCRIPTION_CHANNEL);

  if(status) {
    fprintf(stderr, "ERROR! Redis-X : pub/sub client connection failed. code: %d\n", status);
    return X_NO_SERVICE;
  }

  return X_SUCCESS;
}

/**
 * Sends a Redis notification asynchronously using the Redis "PUBLISH" command.
 * The caller should have an exclusive lock on the interactive Redis channel before calling this.
 *
 * \param redis         Pointer to a Redis instance.
 * \param channel       Redis PUB/SUB channel on which to notify
 * \param data          Message body data.
 * \param length        Bytes of message data to send, ot 0 to determine automatically with strlen().
 *
 * \return      X_SUCCESS (0)   if successful, or else
 *              X_NULL          if the redis instance is NULL
 *              X_NAME_INVALID  if the PUB/SUB channel is null or empty
 *
 *              or an error code returned by redisxSendArrayRequestAsync().
 *
 * @sa redisxPublish()
 * @sa redisxNotify()
 */
int redisxPublishAsync(Redis *redis, const char *channel, const char *data, int length) {
  const char *funcName = "redisSEndInteractiveNotifyAsync()";

  char *args[3];
  int status, L[3] = {0};

  if(redis == NULL) return redisxError(funcName, X_NULL);
  if(channel == NULL) return redisxError(funcName, X_NAME_INVALID);
  if(*channel == '\0') return redisxError(funcName, X_NAME_INVALID);

  args[0] = "PUBLISH";
  args[1] = (char *) channel;
  args[2] = (char *) data;

  if(length <= 0) length = strlen(data);
  L[2] = length;

  status = redisxSkipReplyAsync(redis->interactive);
  if(status) return redisxError(funcName, status);

  status = redisxSendArrayRequestAsync(redis->interactive, args, L, 3);
  if(status) return redisxError(funcName, status);

  return X_SUCCESS;
}

/**
 * Sends a generic Redis PUB/SUB message on the specified channel.
 * Redis must be connected before attempting to send messages. It will send the
 * message over the pipeline client if it is avaiable, or else over
 * the interactive client.
 *
 * \param redis         Pointer to a Redis instance.
 * \param channel       Redis PUB/SUB channel on which to notify
 * \param data          Data to send.
 * \param length        Bytes of data to send, or 0 to determine automatically with strlen().
 *
 *
 * \return      X_SUCCESS       if the message was successfullt sent.
 *              X_NO_INIT       if the Redis library was not initialized via initRedis().
 *              X_NO_SERVICE    if there was a connection problem.
 *              PARSE_ERROR     if the Redis response could not be confirmed.
 *              or the errno returned by send().
 *
 * @sa redisxNotify()
 * @sa redisxPublishAsync()
 * @sa redisxSubscribe()
 */
int redisxPublish(Redis *redis, const char *channel, const char *data, int length) {
  const char *funcName = "redisxNotifyBin()";

  int status = 0;

  if(redis == NULL) return redisxError(funcName, X_NULL);

  status = redisxLockEnabled(redis->interactive);
  if(status) return redisxError(funcName, status);

  // Now send the message
  status = redisxPublishAsync(redis, channel, data, length);

  // Clean up...
  redisxUnlockClient(redis->interactive);

  if(status) return redisxError(funcName, status);

  xvprintf("Redis-X> sent message to %s, channel %s: '%s'.\n", redis->id, channel, data);

  return X_SUCCESS;
}

/**
 * Sends a regular string terminated Redis PUB/SUB message on the specified channel.
 * Same as redisxPublish() with the length argument set to the length of the
 * string message.
 * Redis must be connected before attempting to send messages.
 *
 * \param redis         Pointer to a Redis instance.
 * \param channel       Redis PUB/SUB channel on which to notify
 * \param message       Message to send.
 *
 *
 * \return      X_SUCCESS       if the message was successfullt sent.
 *              X_NO_INIT       if the Redis library was not initialized via initRedis().
 *              X_NO_SERVICE    if there was a connection problem.
 *              PARSE_ERROR     if the Redis response could not be confirmed.
 *              or the errno returned by send();
 *
 * @sa redisxPublish()
 * @sa redisxPublishAsync()
 * @sa redisxSubscribe()
 *
 */
int redisxNotify(Redis *redis, const char *channel, const char *message) {
  return redisxPublish(redis, channel, message, 0);
}

/**
 * Add a targeted subscriber processing function to the list of functions that process
 * Redis PUB/SUB responses. You will still have to subscribe the relevant PUB/SUB messages
 * from redis separately, using redisxSubscribe() before any messages are delivered to
 * this client. If the subscriber with the same callback function and channel stem
 * is already added, this call simply return and will NOT create a duplicate enry.
 * However, the same callback may be added multiple times with different channel stems
 * (which pre-filter what messages each of the callbacks may get).
 *
 * \param redis         Pointer to a Redis instance.
 * \param channelStem   If NULL, the consumer will receive all Redis messages published to the given channel.
 *                      Otherwise, the consumer will be notified only if the incoming channel begins with the
 *                      specified stem.
 * \param f             A function that consumes subscription messages.
 *
 * @sa redisxRemoveSubscribers()
 * @sa redisxSubscribe()
 *
 */
void redisxAddSubscriber(Redis *redis, const char *channelStem, RedisSubscriberCall f) {
  MessageConsumer *c;
  RedisPrivate *p;

  if(redis == NULL) return;

  p = (RedisPrivate *) redis->priv;

  rSubscriberLock(redis);

  // Check if the subscriber is already listed with the same stem. If so, nothing to do...
  for(c = p->subscriberList; c != NULL; c = c->next) {
    if(f != c->func) continue;

    if(channelStem) {
      if(strcmp(channelStem, c->channelStem)) continue;
    }
    else if(c->channelStem) continue;

    // This subscriber is already listed. Nothing to do.
    xdprintf("DEBUG-X> Matching subscriber callback for stem %s already listed. Nothing to do.\n", channelStem);
    rSubscriberUnlock(redis);
    return;
  }

  c = (MessageConsumer *) calloc(1, sizeof(MessageConsumer));

  c->func = f;
  c->channelStem = xStringCopyOf(channelStem);
  c->next = p->subscriberList;
  p->subscriberList = c;

  rSubscriberUnlock(redis);

  xvprintf("Redis-X> Added new subscriber callback for stem %s.\n", channelStem);
}

/**
 * Removes all instances of a subscribe consumer function from the current list of consumers.
 * This calls only deactivates the specified processing callback function(s), without
 * stopping the delivery of associated messages. To stop Redis sending messages that are
 * no longer being processed, you should also call redisxUnsubscribe() as appropriate.
 *
 * \param redis     Pointer to a Redis instance.
 * \param f         The consumer function to remove from the list of active subscribers.
 *
 * \return  The number of instances of f() that have been removed from the list of subscribers.
 *
 * \sa redisxAddSubscriber()
 * @sa redisxClearSubscribers()
 * @sa redisxUnsubscrive()
 */
// cppcheck-suppress constParameter
// cppcheck-suppress constParameterPointer
int redisxRemoveSubscribers(Redis *redis, RedisSubscriberCall f) {
  const char *funcName = "redisxRemoveSubscribers()";

  RedisPrivate *p;
  MessageConsumer *c, *last = NULL;
  int removed = 0;

  if(!f) return redisxError(funcName, X_NULL);
  if(redis == NULL) return redisxError(funcName, X_NULL);

  p = (RedisPrivate *) redis->priv;

  rSubscriberLock(redis);

  for(c = p->subscriberList; c != NULL; ) {
    MessageConsumer *next = c->next;

    if(c->func == f) {
      if(last) last->next = next;
      else p->subscriberList = next;

      if(c->channelStem) free(c->channelStem);
      free(c);

      removed++;
    }
    else last = c;

    c = next;
  }

  rSubscriberUnlock(redis);

  xvprintf("Redis-X> Removed %d subscriber callbacks.\n", removed);

  return removed;
}

/**
 * Stops the custom consumption of PUB/SUB messages from Redis.
 *
 * \param redis         Pointer to a Redis instance.
 *
 * \return              X_SUCCESS (0)   if successful, or
 *                      X_NULL          if the redis instance is NULL.
 *
 */
int redisxClearSubscribers(Redis *redis) {
  RedisPrivate *p;
  MessageConsumer *c;
  int n = 0;

  if(redis == NULL) return redisxError("redisxClearSubscribers()", X_NULL);

  p = (RedisPrivate *) redis->priv;

  rSubscriberLock(redis);
  c = p->subscriberList;
  p->subscriberList = NULL;
  rSubscriberUnlock(redis);

  while(c != NULL) {
    MessageConsumer *next = c->next;
    free(c);
    c = next;
    n++;
  }

  xvprintf("Redis-X> Cleared all (%d) subscriber callbacks.\n", n);

  return n;
}

/**
 * Checks if a given string is a glob-style pattern.
 *
 * \param str       The string to check.
 *
 * \return          TRUE if it is a glob pattern (e.g. has '*', '?' or '['), otherwise FALSE.
 *
 */
static int rIsGlobPattern(const char *str) {
  for(; *str; str++) switch(*str) {
    case '*':
    case '?':
    case '[': return TRUE;
  }
  return FALSE;
}

/**
 * Subscribe to a specific Redis channel. The call will also start the subscription listener
 * thread to processing incoming subscription messages. Subscribing only enabled the delivery
 * of the messages to this client without any actions on these messages. In order to process
 * the messages for your subscriptons, you will also want to call redisxAddSubscriber() to add
 * your custom processor function(s).
 *
 * \param redis         Pointer to a Redis instance.
 * \param pattern       The Channel pattern to subscribe to, e.g. 'acc1', or 'acc*'...
 *
 * \return      X_SUCCESS       if successfully subscribed to the Redis distribution channel.
 *              X_NO_SERVICE    if there is no active connection to the Redis server.
 *              X_NULL          if the channel argument is NULL
 *
 * @sa redisxAddSubscriber()
 * @sa redisxUnsubscribe()
 * @sa redisxNotify()
 * @sa redisxPublish()
 * @sa redisxPublishAsync()
 */
int redisxSubscribe(Redis *redis, const char *pattern) {
  const char *funcName = "redisxSubscribe()";

  const ClientPrivate *cp;
  int status = 0;

  if(redis == NULL || pattern == NULL) return redisxError(funcName, X_NULL);

  // connect subscription client as needed.
  rConfigLock(redis);
  cp = (ClientPrivate *) redis->subscription->priv;
  if(!cp->isEnabled) status = rConnectSubscriptionClientAsync(redis);
  if(!status) {
    // start processing subscriber responses if not already...
    const RedisPrivate *p = (RedisPrivate *) redis->priv;
    if(!p->isSubscriptionListenerEnabled) rStartSubscriptionListenerAsync(redis);
  }
  rConfigUnlock(redis);

  if(status) return redisxError(funcName, status);

  status = redisxLockEnabled(redis->subscription);
  if(status) return redisxError(funcName, status);

  status = redisxSendRequestAsync(redis->subscription, rIsGlobPattern(pattern) ? "PSUBSCRIBE" : "SUBSCRIBE", pattern, NULL, NULL);
  redisxUnlockClient(redis->subscription);

  if(status) return redisxError(funcName, status);

  return X_SUCCESS;
}

/**
 * Unsubscribe from one or all Redis PUB/SUB channel(s). If there are no active subscriptions
 * when Redis confirms the unsubscrive command, the subscription listener thread will also conclude
 * automatically. Unsubscribing will stop delivery of mesasages for the affected channels but
 * any associated processing callbacks remain registered, until redisxRemovesubscribers() is
 * called to deactive them as appropriate.
 *
 * \param redis         Pointer to a Redis instance.
 * \param pattern       The channel pattern, or NULL to unsubscribe all channels and patterns.
 *
 * \return     X_SUCCESS       if successfully subscribed to the Redis distribution channel.
 *             X_NO_SERVICE    if there is no active connection to the Redis server.
 *
 * @sa redisxSubscribe()
 * @sa redisxEndSubscribe()
 * @sa redisxRemoveSubscribers()
 */
int redisxUnsubscribe(Redis *redis, const char *pattern) {
  const char *funcName = "redisxUnsubscribe()";

  int status;

  if(redis == NULL) return redisxError(funcName, X_NULL);

  status = redisxLockEnabled(redis->subscription);
  if(status) return redisxError(funcName, status);

  if(pattern) {
    status = redisxSendRequestAsync(redis->subscription, rIsGlobPattern(pattern) ? "PUNSUBSCRIBE" : "UNSUBSCRIBE", pattern, NULL, NULL);
  }
  else {
    status = redisxSendRequestAsync(redis->subscription, "UNSUBSCRIBE", NULL, NULL, NULL);
    if(!status) status = redisxSendRequestAsync(redis->subscription, "PUNSUBSCRIBE", NULL, NULL, NULL);
  }

  redisxUnlockClient(redis->subscription);

  if(status) return redisxError(funcName, status);

  return X_SUCCESS;
}

/// \cond PRIVATE

/**
 * Unsubscribes from all channels, stops the subscription listener thread, and closes the subscription
 * client connection. This call assumes the caller has an exlusive lock on the configuration properties
 * of the affected Redis instance.
 *
 * @param redis     Pointer to a Redis instance.
 *
 * @sa redisxEndSubscribe()
 * @sa redisxUnsubscribe()
 */
void rEndSubscriptionAsync(Redis *redis) {
  RedisPrivate *p;

  xvprintf("Redis-X> End all subscriptions, and quit listener.\n");

  p = (RedisPrivate *) redis->priv;

  redisxUnsubscribe(redis, NULL);

  p->isSubscriptionListenerEnabled = FALSE;
  rCloseClient(redis->subscription);
}

/// \endcond

/**
 * Unsubscribes from all channels, stops the subscription listener thread, and closes the subscription
 * client connection.
 *
 * @param redis     Pointer to a Redis instance.
 *
 * @sa redisxUnsubscribe()
 */
void redisxEndSubscription(Redis *redis) {
  rConfigLock(redis);
  rEndSubscriptionAsync(redis);
  rConfigUnlock(redis);
}

static void rNotifyConsumers(Redis *redis, char *pattern, char *channel, char *msg, int length) {
#if REDISX_LISTENER_YIELD_COUNT > 0
  static int count;
#endif

  MessageConsumer *c;
  RedisPrivate *p;
  RedisSubscriberCall *f = NULL;
  int n = 0;

  xdprintf("NOTIFY: %s | %s\n", channel, msg);

  p = (RedisPrivate *) redis->priv;

  rSubscriberLock(redis);

  // Count how many matching subscribers there are...
  for(c = p->subscriberList ; c != NULL; c = c->next) {
    if(c->channelStem != NULL) if(strncmp(c->channelStem, channel, strlen(c->channelStem))) continue;
    n++;
  }

  if(n) {
    // Put matching subscribers into an array...
    f = (RedisSubscriberCall *) calloc(n, sizeof(*f));
    n = 0;
    for(c = p->subscriberList ; c != NULL; c = c->next) {
      if(c->channelStem != NULL) if(strncmp(c->channelStem, channel, strlen(c->channelStem))) continue;
      f[n++] = c->func;
    }
  }

  rSubscriberUnlock(redis);

  // If there were matching subscribers, call them...
  if(n) {
    int i;
    for(i=0; i<n; i++) f[i](pattern, channel, msg, length);
    free(f);
  }

#if REDISX_LISTENER_YIELD_COUNT > 0
  // Allow the waiting processes to take control...
  if(++count % REDISX_LISTENER_YIELD_COUNT == 0) sched_yield();
#endif
}

/// \cond PRIVATE

/**
 * This is the subscription client thread listener routine. It is started by redisScubscribe(), and stopped
 * when Redis confirms that there are no active subscriptions following an 'unsubscribe' request.
 *
 * \param pRedis        Pointer to a Redis instance.
 *
 * \return              Always NULL.
 *
 */
void *RedisSubscriptionListener(void *pRedis) {
  static int counter, lastError;

  Redis *redis = (Redis *) pRedis;
  RedisPrivate *p;
  RedisClient *cl;
  const ClientPrivate *cp;
  RESP *reply = NULL, **component;
  int i;

  pthread_detach(pthread_self());

  xvprintf("Redis-X> Started processing subsciptions...\n");

  if(redis == NULL) {
    redisxError("RedisSubscriptionListener", X_NULL);
    return NULL;
  }

  p = (RedisPrivate *) redis->priv;
  cl = redis->subscription;
  cp = (ClientPrivate *) cl->priv;

  while(cp->isEnabled && p->isSubscriptionListenerEnabled && pthread_equal(p->subscriptionListenerTID, pthread_self())) {
    // Discard the response from the prior iteration
    if(reply) redisxDestroyRESP(reply);

    // Get the new response...
    reply = redisxReadReplyAsync(cl);

    counter++;

    // In case client was closed while waiting for response, break out...
    if(!cp->isEnabled) break;

    if(reply == NULL) {
      fprintf(stderr, "WARNING! Redis-X : invalid subscriber response, errno = %d.\n", errno);
      continue;
    }

    if(reply->n < 0) {
      if(reply->n != lastError) fprintf(stderr, "ERROR! Redis-X : subscriber parse error: %d.\n", reply->n);
      lastError = reply->n;
      continue;
    }

    if(reply->type != RESP_ARRAY) {
      fprintf(stderr, "WARNING! Redis-X : unexpected subscriber response type: '%c'.\n", reply->type);
      continue;
    }

    component = (RESP **) reply->value;

    lastError = 0;

    // Check that the response has a valid array dimension
    if(reply->n < 3 || reply->n > 4) {
      fprintf(stderr, "WARNING! Redis-X: unexpected number of components in subscriber response: %d.\n", reply->n);
      continue;
    }

    // Check for NULL component (all except last -- the payload -- which may be NULL)
    for(i=reply->n-1; --i >= 0; ) if(component[i] == NULL) {
      fprintf(stderr, "WARNING! Redis-X : subscriber NULL in component %d.\n", (i+1));
      continue;
    }

    if(!strcmp("message", (char *) component[0]->value)) {
      // Send the message to the matching subscribers or warn if invalid....
      if(reply->n == 3)
        rNotifyConsumers(redis, NULL, (char *) component[1]->value, (char *) component[2]->value, component[2]->n);
      else fprintf(stderr, "WARNING! Redis-X: unexpected subscriber message dimension: %d.\n", reply->n);
    }

    else if(!strcmp("pmessage", (char *) component[0]->value)) {
      // Send the message to the matching subscribers or warn if invalid....
      if(reply->n == 4)
        rNotifyConsumers(redis, (char *) component[1]->value, (char *) component[2]->value, (char *) component[3]->value, component[3]->n);
      else fprintf(stderr, "WARNING! Redis-X: unexpected subscriber pmessage dimension: %d.\n", reply->n);
    }

    //    else if(!strcmp("unsubscribe", (char *) component[0]->value)) if(component[2]->n <= 0) {
    //      xvprintf("Redis-X> no more subscriptions.\n");
    //
    //      // if unsubscribe, and no subscribers left, then exit the listener...
    //      // !!! THIS IS DANGEROUS !!! -- because we can have a concurrent subscribe that will think
    //      // listener is alive, just as we are about to kill it. It's safer to keep it running...
    //      //break;
    //    }

#if REDISX_LISTENER_YIELD_COUNT > 0
    // Allow the waiting processes to take control...
    if(counter % REDISX_LISTENER_YIELD_COUNT == 0) sched_yield();
#endif

  } // <-- End of listener loop

  rConfigLock(redis);
  // If we are the current listener thread, then mark the listener as disabled.
  if(pthread_equal(p->subscriptionListenerTID, pthread_self())) p->isSubscriptionListenerEnabled = FALSE;
  rConfigUnlock(redis);

  xvprintf("Redis-X> Stopped processing subscriptions (%d processed)...\n", counter);

  redisxDestroyRESP(reply);

  return NULL;
}

/// \endcond