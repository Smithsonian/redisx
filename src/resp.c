/**
 * @file
 *
 * @date Created  on Dec 6, 2024
 * @author Attila Kovacs
 *
 *  A set of utilities for handling RESP responses from a Redis / Valkey server.
 *
 */

// We'll use gcc major version as a proxy for the glibc library to decide which feature macro to use.
// gcc 5.1 was released 2015-04-22...
#ifndef __GNUC__
#  define _DEFAULT_SOURCE         ///< strcasecmp() feature macro starting glibc 2.20 (2014-09-08)
#elif __GNUC__ >= 5 || __clang__
#  define _DEFAULT_SOURCE         ///< strcasecmp() feature macro starting glibc 2.20 (2014-09-08)
#else
#  define _BSD_SOURCE             ///< strcasecmp() feature macro for glibc <= 2.19
#endif


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>

#include "redisx-priv.h"

/**
 * Frees up the resources used by a RESP structure that was dynamically allocated.
 * The call will segfault if the same RESP is destroyed twice or if the argument
 * is a static allocation.
 *
 * \param resp      Pointer to the RESP structure to be destroyed, which may be NULL (no action taken).
 */
void redisxDestroyRESP(RESP *resp) {
  if(resp == NULL) return;

  if(resp->value) switch(resp->type) {
    case RESP_ARRAY:
    case RESP3_SET:
    case RESP3_PUSH: {
      RESP **component = (RESP **) resp->value;
      while(--resp->n >= 0) redisxDestroyRESP(component[resp->n]);
      break;
    }
    case RESP3_MAP:
    case RESP3_ATTRIBUTE: {
      RedisMapEntry *component = (RedisMapEntry *) resp->value;
      while(--resp->n >= 0) {
        RedisMapEntry *e = &component[resp->n];
        redisxDestroyRESP(e->key);
        redisxDestroyRESP(e->value);
      }
      break;
    }
    default:
      ;
  }

  if(resp->value != NULL) free(resp->value);
  free(resp);
}

/**
 * Creates an independent deep copy of the RESP, which shares no references with the original.
 *
 * @param resp    The original RESP data structure (it may be NULL).
 * @return        A copy of the original, with no shared references.
 */
RESP *redisxCopyOfRESP(const RESP *resp) {
  RESP *copy;

  if(!resp) return NULL;

  copy = (RESP *) calloc(1, sizeof(RESP));
  copy->type = resp->type;
  copy->n = resp->n;
  if(resp->value == NULL) return copy;

  switch(resp->type) {
    case RESP_ARRAY:
    case RESP3_SET:
    case RESP3_PUSH: {
      RESP **from = (RESP **) resp->value;
      RESP **to = (RESP **) calloc(resp->n, sizeof(RESP *));
      int i;

      x_check_alloc(copy->value);

      for(i = 0; i < resp->n; i++) to[i] = redisxCopyOfRESP(from[i]);
      copy->value = to;
      break;
    }

    case RESP3_MAP:
    case RESP3_ATTRIBUTE: {
      RedisMapEntry **from = (RedisMapEntry **) resp->value;
      RedisMapEntry **to = (RedisMapEntry **) calloc(resp->n, sizeof(RedisMapEntry *));
      int i;

      x_check_alloc(copy->value);

      for(i = 0; i < resp->n; i++) {
        to[i]->key = redisxCopyOfRESP(from[i]->key);
        to[i]->value = redisxCopyOfRESP(from[i]->value);
      }
      copy->value = to;
      break;
    }

    case RESP_SIMPLE_STRING:
    case RESP_ERROR:
    case RESP_BULK_STRING:
    case RESP3_BLOB_ERROR:
    case RESP3_VERBATIM_STRING:
    case RESP3_BIG_NUMBER:
      copy->value = (char *) malloc(resp->n);
      x_check_alloc(copy->value);
      memcpy(copy->value, resp->value, resp->n);
      break;

    case RESP3_DOUBLE:
      copy->value = (double *) malloc(sizeof(double));
      x_check_alloc(copy->value);
      memcpy(copy->value, resp->value, sizeof(double));
      break;

    default:
      ;
  }

  return copy;
}

