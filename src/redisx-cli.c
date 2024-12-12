/**
 * @date Created  on Dec 11, 2024
 * @author Attila Kovacs
 */

#define _POSIX_C_SOURCE  199309L      ///< for nanosleep()

#include <stdio.h>
#include <stdlib.h>
#include <popt.h>
#include <time.h>
#include <math.h>

#include "redisx.h"

#define FORMAT_JSON 1
#define FORMAT_RAW  2

int main(int argc, const char *argv[]) {

  char *host = "127.0.0.1";
  int port = 0;
  double timeout = 0.0;
  char *password = NULL;
  char *user = NULL;
  int repeat = 1;
  double interval = 1.0;
  int dbIndex = 0;
  int protocol = -1;
  int format = 0;

  struct poptOption options[] = { //
          {"host",      'h', POPT_ARG_STRING, &host,     0, "Server hostname (default: 127.0.0.1).", NULL}, //
          {"port",      'p', POPT_ARG_INT,    &port,     0, "Server port (default: 6379).", NULL}, //
          {"timeout",   't', POPT_ARG_DOUBLE, &timeout,  0, "Server connection timeout in seconds (decimals allowed).", NULL}, //
          {"pass",      'a', POPT_ARG_STRING, &password, 0, "Password to use when connecting to the server.", NULL}, //
          {"user",      'u', POPT_ARG_STRING, &user,     0, "Used to send ACL style 'AUTH username pass'. Needs -a.", NULL}, //
          {"repeat",    'r', POPT_ARG_INT,    &repeat,   0, "Execute specified command N times.", NULL}, //
          {"interval",  'i', POPT_ARG_DOUBLE, &interval, 0, "When -r is used, waits <interval> seconds per command. " //
                                                            "It is possible to specify sub-second times like -i 0.1.", NULL}, //
          {"db",        'n', POPT_ARG_INT,    &dbIndex,  0, "Database number.", NULL}, //
          {"resp2",     '2', POPT_ARG_NONE,   NULL,      2, "Start session in RESP2 protocol mode.", NULL}, //
          {"resp3",     '3', POPT_ARG_NONE,   NULL,      3, "Start session in RESP3 protocol mode.", NULL}, //
          {"json",        0, POPT_ARG_NONE,   NULL,    'j', "Print raw strings", NULL}, //
          POPT_AUTOHELP POPT_TABLEEND //
  };

  int rc;
  char **cmdargs;
  int i, nargs = 0;
  Redis *redis;

  poptContext optcon = poptGetContext("redisx-cli", argc, argv, options, 0);

  while((rc = poptGetNextOpt(optcon)) != -1) {
    if(rc < -1) {
      fprintf(stderr, "ERROR! Bad syntax. Try running with --help to see command-line options.\n");
      exit(1);
    }

    switch(rc) {
      case '2': protocol = 2; break;
      case '3': protocol = 3; break;
      case 'j': format = FORMAT_JSON; break;
      case 'r': format = FORMAT_RAW; break;
    }
  }

  cmdargs = (char **) poptGetArgs(optcon);

  if(!cmdargs) {
    poptPrintHelp(optcon, stdout, 0);
    return 1;
  }

  while(cmdargs[nargs]) nargs++;

  redis = redisxInit(host);
  if(port) redisxSetPort(redis, port);
  if(user) redisxSetUser(redis, user);
  if(password) redisxSetPassword(redis, password);
  if(dbIndex > 0) redisxSelectDB(redis, dbIndex);
  if(timeout > 0.0) redisxSetSocketTimeout(redis, (int) ceil(1000 * timeout));
  if(protocol > 0) redisxSetProtocol(redis, protocol);

  redisxConnect(redis, 0);
  xSetDebug(1);

  for(i = 0; i < repeat; i++) {
    int status = X_SUCCESS;
    RESP *reply;

    if(i > 0 && interval > 0.0) {
      struct timespec sleeptime;
      sleeptime.tv_sec = (int) interval;
      sleeptime.tv_nsec = 1000000000 * (interval - sleeptime.tv_sec);
      nanosleep(&sleeptime, NULL);
    }

    reply = redisxArrayRequest(redis, cmdargs, NULL, nargs, &status);

    if(!status) {
      if(format == FORMAT_JSON) redisxPrintJSON("REPLY", reply);
      else redisxPrintRESP(reply);
    }

    redisxDestroyRESP(reply);
  }

  poptFreeContext(optcon);

  return 0;
}
