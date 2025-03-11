/**
 * @date Created  on Dec 11, 2024
 * @author Attila Kovacs
 */

#define _POSIX_C_SOURCE  199309L      ///< for nanosleep()

// We'll use gcc major version as a proxy for the glibc library to decide which feature macro to use.
// gcc 5.1 was released 2015-04-22...
#if defined(__GNUC__) && (__GNUC__ < 5)
#  define _BSD_SOURCE             ///< strcasecmp() feature macro for glibc <= 2.19
#else
#  define _DEFAULT_SOURCE         ///< strcasecmp() feature macro starting glibc 2.20 (2014-09-08)
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <popt.h>
#include <time.h>
#include <math.h>
#include <pthread.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <bsd/readpassphrase.h>

#include "redisx-priv.h"
#include <xjson.h>

#define FORMAT_JSON 1
#define FORMAT_RAW  2

#define POLL_MILLIS 10

static char *host = "127.0.0.1";
static int port = 6379;
static int format = 0;
static char *delim = "\\n";
static char *groupDelim = "\\n";
static int attrib = 0;

static Redis *redis;
static RedisCluster *cluster;

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;


static void printVersion(const char *name) {
  printf("%s %s\n", name, REDISX_VERSION_STRING);
}

static void printRESP(const RESP *resp) {
  switch(format) {
    case FORMAT_JSON: {
      const char *type = "REPLY";

      if(resp) {
        if(resp->type == RESP3_PUSH) type = "PUSH";
        else if(resp->type == RESP3_ATTRIBUTE) type = "ATTRIBUTES";
      }

      redisxPrintJSON(type, resp);
      break;
    }

    case FORMAT_RAW:
      redisxPrintDelimited(resp, delim, groupDelim);
      break;

    default:
      redisxPrintRESP(resp);
  }
}

static void printResult(const RESP *reply, const RESP *attr, const RESP *push) {
  if(format == FORMAT_JSON) {
     // Bundle reply and attributes into one JSON document.
     XStructure *s = xCreateStruct();
     char *json;

     // We add them in reverse order...
     if(push) xSetField(s, redisxRESP2XField("PUSH", push));
     if(attr) xSetField(s, redisxRESP2XField("ATTRIBUTES", attr));
     if(reply) xSetField(s, redisxRESP2XField("REPLY", reply));

     json = xjsonToString(s);
     xDestroyStruct(s);

     if(json) {
       printf("%s", json);
       free(json);
     }
     else printf("{}\n");
   }
   else {
     if(reply) printRESP(reply);
     if(attr) printRESP(attr);
     if(push) printRESP(push);
   }
}

static void *ListenerThread(void *nil) {
  (void) nil;

  while(TRUE) {
    struct timespec nap;

    nap.tv_sec = POLL_MILLIS / 1000;
    nap.tv_nsec = 1000000 * (POLL_MILLIS % 1000);

    pthread_mutex_lock(&mutex);

    if(redis && redisxLockConnected(redis->interactive) == X_SUCCESS) {
      while(redisxGetAvailableAsync(redis->interactive) > 0) {
        int status = X_SUCCESS;
        const RESP *resp = redisxReadReplyAsync(redis->interactive, &status);
        if(status) break;
        printResult(NULL, NULL, resp);
      }
      redisxUnlockClient(redis->interactive);
    }

    pthread_mutex_unlock(&mutex);

    nanosleep(&nap, NULL);
  }

  return NULL; /* NOT REACHED */
}

