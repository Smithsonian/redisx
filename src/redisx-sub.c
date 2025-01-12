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

static int rStartSubscriptionListenerAsync(Redis *redis);

/**
 * Waits to get exlusive access to Redis scubscriber calls.
 *
 * \param redis         Pointer to a Redis instance.
 *
 */
static int rSubscriberLock(Redis *redis) {
  RedisPrivate *p;
  prop_error("rSubscriberLock", redisxCheckValid(redis));
  p = (RedisPrivate *) redis->priv;
  pthread_mutex_lock(&p->subscriberLock);
  return X_SUCCESS;
}

/**
 * Relinquish exlusive access to Redis scubscriber calls.
 *
 * \param redis         Pointer to a Redis instance.
 *
 */
static int rSubscriberUnlock(Redis *redis) {
  RedisPrivate *p;
  prop_error("rSubscriberLock", redisxCheckValid(redis));
  p = (RedisPrivate *) redis->priv;
  pthread_mutex_unlock(&p->subscriberLock);
  return X_SUCCESS;
}

/**
 * Connects the subscription client/channel to the Redis server, for sending and receiving
 * PUB/SUB commands and messages, and starts the SubscriptionListener thread for
 * consuming incoming PUB/SUB messages in the background.
 *
 * \param redis         Pointer to a Redis instance.
 *
 * \return      X_SUCCESS on success, or X_NO_SERVICE if the connection failed.
 */
