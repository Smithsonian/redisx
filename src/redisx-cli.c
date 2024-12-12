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

#include "redisx-priv.h"

#define FORMAT_JSON 1
#define FORMAT_RAW  2

static char *host = "127.0.0.1";
static int port = 6379;
static int format = 0;

static void process(Redis *redis, const char **cmdargs, int nargs) {
  int status = X_SUCCESS;
  RESP *reply = redisxArrayRequest(redis, cmdargs, NULL, nargs, &status);

  if(!status) {
    if(format == FORMAT_JSON) redisxPrintJSON("REPLY", reply);
    else redisxPrintRESP(reply);
  }

  redisxDestroyRESP(reply);
}

static char *get_token(char *line, int *end) {
  int from, to, quote = 0, escaped = 0;
  char *token;

  *end = 0;

  while(isspace(*line)) {
    line++;
    (*end)++;
  }

  for(from = 0; line[from]; from++) {
    char c = line[from];

    if(!escaped) {
      if(isspace(c)) break;
      else if(c == '\'' || c == '"') quote = !quote;
      else if(c == '\\') escaped = 1;
    }
    else escaped = 0;
  }

  if(from == 0) return NULL;

  *end += from;
  token = (char *) malloc(from + 1);

  for(from = 0, to = 0; line[from]; from++) {
    char c = line[from];

    if(!escaped) {
      if(isspace(c)) break;
      else if(c == '\'' || c == '"') quote = !quote;
      else if(c == '\\') escaped = 1;
      else token[to++] = c;
    }
    else {
      escaped = 0;
      token[to++] = c;
    }
  }

  token[to] = '\0';
  return token;
}

static char ** get_args(char *line, int *n) {
  char **array;
  int capacity = 10;
  char *tok;
  int i, end;

  array = (char **) malloc(capacity * sizeof(char *));

  for(i = 0; (tok = get_token(line, &end)) != NULL; i++) {
    if(i >= capacity) {
      capacity *= 2;
      array = (char **) realloc(array, capacity * sizeof(char *));
      if(!array) {
        fprintf(stderr, "ERROR! alloc error (%d char *): %s\n", capacity, strerror(errno));
        exit(1);
      }
    }

    array[i] = tok;
    line += end;
  }

  *n = i;
  return array;
}


static int interactive(Redis *redis) {
  char *prompt = malloc(strlen(host) + 20);
  sprintf(prompt, "%s:%d> ", host, port);

  using_history();

  for(;;) {
    char *line = readline(prompt);
    char **args;
    int nargs;

    if(strcmp("quit", line) == 0 || strcmp("exit", line) == 0) break;

    args = get_args(line, &nargs);
    if(args) {
      if(nargs > 0) {
        process(redis, (const char **) args, nargs);
        add_history(line);
      }
      redisxDestroyKeys(args, nargs);
    }

    free(line);
  }

  printf("Bye!\n");
  redisxDisconnect(redis);

  return X_SUCCESS;
}

int main(int argc, const char *argv[]) {
  static const char *fn = "redis-cli";

  double timeout = 0.0;
  char *password = NULL;
  char *user = NULL;
  int repeat = 1;
  double interval = 1.0;
  int dbIndex = 0;
  int protocol = -1;

  struct poptOption options[] = { //
          {NULL,        'h', POPT_ARG_STRING | POPT_ARGFLAG_SHOW_DEFAULT,    &host,     0, "Server hostname.", "<hostname>"}, //
          {NULL,        'p', POPT_ARG_INT    | POPT_ARGFLAG_SHOW_DEFAULT,    &port,     0, "Server port.", "<port>"}, //
          {NULL,        't', POPT_ARG_DOUBLE, &timeout,    0, "Server connection timeout in seconds (decimals allowed).", "<timeout>"}, //
          {NULL,        'a', POPT_ARG_STRING, &password,   0, "Password to use when connecting to the server.", "<password>"}, //
          {NULL,        'u', POPT_ARG_STRING, &user,       0, "Used to send ACL style 'AUTH username pass'. Needs -a.", "<user>"}, //
          {NULL,        'r', POPT_ARG_INT,    &repeat,     0, "Execute specified command N times.", "<repeat>"}, //
          {NULL,        'i', POPT_ARG_DOUBLE | POPT_ARGFLAG_SHOW_DEFAULT, &interval, 0, "When -r is used, waits <interval> seconds per command. " //
                                                            "It is possible to specify sub-second times like -i 0.1.", "<interval>"}, //
          {"db",        'n', POPT_ARG_INT,    &dbIndex,    0, "Database number.", "<dbIdx>"}, //
          {"resp2",     '2', POPT_ARG_VAL,    &protocol,   2, "Start session in RESP2 protocol mode.", NULL}, //
          {"resp3",     '3', POPT_ARG_VAL,    &protocol,   3, "Start session in RESP3 protocol mode.", NULL}, //
          {"json",        0, POPT_ARG_NONE,   NULL,      'j', "Output in JSON format", NULL}, //
          POPT_AUTOHELP POPT_TABLEEND //
  };

  int rc;
  char **cmdargs;
  int i, nargs = 0;
  Redis *redis;

  poptContext optcon = poptGetContext(fn, argc, argv, options, 0);
  poptSetOtherOptionHelp(optcon, "[OPTIONS] [command [args...]]");

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

  redis = redisxInit(host);

  if(port <= 0) port = 6369;
  else redisxSetPort(redis, port);

  if(user) redisxSetUser(redis, user);
  if(password) redisxSetPassword(redis, password);
  if(dbIndex > 0) redisxSelectDB(redis, dbIndex);
  if(timeout > 0.0) redisxSetSocketTimeout(redis, (int) ceil(1000 * timeout));
  if(protocol > 0) redisxSetProtocol(redis, protocol);

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

    process(redis, (const char **) cmdargs, nargs);
  }

  redisxDisconnect(redis);
  poptFreeContext(optcon);

  return 0;
}
