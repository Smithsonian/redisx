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
#  include <openssl/decoder.h>    // For DH parameters
#  include <openssl/err.h>
#endif

#if WITH_TLS
/// \cond PRIVATE

static int initialized = FALSE;

/**
 * Shuts down SSL and frees up the SSL-related resources on a client.
 *
 * @param cp    Private client data.
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
 * Loads parameters from a file for DH-based ciphers.
 *
 * Based on https://github.com/openssl/openssl/blob/master/apps/dhparam.c
 *
 * @param ctx       The SSL context for which to set DH parameters
 * @param filename  The DH parameter filename (in PEM format)
 * @return          X_SUCCESS (0) if successful, or else an error code &lt;0.
 *
 * @sa rConnectTLSClient()
 */
 static int rSetDHParamsFromFile(SSL_CTX *ctx, const char *filename) {
  static const char *fn = "rSetSHParamsFromFile";

  BIO *in;
  OSSL_DECODER_CTX *dctx = NULL;
  EVP_PKEY *pkey = NULL;
  int status = X_SUCCESS;

  in = BIO_new_file(filename, "rb");
  if(!in) return x_error(0, errno, fn, "Could not open DH parameters: %s", filename);

  dctx = OSSL_DECODER_CTX_new_for_pkey(&pkey, "PEM", NULL, NULL, OSSL_KEYMGMT_SELECT_DOMAIN_PARAMETERS, NULL, NULL);
  if(!dctx) {
    status = x_error(X_FAILURE, errno, fn, "Failed to create decoder context");
    goto cleanup; // @suppress("Goto statement used")
  }

  if(!OSSL_DECODER_from_bio(dctx, in)) {
    status = x_error(X_FAILURE, errno, fn, "Could not decode DH parameters from: %s", filename);
    goto cleanup; // @suppress("Goto statement used")
  }

  if (!EVP_PKEY_is_a(pkey, "DH")) {
    status = x_error(X_FAILURE, errno, fn, "Invalid DH parameters in: %s", filename);
    goto cleanup; // @suppress("Goto statement used")
  }

  // The call references the key, does nor copy it. Therefore we should not free it
  // if successful. The set key is freed then later when the context is freed.
  if(!SSL_CTX_set0_tmp_dh_pkey(ctx, pkey)) {
    EVP_PKEY_free(pkey);
    status = x_error(X_FAILURE, errno, fn, "Failed to set DH parameters");
  }

  cleanup:

  if(dctx) OSSL_DECODER_CTX_free(dctx);
  BIO_free(in);

  return status;
}

/**
 * Connects a client using the specified TLS configuration.
 *
 * @param cp    Private client data.
 * @param tls   TLS configuration.
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

  if(!tls->skip_verify) {
    SSL_CTX_set_verify(cp->ctx, SSL_VERIFY_PEER, NULL);
    SSL_CTX_set_verify_depth(cp->ctx, 4);
  }

  if(tls->ca_certificate || tls->ca_path) if(!SSL_CTX_load_verify_locations(cp->ctx, tls->ca_certificate, tls->ca_path)) {
    x_error(0, errno, fn, "Failed to set CA certificate: %s / %s", tls->ca_path, tls->ca_certificate);
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

    if(!SSL_CTX_check_private_key(cp->ctx)) {
      x_error(0, errno, fn, "Private key does not match the certificate public key.");
      goto abort; // @suppress("Goto statement used")
    }
  }

  if(tls->ciphers) if(!SSL_CTX_set_cipher_list(cp->ctx, tls->ciphers)) {
    x_error(0, errno, fn, "Failed to set ciphers %s", tls->ciphers);
    goto abort; // @suppress("Goto statement used")
  }

#if OPENSSL_VERSION_NUMBER >= 0x1010100f
  // Since OpenSSL version 1.1.1
  if(tls->cipher_suites) if(!SSL_CTX_set_ciphersuites(cp->ctx, tls->cipher_suites)) {
    x_error(0, errno, fn, "Failed to set ciphers= suites %s", tls->ciphers);
    goto abort; // @suppress("Goto statement used")
  }
#endif

  if(tls->dh_params) {
    if(rSetDHParamsFromFile(cp->ctx, tls->dh_params) != X_SUCCESS) goto abort; // @suppress("Goto statement used")
  }
  else if(!SSL_CTX_set_dh_auto(cp->ctx, 1)) {
    x_error(0, errno, fn, "Failed to set automatic DH-based cypher parameters");
    goto abort; // @suppress("Goto statement used")
  }

  cp->ssl = SSL_new(cp->ctx);
  if(!cp->ssl) {
    x_error(0, errno, fn, "Failed to create SSL");
    goto abort; // @suppress("Goto statement used")
  }

  SSL_set_fd(cp->ssl, cp->socket);

  if(tls->hostname) SSL_set_tlsext_host_name(cp->ssl, tls->hostname);

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
 * needed using redisxSetDHCypherParams().
 *
 * @param redis     A Redis instance.
 * @param ca_path   Directory containing CA certificates. It may be NULL to use the default locations.
 * @param ca_file   CA certificate file rel. to specified directory. It may be NULL to use default certificate.
 * @return          X_SUCCESS (0) if successful, or else an error code &lt;0.
 *
 * @sa redisxSetMutualTLS()
 * @sa redisxSetDHCipherParams()
 * @sa redisxSetTLSCiphers()
 * @sa redisxSetTLSCipherSuites()
 * @sa redisxSetTLSServerName()
 * @sa redisxTLSSkipVerify()
 */
