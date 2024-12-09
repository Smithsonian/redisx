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
  RedisEntry *e;
  RESP *resp;
  const char *json;
  int n = -1;

  xSetDebug(TRUE);
  //redisxSetVerbose(TRUE);
  //redisxDebugTraffic(TRUE);

  redisxSetProtocol(redis, REDISX_RESP3);

  if(redisxConnect(redis, FALSE) < 0) {
    perror("ERROR! connect");
    return 1;
  }

  resp = redisxGetHelloData(redis);
  json = redisxRESP2JSON("server_properties", resp);
  printf("%s", json ? json : "<null>");
  redisxDestroyRESP(resp);

  if(redisxGetProtocol(redis) != REDISX_RESP3) {
    fprintf(stderr, "ERROR! verify RESP3 protocol\n");
    return 1;
  }

  if(redisxSetValue(redis, "_test_", "_value_", "1", TRUE) < 0) {
    perror("ERROR! set value");
    return 1;
  }

  e = redisxGetTable(redis, "_test_", &n);
  if(n <= 0) return 1;

  redisxDestroyEntries(e, n);
  redisxDisconnect(redis);
  redisxDestroy(redis);

  fprintf(stderr, "OK\n");

  return 0;

}
