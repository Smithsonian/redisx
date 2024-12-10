![Build Status](https://github.com/Smithsonian/redisx/actions/workflows/build.yml/badge.svg)
![Static Analysis](https://github.com/Smithsonian/redisx/actions/workflows/analyze.yml/badge.svg)
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

A simple, light-weight C/C++ Redis client library.

 - [API documentation](https://smithsonian.github.io/redisx/apidoc/html/files.html)
 - [Project page](https://smithsonian.github.io/redisx) on github.io
 
Author: Attila Kovacs

Last Updated: 10 December 2024

## Table of Contents

 - [Introduction](#introduction)
 - [Prerequisites](#prerequisites)
 - [Building RedisX](#building-redisx)
 - [Linking your application against RedisX](#linking)
 - [Managing Redis server connections](#managing-redis-server-connections)
 - [Simple Redis queries](#simple-redis-queries)
 - [Accessing key / value data](#accessing-key-value-data)
 - [Publish/subscribe (PUB/SUB) support](#publish-subscribe-support)
 - [Atomic execution blocks and LUA scripts](#atomic-transaction-blocks-and-lua-scripts)
 - [Advanced queries and pipelining](#advanced-queries)
 - [Error handling](#error-handling)
 - [Debug support](#debug-support)
 - [Future plans](#future-plans)
 

<a name="introduction"></a>
## Introduction

__RedisX__ is a free, light-weight [Redis](https://redis.io) client library for C/C++. As such, it should work with 
Redis forks / clones like [Dragonfly](https://dragonfly.io) or [Valkey](https://valkey.io) also. It supports both 
interactive and pipelined Redis queries, managing and processing subscriptions, atomic execution blocks, and LUA 
scripts loading. It can be used with multiple Redis servers simultaneously also. __RedisX__ is free to use, in any 
way you like, without licensing restrictions.

While there are other C/C++ Redis clients available, this one is C99 compatible, and hence can be used on older 
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

### Related links

 - [Redis commands](https://redis.io/docs/latest/commands/) (reference documentation)
 - [SMA eXchange (SMA-X)](https://docs.google.com/document/d/1eYbWDClKkV7JnJxv4MxuNBNV47dFXuUWu7C4Ve_YTf0/edit?usp=sharing) 
   -- A structured realtime database built on Redis / Valkey.
   * [Smithsonian/smax-server](https://github.com/Smithsonian/smax-server) -- SMA-X server configuration kit
   * [Smithsonian/smax-clib](https://github.com/Smithsonian/smax-clib) -- A C/C++ client library and toolkit to SMA-X,
     based on __RedisX__
   * [Smithsonian/smax-python](https://github.com/Smithsonian/smax-python) -- A Python 3 client library to SMA-X

-----------------------------------------------------------------------------

<a name="prerequisites"></a>
## Prerequisites

The [Smithsonian/xchange](https://github.com/Smithsonian/xchange) library is both a build and a runtime dependency of 
RedisX.

-----------------------------------------------------------------------------

<a name="building-redisx"></a>
## Building RedisX

The __RedisX__ library can be built either as a shared (`libredisx.so[.1]`) and as a static (`libredisx.a`) library, 
depending on what suits your needs best.

You can configure the build, either by editing `config.mk` or else by defining the relevant environment variables 
prior to invoking `make`. The following build variables can be configured:
   
 - `CC`: The C compiler to use (default: `gcc`).

 - `CPPFLAGS`: C preprocessor flags, such as externally defined compiler constants.
 
 - `CFLAGS`: Flags to pass onto the C compiler (default: `-g -Os -Wall`). Note, `-Iinclude` will be added 
   automatically.
   
 - `CSTANDARD`: Optionally, specify the C standard to compile for, e.g. `c99` to compile for the C99 standard. If
   defined then `-std=$(CSTANDARD)` is added to `CFLAGS` automatically.
   
 - `WEXTRA`: If set to 1, `-Wextra` is added to `CFLAGS` automatically.
   
 - `LDFLAGS`: Extra linker flags (default is _not set_). Note, `-lm -lxchange` will be added automatically.

 - `CHECKEXTRA`: Extra options to pass to `cppcheck` for the `make check` target
 
 - `XCHANGE`: If the [Smithsonian/xchange](https://github.com/Smithsonian/xchange) library is not installed on your
   system (e.g. under `/usr`) set `XCHANGE` to where the distribution can be found. The build will expect to find 
   `xchange.h` under `$(XCHANGE)/include` and `libxchange.so` / `libxchange.a` under `$(XCHANGE)/lib` or else in the 
   default `LD_LIBRARY_PATH`.
 
After configuring, you can simply run `make`, which will build the `shared` (`lib/libredisx.so[.1]`) and `static` 
(`lib/libredisx.a`) libraries, local HTML documentation (provided `doxygen` is available), and performs static
analysis via the `check` target. Or, you may build just the components you are interested in, by specifying the
desired `make` target(s). (You can use `make help` to get a summary of the available `make` targets). 

After building the library you can install the above components to the desired locations on your system. For a 
system-wide install you may simply run:

```bash
  $ sudo make install
```

Or, to install in some other locations, you may set a prefix and/or `DESTDIR`. For example, to install under `/opt` 
instead, you can:

```bash
  $ sudo make prefix="/opt" install
```

Or, to stage the installation (to `/usr`) under a 'build root':

```bash
  $ make DESTDIR="/tmp/stage" install
```

-----------------------------------------------------------------------------

<a name="linking"></a>
## Linking your application against RedisX

Provided you have installed the shared (`libredisx.so` and `libxchange.so`) or static (`libredisx.a` and 
`libxchange.a`) libraries in a location that is in your `LD_LIBRARY_PATH` (e.g. in `/usr/lib` or `/usr/local/lib`) 
you can simply link your program using the  `-lredisx -lxchange` flags. Your `Makefile` may look like: 

```make
myprog: ...
	cc -o $@ $^ $(LDFLAGS) -lredisx -lxchange 
```

(Or, you might simply add `-lredisx -lxchange` to `LDFLAGS` and use a more standard recipe.) And, in if you installed 
the __RedisX__ and/or __xchange__ libraries elsewhere, you can simply add their location(s) to `LD_LIBRARY_PATH` prior 
to linking.


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
the default 6379), and the database authentication (if any):

```c
  Redis *redis = ...
  
  // (optional) configure a non-standard port number
  redisxSetPort(redis, 7089);
  
  // (optional) Configure the database user (since Redis 6.0, using ACL)
  redisxSetUser(redis, "johndoe"); 
  
  // (optional) Configure the database password...
  redisxSetPassword(redis, mySecretPasswordString);
```

You can also set the RESP protocol to use (provided your server is compatible with Redis 6 or later):

```c
  // (optional) Use RESP3 (provided the server supports it)
  redisxSetProtocol(redis, REDISX_RESP3);
```

The above call will use the `HELLO` command (since Redis 6) upon connecting. If you do not set the protocol, `HELLO` 
will not be used, and RESP2 will be assumed -- which is best for older servers. (Note, that you can always check the 
actual protocol used after connecting, using `redisxGetProtocol()`). Note, that after connecting, you may retrieve 
the set of server properties sent in response to `HELLO` using `redisxGetHelloData()`.

You might also tweak the socket options used for clients, if you find the socket defaults sub-optimal for your 
application (note, that this setting is common to all `Redis` instances managed by the library):

```c
   // (optional) Set 1000 ms socket read/write timeout for future connections.
   redisxSetSocketTimeout(redis, 1000);

   // (optional) Set the TCP send/rcv buffer sizes to use if not default values.
   //            This setting applies to all new connections after...
   redisxSetTcpBuf(65536);
```

Optionally, you can select the database index to use now (or later, after connecting), if not the default (index 
0):

```c
  Redis *redis = ...
  
  // (optional) Select the database index 2
  redisxSelectDB(redis, 2); 
```

Note, that you can switch the database index any time, with the caveat that it's not possible to change it for the 
subscription client when there are active subscriptions.


<a name="connecting"></a>
### Connecting

Once configured, you can connect to the server as:

```c
   Redis *redis = ...

   // Connect to Redis, including a 2nd dedicated client for pipelined requests
   int status = redisxConnect(redis, TRUE);
   if (status != X_SUCCESS) {
      // Abort: we could not connect for some reason...
      ...
      // Clean up...
      redisxDestroy(redis);
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

And then to free up all resources used by the `Redis` instance, you might also call

```c
  // Destroy the Redis instance and free up resources
  redisxDestroy(redis);
  
  // Set the destroyed Redis instance pointer to NULL, as best practice.
  redis = NULL;
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

 - [Interactive transactions](#interactive-transactions)
 - [Push notifications](#push-notifications)
 - [RESP data type](#resp-data-type)


Redis queries are sent as strings, according the the specification of the Redis protocol. All responses sent back by 
the server using the RESP protocol. Specifically, Redis uses version 2 of the RESP protocol (a.k.a. RESP2) by 
default, with optional support for the newer RESP3 introduced in Redis version 6.0. The __RedisX__ library provides
support for both RESP2 and RESP3.


<a name="interactive-transactions"></a>
### Interactive transactions

The simplest way for running a few Redis queries is to do it in interactive mode:

```c
  Redis *redis = ...
  RESP *resp;     // This will be the pointer we receive to the Redis response
  int status;     // execution status to be populated.

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
sequential queries. In cases where 3 parameters are not sufficient, you may use `redisxArrayRequest()` instead, e.g.:

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
individual parameters are not 0-terminated strings.

In interactive mode, each request is sent to the Redis server, and the response is collected before the call returns 
with that response (or `NULL` if there was an error).

<a name="push-notifications"></a>
### Push notifications

Redis 6 introduced out-of-band push notifications along with RESP3. It allows the server to send messages to any 
connected client that are not in response to a query. For example, Redis 6 allows `CLIENT TRACKING` to use such push 
notifications (e.g. `INVALIDATE foo`), to notify connected clients when a watched variable has been updated from
somewhere else. 

__RedisX__ allows you to specify a custom callback `RedisPushProcessor` function to handle such push notifications,
e.g.:

```c
  void my_push_processor(RESP *message, void *ptr) {
    char *owner = (char *) ptr;  // Additional argument we need, in this case a string.
    printf("[%s] Got push message: type %c, n = %d.\n", owner, message->type, message->n);
  }
```

Then you can activate the processing of push notifications with `redisxSetPushProcessor()`. You can specify the 
optional additional data that you want to pass along to the push processor function -- just make sure that the data 
has a sufficient scope / lifetime such that it is valid at all times while push messages are being processed. E.g.

```c
  static owner = "my process"; // The long life data we want to pass to my_push_processor...
  
  // Use my_push_processor and pass along the owner as a parameter
  redisxSetPushProcessor(redis, my_push_processor, owner);
```

There are some things to look out for in your `RedisPushProcessor` implementation:

- The call should not block (except perhaps for a quick mutex lock) and should return quickly. If blocking calls, or
  extensive processing is required, you should place a copy of the PUSH notification onto a queue and let an 
  asynchronous thread take it from there.
- The call should not attempt to alter or destroy the push message. If needed it can copy parts or the whole.
- You should not attempt to lock or release clients from the call. If you need access to a client, it's best to put a 
  copy of the RESP notification onto a queue and let an asynchronous thread deal with it.
- You should


<a name="resp-data-type"></a>
### RESP data type
  
All responses coming from the Redis server are represented by a dynamically allocated `RESP` type (defined in 
`redisx.h`) structure. 

```c
typedef struct RESP {
  char type;                    // RESP type: RESP_ARRAY, RESP_INT ...
  int n;                        // Either the integer value of a RESP_INT response, or the 
                                // dimension of the value field.
  void *value;                  // Pointer to text (char *) content or to an array of components 
                                // (RESP **)
} RESP;
```

whose contents are:

 | RESP `type`             | Redis ID | `n`                           |`value` cast in C      |
 |-------------------------|----------|-------------------------------|-----------------------|
 | `RESP_ARRAY`            |   `*`    | number of `RESP *` pointers   | `(RESP **)`           |
 | `RESP_INT`              |   `:`    | integer return value          | `(void)`              |
 | `RESP_SIMPLE_STRING`    |   `+`    | string length                 | `(char *)`            |
 | `RESP_ERROR`            |   `-`    | total string length           | `(char *)`            |
 | `RESP_BULK_STRING`      |   `$`    | string length or -1 if `NULL` | `(char *)`            |
 | `RESP3_NULL`            |   `_`    | 0                             | `(void)`              |
 | `RESP3_BOOLEAN`         |   `#`    | 1 if _true_, 0 if _false_     | `(void)`              |
 | `RESP3_DOUBLE`          |   `,`    | _unused_                      | `(double *)`          |
 | `RESP3_BIG_NUMBER`      |   `(`    | string representation length  | `(char *)`            |
 | `RESP3_BLOB_ERROR`      |   `!`    | total string length           | `(char *)`            | 
 | `RESP3_VERBATIM_TEXT`   |   `=`    | text length (incl. type)      | `(char *)`            |
 | `RESP3_SET`             |   `~`    | number of `RESP *` pointers   | `(RESP *)`            |
 | `RESP3_MAP`             |   `%`    | number of key / value pairs   | `(RedisMapEntry *)`   |
 | `RESP3_ATTRIBUTE`       |   `\|`   | number of key / value pairs   | `(RedisMapEntry *)`   |
 | `RESP3_PUSH`            |   `>`    | number of `RESP *` pointers   | `(RESP **)`           | 
 

Each `RESP` has a type (e.g. `RESP_SIMPLE_STRING`), an integer value `n`, and a `value` pointer
to further data. If the type is `RESP_INT`, then `n` represents the actual return value (and the `value` pointer is
not used). For string type values `n` is the number of characters in the string `value` (not including termination), 
while for `RESP_ARRAY` types the `value` is a pointer to an embedded `RESP` array and `n` is the number of elements 
in that.

To help with deciding what cast to use for a given `value` field of the RESP data structure, we provide the 
convenience methods `redisxIsScalarType()`, `redisxIsStringType()`, `redisxIsArrayType()`, and `redisxIsMapType()`
functions.

You can check that two `RESP` data structures are equivalent with `redisxIsEqualRESP(RESP *a, RESP *b)`.

You may also check the integrity of a `RESP` using `redisxCheckRESP()`. Since `RESP` data is dynamically allocated, 
the user is responsible for discarding them once they are no longer needed, e.g. by calling `redisxDestroyRESP()`. 
The two steps may be combined to automatically discard invalid or unexpected `RESP` data in a single step by calling
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

Before destroying a RESP structure, the caller may want to dereference values within it if they are to be used as is 
(without making copies), e.g.:


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
    stringValue = (char *) r->value;
    r->value = NULL;
     
    redisxDestroyRESP(r);     // 'stringValue' is still a valid pointer after! 
  }
```

Note, that you can usually convert a RESP to an `XField`, and/or to JSON representation using the 
`redisxRESP2XField()` and `redisxRESP2JSON()` functions, e.g.:

```c
 Redis redis = ...
 
 // Obtain a copy of the response received from HELLO upon connecting...
 RESP *resp = redisxGetHelloData(redis);
 
 // Print the response from HELLO to the standard output in JSON format
 char *json = redisxRESP2JSON("hello_response", resp);
 if(json != NULL) {
   printf("%s", json);
   free(json);
 }
   
 ...
 
 // Clean up
 redisxDestroyRESP(resp);
```

All RESP can be represented in JSON format. This is trivial for map entries, which have strings as their keywords -- 
which is the case for all RESP sent by Redis. And, it is also possible for map entries with non-string keys, albeit 
via a more tedious (and less standard) JSON representation, stored under the `.non-string-keys` keyword.


-----------------------------------------------------------------------------

<a name="accessing-key-value-data"></a>
## Accessing key / value data

 - [Getting and setting keyed values](#getting-and-setting-keyed-values)
 - [Listing and scanning](#listing-and-scanning)

<a name="getting-and-setting-keyed-values"></a>
### Getting and setting keyed values

Key/value pairs are the bread and butter of Redis. They come in two varieties: (1) there are top-level key-value 
pairs, and (2) there are key-value pairs organized into hash tables, where the table name is a top-level key, but the 
fields in the table are not. The RedisX library offers a unified approach for dealing with key/value pairs, whether 
they are top level or hash-tables. Simply, a table name `NULL` is used to refer to top-level keys.

Retrieving individual keyed values is simple:

```c
  Redis *redis = ...;
  int len; // Variable in which we return the length of the value or an error code 
  
  // Get the "property" field from the "system:subsystem" hash table
  char *value = redisxGetStringValue(redis, "system:subsystem", "property", &len);
  if (len < 0) {
    // Oops something went wrong.
    ...
  }
  
  ...
  
  // Discard the value once it's no longer needed.
  if(value) free(value);
```

The same goes for top-level keyed values, using `NULL` for the hash table name:

```c
  // Get value for top-level key (not stored in hash table!)
  char *value = redisxGetStringValue(redis, NULL, "my-key", &len);
```

Alternatively, you can get the value as an undigested `RESP`, using `redisxGetValue()` instead, which allows you to
check and inspect the response in more detail (e.g. to check for potential errors, or unexpected response types).

Setting values is straightforward also:

```c
  Redis *redis = ...;
  
  // Set the "property" field in the "system:subsystem" hash table to -2.5
  // using the interactive client connection, without requiring confirmation. 
  int status = redisxSetValue(redis, "system:subsystem", "property", "-2.5", FALSE);
  if (status != X_SUCCESS) {
    // Oops something went wrong.
    ...
  }
```

It's worth noting here, that values in Redis are always represented as strings, hence non-string data, such as 
floating-point values, must be converted to strings first. Additionally, the `redisxSetValue()` function works with 
0-terminated string values only, but Redis allows storing unterminated byte sequences of known length also. If you 
find that you need to store an unterminated string (such as a binary sequence) as a value, you may just use the 
lower-level `redisxArrayRequest()` instead to process a Redis `SET` or `HSET` command with explicit byte-length 
specifications.

In the above example we have set the value using the interactive client to Redis, which means that the call will
return only after confirmation or result is received back from the server. As such, a subsequent `redisxGetValue()` of 
the same table/key will be guaranteed to return the updated value always. 

However, we could have set the new value asynchronously over the pipeline connection (by using `TRUE` as the last 
argument). In that case, the call will return as soon as the request was sent to Redis (but not confirmed, nor 
possibly transmitted yet!). As such, a subsequent `redisxGetValue()` on the same key/value field may race the request 
in transit, and may return the previous value on occasion. So, it's important to remember that while pipelining can 
make setting multiple Redis fields very efficient, we have to be careful about retrieving the same values afterwards 
from the same program thread. (Arguably, however, there should never be a need to query values we set ourselves, since 
we readily know what they are.)

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
also. For the caller the result is the same (notwithstanding the atomicity), except that the result is computed via a 
series of quick Redis queries rather than with one potentially very expensive query.

For example, to retrieve all top-level Redis keys, sorted alphabetically, using the scanning approach, you may write 
something like:

```c
  Redis *redis = ...
  
  int nMatches;  // We'll return the number of matching Redis keys here...
  int status;    // We'll return the error status here...
  
  //  Return all Redis top-level keywords starting with "system:"
  char **keys = redisxScanKeys(redis, "system:*", &nMatches, &status);
  if (status != X_SUCCESS) {
    // Oops something went wrong...
    ...
  }
  
  // Use 'keys' as appropriate, possibly dereferencing values we want to
  // retain in other persistent data structures...
  ...
  
  // Once done using the 'keys' array, we should destroy it
  redisxDestroyKeys(keys, nMatches);
```

Or, to retrieve the values from a hash table for a set of keywords that match a glob pattern:

```c
  ...
  
  // Scan all key/value pairs in hash table "system:subsystem"
  RedisEntry *entries = redisxScanTable(redis, "system:subsystem", "*", &nMatches, &status);
  if (status != X_SUCCESS) {
    // Oops something went wrong.
    ... 
  }
  
  // Use 'entries' as appropriate, possibly dereferencing values we want to
  // retain in other persistent data structures...
  ...
  
  // Once done using the 'keys' array, we should destroy it
  redisxDestroyEntries(entries, nMatches);
```

Finally, you may use `redisxSetScanCount()` to tune just how many results should individual scan queries should return 
(but only if you are really itching to tweak it). Please refer to the Redis documentation on the behavior of the 
`SCAN` and `HSCAN` commands to learn more. 

-----------------------------------------------------------------------------

<a name="publish-subscribe-support"></a>
## Publish/subscribe (PUB/SUB) support
 
 - [Broadcasting messages](#broadcasting-messages)
 - [Subscriptions](#subscriptions)

<a name="broadcasting-messages"></a>
### Broadcasting messages

It is simple to send messages to subscribers of a given channel:

```c
  Redis *redis = ...

  // publish a message to the "hello_channel" subscribers.
  int status = redisxPublish(redis, "hello_channel", "Hello world!", 0);
```

The last argument is an optional string length, if readily available, or if sending a byte sequence that is not 
null-terminated. If zero is used for the length, as in the example above, it will automatically determine the length 
of the 0-terminated string message using `strlen()`.

Alternatively, you may use the `redisxPublishAsync()` instead if you want to publish on a subscription client to which
you have already have exclusive access (e.g. after an appropriate `redisxLockConnected()` call).

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
thread do the heavy lifting without holding up the subscription processing of other callback routines, or without losing
responsiveness to other incoming messages.

Also, it is important that the call should never attempt to modify or call `free()` on the supplied string arguments, 
since that would interfere with other subscriber calls.

Once the function is defined, you can activate it via:

```c
  Redis *redis = ...
  
  int status = redisxAddSubscriber(redis, "event:", my_event_processor);
  if (status != X_SUCCESS) {
    // Oops, something went wrong...
    ...
  }
```

We should also start subscribing to specific channels and/or channel patterns.

```c
  Redis *redis = ...
  
  // We subscribe to all channels that beging with "event:"...
  int status = redisxSubscribe(redis, "event:*");
  if (status != X_SUCCESS) {
    // Oops, something went wrong...
    ...
  }
```

The `redisxSuibscribe()` function will translate to either a Redis `PSUBSCRIBE` or `SUBSCRIBE` command, depending 
whether the `pattern` argument contains globbing patterns or not (respectively).

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
 - [Custom Redis functions](#custom-functions)

Sometimes you may want to execute a series of Redis command atomically, such that nothing else may alter the database 
while the set of commands execute, so that related values are always in a coherent state. For example, you want to set 
or query a collection of related variables so they change together and are reported together. You have two choices. 
(1) you can execute the Redis commands in an execution block, or else (2) load a LUA script onto the Redis server and 
call it with some parameters (possibly many times over).

<a name="execution-blocks"></a>
### Execution blocks

Execution blocks offer a fairly simple way of bunching together a set of Redis commands that need to be executed 
atomically. Such an execution block in __RedisX__ may look something like:

```c
  Redis *redis = ...;
  RESP *result;
  
  // Obtain a lock on the client on which to execute the block.
  // e.g. the interactive client channel.
  RedisClient *cl = redisxGetLockedConnectedClient(redis, REDISX_INTERACTIVE_CHANNEL);
  if (cl == NULL) {
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
abort and discard all prior commands submitted in the execution block already. It is important to remember that every
time you call `redisxStartBlockAsync()`, you must call either `redisxExecBlockAsync()` to execute it or else 
`redisxAbortBlockAsync()` to discard it. Failure to do so, will effectively end you up with a hung Redis client.


<a name="lua-script-loading-and-execution"></a>
### LUA script loading and execution

[LUA](https://www.lua.org/) scripting offers a more capable version of executing complex routines on the Redis server. 
LUA is a scripting language akin to python, and allows you to add extra logic, string manipulation etc. to your Redis 
queries. Best of all, once you upload the script to the server, it can reduce network traffic significantly by not 
having to repeatedly submit the same set of Redis commands every single time. LUA scripts also get executed very 
efficiently on the server, and produce only the result you want/need without returning unnecessary intermediates.

Assuming you have prepared your LUA script appropriately, you can upload it to the Redis server as:

```c
  Redis *redis = ...
  char *script = ...         // The LUA script as a 0-terminated string.
  char *scriptSHA1 = NULL;   // We'll store the SHA1 sum of the script here

  // Load the script onto the Redis server
  int status = redixLoadScript(redis, script, &scriptSHA1);
  if(status != X_SUCCESS) {
    // Oops, something went wrong...
    ...
  }
```

Redis will refer to the script by its SHA1 sum, so it's important keep a record of it. You'll call the script with
its SHA1 sum, a set of Redis keys the script may use, and a set of other parameters it might need.

```c
  Redis *redis = ...
  
  char *keyArgs[] = { "my-redis-key-argument", NULL }; // NULL-terminated array of keyword arguments
  char *params[] = { "some-string", "-1.11", NULL };   // NULL-terminated array of extra parameters
  
  // Execute the script, with the specified keyword arguments and parameters
  RESP *reply = redisxRunScript(redis, scriptSHA1, keyArgs, params);

  // Check and inspect the reply
  ...
```

Or, you can use `redisxRunScriptAsync()` instead to send the request to run the script, and then collect the response 
later, as appropriate.

One thing to keep in mind about LUA scripts is that they are not fully persistent. They will be lost each time the 
Redis server is restarted.


<a name="custom-functions"></a>
### Custom Redis functions

Functions, introduced in Redis 7, offer another evolutionary step over the LUA scripting described above. Unlike 
scripts, functions are persistent and they can be called by name rather than a cryptic SHA1 sum. Otherwise, they offer
more or less the same functionality as scripts. __RedisX__ does not currently have a built-in high-level support
for managing and calling user-defined functions, but it is a feature that may be added in the not-too-distant future.
Stay tuned.


-----------------------------------------------------------------------------

<a name="advanced-queries"></a>
## Advanced queries and pipelining

 - [Asynchronous client processing](#asynchronous-client-processing)
 - [Bundled Attributes](#attributes)
 - [Pipelined transactions](#pipelined-transactions)


<a name="asynchronous-client-processing"></a>
### Asynchronous client processing

Sometimes you might want to micro manage how requests are sent and responses to them are received. __RedisX__ provides 
a set of asynchronous client functions that do that. (You've seen these already further above in the 
[Pipelined transaction](#pipelined-transactions) section.) These functions should be called with the specific client's 
mutex locked, to ensure that other threads do not interfere with your sequence of requests and responses. E.g.:

```c
  // Obtain the appropriate client with an exclusive lock on it.
  RedisClient *cl = redisxGetLockedConnectedClient(...);
  if (cl == NULL) {
    // Abort: no such client is connected
    return;
  }
  
  // Now send commands, and receive responses as you like using the redisx...Async() calls
  ...
  
  // When done, release the lock
  redisxUnlockClient(cl);
```

While you have the exclusive lock you may send any number of requests, e.g. via `redisxSendRequestAsync()` and/or 
`redixSendArrayRequestAsync()`. Then collect replies either with `redisxReadReplyAsync()` or else 
`redisxIgnoreReplyAsync()`. For example, the basic anatomy of sending a single request and then receiving a response, 
while we have exclusive access to the client, might look something like this:

```c
  ...
  // Send a command to Redis
  int status = redisxSendRequestAsync(cl, ...);
  
  if(status == X_SUCCESS) {
    // Read the response
    RESP *reply = redisxReadReplyAsync(cl);
    
    // check and process the response
    if(redisxCheckRESP(reply, ...) != X_SUCCESS) {
      // Ooops, not the reply what we expected...
      ...
    } 
    else {
      // Process the response
      ...
    }
    
    // Destroy the reply
    redisxDestroyRESP(reply);
  }
  ...
```
   
For the best performance, you may want to leave the processing of the replies until after you unlock the client. I.e.,
you only block other threads from accessing the client while you send off the requests and collect the corresponding 
responses. You can then analyze the responses at your leisure outside of the mutexed section.
   
In some cases you may be OK with just firing off some Redis commands, without necessarily caring about responses. 
Rather than ignoring the replies with `redisxIgnoreReplyAsync()` you might call `redisxSkipReplyAsync()` instead 
__before__ `redisxSendRequestAsync()` to instruct Redis to not even bother about sending a response to your request 
(it saves time and network bandwidth!):

```c
 // We don't want to receive a response to our next command... 
 int status = redisxSkipReplyAsync(cl);
 
 if (status == X_SUCCESS) {
   // Now send the request...
   status = redisxSendRequest(cl, ...);
 }
 
 if (status != X_SUCCESS) {
    // Ooops, the request did not go through...
    ...
 }
```

Of course you can build up arbitrarily complex set of queries and deal with a set of responses in different ways. Do 
what works best for your application.


<a name="attributes"></a>
### Bundled Attributes

As of Redis 6, the server might send ancillary data along with replies, if the RESP3 protocol is used. These are 
collected together with the expected responses. However, these optional attributes are not returned to the user 
automatically. Instead, the user may retrieve attributes directly after getting a response from 
`redisxReadReplyAsync()` using `redisxGetAttributesAsync()`. And, attributes that were received previously can be
discarded with `redisxClearAttributesAsync()`. For example,

```c
  RedisClient *cl = ...	 // The client we use for our transactions
  
  if(redisxLockEnabled(cl) == X_SUCCESS) {
    ...
    
    // Clear any prior attributes we may have previously received for the client...
    redisxClearAttributesAsync(cl);
    
    // Read a response for a request we sent earlier...
    RESP *reply = redisxReadReplyAsync(cl);
    
    // Retrieve the attributes (if any) that were sent with the response.
    RESP *attributes = redisxGetAttributes(cl);
    
    ...
    
    redisxUnlockClient(cl);
  }
```


<a name="pipelined-transactions"></a>
### Pipelined transactions

Depending on round-trip times over the network, interactive queries may be suitable for running up to a few thousand 
queries per second. For higher throughput (up to ~1 million Redis transactions per second) you may need to access the 
Redis database in pipelined mode. RedisX provides a dedicated pipeline client/channel to the Redis server (provided
the option to enable it has been used when `redixConnect()` was called).

In pipeline mode, requests are sent to the Redis server over the pipeline client in quick succession, without waiting 
for responses to return for each one individually. Responses are processed in the background by a designated callback 
function (or else discarded if no callback function has been set). This is what a callback function looks like:

```c
  // Your own function to process responses to pipelined requests...
  void my_resp_processor(RESP *r) {
    // Do what you need to do with the asynchronous responses
    // that come from Redis to bulk requests.
    ...
  }
```

It is important to note that the processing function should not call `free` on the `RESP` pointer argument, but it may 
dereference and use parts of it as appropriate (just remember to set the bits referenced elsewhere to `NULL` so they 
do not get destroyed when the pipeline listener destroys the `RESP` after your function is done processing it).
Before sending the pipelined requests, the user first needs to specify the function to process the responses, e.g.:

```c
  Redis *redis = ...

  redisxSetPipelineConsumer(redis, my_resp_processor);
```

Request are sent via the `redisxSendRequestAsync()` and `redisxSendArrayRequestAsync()` functions. Note again, the 
`Async` naming, which indicates the asynchronous nature of this calls -- and which indicates that these functions 
should be called with the appropriate mutex locked to prevent concurrency issues, and to maintain a predictable order 
(very important!) for processing the responses.

```c
  Redis *redis = ...

  // We'll use a dedicated pipeline connection for asynchronous requests
  // This way, interactive requests can still be sent independently on the interactive
  // channel independently, if need be.
  RedisClient *pipe = redisxGetLockedConnectedClient(redis, REDISX_PIPELINE_CHANNEL);

  // Check that the client is valid...
  if (pipe == NULL) {
     // Abort: we do not appear to have an active pipeline connection...
     return;
  }

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

It is important to remember that on the pipeline client you should never try to process responses directly from the 
same function from which commands are being sent. That's what the interactive connection is for. Pipeline responses 
are always processed by a background thread (or, if you don't specify your callback function they will be discarded). 
The only thing your callback function can count on is that the same number of responses will be received as the number 
of asynchronous requests that were sent out, and that the responses arrive in the same order as the order in which the 
requests were sent. 

It is up to you and your callback function to keep track of what responses are expected and in what order. Some best 
practices to help deal with pipeline responses are summarized here:

 - Use `redisxSkipReplyAsync()` prior to sending pipeline requests for which you do not need a response. (This way
   your callback does not have to deal with unnecessary responses at all.
 
 - For requests that return a value, keep a record (in a FIFO) of the expected types and your data that depends on 
   the content of the responses. For example, for pipelined `HGET` commands, your FIFO should have a record that
   specifies that a bulk string response is expected, and a pointer to data which is used to store the returned value 
   -- so that you pipeline response processing callback function can check that the response is the expected type
   (and size) and knows to assign/process the response appropriately to your application data.
 
 - You may insert Redis `PING`/`ECHO` commands to section your responses, or to provide directives to your pipeline
   response processor function. You can tag them uniquely so that the echoed responses can be parsed and interpreted
   by your callback function. For example, you may send a `PING`/`ECHO` commands to Redis with the tag 
   `"@my_resp_processor: START sequence A"`, or something else meaningful that you can uniquely distinguish from all
   other responses that you might receive.
  
__RedisX__ optimizes the pipeline client for high throughput (bandwidth), whereas the interactive and subscription 
clients are optimized for low-latency, at the socket level.
  
-----------------------------------------------------------------------------

<a name="error-handling"></a>
## Error handling

The principal error handling of __RedisX__ is an extension of that of __xchange__, with further error codes defined in 
`redisx.h`. The __RedisX__ functions that return an error status (either directly, or into the integer designated by a 
pointer argument), can be inspected by `redisxErrorDescription()`, e.g.:

```c
  Redis *redis ...
  int status = redisxSetValue(...);
  if (status != X_SUCCESS) {
    // Ooops, something went wrong...
    fprintf(stderr, "WARNING! set value: %s", redisErrorDescription(status));
    ...
  }
```
 
In addition you can define your own handler function to deal with transmission (send/receive) errors, by defining
your own `RedisErrorHandler` function, such as:

```c
  void my_error_handler(Redis *redis, enum redisx_channel channel, const char *op) {
    fprintf(stderr, "ERROR! %s: Redis at %s, channel %d\n", op, redis->id, channel);
  }
```

Then activate it as:

```c
  Redis *redis = ...
  
  redisxSetTransmitErrorHandler(redis, my_error_handler);
```

After that, every time there is an error with sending or receiving packets over the network to any of the Redis
clients used, your handler will report it the way you want it.

-----------------------------------------------------------------------------

<a name="debug-support"></a>
## Debug support

You can enable verbose output of the __RedisX__ library with `xSetVerbose(boolean)`. When enabled, it will produce 
status messages to `stderr`so you can follow what's going on. In addition (or alternatively), you can enable debug 
messages with `xSetDebug(boolean)`. When enabled, all errors encountered by the library (such as invalid arguments 
passed) will be printed to `stderr`, including call traces, so you can walk back to see where the error may have 
originated from. (You can also enable debug messages by default by defining the `DEBUG` constant for the compiler, 
e.g. by adding `-DDEBUG` to `CFLAGS` prior to calling `make`). 

In addition, you can use `redisxDebugTraffic(boolean)` to debug low-level traffic to/from the Redis server, which 
prints excerpts of all incoming and outgoing messages from/to Redis to `stderr`.

For helping to debug your application, the __xchange__ library provides two macros: `xvprintf()` and `xdprintf()`, 
for printing verbose and debug messages to `stderr`. Both work just like `printf()`, but they are conditional on 
verbosity being enabled via `xSetVerbose(boolean)` and `xSetDebug(boolean)`, respectively. Applications using 
__RedisX__ may use these macros to produce their own verbose and/or debugging outputs conditional on the same global 
settings. 



-----------------------------------------------------------------------------

<a name="future-plans"></a>
## Future plans

Some obvious ways the library could evolve and grow in the not too distant future:

 - Automated regression testing and coverage tracking.
 - Support for [Redis Sentinel](https://redis.io/docs/latest/develop/reference/sentinel-clients/) clients, for 
   high-availability server configurations.
 - Keep track of subscription patterns, and automatically resubscribe to them on reconnecting.
 - TLS support (perhaps...)
 - Add high-level support for managing and calling custom Redis functions.
 - Add more high-level [Redis commands](https://redis.io/docs/latest/commands/), e.g. for lists, streams, etc.

If you have an idea for a must have feature, please let me (Attila) know. Pull requests, for new features or fixes to
existing ones, are especially welcome! 
 
-----------------------------------------------------------------------------
Copyright (C) 2024 Attila Kovács