int redisxSetTLS(Redis *redis, const char *ca_path, const char *ca_file) {
  static const char *fn = "redisxSetTLS";

#if WITH_TLS
  RedisPrivate *p;
  TLSConfig *tls;

  if(ca_path) if(access(ca_path, R_OK | X_OK) != 0) return x_error(X_FAILURE, errno, fn, "CA path not accessible: %s", ca_file);
  if(ca_file) if(access(ca_file, R_OK) != 0) return x_error(X_FAILURE, errno, fn, "CA file not readable: %s", ca_file);

  prop_error(fn, rConfigLock(redis));

  p = (RedisPrivate *) redis->priv;
  tls = &p->config.tls;

  tls->enabled = TRUE;
  tls->ca_path = xStringCopyOf(ca_path);
  tls->ca_certificate = xStringCopyOf(ca_file);

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
 * @param redis       A Redis instance.
 * @param cert_file   Path to the server's certificate file.
 * @param key_file    Path to the server'sprivate key file.
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

  tls->certificate = xStringCopyOf(cert_file);
  tls->key = xStringCopyOf(key_file);

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
 * Sets the TLS ciphers to try (TLSv1.2 and earlier).
 *
 * @param redis            A Redis instance.
 * @param cipher_list      a colon (:) separated list of ciphers, or NULL for default ciphers.
 * @return                 X_SUCCESS (0) if successful, or else an error code &lt;0.
 *
 * @sa redisxSetTLSCipherSuites()
 * @sa redisxSetTLS()
 * @sa redisxSetDHCipherParams()
 */
int redisxSetTLSCiphers(Redis *redis, const char *cipher_list) {
  static const char *fn = "redisxSetTLSCiphers";

#if WITH_TLS
  RedisPrivate *p;
  TLSConfig *tls;

  prop_error(fn, rConfigLock(redis));

  p = (RedisPrivate *) redis->priv;
  tls = &p->config.tls;

  tls->ciphers = xStringCopyOf(cipher_list);

  rConfigUnlock(redis);

  return X_SUCCESS;
#else
  (void) redis;
  (void) cipher_list;

  return x_error(X_FAILURE, ENOSYS, fn, "RedisX was built without TLS support");
#endif
}

/**
 * Sets the TLS ciphers suites to try (TLSv1.3 and later).
 *
 * @param redis            A Redis instance.
 * @param list             a colon (:) separated list of cipher suites, or NULL for default cipher suites.
 * @return                 X_SUCCESS (0) if successful, or else an error code &lt;0.
 *
 * @sa redisxSetTLSCiphers()
 * @sa redisxSetTLS()
 * @sa redisxSetDHCipherParams()
 */
int redisxSetTLSCipherSuites(Redis *redis, const char *list) {
  static const char *fn = "redisxSetTLSCiphers";

#if WITH_TLS
#  if OPENSSL_VERSION_NUMBER >= 0x1010100f
  // Since OpenSSL version 1.1.1
  RedisPrivate *p;
  TLSConfig *tls;

  prop_error(fn, rConfigLock(redis));

  p = (RedisPrivate *) redis->priv;
  tls = &p->config.tls;

  tls->cipher_suites = xStringCopyOf(list);

  rConfigUnlock(redis);

  return X_SUCCESS;
#  else
  (void) redis;
  (void) list;

  return x_error(X_FAILURE, ENOSYS, fn, "Needs OpenSSL >= 1.1.1 (for TLSv1.3+)");
#  endif // OpenSSL >= 1.1.1
#else
  (void) redis;
  (void) list;

  return x_error(X_FAILURE, ENOSYS, fn, "RedisX was built without TLS support");
#endif
}

/**
 * Sets parameters for DH-based cyphers when using a TLS encrypted connection to Redis.
 *
 * @param redis            A Redis instance.
 * @param dh_params_file   Path to the DH-based cypher parameters file,or NULL for no params.
 * @return                 X_SUCCESS (0) if successful, or else an error code &lt;0.
 *
 * @sa redisxSetTLS()
 * @sa redisxSetTLSCiphers()
 */
int redisxSetDHCipherParams(Redis *redis, const char *dh_params_file) {
  static const char *fn = "redisxSetDHCipherParams";

#if WITH_TLS
  RedisPrivate *p;
  TLSConfig *tls;

  if(dh_params_file) if(access(dh_params_file, R_OK) != 0) return x_error(X_FAILURE, errno, fn, "CA file not readable: %s", dh_params_file);

  prop_error(fn, rConfigLock(redis));

  p = (RedisPrivate *) redis->priv;
  tls = &p->config.tls;

  tls->dh_params = xStringCopyOf(dh_params_file);

  rConfigUnlock(redis);

  return X_SUCCESS;
#else
  (void) redis;
  (void) dh_params_file;

  return x_error(X_FAILURE, ENOSYS, fn, "RedisX was built without TLS support");
#endif
}

/**
 * Sets the Server name for TLS Server Name Indication (SNI), an optional extra later of security.
 *
 * @param redis   A Redis instance.
 * @param host    server name to use for SNI.
 * @return        X_SUCCESS (0)
 *
 * @sa redisxSetTLS()
 */
int redisxSetTLSServerName(Redis *redis, const char *host) {
  static const char *fn = "redisxSetDHCipherParams";

#if WITH_TLS
  RedisPrivate *p;
  TLSConfig *tls;

  prop_error(fn, rConfigLock(redis));

  p = (RedisPrivate *) redis->priv;
  tls = &p->config.tls;

  tls->hostname = xStringCopyOf(host);

  rConfigUnlock(redis);

  return X_SUCCESS;
#else
  (void) redis;
  (void) host;

  return x_error(X_FAILURE, ENOSYS, fn, "RedisX was built without TLS support");
#endif
}

/**
 * Sets whether to verify the the certificate.
 *
 * @param redis   A Redis instance.
 * @param value   TRUE (non-zero) or FALSE (0)
 * @return        X_SUCCESS (0)
 *
 * @sa redisxSetTLS()
 */
int redisxSetTLSSkipVerify(Redis *redis, boolean value) {
  static const char *fn = "redisxTLSSkipVerify";

#if WITH_TLS
  RedisPrivate *p;
  TLSConfig *tls;

  prop_error(fn, rConfigLock(redis));

  p = (RedisPrivate *) redis->priv;
  tls = &p->config.tls;

  tls->skip_verify = (value != 0);

  rConfigUnlock(redis);

  return X_SUCCESS;
#else
  (void) redis;

  return x_error(X_FAILURE, ENOSYS, fn, "RedisX was built without TLS support");
#endif
}
