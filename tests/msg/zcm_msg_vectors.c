#include "zcm/zcm_msg.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int test_vector_simple(void) {
  /* Build a message: type="Test", int=42, text="ok" */
  zcm_msg_t *m = zcm_msg_new();
  if (!m) return 1;
  printf("zcm_msg_vectors: build test vector\n");
  zcm_msg_set_type(m, "Test");
  zcm_msg_put_int(m, 42);
  zcm_msg_put_text(m, "ok");

  const void *data = NULL;
  size_t len = 0;
  void *owned = NULL;
  extern int zcm_msg__serialize(const zcm_msg_t *, const void **, size_t *, void **);
  printf("zcm_msg_vectors: serialize\n");
  if (zcm_msg__serialize(m, &data, &len, &owned) != 0) return 1;

  /* Now parse back and verify */
  zcm_msg_t *m2 = zcm_msg_new();
  if (!m2) return 1;
  printf("zcm_msg_vectors: deserialize\n");
  if (zcm_msg_from_bytes(m2, data, len) != 0) return 1;

  if (strcmp(zcm_msg_get_type(m2), "Test") != 0) return 1;
  int32_t v = 0;
  const char *t = NULL;
  uint32_t tlen = 0;
  printf("zcm_msg_vectors: validate fields\n");
  if (zcm_msg_get_int(m2, &v) != 0 || v != 42) return 1;
  if (zcm_msg_get_text(m2, &t, &tlen) != 0) return 1;
  if (tlen != 2 || memcmp(t, "ok", 2) != 0) return 1;

  free(owned);
  zcm_msg_free(m);
  zcm_msg_free(m2);
  return 0;
}

static int test_vector_numeric(void) {
  zcm_msg_t *m = zcm_msg_new();
  if (!m) return 1;

  zcm_msg_set_type(m, "NumericTest");
  if (zcm_msg_put_int(m, 42) != 0) return 1;
  if (zcm_msg_put_float(m, 1.5f) != 0) return 1;
  if (zcm_msg_put_double(m, 3.25) != 0) return 1;

  const void *data = NULL;
  size_t len = 0;
  void *owned = NULL;
  extern int zcm_msg__serialize(const zcm_msg_t *, const void **, size_t *, void **);
  if (zcm_msg__serialize(m, &data, &len, &owned) != 0) return 1;

  zcm_msg_t *m2 = zcm_msg_new();
  if (!m2) return 1;
  if (zcm_msg_from_bytes(m2, data, len) != 0) return 1;

  int32_t i = 0;
  float f = 0.0f;
  double d = 0.0;
  if (zcm_msg_get_int(m2, &i) != 0 || i != 42) return 1;
  if (zcm_msg_get_float(m2, &f) != 0 || f != 1.5f) return 1;
  if (zcm_msg_get_double(m2, &d) != 0 || d != 3.25) return 1;

  free(owned);
  zcm_msg_free(m);
  zcm_msg_free(m2);
  return 0;
}

int main(void) {
  int rc = test_vector_simple();
  if (rc == 0) rc = test_vector_numeric();
  if (rc != 0) {
    fprintf(stderr, "zcm_msg_vectors failed\n");
  } else {
    printf("zcm_msg_vectors: PASS\n");
  }
  return rc;
}
