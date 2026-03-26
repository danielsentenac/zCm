#ifndef ZCM_ZCM_DOMAIN_H
#define ZCM_ZCM_DOMAIN_H

/**
 * @file zcm_domain.h
 * @brief Shared zCm domain endpoint resolution helpers.
 */

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Maximum number of bytes reserved for domain and host name strings. */
#define ZCM_DOMAIN_NAME_MAX 256
/** @brief Maximum number of bytes reserved for the resolved `ZCmDomains` path. */
#define ZCM_DOMAIN_PATH_MAX 1024
/** @brief Maximum number of bytes reserved for one broker endpoint string. */
#define ZCM_DOMAIN_ENDPOINT_MAX 512

/**
 * @brief Resolved broker endpoints for the current environment.
 *
 * `query_endpoint` is the advertised/shared endpoint used by CLI tools to find
 * the active broker. `bind_endpoint` is the local endpoint a broker instance on
 * this host should bind to if it becomes the active broker.
 */
typedef struct zcm_domain_info {
  /** Nonzero when values came from a `ZCMDOMAIN` -> `ZCmDomains` mapping. */
  int has_domain_mapping;
  /** Selected domain name from `ZCMDOMAIN`, or empty when explicit broker override is used. */
  char domain[ZCM_DOMAIN_NAME_MAX];
  /** Resolved path to the `ZCmDomains` file used for lookup/update. */
  char domains_file[ZCM_DOMAIN_PATH_MAX];
  /** Shared/advertised broker endpoint used by clients to connect to the broker. */
  char query_endpoint[ZCM_DOMAIN_ENDPOINT_MAX];
  /** Local endpoint the broker on this host should bind to. */
  char bind_endpoint[ZCM_DOMAIN_ENDPOINT_MAX];
  /** Hostname/IP that should be written back to `ZCmDomains` as the published broker host. */
  char publish_host[ZCM_DOMAIN_NAME_MAX];
  /** Broker TCP port extracted from the resolved endpoint. */
  int port;
} zcm_domain_info_t;

/**
 * @brief Resolve broker endpoints from environment and ZCmDomains.
 *
 * When `ZCMBROKER`/`ZCMBROKER_ENDPOINT` is set, both endpoints resolve to that
 * explicit override and `has_domain_mapping` is `0`.
 *
 * Otherwise the current domain is read from `ZCMDOMAIN` and the advertised
 * endpoint is loaded from `ZCmDomains`, while the bind endpoint is rewritten to
 * use a local host identity for this machine.
 *
 * @param out_info Output structure to populate.
 * @return `0` on success, `-1` on failure.
 */
int zcm_domain_info_load(zcm_domain_info_t *out_info);

/**
 * @brief Publish the active broker host back into `ZCmDomains`.
 *
 * This only applies when `has_domain_mapping` is nonzero.
 *
 * @param info Resolved domain info.
 * @return `0` on success, `-1` on failure.
 */
int zcm_domain_info_update_published_host(const zcm_domain_info_t *info);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ZCM_ZCM_DOMAIN_H */
