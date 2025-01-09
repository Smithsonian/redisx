<img src="/redisx/resources/CfA-logo.png" alt="CfA logo" width="400" height="67" align="right">
<br clear="all">
A free, simple, and light-weight C/C++ Redis / Valkey client library.

 - [API documentation](https://smithsonian.github.io/redisx/apidoc/html/files.html)
 - [Project page](https://smithsonian.github.io/redisx) on github.io
 
Author: Attila Kovacs

Last Updated: 8 January 2025

## Table of Contents

 - [Introduction](#introduction)
 - [Prerequisites](#prerequisites)
 - [Building RedisX](#building-redisx)
 - [Command-line interface (`redisx-cli`)](#redisx-cli)
 - [Linking your application against RedisX](#linking)
 - [Managing Redis server connections](#managing-redis-server-connections)
 - [Simple Redis queries](#simple-redis-queries)
 - [Accessing key / value data](#accessing-key-value-data)
 - [Publish / subscribe (PUB/SUB) support](#publish-subscribe-support)
 - [Atomic execution blocks and LUA scripts](#atomic-transaction-blocks-and-lua-scripts)
 - [Advanced queries and pipelining](#advanced-queries)
 - [Redis clusters](#cluster-support)
 - [Error handling](#error-handling)
 - [Debug support](#debug-support)
 - [Future plans](#future-plans)
 

<a name="introduction"></a>
## Introduction

 - [A simple example](#example)
 - [Features overview](#features)
 - [Related links](#related-links)

__RedisX__ is a free, light-weight [Redis](https://redis.io) client library for C/C++. As such, it should work with 
Redis forks / clones like [Dragonfly](https://dragonfly.io) or [Valkey](https://valkey.io) also. It supports both 
interactive and pipelined Redis queries, managing and processing subscriptions, atomic execution blocks, and LUA 
scripts loading. It can be used with multiple Redis servers simultaneously also. __RedisX__ is free to use, in any 
way you like, without licensing restrictions.

While there are other C/C++ Redis clients available, this one is C99 compatible, and hence can be used on older 
platforms also. It is also small and fast, but still capable and versatile.

Rather than providing high-level support for every possible Redis command (which would probably be impossible given 
the pace new commands are being introduced all the time), it provides a basic framework for synchronous and 
asynchronous queries, with some higher-level functions, such as for managing key/value storage types (including hash 
tables) and PUB/SUB. Future releases may add further higher-level functionality based on demand for such features.

The __RedisX__ library was created, and is maintained, by Attila Kovács at the Center for Astrophysics \| Harvard 
&amp; Smithsonian, and it is available through the [Smithsonian/redisx](https://github.com/Smithsonian/redisx) 
repository on GitHub. 

There are no official releases of __RedisX__ yet. An initial 1.0.0 release is expected in early 2025. Before then the 
API may undergo slight changes and tweaks. Use the repository as is at your own risk for now.

<a name="example"></a>
### A simple example

Below is a minimal example of a program snippet, which connects to a Redis server (without authentication) and runs a 
simple `PING` comand with a message, printing out the result on the console, while also following best practices of checking
for errors, and handling them -- in this case by printing informative error messages:

```c
  #include <redisx.h>

  // Initialize for Redis server at 'my-server.com'.
  Redis *redis = redisxInit("my-server.com");
  
  // Connect to redis
  if(redis != NULL && redisxConnect(redis, FALSE) == X_SUCCESS) {
    // Execute 'PING "Hello World!"' on the server 
    RESP *reply = redisxRequest(redis, "PING", "Hello World!", NULL, NULL, NULL);
    
    // Check that we got a response of the expected type (bulk string of any length)
    if(redisxCheckRESP(reply, RESP_BULK_STRING, 0) == X_SUCCESS)
      printf("%s\n", (char *) reply->value);
    else
      fprintf(stderr, "ERROR! bad response\n"); 
    
    // Clean up
    redisxDestroyRESP(reply);
    redisxDisconnect(redis);
  }
  else {
    perror("ERROR! could not connect");
  }
  
  // Free up the resources used by the 'redis' instance.
  redisxDestroy(redis);
```

You may find more details about each step further below. And, of course there are a lot more options and features,
that allow you to customize your interactions with a Redis / Valkey server. But the basic principle, and the steps,
follow the same pattern in general:

 1. [Initialize](#initializing) a Redis instance.
 2. [Configure](#configuring) the server properties: port, authentication, protocol, socket parameters etc. (not shown in above example).
 3. [Connect](#connecting) to the Redis / Valkey server.
 4. Interact with the server: run queries [interactively](#simple-redis-queries) or in [batch mode](#pipelined-transactions), process [push notifications](#push-notifications), [publish](#broadcasting-messages) or [subscribe](#subscriptions)...
 5. [Disconnect](#disconnecting) from the server.
 
And at every step, you should check for and [handle errors](#error-handling) as appropriate.


<a name="features"></a>
### Features overview

#### General Features

 | Feature                           | supported  | comments                                                     |
 | --------------------------------- |:----------:| -------------------------------------------------------------|
 | concurrent Redis instances        |  __yes__   | You can manage and use multiple Redis servers simultaneously |
 | thread safe (MT-safe)             |  __yes__   | synchronized + async calls with locking                      |
 | connect / disconnect hooks        |  __yes__   | user-defined callbacks                                       |
 | socket customization              |  __yes__   | (optional) user-defined timeout, buffer size and/or callback |
 | custom socket error handling      |  __yes__   | (optional) user-defined callback                             |
 | RESP to JSON                      |  __yes__   | via `xchange` library                                        |
 | RESP to structured data           |  __yes__   | via `xchange` library                                        |
 | debug error tracing               |  __yes__   | via `xSetDebug()`                                            |
 | command-line client               |  __yes__   | `redisx-cli`                                                 | 

#### Redis / Valkey Features
 
 | Redis / Valkey Feature            | supported  | comments                                                     |
 | --------------------------------- |:----------:| -------------------------------------------------------------|
 | user authentication               |  __yes__   | via `HELLO` if protocol is set, otherwise via `AUTH`         |
 | RESP3 / `HELLO` support           |  __yes__   | (optional) if specific protocol is set                       |
 | interactive queries               |  __yes__   | dedicated (low-latency) client                               |
 | pipelined (batch) processing      |  __yes__   | dedicated (high-bandwidth) client / user-defined callback    |
 | PUB/SUB support                   |  __yes__   | dedicated client / user callbacks / subscription management  |
 | push messages                     |  __yes__   | (optional) user-defined callback                             |
 | attributes                        |  __yes__   | (optional) on demand                                         |
 | Sentinel support                  |  __yes__   | _help me test it_                                            |
 | cluster support                   |  __yes__   | _help me test it_                                            |
 | ... MOVED redirection             |  __yes__   | automatic for interactive transactions                       |
 | ... ASK redirection               |  __yes__   | automatic for interactive transactions                       |
 | TLS support                       |  __yes__   | _help me test it_                                            |


<a name="related-links"></a>
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
RedisX. OpenSSL (`openssl-devel` on RPM-based, or `libssl-dev` on Debian-based Linux) is also required if built with TLS 
support.

Additionally `redisx-cli` has the following dependencies on standard GNU/POSIX libraries:
 
  - POPT (`popt-devel` on RPM-based, or `libpopt-dev` on Debian based Linux).
  - BSD (`libbsd-devel` on RPM-based, or `libbsd-dev` on Debian based Linux).
  - readline (`readline-devel` on RPM based, or `libreadline-dev` on Debian based Linux).

-----------------------------------------------------------------------------

<a name="building-redisx"></a>
## Building RedisX

The __RedisX__ library can be built either as a shared (`libredisx.so[.1]`) or as a static (`libredisx.a`) library, 
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

 - `WITH_OPENMP`: If set to 1, we will compile and link with OpenMP (i.e., `-fopenmp` is added to both `CFLAGS` and 
   `LDFLAGS` automatically). If not explicitly defined, it will be set automatically if `libgomp` is available.

 - `WITH_TLS`: If set to 1, we will build with TLS support via OpenSSL (And `-lssl` is added to `LDFLAGS` 
   automatically). If not explicitly defined, it will be set automatically if `libssl` is available.

 - `CHECKEXTRA`: Extra options to pass to `cppcheck` for the `make check` target
 
 - `XCHANGE`: If the [Smithsonian/xchange](https://github.com/Smithsonian/xchange) library is not installed on your
   system (e.g. under `/usr`) set `XCHANGE` to where the distribution can be found. The build will expect to find 
   `xchange.h` under `$(XCHANGE)/include` and `libxchange.so` / `libxchange.a` under `$(XCHANGE)/lib` or else in the 
   default `LD_LIBRARY_PATH`.
   
 - `STATICLINK`: Set to 1 to prefer linking tools statically against `libredisx.a`. (It may still link dynamically if 
   `libredisx.so` is also available.)
 
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

<a name="redisx-cli"></a>
## Command-line interface (`redisx-cli`)

The __RedisX__ library provides its own command-line tool, called `redisx-cli`. It works very similar to `redis-cli`,
except that our client has somewhat fewer bells and whistles.

```bash
 $ redisx-cli ping "Hello World"
```
will print:

```bash
 "Hello world!"
```

provided it successfully connected to the Redis / Valkey server on localhost. (Otherwise it will print an error and a 
trace). It can also be used in interactive mode if no Redis command arguments are supplied. And, you can run 
`redisx-cli --help` to see what options are available, and you can also consult the 
[redis-cli](https://redis.io/docs/latest/develop/tools/cli/) documentation for the same general description and usage 
(so far as our implementation supports it).

-----------------------------------------------------------------------------

<a name="linking"></a>
## Linking your application against RedisX

Provided you have installed the shared (`libredisx.so` and `libxchange.so`) or static (`libredisx.a` and 
`libxchange.a`) libraries in a location that is in your `LD_LIBRARY_PATH` (e.g. in `/usr/lib` or `/usr/local/lib`) 
you can simply link your program using the  `-lredisx -lxchange` flags. Your `Makefile` may look like: 

```make
myprog: ...
	$(CC) -o $@ $^ $(LDFLAGS) -lredisx -lxchange 
```

(Or, you might simply add `-lredisx -lxchange` to `LDFLAGS` and use a more standard recipe.) And, in if you installed 
the __RedisX__ and/or __xchange__ libraries elsewhere, you can simply add their location(s) to `LD_LIBRARY_PATH` prior 
to linking.

-----------------------------------------------------------------------------

<a name="managing-redis-server-connections"></a>
## Managing Redis server connections

 - [Initializing](#initializing)
 - [Configuring](#configuring)
 - [Connecting](#connecting)
 - [Disconnecting](#disconnecting)

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
  
  // (optional) configure a non-standard port number to use
  redisxSetPort(redis, 7089);
```

#### Sentinel

Alternatively, instead of `redisxInit()` above you may initialize the client for a high-availability configuration 
with a set of [Redis Sentinel](https://redis.io/docs/latest/develop/reference/sentinel-clients/) servers, using 
`redisxInitSentinel()`, e.g.:

```c
  // An array defining N sentinel servers and ports to use.
  // A port number 0 or negative will use the default Redis port of 6379.
  RedisServer sentinels[N] = { { "server1", 0 }, { "server2", 7024 } ... };
  
  // Configure a Redis client instance for the Sentinel servers and "my-service" service name
  Redis *redis = redisxInitSentinel(sentinels, N, "my-service");
  if (redis == NULL) {
    // Abort: something did not got to plan...
    return;
  }

  // (optional) set a sentinel discovery timeout in ms...
  redisxSetSentinelTimeout(redis, 30);
```

After successful initialization, you may proceed with the configuration the same way as for the regular standalone
server connection. 

One thing to keep in mind about Sentinel is that once the connection to the master is established, it works just like
a regular server connection, including the possibility of that connection being broken. It is up to the application
to initiate reconnection and recovery as appropriate in case of errors. (See more in on [Reconnecting](#reconnecting)
further below).

The Sentinel support is still experimental and requires testing. You can help by submitting bug reports in the GitHub
repository.

<a name="configuring"></a>
### Configuring

Before connecting to the Redis server, you may configure the database authentication (if any):

```c
  Redis *redis = ...
  
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
will not be used, and RESP2 will be assumed -- which is best for older servers (Redis &lt;6). (Note, that you can 
always check the actual protocol used after connecting, using `redisxGetProtocol()`). Note, that after connecting, 
you may retrieve the set of server properties sent in response to `HELLO` using `redisxGetHelloData()`.

You can also set a timeout for the interative transactions, such as:

```c
  // (optional) Set a 1.5 second timeout for interactive replies
  redisxSetReplyTimeout(redis, 1500);
```

You may also select the database index to use now (or later, after connecting), if not the default (index 0):

```c
  // (optional) Select the database index 2
  redisxSelectDB(redis, 2); 
```

Note, that you can switch the database index any time, with the caveat that it's not possible to change it for the 
subscription client when there are active subscriptions.

#### TLS configuration

We provide (experimental) support for TLS (see the Redis docs on 
[TLS support](https://redis.io/docs/latest/operate/oss_and_stack/management/security/encryption/)). Simply configure 
the necessary certificates, keys, and cypher parameters as needed, e.g.:

```c
  Redis *redis = ...
  int status;
  
  // Use TLS with the specified CA certificate file and path
  status = redisxSetTLS(redis, "path/to/certificates", "ca.crt");
  if(status) {
    // Oops, the CA certificate is not accessible...
    ...
  }
  
  // (optional) If servers requires mutual TLS, you will need to provide 
  // a certificate and private key also
  status = redisxSetMutualTLS(redis, "path/to/redis.crt", "path/to/redis.key");
  if(status) {
    // Oops, the certificate or key file is not accessible...
    ...
  }

  // (optional) Skip verification of the certificate (insecure!)
  redisxSkipVerify(redis, TRUE);

  // (optional) Set server name for SNI
  redisxSetTLSServerName(redis, "my.redis-server.com");

  // (optional) Set ciphers to use (TLSv1.2 and earlier)
  redisxSetTLSCiphers(redisx, "HIGH:!aNULL:!kRSA:!PSK:!SRP:!MD5:!RC4");
  
  // (optional) Set cipher suites to use (TLSv1.3 and later)
  redisxSetTLSCipherSuites(redisx, "ECDHE-RSA-AES256-GCM-SHA384:TLS_AES_256_GCM_SHA384");
  
  // (optional) Set parameters for DH-based cyphers
  status = redisxSetDHCypherParams(redisx, "path/to/redis.dh");
  if(status) {
    // Oops, the parameter file is not accessible...
    ...
  }
```

The TLS support is still experimental and requires testing. You can help by submitting bug reports in the GitHub
repository.

<a name="socket-configuration"></a>
#### Socket-level configuration

You might also tweak the socket options used for clients, if you find the socket defaults sub-optimal for your 
application:

```c
   // (optional) Set 1000 ms socket read/write timeout for future connections.
   redisxSetSocketTimeout(redis, 1000);

   // (optional) Set the TCP send/rcv buffer sizes to use if not default values.
   redisxSetTcpBuf(redis, 65536);
```

If you want, you can perform further customization of the client sockets via a user-defined callback function, e.g.:

```c
  int my_socket_config(int sock, enum redisx_channel channel) {
     // Set up the socket any way you like...
     ...
     
     return X_SUCCESS;
  }
```

which you can then apply to your Redis instance as:

```c
  redisxSetSocketConfigurator(my_socket_config);
```

<a name="connection-hooks"></a>
#### Connection &amp; disconnection hooks

The user of the __RedisX__ library might want to know when connections to the server are established, or when clients
get disconnected (either as intended or as a result of errors), and may want to perform some configuration or clean-up 
accordingly. For this reason, the library provides support for connection 'hooks' -- that is custom functions that are 
called in the even of connecting to or disconnecting from a Redis server.

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

You may add multiple callbacks. All of them will be called (in the same order as added) when connection is 
established. You may also remove specific connection callbacks via `redisxRemoveConnectHook()` if you now longer want
a particular function to be called any more in the even.

The same goes for disconnect hooks, using `redisxAddDisconnectHook()` / `redisxRemoveDisconnectHook()` instead.


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
synchronous and asynchronous requests (and responses). For 
[Sentinel](https://redis.io/docs/latest/develop/reference/sentinel-clients/) configurations, it will return with 
`X_SUCCESS` only after having located and connected to the master server, and confirmed that it is indeed the master.


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

<a name="reconnecting"></a>
### Reconnecting

Reconnections to the Redis servers are never automatic, and there is no automatic failover for __RedisX__ clients
(there are good reasons for that). It is up to you to decide when to reconnect and how exactly. For example, the 
application may reconnect to the same or different server (including Sentinel), and perform a set of necessary 
recovery steps, to continue where things were left off on the previous connection, such as:

 - reload LUA scripts
 - reinstate subscriptions
 - re-submit any request for which no replies have been received before the connection was broken.
 - re-publish any notifications on PUB/SUB, which may not have been delivered.
 

-----------------------------------------------------------------------------

<a name="simple-redis-queries"></a>
## Simple Redis queries

 - [Interactive transactions](#interactive-transactions)
 - [Bundled Attributes](#attributes)
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
  char *cmd[] = { "HGET", "my_table", "my_key" };  // command and parameters as an array...

  // Send "HGET my_table my_key" request with an array of 3 components...
  resp = redisxArrayRequest(redis, cmd, NULL, 3, &status);
  ...

```

The 3rd argument in the list is an optional `int[]` array defining the individual string lengths of the parameters (if 
need be, or else readily available). Here, we used `NULL` instead, which will use `strlen()` on each supplied 
string-terminated parameter to determine its length automatically. Specifying the length may be necessary if the 
individual parameters are not 0-terminated strings.

In interactive mode, each request is sent to the Redis server, and the response is collected before the call returns 
with that response (or `NULL` if there was an error).

<a name="attributes"></a>
### Bundled Attributes

Redis 6 introduced the possibility of sending optional attributes along with responses, using the RESP3 protocol. 
These attributes are not returned to users by default, in line with the RESP3 protocol specification. Rather, they 
are available on demand, after the response to a request is received. You may retrieve the attributes to interactive 
requests _after_ the `redisxRequest()` or `redisxArrayRequest()` queries, using `redisxGetAttributes()`, e.g.:

```c
  ...
  
  // Some interactive request
  RESP *reply = redisxRequest(redis, ...);
  
  // Get the attributes (if any) that were bundled with the response...
  RESP *attributes = redisxGetAttributes(redis);
  
  ...
```


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
- You should not attempt to lock or release clients from the call. If you need access to a client (e.g. to submit a
  new Redis request), it's best to put a copy of the RESP notification onto a queue and let an asynchronous thread 
  deal with it.


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
 | `RESP3_MAP`             |   `%`    | number of key / value pairs   | `(RedisMap *)`   |
 | `RESP3_ATTRIBUTE`       |   `\|`   | number of key / value pairs   | `(RedisMap *)`   |
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
  RESP *reply = ...
  
  // Let's say we expect 'reply' to contain of an embedded RESP array of 3 elements... 
  int status = redisxCheckDestroyRESP(reply, RESP_ARRAY, 3);
  if (status != X_SUCCESS) {
     // Oops, 'reply' was either NULL, or does not contain a RESP array with 3 elements...
     ...
  }
  else {
     // Process the expected response...
     ...
     redisxDestroyRESP(reply);
  }
```

Before destroying a RESP structure, the caller may want to dereference values within it if they are to be used as is 
(without making copies), e.g.:


```c
  RESP *reply = ...
  char *stringValue = NULL;   // to be extracted from 'reply'
  
  // Let's say we expect 'rwply' to contain of a simple string response (of whatever length)
  int status = redisxCheckDestroyRESP(reply, RESP_SIMPLE_STRING, 0);
  if (status != X_SUCCESS) {
    // Oops, 'reply' was either NULL, or it was not a simple string type
    ...
  }
  else {
    // Set 'stringValue' and dereference the value field in the RESP so it's not 
    // destroyed with the RESP itself.
    stringValue = (char *) reply->value;
    reply->value = NULL;
     
    redisxDestroyRESP(reply);     // 'stringValue' is still a valid pointer after! 
  }
```

Note, that you can convert a RESP to an `XField`, and/or to JSON representation using the `redisxRESP2XField()` and 
`redisxRESP2JSON()` functions, e.g.:

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
## Publish / subscribe (PUB/SUB) support
 
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

The `redisxSubscribe()` function will translate to either a Redis `PSUBSCRIBE` or `SUBSCRIBE` command, depending on
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
  int status;
  
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
  status = redisxSendRequestAsync(cl, ...);
  if(status < 0) {
    // Oops, something went wrong, we should abort and return
    redisxAbortBlockAsync(cl);
    return status;
  }
  ...

  // Execute the block of commands above atomically, and get the resulting RESP
  result = redisxExecBlockAsync(cl, &status);
  // -------------------------------------------------------------------------
  
  // Release exlusive access to the client
  redisxUnlockClient(cl);
  
  if(status != X_SUCCESS) {
    // Oops, the block execition failed...
    return status;
  }
  
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
  
  int status;     // We will return error status in this variable
  
  // Execute the script, with the specified keyword arguments and parameters
  RESP *reply = redisxRunScript(redis, scriptSHA1, keyArgs, params, &status);

  if(status != X_SUCCESS) {
     // Oops, failed to run script...
     ...
  }

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
 - [Bundled Attributes](#async-attributes)
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
    RESP *reply = redisxReadReplyAsync(cl, &status);
    if(status != X_SUCCESS) {
      // Ooops, not the reply what we expected...
      ...
    }
    
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


<a name="async-attributes"></a>
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
    RESP *attributes = redisxGetAttributesAsync(cl);
    
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

<a name="cluster-support"></a>
## Redis clusters

 - [Cluster basics](#cluster-basics)
 - [Detecting cluster reconfiguration](#cluster-reconfiguration)
 - [Explicit connection management](#cluster-explicit-connect)

__RedisX__ provides support for [Redis clusters](https://redis.io/docs/latest/operate/oss_and_stack/management/scaling/) 
also. In cluster configuration the database is distributed over a collection of servers, each node of which serves
only a subset of the Redis keys. 

 1. Configure a known cluster node as usual.
 2. Initialize a cluster using the known, configured node. (All shards in the cluster will inherit the configuration 
    from the initializing node.)
 3. For every request, you must obtain the appropriate cluster node for the given key to process. (PUB/SUB may be 
    processed on any cluster node.)
 4. Destroy the cluster resources when done using it.
  
The support for Redis clusters is still experimental and requires testing. You can help by submitting bug reports in 
the GitHub repository.

<a name="cluster-basics"></a>
### Cluster basics 

Specifically, start by configuring a known node of the cluster as usual for a single Redis server, setting 
authentication, socket configuration, callbacks etc.:

```c
  // Initialize a known node of the cluster for obtaining the current cluster configuration
  Redis *node = redisxInit(...);
  ...
```

Next, you can use the known node to obtain the cluster configuration:

```c
  // Try obtain the cluster configuration from the known node.
  RedisCluster *cluster = redisxClusterInit(node);
  if(cluster == NULL) {
    // Oops, that did not work. Perhaps try another node...
    ...
  }
  
  // Discard the configuring node if no longer needed...
  redisxDestroy(node);
```

The above will query the cluster configuration from the node (the node need not be explicitly connected prior to the
initialization, and will be returned in the same connection state as before). The cluster will inherit the configuration
of the node, such as pipelining, socket configuration authentication, protocol, and callbacks, from the configuring node 
-- all but the database index, which is always 0 for clusters.

If the initialization fails on a particular node, you might try other known nodes until one of then succeeds. (You
might use `redisxSetHostName()` and `redisxSetPort()` on the configured `Redis` instance to update the address of the 
configuring node, while leaving other configuration settings intact.)

Once the cluster is configured, you may discard the configuring node instance, unless you need it specifically for other
reasons.

You can start using the cluster right away. You can obtain a connected `Redis` instance for a given key using 
`redisxClusterGetShard()`, e.g.:

```c
  const char *key = "my-key";   // The Redis keyword of interest, can use Redis hashtags also
  int status;                   // We'll track error status here.

  // Get the connected Redis server instance that serves the given key
  Redis *shard = redisxClusterGetShard(cluster, key);
  if(shard == NULL) {
    // Oops, there seems to be no server for the given key currently. Perhaps try again later...
    ...
  }

  // Run your query on using the given Redis key / keys.
  RESP *reply = redisxRequest(shard, "GET", key, NULL, NULL, &status);
  ...
```

The interactive queries handle both `MOVED` and `ASK` redirections automatically. However, asynchronous queries do not
since they return before receiving a response. Thus, when using `redisxReadReplyAsync()` later to process replies, you
should check for redirections:

```c
  RESP *reply = redisxReadReplyAsync(...);

  if(redisxClusterMoved(reply)) {
    // The key is now served by another shard.
    // You might want to obtain the new shard and repeat the failed
    // transaction again (interactively or pipelined)...
    ...
  }
  if(redisxClusterIsMigrating(reply)) {
    // The key's slot is currently migrating. You may try the redirected 
    // address indicated in the reply, with the ASKING command, e.g. via a 
    // redisxClusterAskMigrating() interactive transaction.
    ...
  }
  ...
  
```

As a matter a best practice you should never assume that a given keyword is persistently served by the same shard. 
Rather, you should obtain the current shard for the key each time you want to use it with the cluster, and always 
check for errors on shard requests, and repeat failed requests on a newly obtained shard if necessary.

Finally, when you are done using the cluster, simply discard it:

```c
  // Disconnect all shards and free up all resources by the cluster
  redisxClusterDestroy(cluster);
```

<a name="cluster-reconfiguration"></a>
### Detecting cluster reconfiguration

In the above example we have shown one way you might check for errors that result from cluster being reconfigured
on-the-fly, using `redisxClusterMoved()` and/or `redisxClusterIsMigrating()` on the `RESP` reply obtained from the 
shard.

Equivalently, you might use `redisxCheckRESP()` or `redisxCheckDestroyRESP()` also for detecting a cluster 
reconfiguration. Both of these will return a designated `REDIS_MOVED` or `REDIS_MIGRATING` error code if the keyword 
has moved or is migrating, respectively, to another node, e.g.:

```c
  ...
  int s = redisxCheckRESP(reply, ...);
  if(s == REDIS_MOVED) {
    // The key is now served by another shard.
    ...
  }
  if(s == REDIS_MIGRATING) {
    // The key is migrating and may be accessed from new location via an ASKING directive
    ...
  }
  if(s != X_SUCCESS) {
    // The reply is no good for some other reason...
    ...
  }
  ...
```

To help manage redirection responses for asynchronous requests, we provide `redisxClusterGetRedirection()` to obtain 
the redirected Redis instance based on the redirection `RESP`. Once the redirected cluster shard is identified you may 
either resubmit the same query as before (e.h. with `redisxSendArrayRequestAsync()`) if `MOVED`, or else repeat the 
query via an interactive `ASKING` directive using `redisxClusterAskMigrating()`.


<a name="cluster-explicit-connect"></a>
### Manual connection management

The `redisxClusterGetShard()` will automatically connect the associated shard, if not already connected. Thus, you do
not need to explicitly connect to the cluster or its shards. However, in some cases you might want to connect all 
shards before running queries to eliminate the connection overhead during use. If so, you can call 
`redisxClusterConnect()` to explicitly connect to all shards before using the cluster. Similarly, you can also 
explicitly disconnect from all connected shards using `redisxClusterDisconnect()`, e.g. to close unnecessary sockets. 
You may continue to use the cluster after calling `redisxClusterDisconnect()`, as successive calls to 
`redisxClusterGetShard()` will continue to reconnect the shards as needed automatically.


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
  
  redisxSetSocketErrorHandler(redis, my_error_handler);
```

After that, every time there is an error with sending or receiving packets over the network to any of the Redis
clients used, your handler will report it the way you want it.

### Socket-level errors

There are multiple ways the user may get informed when errors happen at the socket read / write level:

 - __RedisX__ will propagate and report errors from failed socket reads and writes, both via return values to its 
   functions (via `X_NO_SERVICE` error code or `NULL` returns, plus `errno`), and
   
 - __RedisX__ will call the configured socket error handler callback function (see above) as soon as the error is 
   encountered.
   
 - __RedisX__ will call all disconnect hooks configured if the client disconnects (either as intended or 
   unexpectedly as a result of a persistent socket-level error).

It is up to you to decide which of the following method(s) you wish to rely on to detect broken connections and
act as appropriate. To help with your decisions, below is a step-by-step outline of how __RedisX__ handles errors
originating from the socket level of clients, indicating also the points at which users are notified by each of the
above mentioned methods.

 1. The socket error handler (if configured) callback function is called is immediately after a read or write error 
    is detected on a client's socket. The error handler callback function is called while the affected client is still 
    locked and nominally in a 'functioning' state. That means you are free to use any `Async` call on the affected 
    client as appropriate, but the error handler should not attempt to release the exclusive lock on the client or call
    synchronized functions on the Redis instance or the affected client. The background processing of replies (on 
    the pipeline and/or subscription clients) is still active at this stage.
    
 2. If the error is caused by a timeout (`errno` being `EAGAIN` or `EWOULDBLOCK`), nothing changes at this stage.
    However, if the error is persistent, the client will be disabled and reset, and subsequent read or write calls 
    will fail immediately. Any disconnection hooks will be called also as the client is disconnected. The background 
    processing of server replies (on the pipeline and subscriptions channels) will stop soon after the disconnection
    is initiated.
    
 3. The __RedisX__ call returns either `X_NO_SERVICE`, or `X_TIMEDOUT`, or else `NULL`. The application should check 
    return values (and/or `errno`) as appropriate.


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
 - Add high-level support for managing and calling custom Redis functions.
 - Add more high-level [Redis commands](https://redis.io/docs/latest/commands/), e.g. for lists, streams, etc.

If you have an idea for a must have feature, please let me (Attila) know. Pull requests, for new features or fixes to
existing ones, are especially welcome! 
 
-----------------------------------------------------------------------------
Copyright (C) 2024 Attila Kovács