static int rConnectSubscriptionClientAsync(Redis *redis) {
  static const char *fn = "rConnectSubscriptionClientAsync";

  int status, isEnabled;
  const ClientPrivate *sp;

  prop_error(fn, redisxCheckValid(redis));
  prop_error(fn, redisxLockClient(redis->subscription));
  sp = (ClientPrivate *) redis->subscription->priv;
  isEnabled = sp->isEnabled;
  redisxUnlockClient(redis->subscription);

  if(isEnabled) {
    x_warn(fn, "Redis-X : pub/sub client is already connected at %s.\n", redis->id);
    return X_SUCCESS;
  }

  xvprintf("Redis-X> Connect pub/sub client.\n");
  status = rConnectClientAsync(redis, REDISX_SUBSCRIPTION_CHANNEL);
  if(status) {
    xvprintf("ERROR! Redis-X : subscription client connection failed: %s\n", redisxErrorDescription(status));
    return x_trace(fn, NULL, X_NO_SERVICE);
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
 *              or an error code (&lt;0) returned by redisxSendArrayRequestAsync().
 *
 * @sa redisxPublish()
 * @sa redisxNotify()
 */
int redisxPublishAsync(Redis *redis, const char *channel, const char *data, int length) {
  static const char *fn = "redisPublishAsync";

  const char *args[3];
  int L[3] = {0};

  prop_error(fn, redisxCheckValid(redis));

  if(channel == NULL) return x_error(X_NULL,EINVAL, fn, "channel parameter is NULL");
  if(*channel == '\0') return x_error(X_NULL,EINVAL, fn, "channel parameter is empty");

  args[0] = "PUBLISH";
  args[1] = (char *) channel;
  args[2] = (char *) data;

  if(length <= 0) length = strlen(data);
  L[2] = length;

  prop_error(fn, redisxSkipReplyAsync(redis->interactive));
  prop_error(fn, redisxSendArrayRequestAsync(redis->interactive, args, L, 3));

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
 *
 *
 * @sa redisxNotify()
 * @sa redisxPublishAsync()
 * @sa redisxSubscribe()
 */
int redisxPublish(Redis *redis, const char *channel, const char *data, int length) {
  static const char *fn = "redisxPublish";

  int status = 0;

  prop_error(fn, redisxCheckValid(redis));
  prop_error(fn, redisxLockConnected(redis->interactive));

  // Now send the message
  status = redisxPublishAsync(redis, channel, data, length);

  // Clean up...
  redisxUnlockClient(redis->interactive);

  prop_error(fn, status);

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
 *
 * @sa redisxPublish()
 * @sa redisxPublishAsync()
 * @sa redisxSubscribe()
 *
 */
int redisxNotify(Redis *redis, const char *channel, const char *message) {
  prop_error("redisxNotify", redisxPublish(redis, channel, message, 0));
  return X_SUCCESS;
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
int redisxAddSubscriber(Redis *redis, const char *channelStem, RedisSubscriberCall f) {
  static const char *fn = "redisxAddSubscriber";

  MessageConsumer *c;
  RedisPrivate *p;

  prop_error(fn, rSubscriberLock(redis));
  p = (RedisPrivate *) redis->priv;

  // Check if the subscriber is already listed with the same stem. If so, nothing to do...
  for(c = p->subscriberList; c != NULL; c = c->next) {
    if(f != c->func) continue;

    if(channelStem) {
      if(strcmp(channelStem, c->channelStem)) continue;
    }
    else if(c->channelStem) continue;

    // This subscriber is already listed. Nothing to do.
    rSubscriberUnlock(redis);

    x_warn(fn, "Matching subscriber callback for stem %s already listed. Nothing to do.\n", channelStem);
    return X_SUCCESS;
  }

  c = (MessageConsumer *) calloc(1, sizeof(MessageConsumer));
  x_check_alloc(c);

  c->func = f;
  c->channelStem = xStringCopyOf(channelStem);
  c->next = p->subscriberList;
  p->subscriberList = c;

  rSubscriberUnlock(redis);

  xvprintf("Redis-X> Added new subscriber callback for stem %s.\n", channelStem);
  return X_SUCCESS;
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
  static const char *fn = "redisxRemoveSubscribers";

  RedisPrivate *p;
  MessageConsumer *c, *last = NULL;
  int removed = 0;

  if(!f) return x_error(X_NULL, EINVAL, fn, "subscriber function parameter is NULL");

  prop_error(fn, rSubscriberLock(redis));
  p = (RedisPrivate *) redis->priv;

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

  prop_error("redisxClearSubscribers", rSubscriberLock(redis));
  p = (RedisPrivate *) redis->priv;
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
  static const char *fn = "redisxSubscribe";

  const ClientPrivate *cp;
  int status = 0;

  if(pattern == NULL) return x_error(X_NULL, EINVAL, fn, "pattern parameter is NULL");

  // connect subscription client as needed.
  prop_error(fn, rConfigLock(redis));
  cp = (ClientPrivate *) redis->subscription->priv;
  if(!cp->isEnabled) status = rConnectSubscriptionClientAsync(redis);
  if(!status) {
    // start processing subscriber responses if not already...
    const RedisPrivate *p = (RedisPrivate *) redis->priv;
    if(!p->isSubscriptionListenerEnabled) rStartSubscriptionListenerAsync(redis);
  }
  rConfigUnlock(redis);
  prop_error(fn, status);

  prop_error(fn, redisxLockConnected(redis->subscription));
  status = redisxSendRequestAsync(redis->subscription, redisxIsGlobPattern(pattern) ? "PSUBSCRIBE" : "SUBSCRIBE", pattern, NULL, NULL);
  redisxUnlockClient(redis->subscription);
  prop_error(fn, status);

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
  static const char *fn = "redisxUnsubscribe";

  int status;

  prop_error(fn, redisxCheckValid(redis));
  prop_error(fn, redisxLockConnected(redis->subscription));

  if(pattern) {
    status = redisxSendRequestAsync(redis->subscription, redisxIsGlobPattern(pattern) ? "PUNSUBSCRIBE" : "UNSUBSCRIBE", pattern, NULL, NULL);
  }
  else {
    status = redisxSendRequestAsync(redis->subscription, "UNSUBSCRIBE", NULL, NULL, NULL);
    if(!status) status = redisxSendRequestAsync(redis->subscription, "PUNSUBSCRIBE", NULL, NULL, NULL);
  }

  redisxUnlockClient(redis->subscription);
  prop_error(fn, status);

  return X_SUCCESS;
}

/// \cond PRIVATE

/**
 * Unsubscribes from all channels, stops the subscription listener thread, and closes the subscription
 * client connection. This call assumes the caller has an exlusive lock on the configuration properties
 * of the affected Redis instance.
 *
 * @param redis     Pointer to a Redis instance.
 * @return          X_SUCCESS (0) if successful or else an error code (&lt;0).
 *
 * @sa redisxEndSubscribe()
 * @sa redisxUnsubscribe()
 */
static int rEndSubscriptionAsync(Redis *redis) {
  static const char *fn = "rEndSubscriptionAsync";

  RedisPrivate *p;
  int status;

  xvprintf("Redis-X> End all subscriptions, and quit listener.\n");

  status = redisxUnsubscribe(redis, NULL);

  prop_error(fn, rConfigLock(redis));
  p = (RedisPrivate *) redis->priv;
  p->isSubscriptionListenerEnabled = FALSE;
  rConfigUnlock(redis);

  rCloseClient(redis->subscription);
  prop_error(fn, status);

  return X_SUCCESS;
}

/// \endcond

/**
 * Unsubscribes from all channels, stops the subscription listener thread, and closes the subscription
 * client connection.
 *
 * @param redis     Pointer to a Redis instance.
 * @return          X_SUCCESS (0) if successful or else an error code (&lt;0).
 *
 * @sa redisxUnsubscribe()
 */
int redisxEndSubscription(Redis *redis) {
  static const char *fn = "redisxEndSubscription";

  int status;

  prop_error(fn, rConfigLock(redis));
  status = rEndSubscriptionAsync(redis);
  rConfigUnlock(redis);
  prop_error(fn, status);

  return X_SUCCESS;
}

static void rNotifyConsumers(Redis *redis, char *pattern, char *channel, char *msg, int length) {
  MessageConsumer *c;
  RedisPrivate *p;
  RedisSubscriberCall *f = NULL;
  int n = 0;

  xdprintf("NOTIFY: %s | %s\n", channel, msg);

  if(rSubscriberLock(redis) != X_SUCCESS) return;
  p = (RedisPrivate *) redis->priv;

  // Count how many matching subscribers there are...
  for(c = p->subscriberList ; c != NULL; c = c->next) {
    if(c->channelStem != NULL) if(strncmp(c->channelStem, channel, strlen(c->channelStem))) continue;
    n++;
  }

  if(n) {
    // Put matching subscribers into an array...
    f = (RedisSubscriberCall *) calloc(n, sizeof(*f));
    x_check_alloc(f);

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
  static long counter, lastError;

  Redis *redis = (Redis *) pRedis;
  RedisPrivate *p;
  RedisClient *cl;
  const ClientPrivate *cp;
  RESP *reply = NULL, **component;
  int i;

  pthread_detach(pthread_self());

  xvprintf("Redis-X> Started processing subsciptions...\n");

  if(redisxCheckValid(redis) != X_SUCCESS) return x_trace_null("RedisSubscriptionListener", NULL);

  p = (RedisPrivate *) redis->priv;
  cl = redis->subscription;
  cp = (ClientPrivate *) cl->priv;

  while(cp->isEnabled && p->isSubscriptionListenerEnabled && pthread_equal(p->subscriptionListenerTID, pthread_self())) {
    // Discard the response from the prior iteration
    if(reply) redisxDestroyRESP(reply);

    // Get the new response...
    reply = redisxReadReplyAsync(cl, NULL);
    if(!reply) continue;

    counter++;

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
      fprintf(stderr, "WARNING! Redis-X : subscriber NULL in component %d\n", (i+1));
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

  if(rConfigLock(redis) == X_SUCCESS) {
    // If we are the current listener thread, then mark the listener as disabled.
    if(pthread_equal(p->subscriptionListenerTID, pthread_self())) p->isSubscriptionListenerEnabled = FALSE;
    rConfigUnlock(redis);
  }

  xvprintf("Redis-X> Stopped processing subscriptions (%ld processed)...\n", counter);

  redisxDestroyRESP(reply);

  return NULL;
}

/// \endcond

/**
 * Starts the PUB/SUB listener thread with the specified thread attributes.
 *
 * \param redis     Pointer to the Redis instance.
 * \param attr      The thread attributes to set for the PUB/SUB listener thread.
 *
 * \return          0 if successful, or -1 if pthread_create() failed.
 *
 */
static int rStartSubscriptionListenerAsync(Redis *redis) {
  RedisPrivate *p = (RedisPrivate *) redis->priv;

#if SET_PRIORITIES
  struct sched_param param;
#endif

  p->isSubscriptionListenerEnabled = TRUE;

  if (pthread_create(&p->subscriptionListenerTID, NULL, RedisSubscriptionListener, redis) == -1) {
    perror("ERROR! Redis-X : pthread_create SubscriptionListener");
    p->isSubscriptionListenerEnabled = FALSE;
    return -1;
  }

#if SET_PRIORITIES
  param.sched_priority = REDISX_LISTENER_PRIORITY;
  pthread_attr_setschedparam(&threadConfig, &param);
  pthread_setschedparam(p->subscriptionListenerTID, SCHED_RR, &param);
#endif

  return 0;
}
