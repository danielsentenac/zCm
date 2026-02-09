#include "zcm/zcm_msg.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define ZCM_MAGIC 0x5A434D31u /* 'ZCM1' */
#define ZCM_VERSION 1u

struct zcm_msg {
  char type[64];
  uint8_t *buf;
  size_t len;
  size_t cap;
  size_t read_off;
  char last_error[128];
};

static void set_error(zcm_msg_t *msg, const char *text) {
  if (!msg || !text) return;
  strncpy(msg->last_error, text, sizeof(msg->last_error) - 1);
  msg->last_error[sizeof(msg->last_error) - 1] = '\0';
}

static uint16_t le16(uint16_t v) {
  uint16_t x = 1;
  if (*(uint8_t *)&x) return v;
  return (uint16_t)((v >> 8) | (v << 8));
}

static uint32_t le32(uint32_t v) {
  uint32_t x = 1;
  if (*(uint8_t *)&x) return v;
  return ((v & 0x000000FFu) << 24) |
         ((v & 0x0000FF00u) << 8) |
         ((v & 0x00FF0000u) >> 8) |
         ((v & 0xFF000000u) >> 24);
}

static uint64_t le64(uint64_t v) {
  uint64_t x = 1;
  if (*(uint8_t *)&x) return v;
  return ((v & 0x00000000000000FFull) << 56) |
         ((v & 0x000000000000FF00ull) << 40) |
         ((v & 0x0000000000FF0000ull) << 24) |
         ((v & 0x00000000FF000000ull) << 8) |
         ((v & 0x000000FF00000000ull) >> 8) |
         ((v & 0x0000FF0000000000ull) >> 24) |
         ((v & 0x00FF000000000000ull) >> 40) |
         ((v & 0xFF00000000000000ull) >> 56);
}

static int ensure_cap(zcm_msg_t *msg, size_t add) {
  if (!msg) return ZCM_MSG_ERR;
  size_t need = msg->len + add;
  if (need <= msg->cap) return ZCM_MSG_OK;
  size_t new_cap = msg->cap ? msg->cap : 256;
  while (new_cap < need) new_cap *= 2;
  uint8_t *nb = (uint8_t *)realloc(msg->buf, new_cap);
  if (!nb) {
    set_error(msg, "out of memory");
    return ZCM_MSG_ERR;
  }
  msg->buf = nb;
  msg->cap = new_cap;
  return ZCM_MSG_OK;
}

static int put_u8(zcm_msg_t *msg, uint8_t v) {
  if (ensure_cap(msg, 1) != 0) return ZCM_MSG_ERR;
  msg->buf[msg->len++] = v;
  return ZCM_MSG_OK;
}

static int put_u32(zcm_msg_t *msg, uint32_t v) {
  if (ensure_cap(msg, 4) != 0) return ZCM_MSG_ERR;
  uint32_t w = le32(v);
  memcpy(msg->buf + msg->len, &w, 4);
  msg->len += 4;
  return ZCM_MSG_OK;
}

static int put_u16(zcm_msg_t *msg, uint16_t v) {
  if (ensure_cap(msg, 2) != 0) return ZCM_MSG_ERR;
  uint16_t w = le16(v);
  memcpy(msg->buf + msg->len, &w, 2);
  msg->len += 2;
  return ZCM_MSG_OK;
}

static int put_u64(zcm_msg_t *msg, uint64_t v) {
  if (ensure_cap(msg, 8) != 0) return ZCM_MSG_ERR;
  uint64_t w = le64(v);
  memcpy(msg->buf + msg->len, &w, 8);
  msg->len += 8;
  return ZCM_MSG_OK;
}

static int put_bytes(zcm_msg_t *msg, const void *data, size_t len) {
  if (ensure_cap(msg, len) != 0) return ZCM_MSG_ERR;
  if (len && data) memcpy(msg->buf + msg->len, data, len);
  msg->len += len;
  return ZCM_MSG_OK;
}

static int get_u8(zcm_msg_t *msg, uint8_t *v) {
  if (!msg || msg->read_off + 1 > msg->len) {
    set_error(msg, "read overflow");
    return ZCM_MSG_ERR_RANGE;
  }
  *v = msg->buf[msg->read_off++];
  return ZCM_MSG_OK;
}

