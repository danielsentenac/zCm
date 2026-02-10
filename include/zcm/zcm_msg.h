#ifndef ZCM_ZCM_MSG_H
#define ZCM_ZCM_MSG_H

/**
 * @file zcm_msg.h
 * @brief Typed message builder/parser API for zCm payloads.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

/** @brief Opaque typed message container. */
typedef struct zcm_msg zcm_msg_t;

/**
 * @brief Encoded item kind stored in a message payload.
 */
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

/**
 * @brief Element type for array payload items.
 */
typedef enum {
  ZCM_MSG_ARRAY_CHAR = 1,
  ZCM_MSG_ARRAY_SHORT = 2,
  ZCM_MSG_ARRAY_INT = 3,
  ZCM_MSG_ARRAY_FLOAT = 4,
  ZCM_MSG_ARRAY_DOUBLE = 5
} zcm_msg_array_type_t;

/**
 * @brief Status/error codes returned by zcm_msg APIs.
 */
typedef enum {
  ZCM_MSG_OK = 0,
  ZCM_MSG_ERR = -1,
  ZCM_MSG_ERR_TYPE = -2,
  ZCM_MSG_ERR_RANGE = -3,
  ZCM_MSG_ERR_FORMAT = -4
} zcm_msg_status_t;

/**
 * @brief Allocate a new empty message.
 *
 * @return New message handle, or `NULL` on allocation failure.
 */
zcm_msg_t *zcm_msg_new(void);

/**
 * @brief Free a message allocated by zcm_msg_new().
 *
 * @param msg Message to free. `NULL` is allowed.
 */
void zcm_msg_free(zcm_msg_t *msg);

/**
 * @brief Clear payload, type name, and read cursor of a message.
 *
 * @param msg Message to reset. `NULL` is allowed.
 */
void zcm_msg_reset(zcm_msg_t *msg);

/**
 * @brief Reset only the payload read cursor to the beginning.
 *
 * @param msg Message to rewind. `NULL` is allowed.
 */
void zcm_msg_rewind(zcm_msg_t *msg);

/**
 * @brief Set application-level message type string.
 *
 * @param msg Message to modify.
 * @param type Null-terminated type name.
 * @return `ZCM_MSG_OK` on success, otherwise an error code.
 */
int zcm_msg_set_type(zcm_msg_t *msg, const char *type);

/**
 * @brief Get the message type string.
 *
 * @param msg Message to inspect.
 * @return Pointer to internal type string, or `NULL` if `msg` is `NULL`.
 */
const char *zcm_msg_get_type(const zcm_msg_t *msg);

/**
 * @brief Append a `char` item.
 *
 * @param msg Message to append to.
 * @param value Value to encode.
 * @return `ZCM_MSG_OK` on success, otherwise an error code.
 */
int zcm_msg_put_char(zcm_msg_t *msg, char value);

/**
 * @brief Append a 16-bit signed integer item.
 *
 * @param msg Message to append to.
 * @param value Value to encode.
 * @return `ZCM_MSG_OK` on success, otherwise an error code.
 */
int zcm_msg_put_short(zcm_msg_t *msg, int16_t value);

/**
 * @brief Append a 32-bit signed integer item.
 *
 * @param msg Message to append to.
 * @param value Value to encode.
 * @return `ZCM_MSG_OK` on success, otherwise an error code.
 */
int zcm_msg_put_int(zcm_msg_t *msg, int32_t value);

/**
 * @brief Append a 64-bit signed integer item.
 *
 * @param msg Message to append to.
 * @param value Value to encode.
 * @return `ZCM_MSG_OK` on success, otherwise an error code.
 */
int zcm_msg_put_long(zcm_msg_t *msg, int64_t value);

/**
 * @brief Append a `float` item.
 *
 * @param msg Message to append to.
 * @param value Value to encode.
 * @return `ZCM_MSG_OK` on success, otherwise an error code.
 */
int zcm_msg_put_float(zcm_msg_t *msg, float value);

/**
 * @brief Append a `double` item.
 *
 * @param msg Message to append to.
 * @param value Value to encode.
 * @return `ZCM_MSG_OK` on success, otherwise an error code.
 */
int zcm_msg_put_double(zcm_msg_t *msg, double value);

/**
 * @brief Append a UTF-8 or ASCII text item.
 *
 * @param msg Message to append to.
 * @param value Null-terminated text pointer. `NULL` is treated as empty text.
 * @return `ZCM_MSG_OK` on success, otherwise an error code.
 */
int zcm_msg_put_text(zcm_msg_t *msg, const char *value);

/**
 * @brief Append an opaque byte buffer item.
 *
 * @param msg Message to append to.
 * @param data Byte pointer to encode. Can be `NULL` when `len == 0`.
 * @param len Number of bytes at `data`.
 * @return `ZCM_MSG_OK` on success, otherwise an error code.
 */
int zcm_msg_put_bytes(zcm_msg_t *msg, const void *data, uint32_t len);