/**
 * Checks a Redis RESP for NULL values or unexpected values.
 *
 * \param resp              Pointer to the RESP structure from Redis.
 * \param expectedType      The RESP type expected (e.g. RESP_ARRAY) or 0 if not checking type.
 * \param expectedSize      The expected size of the RESP (array or bytes) or <=0 to skip checking
 *
 * \return      X_SUCCESS (0)                   if the RESP passes the tests, or
 *              X_NULL                          if the RESP is NULL (garbled response).
 *              REDIS_NULL                      if Redis returned (nil),
 *              REDIS_UNEXPECTED_TYPE           if got a reply of a different type than expected
 *              REDIS_UNEXPECTED_ARRAY_SIZE     if got a reply of different size than expected.
 *
 *              or the error returned in resp->n.
 *
 */
int redisxCheckRESP(const RESP *resp, enum resp_type expectedType, int expectedSize) {
  static const char *fn = "redisxCheckRESP";

  if(resp == NULL) return x_error(X_NULL, EINVAL, fn, "RESP is NULL");
  if(resp->type == RESP3_BOOLEAN) {
    if(resp->n != (expectedSize ? 1 : 0)) return x_error(X_FAILURE, EBADMSG, fn, "unexpected boolean value: expected %d, got %d", (expectedSize ? 1 : 0), resp->n);
  }
  if(resp->type != RESP_INT && resp->type != RESP3_NULL) {
    if(resp->n < 0) return x_error(X_FAILURE, EBADMSG, fn, "RESP error code: %d", resp->n);
    if(resp->value == NULL) if(resp->n) return x_error(REDIS_NULL, ENOMSG, fn, "RESP with NULL value, n=%d", resp->n);
  }
  if(expectedType) if(resp->type != expectedType)
    return x_error(REDIS_UNEXPECTED_RESP, ENOMSG, fn, "unexpected RESP type: expected '%c', got '%c'", expectedType, resp->type);
  if(expectedSize > 0) if(resp->n != expectedSize)
    return x_error(REDIS_UNEXPECTED_RESP, ENOMSG, fn, "unexpected RESP size: expected %d, got %d", expectedSize, resp->n);
  return X_SUCCESS;
}

/**
 * Like redisxCheckRESP(), but it also destroys the RESP in case of an error.
 *
 * \param resp              Pointer to the RESP structure from Redis.
 * \param expectedType      The RESP type expected (e.g. RESP_ARRAY) or 0 if not checking type.
 * \param expectedSize      The expected size of the RESP (array or bytes) or <=0 to skip checking
 *
 * \return      The return value of redisxCheckRESP().
 *
 * \sa redisxCheckRESP()
 *
 */
int redisxCheckDestroyRESP(RESP *resp, enum resp_type expectedType, int expectedSize) {
  int status = redisxCheckRESP(resp, expectedType, expectedSize);
  if(status) redisxDestroyRESP(resp);
  prop_error("redisxCheckDestroyRESP", status);
  return status;
}


/**
 * Splits the string value of a RESP into two components, by terminating the first component with a null
 * byte and optionally returning the remaining part and length in the output parameters. Only RESP_ERROR
 * RESP_BLOB_ERROR and RESP_VERBATIM_STRING types can be split this way. All others will return
 * REDIS_UNEXPECTED_RESP.
 *
 * @param resp        The input RESP.
 * @param[out] text   (optional) pointer in which to return the start of the remnant text component.
 * @return n          the length of the remnant text (&lt;=0), or else X_NULL if the input RESP was NULL,
 *                    or REDIS_UNEXPEXCTED_RESP if the input RESP does not contain a two-component string
 *                    value.
 *
 * @sa RESP_ERROR
 * @sa RESP3_BLOB_ERROR
 * @sa RESP3_VERBATIM_STRING
 */