static int get_u32(zcm_msg_t *msg, uint32_t *v) {
  if (!msg || msg->read_off + 4 > msg->len) {
    set_error(msg, "read overflow");
    return ZCM_MSG_ERR_RANGE;
  }
  uint32_t w = 0;
  memcpy(&w, msg->buf + msg->read_off, 4);
  *v = le32(w);
  msg->read_off += 4;
  return ZCM_MSG_OK;
}

static int get_u16(zcm_msg_t *msg, uint16_t *v) {
  if (!msg || msg->read_off + 2 > msg->len) {
    set_error(msg, "read overflow");
    return ZCM_MSG_ERR_RANGE;
  }
  uint16_t w = 0;
  memcpy(&w, msg->buf + msg->read_off, 2);
  *v = le16(w);
  msg->read_off += 2;
  return ZCM_MSG_OK;
}

static int get_u64(zcm_msg_t *msg, uint64_t *v) {
  if (!msg || msg->read_off + 8 > msg->len) {
    set_error(msg, "read overflow");
    return ZCM_MSG_ERR_RANGE;
  }
  uint64_t w = 0;
  memcpy(&w, msg->buf + msg->read_off, 8);
  *v = le64(w);
  msg->read_off += 8;
  return ZCM_MSG_OK;
}

static int get_bytes(zcm_msg_t *msg, const void **data, uint32_t len) {
  if (!msg || msg->read_off + len > msg->len) {
    set_error(msg, "read overflow");
    return ZCM_MSG_ERR_RANGE;
  }
  *data = msg->buf + msg->read_off;
  msg->read_off += len;
  return ZCM_MSG_OK;
}

zcm_msg_t *zcm_msg_new(void) {
  zcm_msg_t *m = (zcm_msg_t *)calloc(1, sizeof(zcm_msg_t));
  return m;
}

void zcm_msg_free(zcm_msg_t *msg) {
  if (!msg) return;
  free(msg->buf);
  free(msg);
}

void zcm_msg_reset(zcm_msg_t *msg) {
  if (!msg) return;
  msg->len = 0;
  msg->read_off = 0;
  msg->type[0] = '\0';
  msg->last_error[0] = '\0';
}

int zcm_msg_set_type(zcm_msg_t *msg, const char *type) {
  if (!msg || !type) return ZCM_MSG_ERR;
  strncpy(msg->type, type, sizeof(msg->type) - 1);
  msg->type[sizeof(msg->type) - 1] = '\0';
  return ZCM_MSG_OK;
}

const char *zcm_msg_get_type(const zcm_msg_t *msg) {
  if (!msg) return NULL;
  return msg->type;
}

int zcm_msg_put_char(zcm_msg_t *msg, char value) {
  if (put_u8(msg, ZCM_MSG_ITEM_CHAR) != 0) return ZCM_MSG_ERR;
  return put_bytes(msg, &value, 1);
}

int zcm_msg_put_short(zcm_msg_t *msg, int16_t value) {
  if (put_u8(msg, ZCM_MSG_ITEM_SHORT) != 0) return ZCM_MSG_ERR;
  return put_u16(msg, (uint16_t)value);
}

int zcm_msg_put_int(zcm_msg_t *msg, int32_t value) {
  if (put_u8(msg, ZCM_MSG_ITEM_INT) != 0) return ZCM_MSG_ERR;
  return put_u32(msg, (uint32_t)value);
}

int zcm_msg_put_long(zcm_msg_t *msg, int64_t value) {
  if (put_u8(msg, ZCM_MSG_ITEM_LONG) != 0) return ZCM_MSG_ERR;
  return put_u64(msg, (uint64_t)value);
}

int zcm_msg_put_float(zcm_msg_t *msg, float value) {
  if (put_u8(msg, ZCM_MSG_ITEM_FLOAT) != 0) return ZCM_MSG_ERR;
  uint32_t bits = 0;
  memcpy(&bits, &value, sizeof(bits));
  return put_u32(msg, bits);
}

int zcm_msg_put_double(zcm_msg_t *msg, double value) {
  if (put_u8(msg, ZCM_MSG_ITEM_DOUBLE) != 0) return ZCM_MSG_ERR;
  uint64_t bits = 0;
  memcpy(&bits, &value, sizeof(bits));
  return put_u64(msg, bits);
}