/**
 * @brief Append an array item.
 *
 * @param msg Message to append to.
 * @param type Array element type.
 * @param elements Element count.
 * @param data Pointer to contiguous array memory. Must be non-`NULL` when `elements > 0`.
 * @return `ZCM_MSG_OK` on success, otherwise an error code.
 */
int zcm_msg_put_array(zcm_msg_t *msg, zcm_msg_array_type_t type,
                      uint32_t elements, const void *data);

/**
 * @brief Read the next `char` item.
 *
 * @param msg Message to read from.
 * @param value Output storage for decoded value.
 * @return `ZCM_MSG_OK` on success, otherwise an error code.
 */
int zcm_msg_get_char(zcm_msg_t *msg, char *value);

/**
 * @brief Read the next 16-bit signed integer item.
 *
 * @param msg Message to read from.
 * @param value Output storage for decoded value.
 * @return `ZCM_MSG_OK` on success, otherwise an error code.
 */
int zcm_msg_get_short(zcm_msg_t *msg, int16_t *value);

/**
 * @brief Read the next 32-bit signed integer item.
 *
 * @param msg Message to read from.
 * @param value Output storage for decoded value.
 * @return `ZCM_MSG_OK` on success, otherwise an error code.
 */
int zcm_msg_get_int(zcm_msg_t *msg, int32_t *value);

/**
 * @brief Read the next 64-bit signed integer item.
 *
 * @param msg Message to read from.
 * @param value Output storage for decoded value.
 * @return `ZCM_MSG_OK` on success, otherwise an error code.
 */
int zcm_msg_get_long(zcm_msg_t *msg, int64_t *value);

/**
 * @brief Read the next `float` item.
 *
 * @param msg Message to read from.
 * @param value Output storage for decoded value.
 * @return `ZCM_MSG_OK` on success, otherwise an error code.
 */
int zcm_msg_get_float(zcm_msg_t *msg, float *value);

/**
 * @brief Read the next `double` item.
 *
 * @param msg Message to read from.
 * @param value Output storage for decoded value.
 * @return `ZCM_MSG_OK` on success, otherwise an error code.
 */
int zcm_msg_get_double(zcm_msg_t *msg, double *value);

/**
 * @brief Read the next text item.
 *
 * Returned pointer references internal message storage.
 *
 * @param msg Message to read from.
 * @param value Output pointer to text bytes (not null-terminated by contract).
 * @param len Optional output string length in bytes.
 * @return `ZCM_MSG_OK` on success, otherwise an error code.
 */
int zcm_msg_get_text(zcm_msg_t *msg, const char **value, uint32_t *len);

/**
 * @brief Read the next raw byte item.
 *
 * Returned pointer references internal message storage.
 *
 * @param msg Message to read from.
 * @param data Output pointer to the byte span.
 * @param len Optional output byte count.
 * @return `ZCM_MSG_OK` on success, otherwise an error code.
 */
int zcm_msg_get_bytes(zcm_msg_t *msg, const void **data, uint32_t *len);

/**
 * @brief Read the next array item.
 *
 * Returned pointer references internal message storage.
 *
 * @param msg Message to read from.
 * @param type Output array element type.
 * @param elements Output element count.
 * @param data Output pointer to contiguous array bytes.
 * @return `ZCM_MSG_OK` on success, otherwise an error code.
 */
int zcm_msg_get_array(zcm_msg_t *msg, zcm_msg_array_type_t *type,
                      uint32_t *elements, const void **data);

/**
 * @brief Get raw payload bytes (without transport envelope).
 *
 * @param msg Message to inspect.
 * @param len Optional payload byte length output.
 * @return Pointer to internal payload buffer, or `NULL` when `msg` is `NULL`.
 */
const void *zcm_msg_data(const zcm_msg_t *msg, size_t *len);

/**
 * @brief Get unread payload bytes remaining from current read cursor.
 *
 * @param msg Message to inspect.
 * @return Remaining unread payload bytes.
 */
size_t zcm_msg_remaining(const zcm_msg_t *msg);

/**
 * @brief Load message state from serialized envelope bytes.
 *
 * @param msg Destination message to populate.
 * @param data Serialized input bytes.
 * @param len Size of `data` in bytes.
 * @return `ZCM_MSG_OK` on success, otherwise an error code.
 */
int zcm_msg_from_bytes(zcm_msg_t *msg, const void *data, size_t len);

/**
 * @brief Validate encoded payload structure.
 *
 * @param msg Message to validate.
 * @return `ZCM_MSG_OK` if the payload is structurally valid, otherwise an error code.
 */
int zcm_msg_validate(const zcm_msg_t *msg);

/**
 * @brief Get the last decode/validation error text for a message.
 *
 * @param msg Message to inspect.
 * @return Internal error string, or `NULL` when `msg` is `NULL`.
 */
const char *zcm_msg_last_error(const zcm_msg_t *msg);

#ifdef __cplusplus
}
#endif

#endif /* ZCM_ZCM_MSG_H */
