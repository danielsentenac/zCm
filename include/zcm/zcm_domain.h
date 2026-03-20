#ifndef ZCM_ZCM_DOMAIN_H
#define ZCM_ZCM_DOMAIN_H

/**
 * @file zcm_domain.h
 * @brief Shared zCm domain endpoint resolution helpers.
 */

#ifdef __cplusplus
extern "C" {
#endif

#define ZCM_DOMAIN_NAME_MAX 256
#define ZCM_DOMAIN_PATH_MAX 1024
#define ZCM_DOMAIN_ENDPOINT_MAX 512

/**
 * @brief Resolved broker endpoints for the current environment.
 *
 * `query_endpoint` is the advertised/shared endpoint used by CLI tools to find
 * the active broker. `bind_endpoint` is the local endpoint a broker instance on
 * this host should bind to if it becomes the active broker.
 */
typedef struct zcm_domain_info {
  int has_domain_mapping;
  char domain[ZCM_DOMAIN_NAME_MAX];
  char domains_file[ZCM_DOMAIN_PATH_MAX];
  char query_endpoint[ZCM_DOMAIN_ENDPOINT_MAX];
  char bind_endpoint[ZCM_DOMAIN_ENDPOINT_MAX];
  char publish_host[ZCM_DOMAIN_NAME_MAX];
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
 * This only applies when @ref zcm_domain_info_t.has_domain_mapping is nonzero.
 *
 * @param info Resolved domain info.
 * @return `0` on success, `-1` on failure.
 */
int zcm_domain_info_update_published_host(const zcm_domain_info_t *info);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ZCM_ZCM_DOMAIN_H */