static void process(const char **cmdargs, int nargs) {
  int status = X_SUCCESS;
  RESP *reply, *attr = NULL;

  pthread_mutex_lock(&mutex);

  if(cluster) {
    const char *key = NULL;

    if(nargs > 1) {
      key = cmdargs[1];
      if(nargs > 3) if(strcasecmp("EVAL", cmdargs[0]) == 0 || strcasecmp("EVALSHA", cmdargs[0]) || strcasecmp("FCALL", cmdargs[0]))
        key = cmdargs[3];
    }

    redis = redisxClusterGetShard(cluster, key);
    if(!redis) {
      fprintf(stderr, "ERROR! No suitable cluster node found for transaction.");
      pthread_mutex_unlock(&mutex);
      return;
    }
  }

  reply = redisxArrayRequest(redis, cmdargs, NULL, nargs, &status);
  if(!status && attrib) attr = redisxGetAttributes(redis);
  pthread_mutex_unlock(&mutex);

  printResult(reply, attr, NULL);

  redisxDestroyRESP(attr);
  redisxDestroyRESP(reply);
}

// cppcheck-suppress constParameterCallback
static void PushProcessor(RedisClient *cl, RESP *resp, void *ptr) {
  (void) cl;
  (void) ptr;
  printRESP(resp);
}