int zcm_msg_put_text(zcm_msg_t *msg, const char *value) {
  if (!value) value = "";
  uint32_t len = (uint32_t)strlen(value);
  if (put_u8(msg, ZCM_MSG_ITEM_TEXT) != 0) return ZCM_MSG_ERR;
  if (put_u32(msg, len) != 0) return ZCM_MSG_ERR;
  return put_bytes(msg, value, len);
}

int zcm_msg_put_bytes(zcm_msg_t *msg, const void *data, uint32_t len) {
  if (put_u8(msg, ZCM_MSG_ITEM_BYTES) != 0) return ZCM_MSG_ERR;
  if (put_u32(msg, len) != 0) return ZCM_MSG_ERR;
  return put_bytes(msg, data, len);
}

int zcm_msg_put_array(zcm_msg_t *msg, zcm_msg_array_type_t type,
                      uint32_t elements, const void *data) {
  if (put_u8(msg, ZCM_MSG_ITEM_ARRAY) != 0) return ZCM_MSG_ERR;
  if (put_u8(msg, (uint8_t)type) != 0) return ZCM_MSG_ERR;
  if (put_u32(msg, elements) != 0) return ZCM_MSG_ERR;
  size_t elem_size = 0;
  switch (type) {
    case ZCM_MSG_ARRAY_CHAR: elem_size = 1; break;
    case ZCM_MSG_ARRAY_SHORT: elem_size = 2; break;
    case ZCM_MSG_ARRAY_INT: elem_size = 4; break;
    case ZCM_MSG_ARRAY_FLOAT: elem_size = 4; break;
    case ZCM_MSG_ARRAY_DOUBLE: elem_size = 8; break;
    default: return ZCM_MSG_ERR_TYPE;
  }
  if (elements > 0 && data == NULL) return ZCM_MSG_ERR;
  if (type == ZCM_MSG_ARRAY_CHAR) {
    return put_bytes(msg, data, elem_size * elements);
  }
  if (ensure_cap(msg, elem_size * elements) != 0) return ZCM_MSG_ERR;
  const uint8_t *src = (const uint8_t *)data;
  for (uint32_t i = 0; i < elements; i++) {
    switch (type) {
      case ZCM_MSG_ARRAY_SHORT: {
        uint16_t v = 0;
        memcpy(&v, src + i * elem_size, 2);
        uint16_t w = le16(v);
        memcpy(msg->buf + msg->len, &w, 2);
        msg->len += 2;
        break;
      }
      case ZCM_MSG_ARRAY_INT:
      case ZCM_MSG_ARRAY_FLOAT: {
        uint32_t v = 0;
        memcpy(&v, src + i * elem_size, 4);
        uint32_t w = le32(v);
        memcpy(msg->buf + msg->len, &w, 4);
        msg->len += 4;
        break;
      }
      case ZCM_MSG_ARRAY_DOUBLE: {
        uint64_t v = 0;
        memcpy(&v, src + i * elem_size, 8);
        uint64_t w = le64(v);
        memcpy(msg->buf + msg->len, &w, 8);
        msg->len += 8;
        break;
      }
      default:
        return ZCM_MSG_ERR_TYPE;
    }
  }
  return ZCM_MSG_OK;
}

static int expect_type(zcm_msg_t *msg, zcm_msg_item_type_t type) {
  uint8_t t = 0;
  if (get_u8(msg, &t) != 0) return ZCM_MSG_ERR_RANGE;
  if (t != (uint8_t)type) {
    set_error(msg, "unexpected item type");
    return ZCM_MSG_ERR_TYPE;
  }
  return ZCM_MSG_OK;
}

int zcm_msg_get_char(zcm_msg_t *msg, char *value) {
  if (!value) return ZCM_MSG_ERR;
  if (expect_type(msg, ZCM_MSG_ITEM_CHAR) != 0) return ZCM_MSG_ERR_TYPE;
  const void *p = NULL;
  if (get_bytes(msg, &p, 1) != 0) return ZCM_MSG_ERR_RANGE;
  *value = *(const char *)p;
  return ZCM_MSG_OK;
}

