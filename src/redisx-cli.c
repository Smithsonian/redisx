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
#include <xjson.h>

#define FORMAT_JSON 1
#define FORMAT_RAW  2

static char *host = "127.0.0.1";
static int port = 6379;
static int format = 0;
static char *delim = "\\n";
static char *groupDelim = "\\n";
static int attrib = 0;

static void printVersion(const char *name) {
  printf("%s %s\n", name, REDISX_VERSION_STRING);
}


static void printRESP(const RESP *resp) {
  if(format == FORMAT_JSON) {
    const char *type = "REPLY";

    if(resp) {
      if(resp->type == RESP3_PUSH) type = "PUSH";
      else if(resp->type == RESP3_ATTRIBUTE) type = "ATTRIBUTES";
    }

    redisxPrintJSON(type, resp);
  }
  if(format == FORMAT_RAW) redisxPrintDelimited(resp, delim, groupDelim);
  else redisxPrintRESP(resp);
}

static void process(Redis *redis, const char **cmdargs, int nargs) {
  int status = X_SUCCESS;
  RESP *reply = redisxArrayRequest(redis, cmdargs, NULL, nargs, &status);
  if(!status) printRESP(reply);
  redisxDestroyRESP(reply);

  if(attrib) {
    reply = redisxGetAttributes(redis);
    if(reply) {
      printRESP(reply);
      redisxDestroyRESP(reply);
    }
  }
}

