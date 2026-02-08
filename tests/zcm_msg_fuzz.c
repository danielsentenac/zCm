#include "zcm/zcm_msg.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint32_t prng(uint32_t *state) {
  *state = (*state * 1664525u) + 1013904223u;
  return *state;
}

int main(void) {
  zcm_msg_t *msg = zcm_msg_new();
  if (!msg) return 1;

  printf("zcm_msg_fuzz: generating random inputs\n");
  uint32_t seed = 0xC0FFEEu;
  const int cases = 2000;

  for (int i = 0; i < cases; i++) {
    uint32_t r = prng(&seed);
    size_t len = (size_t)(r % 512u);
    if (len < 12 && (r & 1u)) len = 12; /* sometimes hit header size */

    uint8_t *buf = (uint8_t *)malloc(len ? len : 1);
    if (!buf) {
      zcm_msg_free(msg);
      return 1;
    }

    for (size_t j = 0; j < len; j++) {
      buf[j] = (uint8_t)prng(&seed);
    }

    int rc = zcm_msg_from_bytes(msg, buf, len);
    if (rc == 0) {
      /* Validate may still fail; we just ensure no crash. */
      (void)zcm_msg_validate(msg);
    }

    free(buf);
  }

  zcm_msg_free(msg);
  printf("zcm_msg_fuzz: PASS\n");
  return 0;
}