int zcm_msg_get_short(zcm_msg_t *msg, int16_t *value) {
  if (!value) return ZCM_MSG_ERR;
  if (expect_type(msg, ZCM_MSG_ITEM_SHORT) != 0) return ZCM_MSG_ERR_TYPE;
  uint16_t v = 0;
  if (get_u16(msg, &v) != 0) return ZCM_MSG_ERR_RANGE;
  *value = (int16_t)v;
  return ZCM_MSG_OK;
}

int zcm_msg_get_int(zcm_msg_t *msg, int32_t *value) {
  if (!value) return ZCM_MSG_ERR;
  if (expect_type(msg, ZCM_MSG_ITEM_INT) != 0) return ZCM_MSG_ERR_TYPE;
  uint32_t v = 0;
  if (get_u32(msg, &v) != 0) return ZCM_MSG_ERR_RANGE;
  *value = (int32_t)v;
  return ZCM_MSG_OK;
}

int zcm_msg_get_long(zcm_msg_t *msg, int64_t *value) {
  if (!value) return ZCM_MSG_ERR;
  if (expect_type(msg, ZCM_MSG_ITEM_LONG) != 0) return ZCM_MSG_ERR_TYPE;
  uint64_t v = 0;
  if (get_u64(msg, &v) != 0) return ZCM_MSG_ERR_RANGE;
  *value = (int64_t)v;
  return ZCM_MSG_OK;
}

int zcm_msg_get_float(zcm_msg_t *msg, float *value) {
  if (!value) return ZCM_MSG_ERR;
  if (expect_type(msg, ZCM_MSG_ITEM_FLOAT) != 0) return ZCM_MSG_ERR_TYPE;
  uint32_t bits = 0;
  if (get_u32(msg, &bits) != 0) return ZCM_MSG_ERR_RANGE;
  memcpy(value, &bits, sizeof(bits));
  return ZCM_MSG_OK;
}

int zcm_msg_get_double(zcm_msg_t *msg, double *value) {
  if (!value) return ZCM_MSG_ERR;
  if (expect_type(msg, ZCM_MSG_ITEM_DOUBLE) != 0) return ZCM_MSG_ERR_TYPE;
  uint64_t bits = 0;
  if (get_u64(msg, &bits) != 0) return ZCM_MSG_ERR_RANGE;
  memcpy(value, &bits, sizeof(bits));
  return ZCM_MSG_OK;
}

int zcm_msg_get_text(zcm_msg_t *msg, const char **value, uint32_t *len) {
  if (!value) return ZCM_MSG_ERR;
  if (expect_type(msg, ZCM_MSG_ITEM_TEXT) != 0) return ZCM_MSG_ERR_TYPE;
  uint32_t l = 0;
  if (get_u32(msg, &l) != 0) return ZCM_MSG_ERR_RANGE;
  const void *p = NULL;
  if (get_bytes(msg, &p, l) != 0) return ZCM_MSG_ERR_RANGE;
  *value = (const char *)p;
  if (len) *len = l;
  return ZCM_MSG_OK;
}

int zcm_msg_get_bytes(zcm_msg_t *msg, const void **data, uint32_t *len) {
  if (!data) return ZCM_MSG_ERR;
  if (expect_type(msg, ZCM_MSG_ITEM_BYTES) != 0) return ZCM_MSG_ERR_TYPE;
  uint32_t l = 0;
  if (get_u32(msg, &l) != 0) return ZCM_MSG_ERR_RANGE;
  const void *p = NULL;
  if (get_bytes(msg, &p, l) != 0) return ZCM_MSG_ERR_RANGE;
  *data = p;
  if (len) *len = l;
  return ZCM_MSG_OK;
}

