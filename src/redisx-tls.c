/**
 * @file
 *
 * @date Created  on Jan 6, 2025
 * @author Attila Kovacs
 */

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>

#include "redisx-priv.h"

#if WITH_TLS
#  include <openssl/err.h>
#endif

#if WITH_TLS
/// \cond PRIVATE

static int initialized = FALSE;

/**
 * Shuts down SSL and frees up the SSL-related resources on a client.
 *
 * @param cp    Private client data
 *
 * @sa rConnectTLSClient()
 */
void rDestroyClientTLS(ClientPrivate *cp) {
  if(cp->ssl) {
    SSL_shutdown(cp->ssl);
    cp->ssl = NULL;
  }

  if(cp->ctx) {
    SSL_CTX_free(cp->ctx);
    cp->ctx = NULL;
  }
}

/**
 * Connects a client using the specified TLS configuration.
 *
 * @param cp    Private client data
 * @param tls   TLS configuration
 * @return      X_SUCCESS (0) if successful, or else an error code &lt;0.
 */
int rConnectTLSClient(ClientPrivate *cp, const TLSConfig *tls) {
  static const char *fn = "rConnectClientTLS";
  static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

  const SSL_METHOD *method;
  X509 *server_cert;

  if(!tls->certificate) return x_error(X_NULL, EINVAL, fn, "certificate is NULL");

  // Initialize SSL lib only once...
  pthread_mutex_lock(&mutex);
  if(!initialized) {
    SSL_library_init();
    SSL_load_error_strings();
    SSLeay_add_ssl_algorithms();
    initialized = TRUE;
  }
  pthread_mutex_unlock(&mutex);

  method = TLS_client_method();

  cp->ctx = SSL_CTX_new(method);
  if (!cp->ctx) {
    x_error(0, errno, fn, "Failed to create SSL context");
    if(redisxIsVerbose()) ERR_print_errors_fp(stderr);
    goto abort; // @suppress("Goto statement used")
  }

  if(tls->certificate && tls->key) {
    /* Set the key and cert */
    if (SSL_CTX_use_certificate_file(cp->ctx, tls->certificate, SSL_FILETYPE_PEM) <= 0) {
      x_error(0, errno, fn, "Failed to set certificate: %s", tls->certificate);
      if(redisxIsVerbose()) ERR_print_errors_fp(stderr);
      goto abort; // @suppress("Goto statement used")
    }

    if (SSL_CTX_use_PrivateKey_file(cp->ctx, tls->key, SSL_FILETYPE_PEM) <= 0 ) {
      x_error(0, errno, fn, "Failed to set certificate: %s", tls->key);
      if(redisxIsVerbose()) ERR_print_errors_fp(stderr);
      goto abort; // @suppress("Goto statement used")
    }
  }

  if(tls->dh_params) if(!SSL_CTX_set_tmp_dh(cp->ctx, tls->dh_params)) {
    x_error(0, errno, fn, "Failed to set DH-based cypher parameters from: %s", tls->dh_params);
    goto abort; // @suppress("Goto statement used")
  }

  SSL_set_fd(cp->ssl, cp->socket);
  if(!SSL_connect(cp->ssl)) {
    x_error(0, errno, fn, "TLS connect failed");
    goto abort; // @suppress("Goto statement used")
  }

  server_cert = SSL_get_peer_certificate(cp->ssl);
  if(!server_cert) {
    x_error(0, errno, fn, "Failed to obtain X.509 certificate");
    goto abort; // @suppress("Goto statement used")
  }

  if(redisxIsVerbose()) {
    printf("Redis-X> Server certificate: \n");
    char *str = X509_NAME_oneline(X509_get_subject_name(server_cert), 0, 0);
    if(str) {
      printf("    subject: %s\n", str);
      OPENSSL_free(str);
    }
    else printf("<null>\n");

    str = X509_NAME_oneline(X509_get_issuer_name(server_cert), 0, 0);
    if(str) {
      printf("    issuer: %s\n", str);
      OPENSSL_free(str);
    }
    else printf("<null>\n");
  }

  X509_free(server_cert);

  return X_SUCCESS;

  // -------------------------------------------------------------------------

  abort:

  rDestroyClientTLS(cp);
  return x_error(X_FAILURE, errno, fn, "TLS connection failed.");
}

/// \endcond

#endif



