/**
 * @file
 *
 * @date Created  on Jan 5, 2025
 * @author Attila Kovacs
 */


#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "redisx-priv.h"

#define TEST_KEY    "123456789"
#define TEST_HASH   0x31C3

int main() {
  uint16_t hash;

  hash = rCalcHash(TEST_KEY);
  if(hash != TEST_HASH) {
    fprintf(stderr, "ERROR! test key: got %hu, expected %hu\n", hash, TEST_HASH);
    return 1;
  }

  hash = rCalcHash("{" TEST_KEY "}.blah");
  if(hash != TEST_HASH) {
    fprintf(stderr, "ERROR! hashtag: got %hu, expected %hu\n", hash, TEST_HASH);
    return 1;
  }

  fprintf(stderr, "OK\n");
  return 0;
}