int redisxSplitText(RESP *resp, char **text) {
  static const char *fn = "redisxSplitText";
  char *str;

  if(!resp) return x_error(X_NULL, EINVAL, fn, "input RESP is NULL");

  if(!resp->value) {
    if(text) *text = NULL;
    return 0;
  }

  str = (char *) resp->value;

  switch(resp->type) {
    case RESP3_VERBATIM_STRING:
      if(resp->n < 4)
        return x_error(X_PARSE_ERROR, ERANGE, fn, "value '%s' is too short (%d bytes) for verbatim string type", str, resp->n);
      str[3] = '\0';
      if(text) *text = &str[4];
      return resp->n - 4;

    case RESP_ERROR:
    case RESP3_BLOB_ERROR: {
      const char *code = strtok(str, " \t\r\n");
      int offset = strlen(code) + 1;

      if(offset < resp->n) {
        if(text) *text = &str[offset];
        return resp->n - offset - 1;
      }

      if(text) *text = NULL;
      return 0;
    }

    default:
      return x_error(REDIS_UNEXPECTED_RESP, EINVAL, fn, "RESP type '%c' does not have a two-component string value", resp->type);
  }
}

/**
 * Checks if a RESP holds a scalar type value, such as an integer, a boolean or a double-precision value, or a <i>null</i> value.
 *
 * @param r   Pointer to a RESP data structure
 * @return    TRUE (1) if the data holds a scalar-type value, or else FALSE (0).
 *
 * @sa redisxIsStringType()
 * @sa redisxIsArrayType()
 * @sa redisxIsMapType()
 * @sa RESP_INT
 * @sa RESP3_BOOLEAN
 * @sa RESP3_DOUBLE
 * @sa RESP3_NULL
 *
 */
boolean redisxIsScalarType(const RESP *r) {
  if(!r) return FALSE;

  switch(r->type) {
    case RESP_INT:
    case RESP3_BOOLEAN:
    case RESP3_DOUBLE:
    case RESP3_NULL:
      return TRUE;

    default:
      return FALSE;
  }

}

/**
 * Checks if a RESP holds a string type value, whose `value` can be cast to `(char *)` to use.
 *
 * @param r   Pointer to a RESP data structure
 * @return    TRUE (1) if the data holds a string type value, or else FALSE (0).
 *
 * @sa redisxIsScalarType()
 * @sa redisxIsArrayType()
 * @sa redisxIsMapType()
 * @sa RESP_SIMPLE_STRING
 * @sa RESP_ERROR
 * @sa RESP_BULK_STRING
 * @sa RESP3_BLOB_ERROR
 * @sa RESP3_VERBATIM_STRING
 *
 */
boolean redisxIsStringType(const RESP *r) {
  if(!r) return FALSE;

  switch(r->type) {
    case RESP_SIMPLE_STRING:
    case RESP_ERROR:
    case RESP_BULK_STRING:
    case RESP3_BLOB_ERROR:
    case RESP3_VERBATIM_STRING:
    case RESP3_BIG_NUMBER:
      return TRUE;

    default:
      return FALSE;
  }
}

/**
 * Checks if a RESP holds an array of RESP pointers, and whose `value` can be cast to `(RESP **)` to use.
 *
 * @param r   Pointer to a RESP data structure
 * @return    TRUE (1) if the data holds an array of `RESP *` pointers, or else FALSE (0).
 *
 * @sa redisxIsScalarType()
 * @sa redisxIsStringType()
 * @sa redisxIsMapType()
 * @sa RESP_ARRAY
 * @sa RESP3_SET
 * @sa RESP3_PUSH
 *
 */
