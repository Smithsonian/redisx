![Build Status](https://github.com/Smithsonian/redisx/actions/workflows/build.yml/badge.svg)
![Static Analysis](https://github.com/Smithsonian/redisx/actions/workflows/check.yml/badge.svg)
<a href="https://smithsonian.github.io/redisx/apidoc/html/files.html">
 ![API documentation](https://github.com/Smithsonian/redisx/actions/workflows/dox.yml/badge.svg)
</a>
<a href="https://smithsonian.github.io/redisx/index.html">
 ![Project page](https://github.com/Smithsonian/redisx/actions/workflows/pages/pages-build-deployment/badge.svg)
</a>

<picture>
  <source srcset="resources/CfA-logo-dark.png" alt="CfA logo" media="(prefers-color-scheme: dark)"/>
  <source srcset="resources/CfA-logo.png" alt="CfA logo" media="(prefers-color-scheme: light)"/>
  <img src="resources/CfA-logo.png" alt="CfA logo" width="400" height="67" align="right"/>
</picture>
<br clear="all">


# RedisX

A simple, light-weight C/C++ Redis client.

## Table of Contents

 - [Introduction](#introduction)
 - [Prerequisites](#prerequisites)
 - [Building RedisX](#building-redisx)
 - [Managing Redis server connections](#managing-redis-server-connections)
 - [Simple Redis queries](#simple-redis-queries)
 - [Publish/subscribe (PUB/SUB) support](#publish-subscribe-support)
 - [Atomic execution blocks and LUA scripts](#atomic-transaction-blocks-and-lua-scripts)
 - [Error handling](#error-handling)
 - [Debug support](#debug-support)
 - [Future plans](#future-plans)
 

<a name="introduction"></a>
## Introduction

__RedisX__ is a light-weight [Redis](https://redis.io) client for C/C++. As such, it should also work with Redis forks 
/ clones like [Dragonfly](https://dragonfly.io) or [Valkey](https://valkey.io). It supports both interactive and 
pipelined Redis queries, managing and processing subscriptions, atomic execution blocks, and LUA scripts loading. It 
can be used with multiple Redis servers simultaneously also.

While there are other C/C++ Redis clients available, this one is C90 compatible, and hence can be used on older 
platforms also. It is also small and fast, but still capable and versatile.

Rather than providing high-level support for every possible Redis command (which would probably be impossible given 
the pace new commands are being introduced all the time), it provides a basic framework for synchronous and 
asynchronous queries, with some higher-level functions for managing key/value storage types (including hash tables), 
and PUB/SUB. Future releases may add further higher-level functionality based on demand for such features.

The __RedisX__ library was created, and is maintained, by Attila Kovács at the Center for Astrophysics \| Harvard 
&amp; Smithsonian, and it is available through the [Smithsonian/redisx](https://github.com/Smithsonian/redisx) 
repository on GitHub. 

There are no official releases of __RedisX__ yet. An initial 1.0.0 release is expected in late 2024 or early 2025. 
Before then the API may undergo slight changes and tweaks. Use the repository as is at your own risk for now.

Some related links:

 - [API documentation](https://smithsonian.github.io/redisx/apidoc/html/files.html)
 - [Project page](https://smithsonian.github.io/redisx) on github.io


-----------------------------------------------------------------------------

<a name="prerequisites"></a>
## Prerequisites

The [Smithsonian/xchange](https://github.com/Smithsonian/xchange) library is both a build and a runtime dependency of 
RedisX.

-----------------------------------------------------------------------------

<a name="building-redisx"></a>
## Building RedisX

The RedisX library can be built either as a shared (`libredisx.so[.1]`) and as a static (`libredisx.a`) library, 
depending on what suites your needs best.

You can configure the build, either by editing `config.mk` or else by defining the relevant environment variables 
prior to invoking `make`. The following build variables can be configured:

 - `XCHANGE`: the root of the location where the [Smithsonian/xchange](https://github.com/Smithsonian/xchange) library 
   is installed. It expects to find `xchange.h` under `$(XCHANGE)/include` and `libxchange.so` under `$(XCHANGE)/lib`
   or else in the default `LD_LIBRARY_PATH`.
   
 - `CC`: The C compiler to use (default: `gcc`).

 - `CPPFLAGS`: C pre-processor flags, such as externally defined compiler constants.
 
 - `CFLAGS`: Flags to pass onto the C compiler (default: `-Os -Wall`). Note, `-Iinclude` will be added automatically.
   
 - `LDFLAGS`: Linker flags (default is `-lm`). Note, `-lxchange` will be added automatically.

 - `CHECKEXTRA`: Extra options to pass to `cppcheck` for the `make check` target
 
After configuring, you can simply run `make`, which will build the `shared` (`lib/libxchange.so[.1]`) and `static` 
(`lib/libxchange.a`) libraries, local HTML documentation (provided `doxygen` is available), and performs static
analysis via the `check` target. Or, you may build just the components you are interested in, by specifying the
desired `make` target(s). (You can use `make help` to get a summary of the available `make` targets). 


-----------------------------------------------------------------------------

<a name="managing-redis-server-connections"></a>
## Managing Redis server connections

 - [Initializing](#initializing)
 - [Connecting](#connecting)
 - [Disconnecting](#disconnecting)
 - [Connection hooks](#connection-hooks)

The library maintains up to three separate connections (channels) for each separate Redis server instance used: (1) an 
interactive client for sequential round-trip transactions, (2) a pipeline client for bulk queries and asynchronous 
background processing, and (3) a subscription client for PUB/SUB requests and notifications. The interactive client is 
always connected, the pipeline client is connected only if explicitly requested at the time of establishing the server 
connection, while the subscription client is connected only as needed.

<a name="initializing"></a>
### Initializing

The first step is to create a `Redis` object, with the server name or IP address.

```c 
  // Configure the redis server to connect to "redis.mydomain.com".
  Redis *redis = redisxInit("redis.mydomain.com");
  if (redis == NULL) {
    // Abort: something did not got to plan...
    return;
  }
```

Before connecting to the Redis server, you may configure optional settings, such as the TCP port number to use (if not
the default 6379), and the database authentication password (if any):

```c
  Redis *redis = ...
  
  // (optional) configure a non-standard port number
  redisxSetPort(&redis, 7089);
  
  // (optional) Configure the database password...
  redisxSetPassword(&redis, mySecretPasswordString);
```

You might also tweak the send/receive buffer sizes to use for clients, if you find the socket defaults sub-optimal for
you application:

```c
   // (optional) Set the TCP send/rcv buffer sizes to use if not default values.
   //            This setting applies to all new connections after...
   redisxSetTcpBuf(65536);
```

Optionally you can select the database index to use now (and also later, after connecting), if not the default (index 
0):

```c
  Redis *redis = ...
  
  // Select the database index 2
  redisxSelectDB(redis); 
```

(Note that you can switch the database index any time, with the caveat that it's not possible to change it for the 
subscription client when there are active subscriptions.)

<a name="connecting"></a>
### Connecting

Once configured, you can connect to the server as:

```c
   // Connect to Redis, including a 2nd dedicated client for pipelined requests
   int status = redisxConnect(&redis, TRUE);
   if (status != X_SUCCESS) {
      // Abort: we could not connect for some reason...
      ...
      // Clean up...
      redisxDestroy(&redis);
      ...
   }
```

The above will establish both an interactive connection and a pipelined connection client, for processing both 
synchronous and asynchronous requests (and responses).

<a name="disconnecting"></a>
### Disconnecting

When you are done with a specific Redis server, you should disconnect from it:

```c
  Redis *redis = ...
  
  redisxDisconnect(redis);
```

<a name="connection-hooks"></a>
### Connection hooks

The user of the __RedisX__ library might want to know when connections to the server are established, or when 
disconnections happen, and may want to perform some configuration or clean-up accordingly. For this reason, the 
library provides support for connection 'hooks' -- that is custom functions that are called in the even of connecting 
to or disconnecting from a Redis server.

Here is an example of a connection hook, which simply prints a message about the connection to the console.

```c
  void my_connect_hook(Redis *redis) {
     printf("Connected to Redis server: %s\n", redis->id);
  }
```

And, it can be added to a Redis instance, between the `redisxInit()` and the `redisxConnect()` calls.

```c
  Redis *redis = ...
  
  redisxAddConnectHook(redis, my_connect_hook);
```

The same goes for disconnect hooks, using `redisxAddDisconnectHook()` instead.

-----------------------------------------------------------------------------

<a name="simple-redis-queries"></a>
## Simple Redis queries

 - [RESP data type](#resp-data-type)
 - [Interactive transactions](#interactive-transactions)
 - [Pipelined transactions](#pipelined-transactions)

Redis queries are sent as strings, according the the specification of the Redis protocol. All responses sent back by 
the server using the RESP protocol. Specifically, Redis uses version 2.0 of the RESP protocol (a.k.a. RESP2) by 
default, with optional support for the newer RESP3 introduced in Redis version 6.0. The RedisX library currently
processes the standard RESP2 replies only. RESP3 support to the library may be added in the future (stay tuned...)

<a name="resp-data-type"></a>
### RESP data type
  
All responses coming from the Redis server are represented by a dynamically allocated `RESP` type (defined in 
`redisx.h`) structure. Each `RESP` has a type (e.g. `RESP_SIMPLE_STRING`), an integer value `n`, and a `value` pointer
to further data. If the type is `RESP_INT`, then `n` represents the actual return value (and the `value` pointer is
not used). For string type values `n` is the number of characters in the string `value` (not including termination), 
while for `RESP_ARRAY` types the `value` is a pointer to an embedded `RESP` array and `n` is the number of elements 
in that.

You may check the integrity of a `RESP` using `redisxCheckRESP()`. Since `RESP` data is dynamically allocated, the 
user is responsible for discarding them once they are no longer needed, e.g. by calling `redisxDestroyRESP()`. The
two steps may be combined to automatically discard invalid or unexpected `RESP` data in a single step by calling
`redisxCheckDestroyRESP()`.

```c
  RESP *r = ...
  
  // Let's say we expect 'r' to contain of an embedded RESP array of 3 elements... 
  int status = redisxCheckDestroyRESP(r, RESP_ARRAY, 3);
  if (status != X_SUCCESS) {
     // Oops, 'r' was either NULL, or does not contain a RESP array with 3 elements...
     ...
  }
  else {
     // Process the expected response...
     ...
     redisxDestroyRESP(r);
  }
```

Before destroying a RESP structure, the caller may want to de-reference values within it if they are to be used
as is (without making copies), e.g.:


```c
  RESP *r = ...
  char *stringValue = NULL;   // to be extracted from 'r'
  
  // Let's say we expect 'r' to contain of a simple string response (of whatever length)
  int status = redisxCheckDestroyRESP(r, RESP_SIMPLE_STRING, 0);
  if (status != X_SUCCESS) {
    // Oops, 'r' was either NULL, or it was not a simple string type
    ...
  }
  else {
    // Set 'stringValue' and dereference the value field in the RESP so it's not 
    // destroyed with the RESP itself.
    stringValue = r->value;
    r->value = NULL;
     
    redisxDestroyRESP(r);     // The 'stringValue' is still a valid pointer after! 
  }
```

<a name="interactive-transactions"></a>
### Interactive transactions

The simplest way for running a few Redis queries is to do it in interactive mode:

```c
  Redis *redis = ...
  RESP *resp;
  int status;

  // Send "HGET my_table my_key" request
  resp = redisxRequest(redis, "HGET", "my_table", "my_key", NULL, &status);
  
  // Check return status...
  if (status != X_SUCCESS) {
    // Oops something went wrong...
    ...
  }
  ...
```

The `redisxRequest()` sends a command with up to three arguments. If the command takes fewer than 3 parameters, then
the remaining ones must be set to `NULL`. This function thus offers a simple interface for running most basic 
sequential queries. In cases where 3 parameters are nut sufficient, you may use `redisxArrayRequest()` instead, e.g.:

```c
  ...
  char *args[] = { "my_table", "my_key" };  // parameters as an array...

  // Send "HGET my_table my_key" request with an array of 2 parameters...
  resp = redisxRequest(redis, "HGET", args, NULL, 2, &status);
  ...

```

The 4th argument in the list is an optional `int[]` array defining the individual string lengths of the parameters (if 
need be, or else readily available). Here, we used `NULL` instead, which will use `strlen()` on each supplied 
string-terminated parameter to determine its length automatically. Specifying the length may be necessary if the 
individual parameters are not 0-terminated strings, or else substrings from a continuing string are to be used as 
the parameter value. 

In interactive mode, each request is sent to the Redis server, and the response is collected before the call returns 
with that response (or `NULL` if there was an error). 

<a name="pipelined-transactions"></a>
### Pipelined transactions

Depending on round-trip times over the network, interactive queries may be suitable for running up to a few hundred 
(or a few thousand) queries per second. For higher throughput (up to millions Redis transactions per second) you may 
need to access the Redis database in pipelined mode.

In pipeline mode, requests are sent to the Redis server in quick succession without waiting for responses to return 
for each one individually. Responses are then processed in the background by a designated callback function.

```c
  // Your own function to process responses to pipelined requests...
  void my_resp_processor(RESP *r) {
    // Do what you need to do with the asynchronous responses
    // that come from Redis to bulk requests
    ...
  }
```

Before sending the requests, the user first needs to specify the function to process the responses, e.g.:

```c
  Redis *redis = ...

  redisxSetPipelineConsumer(redis, my_resp_processor);
```

Request are sent via the `redisxSendRequestAsync()` and `redisxSendArrayRequestAsync()` functions. Note, the `Async` 
naming, which indicates the asynchronous nature of this calls -- and which also suggests that these should be called
with the approrpiate mutex locked to prevent concurrency issues.

```c
  Redis *redis = ...

  // We'll use a dedicated pipeline connection for asynchronous requests
  // This way, interactive requests can still be sent independently on the interactive
  // channel independently, if need be.
  RedisClient *pipe = redisxGetClient(redis, PIPELINE_CHANNEL);

  // Check that the client is valid...
  if (pipe == NULL) {
     // Abort: we do not appear to have an active pipeline connection...
     return;
  }

  // Get exclusive access to the pipeline channel, so no other thread may send
  // other requests concurrently...
  redisxLockEnabled(pipe);

  // -------------------------------------------------------------------------
  // Submit a whole bunch of asynchronous requests, e.g. from a loop...
  for (...) {
    int status = redisxSendRequestAsync(pipe, ...);
    if (status != X_SUCCESS) {
      // Oops, that did not go through...
      ...
    }
    else {
      // We probably want to keep a record of what was requested and in what order
      // so our processing function can make sense of the reponses as they arrive
      // (in the same order...)
      ...
    }
  }
  // -------------------------------------------------------------------------
  
  // Release the exclusive lock on the pipeline channel, so
  // other threads may use it now that we sent off our requests...
  redisxUnlockClient(pipe);
```

-----------------------------------------------------------------------------

<a name="accessing-key-value-data"></a>
## Accessing key / value data

 - [Getting and setting keyed values](#getting-and-setting-keyed-values)
 - [Listing and scanning](#listing-and-scanning)

<a name="getting-and-setting-keyed-values"></a>
### Getting and setting keyed values

Key/value pairs are the bread and butter of Redis. They come in two variaties: (1) there are top-level key-value 
pairs, and (2) there are key-value pairs organized into hash tables, where the table name is a top-level key, but the 
fields in the table are not. The RedisX library offers a unified approach for dealing with key/value pairs, whether 
they are top level or hash-tables. Simply, a table name `NULL` is used to refer to top-level keys.

Retrieving individual keyed values is simple:

```c
  Redis *redis = ...;
  int status;    // Variable in which we'll report error status. 
  
  // Get the "property" field from the "system:subsystem" hash table
  RESP *resp = redisxGetValue(redis, "system:subsystem", "property", &status);
  if (status != X_SUCCESS) {
    // Oops something went wrong.
    ...
  }
  
  // Check and process resp
  ...
  
  // Destroy resp
  redisxDestroyRESP(resp);
```

The same goes for top-level keyed values, using `NULL` for the hash table name:

```c
  // Get value for top-level key (not stored in hash table!)
  RESP *resp = redisxGetValue(redis, NULL, "my-key", &status);
```

The reason the return value is a `RESP` pointer, rather than a string is twofold: (1) because it lets you process possible
error responses from Redis also, and (2) because it lets you deal with unterminated string values, such as binary sequences
of known length.

In turn, setting values is also straightforward:

```c
  Redis *redis = ...;
  
  // Set the "property" field in the "system:subsystem" hash table to -2.5
  // using the interactive client connection. 
  int status = redisxSetValue(redis, "system:subsystem", "property", "-2.5", FALSE);
  if (status != X_SUCCESS) {
    // Oops something went wrong.
    ...
  }
```

It's worth noting here, that values in Redis are always represented as 'strings', hence non-string data, such as 
floating-point values, must be converted to strings first. Additionally, the `redisxSetValue()` function works with 
0-terminated string values only, but Redis may also store unterminated byte sequences of known length also. If you 
find that you need to store an unterminated string (such as a binary sequence) as a value, you may just use the 
lower-level `redisxArrayRequest()` instead to process a Redis `GET` or `HGET` command with explicit byte-length 
specifications.

In the above example we have set the value using the interactive client to Redis, which means that the call will
return only after confirmation is received from the server. As such, a subsequent `redisxGetValue()` of the same
table/key will be guaranteed to return the updated value always. However, we could have set the new value 
asynchronously over the pipeline connection (by using `TRUE` as the last argument). In that case, the call will return 
as soon as the request was sent to Redis (but not confirmed, nor possibly transmitted yet!). As such, a subsequent 
`redisxGetValue()` on the same key/value field may race the request in transit, and may return the previous value on 
occasion. So, it's important to remember that while pipelining can make setting multiple Redis fields very efficient, 
we have to be careful about retrieving the same values afterwards from the same program thread. (Arguably, however, 
there should never be a need to query values we set ourselves, since we readily know what they are.)

Finally, if you want to set values for multiple fields in a Redis hash table atomically, you may use 
`redisxMultiSet()`, which provides a high-level interface to the Redis `HMSET` command.

<a name="listing-and-scanning"></a>
### Listing and Scanning

The functions `redisxGetKeys()` and `redisxGetTable()` allow to return the set of Redis keywords or all key/value 
pairs in a table atomically. However, these commands can be computationally expensive for large tables and/or many 
top-level keywords, which means that the Redis server may block for undesirably long times while the result is 
computed.

This is where scanning offers a less selfish (hence much preferred) alternative. Rather than returning all the keys 
or key/value pairs contained in a table atomically at once, it allows to do it bit by bit with byte-sized individual 
transactions that are guaranteed to not block the Redis server long, so it may remain responsive to other queries 
also. For the caller the result is the same, the only difference being that the result is computed via a series of 
quick Redis queries rather than with one potentially very expensive query.

For example, to retrieve all top-level Redis keys, sorted alphabetically, using the scanning approach, you may write 
something like:

```c
  Redis *redis = ...
  
  int nMatches;  // We'll return the number of matching Redis keys here...
  int status;    // We'll return the error status here...
  
  //  Return all redis keywords starting with "system:"
  char **keys = redisxScanKeys(redis, "system:*", &n, &status);
  if (status != X_SUCCESS) {
    // Oops something went wrong...
    ...
  }
  
  // Use 'keys' as appropriate, possibly de-referencing values we want to
  // retain in other persistent data structures...
  ...
  
  // Once done using the 'keys' array, we should destroy it
  redisxDestroyKeys(keys, nMatches);
```

Similarly, to retrieve a set of keywords from a table, matching a glob pattern:

```c
  ...
  
  // Scan all key/value pairs in hash table "system:subsystem"
  RedisEntry *entries = redisxScanTable(redis, "system:subsystem", "*", &nMatches, &status);
  if (status != X_SUCCESS) {
    // Oops something went wrong.
    ... 
  }
  
  // Use 'entries' as appropriate, possibly de-referencing values we want to
  // retain in other persistent data structures...
  ...
  
  // Once done using the 'keys' array, we should destroy it
  redisxDEstroyEntries(entries, nMatches);
```

Finally, you may use `redisxSetScanCount()` to tune just how many results should individial scan queries return. 
Please refer to the Redis documentation on the behavior of the `SCAN` and `HSCAN` commands to learn more. 

-----------------------------------------------------------------------------

<a name="publish-subscribe-support"></a>
## Publish/subscribe (PUB/SUB) support
 
 - [Broadcasting messages](#broadcasting-messages)
 - [Subscriptions](#subscriptions)

<a name="broadcasting-messages"></a>
### Broadcasting messages

```c
  Redis *redis = ...

  // publish a message to the "hello_channel" subscribers.
  int status = redisxPublish(redis, "hello_channel", "Hello world!", 0);
```

The last argument is an optional string length, if readily available, or if sending a substring only (or a string that 
is not 0-terminated). If zero is used, as in the example above, it will automatically determine the length of the
0-terminated string message using `strlen()`.

Alternatively, you may use the `redisxPublishAsync()` instead if you want to publish on a subscription client to which
you have already have exlusive access (e.g. after an appropriate `redisxLockEnabled()` call).

<a name="subscriptions"></a>
### Subscriptions

Subscriptions work conceptually similarly to pipelined requests. To process incoming messages you need to first 
specify one or more `RedisSubscriberCall` functions, which will process PUB/SUB notifications automatically, in the 
background, as soon as they are received. Each `RedisSubscriberCall` can pre-filter the channels for which it receives 
notifications, by defining a channel stem. This way, the given processor function won't even be invoked if a 
notification on a completely different channel arrives. Still, each `RedisSubscriberCall` implementation should 
further check the notifying channel name as appropriate to ensure that it is in fact qualified to deal with a given 
message.

Here is an example `RedisSubscriberCall` implementation to process messages:

```c
  void my_event_processor(const char *pattern, const char *channel, const char *msg, long len) {
    // We'll print the message onto the console
    printf("Incoming message on channel %s: %s\n", channel, msg == NULL ? "<null>" : msg);
  }
```

There are some basic rules (best practices) for message processing. They should be fast, and never block for extended 
periods. If extensive processing is required, or may need to wait extensively for some resource or mutex locking, then
its best that the processing function simply places the incoming message onto a queue, and let a separate background 
thread to the heavy lifting without holding up the subsription processing of other callback routines.

Also, it is important that the call should never attempt to modify or call `free()` on the supplied string arguments, 
since that would interfere with other subscrivber calls.

Once the function is defined, you can activate it via:

```c
  Redis *redis = ...
  
  int status = redisxAddSubscriber(redis, "event:", my_event_processor);
  if (status != X_SUCCESS) {
    // Oops, something went wrong...
    ...
  }
```

We should also start subsribing to specific channels and/or channel patterns (seethe Redis `SUBSCRIBE` and 
`PSUBSCRIBE` commands for details).

```c
  Redis *redis = ...
  
  // We subscribe to all channels that beging with "event:"...
  int status = redisxSubscribe(redis, "event:*");
  if (status != X_SUCCESS) {
    // Oops, something went wrong...
    ...
  }
```

Now, we are capturing and processing all messages published to channels whose name begins with `"event:"`, using our
custom `my_event_processor` function.

To end the subscription, we trace back the same steps by calling `redisxUnsubscribe()` to stop receiving further 
messages to the subscription channel or pattern, and by removing the `my_event_procesor` subscriber function as 
appropriate (provided no other subscription needs it) via `redisxRemoveSubscriber()`.

-----------------------------------------------------------------------------

<a name="atomic-transaction-blocks-and-lua-scripts"></a>
## Atomic execution blocks and LUA scripts

 - [Execution blocks](#execution-blocks)
 - [LUA script loading and execution](#lua-script-loading-and-execution)

Sometimes you want to ececute a series of Redis command atomically, such that nothing else may alter the database 
while the set of commands execute, so they may return a coherent state. For example, you want to set or query a 
collection of related variables so they change together and are reported together. You have two choices. (1) you
can execute the Redis commands in an execution block, or else (2) load a LUA script onto the Redis server and call 
it with some parameters (possibly many times over).

<a name="execution-blocks"></a>
### Execution blocks

Execution blocks offer a fairly simple way of bunching 

```c
  Redis *redis = ...;
  RESP *result;
  
  // Obtain a lock on the client on which to execute the block.
  // e.g. the INTERACTIVE_CHANNEL
  RedisClient *cl = redisxGetClient(redis, INTERACTIVE_CHANNEL);
  
  int status = redisxLockEnabled(cl);
  if (status != X_SUCCESS) {
    // Abort: we don't have exclusive access to the client
    return;
  }

  // -------------------------------------------------------------------------
  // Start an atomic execution block
  redisxStartBlockAsync(cl);
  
  // Send a number of Async requests
  redisxSendRequestAsync(cl, ...);
  ...

  // Execute the block of commands above atomically, and get the resulting RESP
  result = redisxExecBlockAsync(cl);
  // -------------------------------------------------------------------------
  
  // Release exlusive access to the client
  redisxUnlockClient(cl);
  
  // Inspect the RESP, etc...
  ... 
```

If at any point things don't go according to plan in the middle of the block, you can call `redisAbortBlockAsync()` to
abort and discard all prior commands submitted in the execution block already.

<a name="lua-script-loading-and-execution"></a>
### LUA script loading and execution

LUA scripting offers a more capable version of executing more complex routines on the Redis server. LUA is a scripting
language akin to python, and allows you to add extra logic, string manipulation etc to your Redis queries. Best of all, 
once you upload the script to the server, it can reduce network traffic significantly by not having to repeatedly 
submit the same set of Redis commands every single time. LUA scipts also get executed very efficiently on the server, 
and produce only the result you want/need.

Assuming you have prepared your LUA script, you can upload it to the Redis server as:

```c
  Redis *redis = ...
  char *script = ...         // The LUA script as a 0-terminated string.
  char *scriptSHA1 = NULL;   // We'll store the SHA1 sum of the script here

  // Load the script onto the Redis server
  int status = redixLoadScript(redis, script, &scriptSHA);
  if(status != X_SUCCESS) {
    // Oops, something went wrong...
    ...
  }
```

Redis will refer to the script by its SHA1 sum, so it's important keep a record of it. You'll call the script with
its SHA1 sum, a set of redis keys the script may use, and a set of other parameters it might need.

```c
  Redis *redis = ...
  int status;
  
  // Execute the script, with one redis key argument (and no parameters)...
  RESP *r = redisxRequest("EVALSHA", SHA1, "1", "my-redis-key-argument", &status);

  // Check status and inspect RESP
  ...
```

Clearly, if you have additional Redis key arguments and/or parameters to pass to the script, you'll have to use 
`redisxArrayRequest()`, instead.

One thing to keep in mind about LUA scripts is that they are not persistent. They are lost each time the Redis 
server is restarted.


-----------------------------------------------------------------------------

<a name="error-handling"></a>
## Error handling

Error handling of RedisX is an extension of that of __xchange__, with further error codes defined in `redisx.h`. 
The RedisX functions that return an error status (either directly, or into the integer designated by a pointer 
argument), can be inspected by `redisxErrorDescription()`, e.g.:

```c
  Redis *redis ...
  int status = redisxSetValue(...);
  if (status != X_SUCCESS) {
    // Ooops, something went wrong...
    fprintf(stderr, "WARNING! set value: %s", redisErrorDescription(status));
    ...
  }
```
 

-----------------------------------------------------------------------------

<a name="debug-support"></a>
## Debug support

The __xchange__ library provides two macros: `xvprintf()` and `xdprintf()`, for printing verbose and debug messages
to `stderr`. Both work just like `printf()`, but they are conditional on verbosity being enabled via 
`redisxSetVerbose(boolean)` and the global variable `xDebug` being `TRUE` (non-zero), respectively. Applications using 
__RedisX__ may use these macros to produce their own verbose and/or debugging outputs conditional on the same global 
settings. 

You can also turn debug messages by defining the `DEBUG` constant for the compiler, e.g. by adding `-DDEBUG` to 
`CFLAGS` prior to calling `make`. 

-----------------------------------------------------------------------------

<a name="future-plans"></a>
## Future plans

Some obvious ways the library could evolve and grow in the not too distant future:

 - Automated regression testing and coverage tracking.
 - Support for the [RESP3](https://github.com/antirez/RESP3/blob/master/spec.md) standard and Redis `HELLO`.
 - Support for Redis sentinel, high-availability server configurations.
 - TLS support (perhaps...)
 - Add functions for `CLIENT TRACKING` / `CLIENT CACHING` support. 
 - Add more high-level redis commands, e.g. for lists, streams, etc.
 - Improved debug capabilities (e.g. with built-in error traces)
 - Improved error handling (e.g. by consistently setting `errno` beyond just the __RedisX__ error status).

If you have an idea for a must have feature, please let me (Attila) know. Pull requests, for new features or fixes to
existing ones are especially welcome! 
 