// cppcheck-suppress constParameterCallback
static void PushProcessor(RedisClient *cl, RESP *resp, void *ptr) {
  (void) cl;
  (void) ptr;
  printRESP(resp);
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
        process(redis, (const char **) args, nargs);
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

/**
 * Converts command-line arguments to parameters passed to EVAL.
 *
 * @param script    The script itself
 * @param args      The command-line arguments of the form `key1 key2 ... , arg1 arg2...`.
 * @param nargs     The number of command-line args
 * @return          The argument list to pass to Redis , of the form:
 *                  `EVAL <script> <NKEYS> [key1 [key2] ...] [arg1 [arg2 ...]]`
 */
static char **setScriptArgs(char *script, char **args, int *nargs) {
  int i, to = 0, n = *nargs, nkeys = 0;
  char keys[20], **a;

  // Count the number of keys on the command-line up to the comma separator
  for(nkeys = 0; nkeys < n; nkeys++) if(strcmp(",", args[nkeys])) break;
  sprintf(keys, "%d", nkeys);

  a = (char **) calloc((n + 2), sizeof(char *));
  x_check_alloc(a);

  a[to++] = "EVAL";
  a[to++] = script;
  a[to++] = xStringCopyOf(keys);

  for(i = 0; i < nkeys; i++) a[to++] = args[i];       // key arguments
  for(i = nkeys + 1; i < n; i++) a[to++] = args[i];   // additional parametes

  *nargs += 2;

  return a;
}

int main(int argc, const char *argv[]) {
  static const char *fn = "redisx-cli";

  double timeout = 0.0;
  char *password = NULL;
  char *user = NULL;
  int askpass = 0;
  int repeat = 1;
  double interval = 1.0;
  int dbIndex = 0;
  int protocol = -1;
  int input = 0;
  char *push = "yes";
  const char *eval = NULL;
  int verbose = 0;
  int debug = 0;

  struct poptOption options[] = { //
          {"host",       'h', POPT_ARG_STRING | POPT_ARGFLAG_SHOW_DEFAULT,    &host,     0, "Server hostname.", "<hostname>"}, //
          {"port",       'p', POPT_ARG_INT    | POPT_ARGFLAG_SHOW_DEFAULT,    &port,     0, "Server port.", "<port>"}, //
          {"timeout",    't', POPT_ARG_DOUBLE, &timeout,    0, "Server connection timeout (decimals allowed).", "<seconds>"}, //
          {"pass",       'a', POPT_ARG_STRING, &password,   0, "Password to use when connecting to the server.", "<password>"}, //
          {"user",        0, POPT_ARG_STRING, &user,       0, "Used to send ACL style 'AUTH username pass'. Needs -a.", "<username>"}, //
          {"askpass",     0, POPT_ARG_NONE,   &askpass,    0, "Force user to input password with mask from STDIN.  " //
                  "If this argument is used, '-a' will be ignored.", NULL //
          }, //
          {"repeat",    'r', POPT_ARG_INT,    &repeat,     0, "Execute specified command N times.", "<times>"}, //
          {"interval",  'i', POPT_ARG_DOUBLE | POPT_ARGFLAG_SHOW_DEFAULT, &interval, 0, "When -r is used, waits <interval> seconds per command.  " //
                  "It is possible to specify sub-second times like -i 0.1.", "<seconds>" //
          }, //
          {"db",        'n', POPT_ARG_INT,    &dbIndex,    0, "Database number.", "<index>"}, //
          {"RESP2",     '2', POPT_ARG_VAL,    &protocol,   2, "Start session in RESP2 protocol mode.", NULL}, //
          {"RESP3",     '3', POPT_ARG_VAL,    &protocol,   3, "Start session in RESP3 protocol mode.", NULL}, //
          {"stdin",     'x', POPT_ARG_VAL,    &input,      0, "Read last argument from STDIN", NULL}, //
          {"json",        0, POPT_ARG_NONE,   NULL,      'j', "Output in JSON format", NULL}, //
          {"raw",         0, POPT_ARG_NONE,   NULL,      'r', "Use raw formatting for replies (with delimiters).", NULL}, //
          {"delim",     'd', POPT_ARG_STRING  | POPT_ARGFLAG_SHOW_DEFAULT, &delim,      0, "delimiter between elements for raw format.  " //
                  "You can use JSON convention for escaping special characters.", "<string>" //
          },
          {"group",     'D', POPT_ARG_STRING  | POPT_ARGFLAG_SHOW_DEFAULT, &groupDelim, 0, "group prefix for raw format.  " //
                  "You can use JSON convention for escaping special characters.", "<string>" //
          },
          {"show-pushes", 0, POPT_ARG_STRING  | POPT_ARGFLAG_SHOW_DEFAULT, &push,       0, "Whether to print RESP3 PUSH messages.", "yes|no" }, //
          {"attributes",  0, POPT_ARG_NONE,   &attrib,     0, "Show RESP3 attributes also, if available.", NULL}, //
          {"eval",        0, POPT_ARG_STRING, &push,       0, "Send an EVAL command using the Lua script at <file>.  " //
                  "The keyword and other arguments should be separated with a standalone comma on the command-line, such as: 'key1 key2 , arg1 arg2 ...'", "<file>" //
          },
          {"verbose",     0, POPT_ARG_NONE,   &verbose,    0, "Verbose mode.", NULL }, //
          {"debug",       0, POPT_ARG_NONE   | POPT_ARGFLAG_DOC_HIDDEN,   &debug  ,    0, "Debug mode. Prints all network traffic.", NULL }, //
          {"version",     0, POPT_ARG_NONE,   NULL,      'v', "Output version and exit.", NULL }, //

          POPT_AUTOHELP POPT_TABLEEND //
  };

  int rc;
  char **cmdargs;
  int i, nargs = 0;
  Redis *redis;

  poptContext optcon = poptGetContext(fn, argc, argv, options, 0);
  poptSetOtherOptionHelp(optcon, "[OPTIONS] [cmd [arg [arg ...]]]");

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
      case 'v': printVersion(fn); return 0;
    }
  }

  delim = xjsonUnescape(delim);
  groupDelim = xjsonUnescape(groupDelim);

  cmdargs = (char **) poptGetArgs(optcon);

  if(askpass) {
    password = (char *) malloc(1024);
    if(readpassphrase("Enter password: ", password, 1024, 0) == NULL) {
      free(password);
      password = NULL;
    }
  }

  if(eval) {
    // Make an EVAL <script> <NKEYS> ... command
    cmdargs = setScriptArgs(readScript(eval), cmdargs, &nargs);
  }

  if(input) {
    // Add trailing argument from STDIN.
    cmdargs = realloc(cmdargs, nargs + 1 * sizeof(char *));
    x_check_alloc(cmdargs);
    cmdargs[nargs++] = readline(NULL);
  }

  if(verbose) redisxSetVerbose(1);
  if(debug) redisxDebugTraffic(1);

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

    if(nargs) process(redis, (const char **) cmdargs, nargs);
  }

  redisxDisconnect(redis);
  poptFreeContext(optcon);

  return 0;
}
