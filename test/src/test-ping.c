/**
 * @file
 *
 * @date Created  on Dec 8, 2024
 * @author Attila Kovacs
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "redisx.h"
#include "xchange.h"

int main() {
  Redis *redis = redisxInit("localhost");

  xSetDebug(TRUE);
  //redisxSetVerbose(TRUE);
  //redisxDebugTraffic(TRUE);

  if(redisxConnect(redis, FALSE) < 0) {
    perror("ERROR! connect");
    return 1;
  }

  if(redisxPing(redis, NULL) != X_SUCCESS) {
    perror("ERROR! ping");
    return 1;
  }

  if(redisxPing(redis, "hello") != X_SUCCESS) {
    perror("ERROR! ping hello");
    return 1;
  }

  redisxDisconnect(redis);
  redisxDestroy(redis);

  fprintf(stderr, "OK\n");

  return 0;

}
