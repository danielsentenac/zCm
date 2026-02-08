#ifndef ZCM_ZCM_MSG_H
#define ZCM_ZCM_MSG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

typedef struct zcm_msg zcm_msg_t;

typedef enum {
  ZCM_MSG_ITEM_CHAR = 1,
  ZCM_MSG_ITEM_SHORT = 2,
  ZCM_MSG_ITEM_INT = 3,
  ZCM_MSG_ITEM_LONG = 4,
  ZCM_MSG_ITEM_FLOAT = 5,
  ZCM_MSG_ITEM_DOUBLE = 6,
  ZCM_MSG_ITEM_ARRAY = 7,
  ZCM_MSG_ITEM_TEXT = 8,
  ZCM_MSG_ITEM_BYTES = 9
} zcm_msg_item_type_t;

typedef enum {
  ZCM_MSG_ARRAY_CHAR = 1,
  ZCM_MSG_ARRAY_SHORT = 2,
  ZCM_MSG_ARRAY_INT = 3,
  ZCM_MSG_ARRAY_FLOAT = 4,
  ZCM_MSG_ARRAY_DOUBLE = 5
} zcm_msg_array_type_t;

typedef enum {
  ZCM_MSG_OK = 0,
  ZCM_MSG_ERR = -1,
  ZCM_MSG_ERR_TYPE = -2,
  ZCM_MSG_ERR_RANGE = -3,
  ZCM_MSG_ERR_FORMAT = -4
} zcm_msg_status_t;

zcm_msg_t *zcm_msg_new(void);
void zcm_msg_free(zcm_msg_t *msg);
void zcm_msg_reset(zcm_msg_t *msg);

int zcm_msg_set_type(zcm_msg_t *msg, const char *type);
const char *zcm_msg_get_type(const zcm_msg_t *msg);

int zcm_msg_put_char(zcm_msg_t *msg, char value);
int zcm_msg_put_short(zcm_msg_t *msg, int16_t value);
int zcm_msg_put_int(zcm_msg_t *msg, int32_t value);
int zcm_msg_put_long(zcm_msg_t *msg, int64_t value);
int zcm_msg_put_float(zcm_msg_t *msg, float value);
int zcm_msg_put_double(zcm_msg_t *msg, double value);
int zcm_msg_put_text(zcm_msg_t *msg, const char *value);
int zcm_msg_put_bytes(zcm_msg_t *msg, const void *data, uint32_t len);
int zcm_msg_put_array(zcm_msg_t *msg, zcm_msg_array_type_t type,
                      uint32_t elements, const void *data);

int zcm_msg_get_char(zcm_msg_t *msg, char *value);
int zcm_msg_get_short(zcm_msg_t *msg, int16_t *value);
int zcm_msg_get_int(zcm_msg_t *msg, int32_t *value);
int zcm_msg_get_long(zcm_msg_t *msg, int64_t *value);
int zcm_msg_get_float(zcm_msg_t *msg, float *value);
int zcm_msg_get_double(zcm_msg_t *msg, double *value);
int zcm_msg_get_text(zcm_msg_t *msg, const char **value, uint32_t *len);
int zcm_msg_get_bytes(zcm_msg_t *msg, const void **data, uint32_t *len);
int zcm_msg_get_array(zcm_msg_t *msg, zcm_msg_array_type_t *type,
                      uint32_t *elements, const void **data);

const void *zcm_msg_data(const zcm_msg_t *msg, size_t *len);
int zcm_msg_from_bytes(zcm_msg_t *msg, const void *data, size_t len);

int zcm_msg_validate(const zcm_msg_t *msg);
const char *zcm_msg_last_error(const zcm_msg_t *msg);

#ifdef __cplusplus
}
#endif

#endif /* ZCM_ZCM_MSG_H */