int zcm_msg_get_array(zcm_msg_t *msg, zcm_msg_array_type_t *type,
                      uint32_t *elements, const void **data) {
  if (!type || !elements || !data) return ZCM_MSG_ERR;
  if (expect_type(msg, ZCM_MSG_ITEM_ARRAY) != 0) return ZCM_MSG_ERR_TYPE;
  uint8_t t = 0;
  if (get_u8(msg, &t) != 0) return ZCM_MSG_ERR_RANGE;
  uint32_t elems = 0;
  if (get_u32(msg, &elems) != 0) return ZCM_MSG_ERR_RANGE;
  size_t elem_size = 0;
  switch ((zcm_msg_array_type_t)t) {
    case ZCM_MSG_ARRAY_CHAR: elem_size = 1; break;
    case ZCM_MSG_ARRAY_SHORT: elem_size = 2; break;
    case ZCM_MSG_ARRAY_INT: elem_size = 4; break;
    case ZCM_MSG_ARRAY_FLOAT: elem_size = 4; break;
    case ZCM_MSG_ARRAY_DOUBLE: elem_size = 8; break;
    default: return ZCM_MSG_ERR_TYPE;
  }
  size_t total = elem_size * elems;
  const void *p = NULL;
  if (get_bytes(msg, &p, (uint32_t)total) != 0) return ZCM_MSG_ERR_RANGE;
  if (t != ZCM_MSG_ARRAY_CHAR) {
    uint8_t *buf = (uint8_t *)p;
    for (uint32_t i = 0; i < elems; i++) {
      switch ((zcm_msg_array_type_t)t) {
        case ZCM_MSG_ARRAY_SHORT: {
          uint16_t v = 0;
          memcpy(&v, buf + i * elem_size, 2);
          v = le16(v);
          memcpy(buf + i * elem_size, &v, 2);
          break;
        }
        case ZCM_MSG_ARRAY_INT:
        case ZCM_MSG_ARRAY_FLOAT: {
          uint32_t v = 0;
          memcpy(&v, buf + i * elem_size, 4);
          v = le32(v);
          memcpy(buf + i * elem_size, &v, 4);
          break;
        }
        case ZCM_MSG_ARRAY_DOUBLE: {
          uint64_t v = 0;
          memcpy(&v, buf + i * elem_size, 8);
          v = le64(v);
          memcpy(buf + i * elem_size, &v, 8);
          break;
        }
        default:
          return ZCM_MSG_ERR_TYPE;
      }
    }
  }
  *type = (zcm_msg_array_type_t)t;
  *elements = elems;
  *data = p;
  return ZCM_MSG_OK;
}

const void *zcm_msg_data(const zcm_msg_t *msg, size_t *len) {
  if (!msg) return NULL;
  if (len) *len = msg->len;
  return msg->buf;
}

int zcm_msg_from_bytes(zcm_msg_t *msg, const void *data, size_t len) {
  if (!msg || !data || len < 12) return ZCM_MSG_ERR_FORMAT;
  const uint8_t *p = (const uint8_t *)data;
  uint32_t magic = 0;
  uint16_t version = 0;
  uint16_t type_len = 0;
  uint32_t payload_len = 0;
  memcpy(&magic, p, 4); p += 4;
  memcpy(&version, p, 2); p += 2;
  memcpy(&type_len, p, 2); p += 2;
  memcpy(&payload_len, p, 4); p += 4;
  magic = le32(magic);
  version = le16(version);
  type_len = le16(type_len);
  payload_len = le32(payload_len);

  if (magic != ZCM_MAGIC || version != ZCM_VERSION) {
    set_error(msg, "bad magic/version");
    return ZCM_MSG_ERR_FORMAT;
  }
  size_t need = 12 + type_len + payload_len;
  if (len < need) {
    set_error(msg, "truncated message");
    return ZCM_MSG_ERR_RANGE;
  }

  zcm_msg_reset(msg);
  if (type_len >= sizeof(msg->type)) {
    set_error(msg, "type too long");
    return ZCM_MSG_ERR_RANGE;
  }
  const char *type = (const char *)p;
  memcpy(msg->type, type, type_len);
  msg->type[type_len] = '\0';

  const uint8_t *payload = p + type_len;
  if (ensure_cap(msg, payload_len) != 0) return ZCM_MSG_ERR;
  memcpy(msg->buf, payload, payload_len);
  msg->len = payload_len;
  msg->read_off = 0;
  return ZCM_MSG_OK;
}