/**
 * Configures a TLS-encrypted connection to Redis with the specified CA certificate file. Normally you
 * will want to set up mutual TLS with redisxSetMutualTLS() also, unless the server is not requiring
 * mutual authentication. Additionally, you might also want to set parameters for DH-based cyphers if
 * needed using redisxSetDHParams().
 *
 * @param redis     A Redis instance
 * @param ca_file   Path to the CA certificate file
 * @return          X_SUCCESS (0) if successful, or else an error code &lt;0.
 *
 * @sa redisxSetMutualTLS()
 * @sa redisxSetDHParams()
 */
int redisxSetTLS(Redis *redis, const char *ca_file) {
  static const char *fn = "redisxSetCA";

#if WITH_TLS
  RedisPrivate *p;
  TLSConfig *tls;

  if(!ca_file) return x_error(X_NULL, EINVAL, fn, "CA file is NULL");

  if(access(ca_file, R_OK) != 0) return x_error(X_FAILURE, errno, fn, "CA file not readable: %s", ca_file);

  prop_error(fn, rConfigLock(redis));

  p = (RedisPrivate *) redis->priv;
  tls = &p->config.tls;

  tls->enabled = TRUE;
  tls->ca_certificate = (char *) ca_file;

  rConfigUnlock(redis);

  return X_SUCCESS;
#else
  (void) redis;
  (void) ca_file;

  return x_error(X_FAILURE, ENOSYS, fn, "RedisX was built without TLS support");
#endif
}

/**
 * Set a TLS certificate and private key for mutual TLS. You will still need to call redisxSetTLS() also to create a
 * complete TLS configuration. Redis normally uses mutual TLS, which requires both the client and the server to
 * authenticate themselves. For this you need the server's TLS certificate and private key also. It is possible to
 * configure Redis servers to verify one way only with a CA certificate, in which case you don't need to call this to
 * configure the client.
 *
 * @param redis       A Redis instance
 * @param cert_file   Path to the server's certificate file
 * @param key_file    Path to the server'sprivate key file
 * @return            X_SUCCESS (0) if successful, or else an error code &lt;0.
 *
 * @sa redisxSetTLS()
 */
int redisxSetMutualTLS(Redis *redis, const char *cert_file, const char *key_file) {
  static const char *fn = "redisxSetMutualTLS";

#if WITH_TLS
  RedisPrivate *p;
  TLSConfig *tls;

  if(!cert_file || !key_file) return x_error(X_NULL, EINVAL, fn, "Null parameter(s): cert_file=%p, key_file=%p", cert_file, key_file);

  if(access(cert_file, R_OK) != 0) return x_error(X_FAILURE, errno, fn, "Certificate file not readable: %s", cert_file);
  if(access(key_file, R_OK) != 0) return x_error(X_FAILURE, errno, fn, "Private key file not readable: %s", key_file);

  prop_error(fn, rConfigLock(redis));

  p = (RedisPrivate *) redis->priv;
  tls = &p->config.tls;

  tls->certificate = (char *) cert_file;
  tls->key = (char *) key_file;

  rConfigUnlock(redis);

  return X_SUCCESS;
#else
  (void) redis;
  (void) cert_file;
  (void) key_file;

  return x_error(X_FAILURE, ENOSYS, fn, "RedisX was built without TLS support");
#endif
}


/**
 * Sets parameters for DH-based cyphers when using a TLS encrypted connection to Redis.
 *
 * @param redis            A Redis instance
 * @param dh_params_file   Path to the DH-based cypher parameters file
 * @return                 X_SUCCESS (0) if successful, or else an error code &lt;0.
 *
 * @sa redisxSetTLS()
 */
int redisxSetDHParams(Redis *redis, const char *dh_params_file) {
  static const char *fn = "redisxSetDHParams";

#if WITH_TLS
  RedisPrivate *p;
  TLSConfig *tls;

  if(!dh_params_file) return x_error(X_NULL, EINVAL, fn, "DH parameters file is NULL");

  if(access(dh_params_file, R_OK) != 0) return x_error(X_FAILURE, errno, fn, "CA file not readable: %s", dh_params_file);

  prop_error(fn, rConfigLock(redis));

  p = (RedisPrivate *) redis->priv;
  tls = &p->config.tls;

  tls->dh_params = (char *) dh_params_file;

  rConfigUnlock(redis);

  return X_SUCCESS;
#else
  (void) redis;
  (void) dh_params_file;

  return x_error(X_FAILURE, ENOSYS, fn, "RedisX was built without TLS support");
#endif
}