boolean redisxIsArrayType(const RESP *r) {
  if(!r) return FALSE;

  switch(r->type) {
    case RESP_ARRAY:
    case RESP3_SET:
    case RESP3_PUSH:
      return TRUE;

    default:
      return FALSE;
  }
}

/**
 * Checks if a RESP holds a dictionary, and whose `value` can be cast to `(RedisMapEntry *)` to use.
 *
 * @param r   Pointer to a RESP data structure
 * @return    TRUE (1) if the data holds a dictionary (a RedisMapEntry array), or else FALSE (0).
 *
 * @sa redisxIsScalarType()
 * @sa redisxIsStringType()
 * @sa redisxIsMapType()
 * @sa RESP3_MAP
 * @sa RESP3_ATTRIBUTE
 *
 */
boolean redisxIsMapType(const RESP *r) {
  if(!r) return FALSE;

  switch(r->type) {
    case RESP3_MAP:
    case RESP3_ATTRIBUTE:
      return TRUE;

    default:
      return FALSE;
  }
}

/**
 * Checks if a RESP has subcomponents, such as arrays or maps (dictionaries).
 *
 * @param r   Pointer to a RESP data structure
 * @return    TRUE (1) if the data has sub-components, or else FALSE (0).
 *
 * @sa redisxIsArrayType()
 * @sa redisxIsMapType()
 * @sa RESP3_MAP
 * @sa RESP3_ATTRIBUTE
 *
 */
boolean redisxHasComponents(const RESP *r) {
  if(!r) return FALSE;

  return r->n > 0 && (redisxIsArrayType(r) || redisxIsMapType(r));
}


/**
 * Appends a part to an existing RESP of the same type, before discarding the part.
 *
 * @param[in, out] resp   The RESP to which the part is appended
 * @param part            The part, which is destroyed after the content is appended to the first RESP argument.
 * @return                X_SUCCESS (0) if successful, or else X_NULL if the first argument is NULL, or
 *                        REDIS_UNEXPECTED_RESP if the types do not match, or X_FAILURE if there was an allocation
 *                        error.
 */
int redisxAppendRESP(RESP *resp, RESP *part) {
  static const char *fn = "redisxAppendRESP";
  char *old, *extend;
  size_t eSize;

  if(!resp)
    return x_error(X_NULL, EINVAL, fn, "NULL resp");
  if(!part || part->type == RESP3_NULL || part->n <= 0)
    return 0;
  if(resp->type != part->type) {
    int err = x_error(REDIS_UNEXPECTED_RESP, EINVAL, fn, "Mismatched types: '%c' vs. '%c'", resp->type, part->type);
    redisxDestroyRESP(part);
    return err;
  }
  if(redisxIsScalarType(resp))
    return x_error(REDIS_UNEXPECTED_RESP, EINVAL, fn, "Cannot append to RESP type '%c'", resp->type);

  if(redisxIsArrayType(resp))
    eSize = sizeof(RESP *);
  else if(redisxIsMapType(resp))
    eSize = sizeof(RedisMapEntry);
  else
    eSize = 1;

  old = resp->value;
  extend = (char *) realloc(resp->value, (resp->n + part->n) * eSize);
  if(!extend) {
    free(old);
    return x_error(X_FAILURE, errno, fn, "alloc RESP array (%d components)", resp->n + part->n);
  }

  memcpy(extend + resp->n * eSize, part->value, part->n * eSize);
  resp->n += part->n;
  resp->value = extend;
  free(part);

  return X_SUCCESS;
}

/**
 * Checks if two RESP are equal, that is they hold the same type of data, have the same 'n' value,
 * and the values match byte-for-byte, or are both NULL.
 *
 * @param a   Ponter to a RESP data structure.
 * @param b   Pointer to another RESP data structure.
 * @return    TRUE (1) if the two RESP structures match, or else FALSE (0).
 */