int zcm_msg_validate(const zcm_msg_t *msg) {
  if (!msg) return ZCM_MSG_ERR;
  zcm_msg_t tmp = *msg;
  tmp.read_off = 0;
  tmp.last_error[0] = '\0';

  while (tmp.read_off < tmp.len) {
    uint8_t t = 0;
    if (get_u8(&tmp, &t) != 0) return ZCM_MSG_ERR_RANGE;
    switch (t) {
      case ZCM_MSG_ITEM_CHAR:
        if (tmp.read_off + 1 > tmp.len) return ZCM_MSG_ERR_RANGE;
        tmp.read_off += 1;
        break;
      case ZCM_MSG_ITEM_SHORT:
        if (tmp.read_off + 2 > tmp.len) return ZCM_MSG_ERR_RANGE;
        tmp.read_off += 2;
        break;
      case ZCM_MSG_ITEM_INT:
      case ZCM_MSG_ITEM_FLOAT:
        if (tmp.read_off + 4 > tmp.len) return ZCM_MSG_ERR_RANGE;
        tmp.read_off += 4;
        break;
      case ZCM_MSG_ITEM_LONG:
      case ZCM_MSG_ITEM_DOUBLE:
        if (tmp.read_off + 8 > tmp.len) return ZCM_MSG_ERR_RANGE;
        tmp.read_off += 8;
        break;
      case ZCM_MSG_ITEM_TEXT:
      case ZCM_MSG_ITEM_BYTES: {
        uint32_t l = 0;
        if (get_u32(&tmp, &l) != 0) return ZCM_MSG_ERR_RANGE;
        if (tmp.read_off + l > tmp.len) return ZCM_MSG_ERR_RANGE;
        tmp.read_off += l;
        break;
      }
      case ZCM_MSG_ITEM_ARRAY: {
        uint8_t at = 0;
        if (get_u8(&tmp, &at) != 0) return ZCM_MSG_ERR_RANGE;
        uint32_t elems = 0;
        if (get_u32(&tmp, &elems) != 0) return ZCM_MSG_ERR_RANGE;
        size_t elem_size = 0;
        switch ((zcm_msg_array_type_t)at) {
          case ZCM_MSG_ARRAY_CHAR: elem_size = 1; break;
          case ZCM_MSG_ARRAY_SHORT: elem_size = 2; break;
          case ZCM_MSG_ARRAY_INT: elem_size = 4; break;
          case ZCM_MSG_ARRAY_FLOAT: elem_size = 4; break;
          case ZCM_MSG_ARRAY_DOUBLE: elem_size = 8; break;
          default: return ZCM_MSG_ERR_TYPE;
        }
        size_t total = elem_size * elems;
        if (tmp.read_off + total > tmp.len) return ZCM_MSG_ERR_RANGE;
        tmp.read_off += total;
        break;
      }
      default:
        return ZCM_MSG_ERR_TYPE;
    }
  }

  return ZCM_MSG_OK;
}

const char *zcm_msg_last_error(const zcm_msg_t *msg) {
  if (!msg) return NULL;
  return msg->last_error;
}

static int zcm_msg_build_envelope(const zcm_msg_t *msg, uint8_t **out, size_t *out_len) {
  if (!msg || !out || !out_len) return ZCM_MSG_ERR;
  size_t type_len = strlen(msg->type);
  size_t total = 12 + type_len + msg->len;
  uint8_t *buf = (uint8_t *)malloc(total);
  if (!buf) return ZCM_MSG_ERR;
  uint32_t magic = le32(ZCM_MAGIC);
  uint16_t version = le16((uint16_t)ZCM_VERSION);
  uint16_t tlen = le16((uint16_t)type_len);
  uint32_t plen = le32((uint32_t)msg->len);
  memcpy(buf, &magic, 4);
  memcpy(buf + 4, &version, 2);
  memcpy(buf + 6, &tlen, 2);
  memcpy(buf + 8, &plen, 4);
  memcpy(buf + 12, msg->type, type_len);
  memcpy(buf + 12 + type_len, msg->buf, msg->len);
  *out = buf;
  *out_len = total;
  return ZCM_MSG_OK;
}

/* internal helper used by transport */
int zcm_msg__serialize(const zcm_msg_t *msg, const void **data, size_t *len, void **owned) {
  if (!msg || !data || !len || !owned) return ZCM_MSG_ERR;
  uint8_t *buf = NULL;
  size_t blen = 0;
  if (zcm_msg_build_envelope(msg, &buf, &blen) != 0) return ZCM_MSG_ERR;
  *data = buf;
  *len = blen;
  *owned = buf;
  return ZCM_MSG_OK;
}
