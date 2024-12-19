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
  XLookupTable *info;
  XField *role;

  xSetDebug(TRUE);
  //redisxSetVerbose(TRUE);
  //redisxDebugTraffic(TRUE);

  if(redisxConnect(redis, FALSE) < 0) {
    perror("ERROR! connect");
    return 1;
  }

  info = redisxGetInfo(redis, "replication");
  if(!info) {
    perror("ERROR! NULL info");
    return 1;
  }

  role = xLookupField(info, "role");
  if(!role) {
    fprintf(stderr, "ERROR! role not found (count = %ld)", xLookupCount(info));
    return 1;
  }

  xDestroyLookup(info);
  redisxDisconnect(redis);
  redisxDestroy(redis);

  fprintf(stderr, "OK\n");

  return 0;

}
