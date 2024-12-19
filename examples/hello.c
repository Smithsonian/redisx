#include <stdio.h>
#include <stdlib.h>

#include <redisx.h>

/*
 * Pings a Redis server with a hello message, and prints the response, or else an error.
 *
 * Usage: hello [host]
 *
 * Options:
 *    host      The Redis server host (default: "127.0.0.1")
 *
 * Exit status:
 *    0         Successful completion
 *    1         Could not connect to Redis server
 *    2         Bad response from server
 *
 * Author: Attila Kovacs
 * Version: 19 December 2024
 */
int main(int argc, char *argv[]) {
  // Initialize for Redis server at 'my-server.com'.
  Redis *redis;
  char *host = "127.0.0.1";     // Default host

  if(argc > 1) host = argv[1];  // program argument designates host

  // Initialize with the designated or default host
  redis = redisxInit(host);

  // Connect to Redis / Valkey server
  if(redis != NULL && redisxConnect(redis, FALSE) == X_SUCCESS) {

    // Execute 'PING "Hello World!"' query on the server
    RESP *reply = redisxRequest(redis, "PING", "Hello World!", NULL, NULL, NULL);

    // Check that we got a response of the expected type (bulk string of any length)
    if(redisxCheckRESP(reply, RESP_BULK_STRING, 0) == X_SUCCESS)
      printf("%s\n", (char *) reply->value);
    else {
      fprintf(stderr, "ERROR! bad response\n"); 
      return 2;
    }

    // Clean up
    redisxDestroyRESP(reply);
    redisxDisconnect(redis);
  }
  else {
    perror("ERROR! could not connect");
    return 1;
  }
  
  // Free up the resources used by the 'redis' instance.
  redisxDestroy(redis);
  
  return 0;
}
