/**
 * @date Created  on Dec 11, 2024
 * @author Attila Kovacs
 */

#define _POSIX_C_SOURCE  199309L      ///< for nanosleep()

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <popt.h>
#include <time.h>
#include <math.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <bsd/readpassphrase.h>

#include "redisx-priv.h"

#define FORMAT_JSON 1
#define FORMAT_RAW  2

static char *host = "127.0.0.1";
static int port = 6379;
static int format = 0;


static void printRESP(const char *prefix, const RESP *resp) {
  if(format == FORMAT_JSON) redisxPrintJSON(prefix, resp);
  else redisxPrintRESP(resp);
}

static void process(Redis *redis, const char *prefix, const char **cmdargs, int nargs) {
  int status = X_SUCCESS;
  RESP *reply = redisxArrayRequest(redis, cmdargs, NULL, nargs, &status);
  if(!status) printRESP(prefix, reply);
  redisxDestroyRESP(reply);
}

// cppcheck-suppress constParameterCallback
static void PushProcessor(RedisClient *cl, RESP *resp, void *ptr) {
  (void) cl;
  (void) ptr;
  printRESP("[PUSH]", resp);
}

static int interactive(Redis *redis) {
  char *prompt = malloc(strlen(host) + 20);
  sprintf(prompt, "%s:%d> ", host, port);

  using_history();

  for(;;) {
    char *line = readline(prompt);
    char **args;
    int nargs;

    if(!line) continue;

    if(strcmp("quit", line) == 0 || strcmp("exit", line) == 0) break;

    poptParseArgvString(line, &nargs, (const char ***) &args);

    if(args) {
      if(nargs > 0) {
        process(redis, "[REPLY]", (const char **) args, nargs);
        add_history(line);
      }
      free(args);
    }

    free(line);
  }

  printf("Bye!\n");
  redisxDisconnect(redis);

  return X_SUCCESS;
}

static char *readScript(const char *eval) {
  FILE *fp = fopen(eval, "r");
  size_t filesize;
  char *script;

  if(!fp) {
    fprintf(stderr, "ERROR! %s: %s", eval, strerror(errno));
    exit(1);
  }

  fseek(fp, 0L, SEEK_END);
  filesize = ftell(fp);

  script = (char *) malloc(filesize);
  x_check_alloc(script);

  if(fread(script, filesize, 1, fp) < 1) {
    fprintf(stderr, "ERROR! reading %s: %s", eval, strerror(errno));
    exit(1);
  }

  fclose(fp);

  return script;
}

int main(int argc, const char *argv[]) {
  static const char *fn = "redis-cli";

  double timeout = 0.0;
  char *password = NULL;
  char *user = NULL;
  int askpass = 0;
  int repeat = 1;
  double interval = 1.0;
  int dbIndex = 0;
  int protocol = -1;
  char *push = "y";
  const char *eval = NULL;

  struct poptOption options[] = { //
          {NULL,        'h', POPT_ARG_STRING | POPT_ARGFLAG_SHOW_DEFAULT,    &host,     0, "Server hostname.", "<hostname>"}, //
          {NULL,        'p', POPT_ARG_INT    | POPT_ARGFLAG_SHOW_DEFAULT,    &port,     0, "Server port.", "<port>"}, //
          {NULL,        't', POPT_ARG_DOUBLE, &timeout,    0, "Server connection timeout in seconds (decimals allowed).", "<timeout>"}, //
          {"pass",       'a', POPT_ARG_STRING, &password,   0, "Password to use when connecting to the server.", "<password>"}, //
          {"user",        0, POPT_ARG_STRING, &user,       0, "Used to send ACL style 'AUTH username pass'. Needs -a.", "<username>"}, //
          {"askpass",     0, POPT_ARG_NONE,   &askpass,    0, "Force user to input password with mask from STDIN." //
                  "If this argument is used, '-a' will be ignored.", NULL //
          }, //
          {NULL,        'r', POPT_ARG_INT,    &repeat,     0, "Execute specified command N times.", "<repeat>"}, //
          {NULL,        'i', POPT_ARG_DOUBLE | POPT_ARGFLAG_SHOW_DEFAULT, &interval, 0, "When -r is used, waits <interval> seconds per command. " //
                  "It is possible to specify sub-second times like -i 0.1.", "<interval>" //
          }, //
          {NULL,        'n', POPT_ARG_INT,    &dbIndex,    0, "Database number.", "<db>"}, //
          {NULL,        '2', POPT_ARG_VAL,    &protocol,   2, "Start session in RESP2 protocol mode.", NULL}, //
          {NULL,        '3', POPT_ARG_VAL,    &protocol,   3, "Start session in RESP3 protocol mode.", NULL}, //
          {"json",        0, POPT_ARG_NONE,   NULL,      'j', "Output in JSON format", NULL}, //
          {"show-pushes", 0, POPT_ARG_STRING, &push,       0, "Whether to print RESP3 PUSH messages.  Enabled by default when STDOUT " //
                  "is a tty but can be overridden with --show-pushes no.", "<yes|no>" //
          }, //
          {"eval",        0, POPT_ARG_STRING, &push,       0, "Send an EVAL command using the Lua script at <file>.", "<file>" },

          POPT_AUTOHELP POPT_TABLEEND //
  };

  int rc;
  char **cmdargs;
  char *script;
  int i, nargs = 0;
  Redis *redis;

  poptContext optcon = poptGetContext(fn, argc, argv, options, 0);
  poptSetOtherOptionHelp(optcon, "[OPTIONS] [cmd [args [arg ...]]]");

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

  if(askpass) {
    password = (char *) malloc(1024);
    if(readpassphrase("Enter password: ", password, 1024, 0) == NULL) {
      free(password);
      password = NULL;
    }
  }

  if(eval) {
    // Insert 'EVAL <script>' in front of the command-line args.
    script = readScript(eval);
    cmdargs = realloc(cmdargs, nargs + 2 * sizeof(char *));
    x_check_alloc(cmdargs);
    memmove(cmdargs[2], cmdargs, nargs * sizeof(char *));
    cmdargs[0] = "EVAL";
    cmdargs[1] = script;
    nargs += 2;
  }

  redis = redisxInit(host);

  if(port <= 0) port = 6369;
  else redisxSetPort(redis, port);

  if(user) redisxSetUser(redis, user);
  if(password) redisxSetPassword(redis, password);
  if(dbIndex > 0) redisxSelectDB(redis, dbIndex);
  if(timeout > 0.0) redisxSetSocketTimeout(redis, (int) ceil(1000 * timeout));
  if(protocol > 0) redisxSetProtocol(redis, protocol);

  if(push) if(strcmp("y", push) == 0 || strcmp("yes", push) == 0) redisxSetPushProcessor(redis, PushProcessor, NULL);

  xSetDebug(1);
  prop_error(fn, redisxConnect(redis, 0));

  if(!cmdargs) return interactive(redis);

  while(cmdargs[nargs]) nargs++;

  for(i = 0; i < repeat; i++) {
    if(i > 0 && interval > 0.0) {
      struct timespec sleeptime;
      sleeptime.tv_sec = (int) interval;
      sleeptime.tv_nsec = 1000000000 * (interval - sleeptime.tv_sec);
      nanosleep(&sleeptime, NULL);
    }

    if(nargs) process(redis, "[REPLY]", (const char **) cmdargs, nargs);
  }

  redisxDisconnect(redis);
  poptFreeContext(optcon);

  return 0;
}