boolean redisxIsEqualRESP(const RESP *a, const RESP *b) {
  if(a == b) return TRUE;
  if(!a || !b) return FALSE;


  if(a->type != b->type) return FALSE;
  if(a->n != b->n) return FALSE;
  if(a->value == NULL) return (b->value == NULL);
  if(!b->value) return FALSE;

  return (memcmp(a->value, b->value, a->n) == 0);
}

/**
 * Retrieves a keyed entry from a map-type RESP data structure.
 *
 * @param map   The map-type REST data structure containing a dictionary
 * @param key   The RESP key to match
 * @return      The matching map entry or NULL if the map contains no such entry.
 *
 * @sa RESP3_MAP
 * @sa RESP3_ATTRIBUTE
 *
 * @sa redisxGetKeywordEntry()
 */
RedisMapEntry *redisxGetMapEntry(const RESP *map, const RESP *key) {
  int i;
  RedisMapEntry *entries;

  if(!key) return NULL;
  if(!redisxIsMapType(map)) return NULL;
  if(!map->value) return NULL;

  entries = (RedisMapEntry *) map->value;

  for(i = 0; i < map->n; i++) {
    RedisMapEntry *e = &entries[i];

    if(e->key->type != key->type) continue;
    if(e->key->n != key->n) continue;
    if(key->value == NULL) {
      if(e->key->value == NULL) return e;
      continue;
    }
    if(e->key->value == NULL) continue;
    if(memcmp(e->key->value, key->value, key->n) == 0) return e;
  }

  return NULL;
}

/**
 * Retrieves a entry, by its string keyword, from a map-type RESP data structure.
 *
 * @param map   The map-type REST data structure containing a dictionary
 * @param key   The string keyword to match
 * @return      The matching map entry or NULL if the map contains no such entry.
 *
 * @sa RESP3_MAP
 * @sa RESP3_ATTRIBUTE
 *
 * @sa redisxGetMapEntry()
 */
RedisMapEntry *redisxGetKeywordEntry(const RESP *map, const char *key) {
  int i;
  RedisMapEntry *entries;

  if(!key) return NULL;
  if(!redisxIsMapType(map)) return NULL;
  if(!map->value) return NULL;

  entries = (RedisMapEntry *) map->value;

  for(i = 0; i < map->n; i++) {
    RedisMapEntry *e = &entries[i];

    if(!redisxIsStringType(e->key)) continue;
    if(strcmp(e->key->value, key) == 0) return e;
  }

  return NULL;
}

static XType resp2xType(enum resp_type type) {
  switch(type) {
    case RESP3_NULL:
      return X_UNKNOWN;
    case RESP3_BOOLEAN:
      return X_BOOLEAN;
    case RESP_INT:
      return X_LONG;
    case RESP3_DOUBLE:
      return X_DOUBLE;
    case RESP_SIMPLE_STRING:
    case RESP_BULK_STRING:
    case RESP_ERROR:
    case RESP3_BLOB_ERROR:
    case RESP3_VERBATIM_STRING:
    case RESP3_BIG_NUMBER:
      return X_STRING;
    case RESP_ARRAY:
    case RESP3_SET:
    case RESP3_PUSH:
      return X_FIELD;
    case RESP3_MAP:
    case RESP3_ATTRIBUTE:
      return X_STRUCT;
  }

  return X_UNKNOWN;
}