static int interactive(Redis *redis) {
  pthread_t listenerTID;
  char *prompt = malloc(strlen(host) + 20);
  sprintf(prompt, "%s:%d> ", host, port);

  using_history();

  if(pthread_create(&listenerTID, NULL, ListenerThread, NULL) < 0) {
    perror("ERROR! launching listener thread");
    exit(1);
  }

  for(;;) {
    char *line = readline(prompt);
    const char **args;
    int nargs;

    if(!line) continue;

    if(strcmp("quit", line) == 0 || strcmp("exit", line) == 0) break;

    poptParseArgvString(line, &nargs, &args);

    if(args) {
      if(nargs > 0) {
        process(args, nargs);
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
static const char **setScriptArgs(char *script, const char **args, int *nargs) {
  int i, to = 0, n = *nargs, nkeys = 0;
  char keys[20];
  const char **a;

  // Count the number of keys on the command-line up to the comma separator
  for(nkeys = 0; nkeys < n; nkeys++) if(strcmp(",", args[nkeys])) break;
  sprintf(keys, "%d", nkeys);

  a = (const char **) calloc((n + 2), sizeof(char *));
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
  int tls = 0;
  char *ca_file = NULL;
  char *ca_path = NULL;
  char *cert_file = NULL;
  char *key_file = NULL;
  char *ciphers = NULL;
  char *cipher_suites = NULL;
  char *sni = NULL;
  int skip_verify = 0;
  int clusterMode = 0;
  int scan = 0;
  char *pattern = NULL;
  int scanCount = 10;

  struct poptOption options[] = { //
          {"host",       'h', POPT_ARG_STRING | POPT_ARGFLAG_SHOW_DEFAULT,    &host,     0, "Server hostname.", "<hostname>"}, //
          {"port",       'p', POPT_ARG_INT    | POPT_ARGFLAG_SHOW_DEFAULT,    &port,     0, "Server port.", "<port>"}, //
          {"timeout",    't', POPT_ARG_DOUBLE, &timeout,     0, "Server connection timeout (decimals allowed).", "<seconds>"}, //
          {"pass",       'a', POPT_ARG_STRING, &password,    0, "Password to use when connecting to the server.", "<password>"}, //
          {"user",         0, POPT_ARG_STRING, &user,        0, "Used to send ACL style 'AUTH username pass'. Needs -a.", "<username>"}, //
          {"askpass",      0, POPT_ARG_NONE,   &askpass,     0, "Force user to input password with mask from STDIN.  " //
                  "If this argument is used, '-a' will be ignored.", NULL //
          }, //
          {"repeat",    'r', POPT_ARG_INT,    &repeat,       0, "Execute specified command this many times.", "<times>"}, //
          {"interval",  'i', POPT_ARG_DOUBLE | POPT_ARGFLAG_SHOW_DEFAULT, &interval,     0, "When -r is used, waits this many seconds before repeating.  " //
                  "It is possible to specify sub-second times like -i 0.1.", "<seconds>" //
          }, //
          {"db",        'n', POPT_ARG_INT     | POPT_ARGFLAG_SHOW_DEFAULT, &dbIndex,     0, "Database number.", "<index>"}, //
          {"RESP2",     '2', POPT_ARG_VAL,    &protocol,     2, "Start session in RESP2 protocol mode.", NULL}, //
          {"RESP3",     '3', POPT_ARG_VAL,    &protocol,     3, "Start session in RESP3 protocol mode.", NULL}, //
          {"stdin",     'x', POPT_ARG_VAL,    &input,        0, "Read last argument from STDIN", NULL}, //
          {"json",        0, POPT_ARG_NONE,   NULL,        'j', "Output in JSON format", NULL}, //
          {"raw",         0, POPT_ARG_NONE,   NULL,        'r', "Use raw formatting for replies (with delimiters).", NULL}, //
          {"delim",     'd', POPT_ARG_STRING  | POPT_ARGFLAG_SHOW_DEFAULT, &delim,       0, "Delimiter between elements for raw format.  " //
                  "You can use JSON convention for escaping special characters.", "<string>" //
          },
          {"prefix",    'D', POPT_ARG_STRING  | POPT_ARGFLAG_SHOW_DEFAULT, &groupDelim,  0, "Group prefix for raw format.  " //
                  "You can use JSON convention for escaping special characters.", "<string>" //
          },
          {"cluster",   'c', POPT_ARG_NONE,   &clusterMode,  0, "Enable cluster mode (follow -ASK and -MOVED redirections).", NULL}, //
          {"tls",         0, POPT_ARG_NONE,   &tls,          0, "Establish a secure TLS connection.", NULL}, //
          {"sni",         0, POPT_ARG_NONE,   &sni,          0, "Server name indication for TLS.", "<host>"}, //
          {"cacert",      0, POPT_ARG_STRING, &ca_file,      0, "CA Certificate file to verify with.", "<file>"}, //
          {"cacertdir",   0, POPT_ARG_STRING, &ca_path,      0, "Directory where trusted CA certificates are stored.  If neither cacert nor cacertdir are "
                 "specified, the default system-wide trusted root certs configuration will apply.", "<path>" //
          },
          {"insecure",    0, POPT_ARG_NONE,   &skip_verify,  0, "Allow insecure TLS connection by skipping cert validation.", NULL}, //
          {"cert",        0, POPT_ARG_STRING, &cert_file,    0, "Client certificate to authenticate with.", "<path>"}, //
          {"key",         0, POPT_ARG_STRING, &key_file,     0, "Private key file to authenticate with.", "<path>"}, //
          {"tls-ciphers", 0, POPT_ARG_STRING, &ciphers,      0, "Sets the list of preferred ciphers (TLSv1.2 and below) in order of preference from "
                  "highest to lowest separated by colon (':').", "<list>"}, //
          {"tls-ciphersuites", 0, POPT_ARG_STRING, &cipher_suites,    0, "Sets the list of preferred ciphersuites (TLSv1.3) in order of preference from "
                  "highest to lowest separated by colon (':').", "<list>"}, //
          {"show-pushes", 0, POPT_ARG_STRING  | POPT_ARGFLAG_SHOW_DEFAULT, &push,        0, "Whether to print RESP3 PUSH messages.", "yes|no" }, //
          {"attributes",  0, POPT_ARG_NONE,   &attrib,       0, "Show RESP3 attributes also, if available.", NULL}, //
          {"scan",        0, POPT_ARG_NONE,   &scan,         0, "List all keys using the SCAN command.", NULL}, //
          {"pattern",     0, POPT_ARG_STRING, &pattern,      0, "Keys pattern when using the --scan (default: *).", "<glob>"}, //
          {"count",       0, POPT_ARG_INT     | POPT_ARGFLAG_SHOW_DEFAULT, &scanCount,   0, "Count option when using the --scan.", "<n>"}, //
          {"eval",        0, POPT_ARG_STRING, &push,         0, "Send an EVAL command using the Lua script at <file>.  " //
                  "The keyword and other arguments should be separated with a standalone comma on the command-line, such as: 'key1 key2 , arg1 arg2 ...'", "<file>" //
          },
          {"verbose",     0, POPT_ARG_NONE,   &verbose,      0, "Verbose mode.", NULL }, //
          {"debug",       0, POPT_ARG_NONE   | POPT_ARGFLAG_DOC_HIDDEN,    &debug,       0, "Debug mode. Prints all network traffic.", NULL }, //
          {"version",     0, POPT_ARG_NONE,   NULL,        'v', "Output version and exit.", NULL }, //

          POPT_AUTOHELP POPT_TABLEEND //
  };

  int rc;
  const char **cmdargs;
  int i, nargs = 0;

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

  cmdargs = (const char **) poptGetArgs(optcon);

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
  if(!redis) return x_trace(fn, NULL, X_FAILURE);

  if(port <= 0) port = 6369;
  else redisxSetPort(redis, port);

  if(user) redisxSetUser(redis, user);
  if(password) redisxSetPassword(redis, password);
  if(dbIndex > 0) redisxSelectDB(redis, dbIndex);
  if(timeout > 0.0) {
    int ms = (int) ceil(1000 * timeout);
    redisxSetSocketTimeout(redis, ms);
    redisxSetReplyTimeout(redis, ms);
  }
  if(protocol > 0) redisxSetProtocol(redis, protocol);

  if(push) if(strcmp("y", push) == 0 || strcmp("yes", push) == 0) redisxSetPushProcessor(redis, PushProcessor, NULL);

  if(tls) {
#if WITH_TLS
    if(skip_verify) redisxSetTLSVerify(redis, FALSE);
    if(sni) redisxSetTLSServerName(redis, sni);
    if(ca_path || ca_file) redisxSetTLS(redis, ca_path, ca_file);
    if(cert_file && key_file) redisxSetMutualTLS(redis, cert_file, key_file);
    else if(cert_file || key_file) {
      fprintf(stderr, "ERROR! Need both --cert and --key options.\n");
      exit(1);
    }
    if(ciphers) redisxSetTLSCiphers(redis, ciphers);
    if(cipher_suites) redisxSetTLSCipherSuites(redis, cipher_suites);
#else
    fprintf(stderr, "ERROR! Cannot use TLS: RedisX was built without TLS support.\n");
#endif
  }

  xSetDebug(1);

  if(clusterMode) {
    cluster = redisxClusterInit(redis);
    if(cluster) {
      redisxDestroy(redis);
      redis = NULL;
    }
  }

  if(!cluster) {
    if(!redis) return x_error(X_NULL, EAGAIN, fn, "redis is NULL");
    prop_error(fn, redisxConnect(redis, FALSE));

    if(scan) {
      int n = 0;
      char **keys;

      prop_error(fn, redisxSetScanCount(redis, scanCount));

      keys = redisxScanKeys(redis, pattern, &n);
      prop_error(fn, n);

      for(i = 0; i < n; i++) printf("\"%s\"\n", keys[i]);
      redisxDestroyKeys(keys, n);

      if(!cmdargs) return X_SUCCESS;
    }
  }

  if(!cmdargs) return interactive(redis);

  while(cmdargs[nargs]) nargs++;

  for(i = 0; i < repeat; i++) {
    if(i > 0 && interval > 0.0) {
      struct timespec nap;
      nap.tv_sec = (int) interval;
      nap.tv_nsec = 1000000000 * (interval - nap.tv_sec);
      nanosleep(&nap, NULL);
    }

    if(nargs) process(cmdargs, nargs);
  }

  if(cluster) redisxClusterDisconnect(cluster);
  else redisxDisconnect(redis);

  poptFreeContext(optcon);

  return 0;
}
