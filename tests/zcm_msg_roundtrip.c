#include "zcm/zcm_msg.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int roundtrip(void) {
  zcm_msg_t *m = zcm_msg_new();
  if (!m) return 1;

  printf("zcm_msg_roundtrip: building message\n");
  zcm_msg_set_type(m, "TestType");
  zcm_msg_put_char(m, 'A');
  zcm_msg_put_short(m, (int16_t)-1234);
  zcm_msg_put_int(m, 123456);
  zcm_msg_put_long(m, (int64_t)-9876543210LL);
  zcm_msg_put_float(m, 3.14f);
  zcm_msg_put_double(m, 2.718281828);
  zcm_msg_put_text(m, "hello");
  const uint8_t raw[4] = {1,2,3,4};
  zcm_msg_put_bytes(m, raw, 4);
  const int32_t arr[3] = {7,8,9};
  zcm_msg_put_array(m, ZCM_MSG_ARRAY_INT, 3, arr);

  const void *data = NULL;
  size_t len = 0;
  void *owned = NULL;
  extern int zcm_msg__serialize(const zcm_msg_t *, const void **, size_t *, void **);
  printf("zcm_msg_roundtrip: serialize\n");
  if (zcm_msg__serialize(m, &data, &len, &owned) != 0) {
    zcm_msg_free(m);
    return 1;
  }

  zcm_msg_t *m2 = zcm_msg_new();
  if (!m2) return 1;
  printf("zcm_msg_roundtrip: deserialize\n");
  if (zcm_msg_from_bytes(m2, data, len) != 0) {
    zcm_msg_free(m);
    zcm_msg_free(m2);
    free(owned);
    return 1;
  }

  if (strcmp(zcm_msg_get_type(m2), "TestType") != 0) return 1;

  char c = 0; int16_t s = 0; int32_t i = 0; int64_t l = 0;
  float f = 0; double d = 0; const char *t = NULL; uint32_t tlen = 0;
  const void *b = NULL; uint32_t blen = 0;
  zcm_msg_array_type_t at; uint32_t elems = 0; const void *ap = NULL;

  printf("zcm_msg_roundtrip: validate fields\n");
  if (zcm_msg_get_char(m2, &c) != 0 || c != 'A') return 1;
  if (zcm_msg_get_short(m2, &s) != 0 || s != -1234) return 1;
  if (zcm_msg_get_int(m2, &i) != 0 || i != 123456) return 1;
  if (zcm_msg_get_long(m2, &l) != 0 || l != -9876543210LL) return 1;
  if (zcm_msg_get_float(m2, &f) != 0 || (f < 3.139f || f > 3.141f)) return 1;
  if (zcm_msg_get_double(m2, &d) != 0 || (d < 2.71828 || d > 2.71829)) return 1;
  if (zcm_msg_get_text(m2, &t, &tlen) != 0) return 1;
  if (tlen != 5 || memcmp(t, "hello", 5) != 0) return 1;
  if (zcm_msg_get_bytes(m2, &b, &blen) != 0 || blen != 4) return 1;
  if (zcm_msg_get_array(m2, &at, &elems, &ap) != 0 || at != ZCM_MSG_ARRAY_INT || elems != 3) return 1;

  const int32_t *arr2 = (const int32_t *)ap;
  if (arr2[0] != 7 || arr2[1] != 8 || arr2[2] != 9) return 1;

  free(owned);
  zcm_msg_free(m);
  zcm_msg_free(m2);
  return 0;
}

int main(void) {
  int rc = roundtrip();
  if (rc != 0) {
    fprintf(stderr, "zcm_msg_roundtrip failed\n");
  } else {
    printf("zcm_msg_roundtrip: PASS\n");
  }
  return rc;
}
