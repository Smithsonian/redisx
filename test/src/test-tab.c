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
  int status;
  char **keys, *value;
  int n1 = -1, n2 = -1;

  xSetDebug(TRUE);
  //redisxSetVerbose(TRUE);
  //redisxDebugTraffic(TRUE);

  if(redisxConnect(redis, TRUE) < 0) {
    perror("ERROR! connect");
    return 1;
  }

  if(redisxSetValue(redis, "_test_", "_value_", "2", TRUE) < 0) {
    perror("ERROR! set value");
    return 1;
  }

  resp = redisxGetValue(redis, "_test_", "_value_", &status);
  if(status) {
    perror("ERROR! get value");
    return 1;
  }
  redisxPrintRESP("get value", resp);
  redisxCheckDestroyRESP(resp, RESP_BULK_STRING, 1);
  if(strcmp("2", (char *) resp->value) != 0) {
    fprintf(stderr, "ERROR! mismatched value: got '%s', expected '%s'\n", (char *) resp->value, "2");
    return 1;
  }
  redisxDestroyRESP(resp);

  if(redisxSetValue(redis, "_test_", "_value_", "3", FALSE) < 0) {
    perror("ERROR! set value (noconfirm)");
    return 1;
  }

  resp = redisxGetValue(redis, "_test_", "_value_", &status);
  if(status) {
    perror("ERROR! get value (noconfirm)");
    return 1;
  }
  redisxCheckDestroyRESP(resp, RESP_BULK_STRING, 1);
  if(strcmp("3", (char *) resp->value) != 0) {
    fprintf(stderr, "ERROR! mismatched value: got '%s', expected '%s'\n", (char *) resp->value, "3");
    return 1;
  }
  redisxDestroyRESP(resp);

  e = redisxGetTable(redis, "_test_", &n1);
  if(n1 <= 0) {
    fprintf(stderr, "ERROR! get table: %d\n", n1);
    return 1;
  }
  redisxDestroyEntries(e, n1);

  e = redisxScanTable(redis, "_test_", "*", &n2, &status);
  if(n2 <= 0 || status) {
    fprintf(stderr, "ERROR! scan table: n = %d, status = %d\n", n2, status);
    return 1;
  }
  redisxDestroyEntries(e, n2);

  if(n1 != n2) {
    fprintf(stderr, "ERROR! scan table mismatch: got %d, expected %d\n", n2, n1);
    return 1;
  }

  // table keys
  keys = redisxGetKeys(redis, "_test_", &n1);
  if(n1 <= 0 || keys == NULL) {
    fprintf(stderr, "ERROR! get test table keys: %d\n", n1);
    return 1;
  }
  redisxDestroyKeys(keys, n1);

  // Top-level keys
  keys = redisxGetKeys(redis, NULL, &n1);
  if(n1 <= 0 || keys == NULL) {
    fprintf(stderr, "ERROR! get keys: %d\n", n1);
    return 1;
  }
  redisxDestroyKeys(keys, n1);

  // Scanned top-level keys
  keys = redisxScanKeys(redis, "*", &n2, &status);
  if(n2 <= 0 || keys == NULL) {
    fprintf(stderr, "ERROR! scan keys: n = %d, status = %d\n", n2, status);
    return 1;
  }
  redisxDestroyKeys(keys, n2);

  if(n1 != n2) {
    fprintf(stderr, "ERROR! scan keys mismatch: got %d, expected %d\n", n2, n1);
    return 1;
  }

  // TODO multiset
  e = (RedisEntry *) calloc(2, sizeof(RedisEntry));
  e[0].key = "_value_";
  e[0].value = "4";
  e[1].key = "_extra_";
  e[1].value = "5";
  status = redisxMultiSet(redis, "_test_", e, 2, TRUE);
  if(status) {
    perror("ERROR! multiset");
    return 1;
  }

  value = redisxGetStringValue(redis, "_test_", e[0].key, 0);
  if(strcmp(e[0].value, value) != 0) {
    fprintf(stderr, "ERROR! mismatched multi value 1: got '%s', expected '%s'\n", value, "4");
    return 1;
  }
  free(value);

  value = redisxGetStringValue(redis, "_test_", e[1].key, 0);
  if(strcmp(e[1].value, value) != 0) {
    fprintf(stderr, "ERROR! mismatched multi value 2: got '%s', expected '%s'\n", value, "4");
    return 1;
  }
  free(value);

  e[1].value = "6";
  status = redisxMultiSet(redis, "_test_", e, 2, FALSE);
  if(status) {
    perror("ERROR! multiset (noconfirm)");
    return 1;
  }

  value = redisxGetStringValue(redis, "_test_", e[1].key, 0);
  if(strcmp(e[1].value, value) != 0) {
    fprintf(stderr, "ERROR! mismatched multi value 2 (noconfirm): got '%s', expected '%s'\n", value, "4");
    return 1;
  }
  free(value);
  free(e);


#if __STDC_VERSION__ > 201112L
  // The following is not available on prior to the POSIX.1-2008 standard
  // We'll use the __STDC_VERSION__ constant as a proxy to see if fnmatch is available

  n1 = redisxDeleteEntries(redis, "_*:_extra_");
  if(n1 < 0) {
    perror("ERROR! delete entries");
    return 1;
  }
  if(n1 !=1) {
    fprintf(stderr, "ERROR! mismatched deleted entries: got %d, expected %d\n", n1, 1);
    return 1;
  }

  xSetDebug(FALSE);
  value = redisxGetStringValue(redis, "_test_", "_extra_", 0);
  if(value) {
    fprintf(stderr, "ERROR! deleted entry exists\n");
    return 1;
  }
  xSetDebug(TRUE);

#endif

  redisxDisconnect(redis);
  redisxDestroy(redis);

  fprintf(stderr, "OK\n");

  return 0;

}