static XField *respArrayToXField(const char *name, const RESP **component, int n) {
  static const char *fn = "respArrayToXField";

  XField *f;
  enum resp_type type = RESP3_NULL;
  int i;

  if(n < 0) return NULL;

  for(i = 0; i < n; i++) {
    if(i == 0) type = component[i]->type;
    else if(component[i]->type != type) break;
  }

  if(i < n) {
    // --------------------------------------------------------
    // Heterogeneous array...

    XField *array;

    f = xCreate1DFieldArray(name, n);

    if(!f->value) return x_trace_null(fn, "field array");

    array = (XField *) f->value;

    for(i = 0; i < n; i++) {
      XField *e = resp2XField(array[i].name, component[i]);
      if(e) {
        array[i] = *e;
        free(e);
      }
    }
  }

  else {
    // --------------------------------------------------------
    // Homogeneous array...

    XType eType = resp2xType(type);
    char *array;
    size_t eSize;

    if(eType == X_UNKNOWN) return NULL;

    eSize = xElementSizeOf(eType);
    array = (char *) calloc(1, n * eSize);

    f = xCreate1DField(name, eType, n, array);
    f->flags = type;
    if(!array) return x_trace_null(fn, "field array");

    for(i = 0; i < n; i++) {
      XField *e = resp2XField("<element>", component[i]);
      if(e) {
        memcpy(&array[i * eSize], e, sizeof(XField));
        free(e);
      }
    }
  }

  return f;
}


static XField *respMap2XField(const char *name, const RedisMapEntry *map, int n) {
  XStructure *s = xCreateStruct();
  XField *f;

  while(--n >= 0) {
    const RedisMapEntry *e = &map[n];
    XField *fi = NULL;
    if(redisxIsStringType(e->key)) {
      fi = resp2XField((char *) e->key->value, e->value->value);
      fi->next = s->firstField;
      s->firstField = fi;
    }
    else {
      xvprintf("WARNING! cannot convert RESP map entry with non-string key");
      errno = ENOSYS;
    }
  }

  f = xCreateScalarField(name, X_STRUCT, s);
  return f;
}

/**
 * Converts a RESP to the xchange representation as an appropriate XField.
 *
 * <ul>
 * <li>RESP3_NULL values are converted to NULL.</li>
 * <li>Scalar values are converted to an XField with the equivalent type.</li>
 * <li>Homogenerous arrays are converted to a field with a 1D array of corresponding xchange type.</li>
 * <li>Heterogeneous arrays are converted to a field with a 1D array of X_FIELD type (containing an array of fields).</li>
 * <li>Maps with string keywords are converted to an X_STRUCT.</li>
 * <li>Maps with non-string keywords cannot be converted and will be ignored. However, `errno` is set to ENOSYS
 * to indicate the failure, and warnings are printed to the standard error, provided redisxSetVerbose() was
 * used to enable verbose output.</li>
 * </ul>
 *
 * @param name
 * @param resp
 * @return
 */
XField *resp2XField(const char *name, const RESP *resp) {
  static const char *fn = "resp2XField";

  errno = 0;

  if(!resp) {
    x_error(0, EINVAL, fn, "input RESP is NULL");
    return NULL;
  }

  switch(resp->type) {
    case RESP3_NULL:
      return NULL;

    case RESP3_BOOLEAN:
      return xCreateBooleanField(name, resp->n);

    case RESP_INT:
      return xCreateIntField(name, resp->n);

    case RESP3_DOUBLE:
      return xCreateDoubleField(name, *(double *) resp->value);

    case RESP_SIMPLE_STRING:
    case RESP_BULK_STRING:
    case RESP_ERROR:
    case RESP3_BLOB_ERROR:
    case RESP3_VERBATIM_STRING:
    case RESP3_BIG_NUMBER: {
      XField *f = xCreateStringField(name, (char *) resp->value);
      f->flags = resp->type;
      return f;
    }

    case RESP_ARRAY:
    case RESP3_SET:
    case RESP3_PUSH: {
      XField *f = respArrayToXField(name, (const RESP **) resp->value, resp->n);
      if(!f) return x_trace_null(fn, NULL);
      f->flags = resp->type;
      return f;
    }

    case RESP3_MAP:
    case RESP3_ATTRIBUTE: {
      XField *f = respMap2XField(name, (const RedisMapEntry *) resp->value, resp->n);
      if(!f) return x_trace_null(fn, NULL);
      f->flags = resp->type;
      return f;
    }

  }

  return NULL;
}

