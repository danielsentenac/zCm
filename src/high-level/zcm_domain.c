#include "zcm/zcm_domain.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int resolve_domains_file_path(char *out_path, size_t out_size) {
  if (!out_path || out_size == 0) return -1;
  out_path[0] = '\0';

  const char *env = getenv("ZCMDOMAIN_DATABASE");
  if (!env || !*env) env = getenv("ZCMMGR");

  if (env && *env) {
    if (snprintf(out_path, out_size, "%s/ZCmDomains", env) >= (int)out_size) return -1;
    return 0;
  }

  const char *root = getenv("ZCMROOT");
  if (!root || !*root) return -1;
  if (snprintf(out_path, out_size, "%s/mgr/ZCmDomains", root) >= (int)out_size) return -1;
  return 0;
}

static int load_domain_endpoint(const char *domain, const char *file_name,
                                char *out_host, size_t out_host_size,
                                int *out_port) {
  if (!domain || !*domain || !file_name || !*file_name ||
      !out_host || out_host_size == 0 || !out_port) {
    return -1;
  }

  FILE *f = fopen(file_name, "r");
  if (!f) return -1;

  char line[1024];
  while (fgets(line, sizeof(line), f)) {
    char *nl = strchr(line, '\n');
    if (nl) *nl = '\0';
    if (line[0] == '#' || line[0] == '\0') continue;

    char *p = line;
    while (p && (*p == ' ' || *p == '\t')) p++;
    char *tok_domain = strsep(&p, " \t");
    if (!tok_domain || strcmp(tok_domain, domain) != 0) continue;

    while (p && (*p == ' ' || *p == '\t')) p++;
    char *tok_host = strsep(&p, " \t");
    while (p && (*p == ' ' || *p == '\t')) p++;
    char *tok_port = strsep(&p, " \t");

    if (!tok_host || !tok_port || !*tok_host || !*tok_port) {
      fclose(f);
      return -1;
    }

    char *end = NULL;
    long port = strtol(tok_port, &end, 10);
    if (!end || *end != '\0' || port < 1 || port > 65535) {
      fclose(f);
      return -1;
    }

    snprintf(out_host, out_host_size, "%s", tok_host);
    *out_port = (int)port;
    fclose(f);
    return 0;
  }

  fclose(f);
  return -1;
}

static int detect_local_identity(char *out_host, size_t out_host_size) {
  if (!out_host || out_host_size == 0) return -1;
  out_host[0] = '\0';

  struct ifaddrs *ifaddr = NULL;
  if (getifaddrs(&ifaddr) == 0) {
    for (struct ifaddrs *ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
      if (!ifa->ifa_addr) continue;
      if (ifa->ifa_addr->sa_family != AF_INET) continue;
      if (ifa->ifa_flags & IFF_LOOPBACK) continue;
      const struct sockaddr_in *sa = (const struct sockaddr_in *)ifa->ifa_addr;
      if (inet_ntop(AF_INET, &sa->sin_addr, out_host, out_host_size)) {
        freeifaddrs(ifaddr);
        return 0;
      }
    }
    freeifaddrs(ifaddr);
  }

  if (gethostname(out_host, out_host_size - 1) == 0 && out_host[0]) {
    out_host[out_host_size - 1] = '\0';
    return 0;
  }
  return -1;
}

static int parse_tcp_endpoint(const char *endpoint, char *out_host,
                              size_t out_host_size, int *out_port) {
  if (!endpoint || !out_host || out_host_size == 0 || !out_port) return -1;
  if (strncmp(endpoint, "tcp://", 6) != 0) return -1;

  const char *addr = endpoint + 6;
  const char *port_text = NULL;
  size_t host_len = 0;

  if (*addr == '[') {
    const char *end = strchr(addr + 1, ']');
    if (!end || end[1] != ':') return -1;
    host_len = (size_t)(end - (addr + 1));
    port_text = end + 2;
    if (host_len == 0) return -1;
    if (host_len >= out_host_size) host_len = out_host_size - 1;
    memcpy(out_host, addr + 1, host_len);
  } else {
    const char *colon = strrchr(addr, ':');
    if (!colon || !colon[1]) return -1;
    host_len = (size_t)(colon - addr);
    port_text = colon + 1;
    if (host_len == 0) return -1;
    if (host_len >= out_host_size) host_len = out_host_size - 1;
    memcpy(out_host, addr, host_len);
  }
  out_host[host_len] = '\0';

  char *end = NULL;
  long port = strtol(port_text, &end, 10);
  if (!end || *end != '\0' || port < 1 || port > 65535) return -1;
  *out_port = (int)port;
  return 0;
}

static int host_is_wildcard(const char *host) {
  if (!host || !*host) return 0;
  return strcmp(host, "0.0.0.0") == 0 ||
         strcmp(host, "::") == 0 ||
         strcmp(host, "*") == 0;
}

static int update_domain_host_in_file(const char *file_name, const char *domain,
                                      const char *new_host, int new_port) {
  if (!file_name || !*file_name || !domain || !*domain || !new_host || !*new_host) return -1;
  if (new_port < 1 || new_port > 65535) return -1;
  const mode_t domains_mode = (S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

  FILE *in = fopen(file_name, "r");
  if (!in) return -1;

  char tmp_name[ZCM_DOMAIN_PATH_MAX];
  if (snprintf(tmp_name, sizeof(tmp_name), "%s.tmpXXXXXX", file_name) >= (int)sizeof(tmp_name)) {
    fclose(in);
    return -1;
  }
  int fd = mkstemp(tmp_name);
  if (fd < 0) {
    fclose(in);
    return -1;
  }
  if (fchmod(fd, domains_mode) != 0) {
    close(fd);
    unlink(tmp_name);
    fclose(in);
    return -1;
  }
  FILE *out = fdopen(fd, "w");
  if (!out) {
    close(fd);
    unlink(tmp_name);
    fclose(in);
    return -1;
  }

  int updated = 0;
  char line[2048];
  while (fgets(line, sizeof(line), in)) {
    char original[2048];
    snprintf(original, sizeof(original), "%s", line);

    char parse_buf[2048];
    snprintf(parse_buf, sizeof(parse_buf), "%s", line);
    char *nl = strchr(parse_buf, '\n');
    if (nl) *nl = '\0';

    char *p = parse_buf;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p == '#' || *p == '\0') {
      fputs(original, out);
      continue;
    }

    char *tokens[16] = {0};
    size_t ntok = 0;
    char *saveptr = NULL;
    for (char *tok = strtok_r(p, " \t", &saveptr);
         tok && ntok < 16;
         tok = strtok_r(NULL, " \t", &saveptr)) {
      tokens[ntok++] = tok;
    }

    if (ntok >= 3 && strcmp(tokens[0], domain) == 0) {
      fprintf(out, "%s %s %d", tokens[0], new_host, new_port);
      for (size_t i = 3; i < ntok; i++) {
        fprintf(out, " %s", tokens[i]);
      }
      fputc('\n', out);
      updated = 1;
    } else {
      fputs(original, out);
    }
  }

  int write_ok = (fflush(out) == 0);
  fclose(out);
  fclose(in);
  if (!write_ok) {
    unlink(tmp_name);
    return -1;
  }

  if (!updated) {
    unlink(tmp_name);
    return -1;
  }

  if (rename(tmp_name, file_name) != 0) {
    unlink(tmp_name);
    return -1;
  }
  if (chmod(file_name, domains_mode) != 0) {
    return -1;
  }
  return 0;
}

int zcm_domain_info_load(zcm_domain_info_t *out_info) {
  if (!out_info) return -1;
  memset(out_info, 0, sizeof(*out_info));

  const char *override = getenv("ZCMBROKER");
  if (!override || !*override) override = getenv("ZCMBROKER_ENDPOINT");
  if (override && *override) {
    char host[ZCM_DOMAIN_NAME_MAX] = {0};
    int port = 0;
    snprintf(out_info->query_endpoint, sizeof(out_info->query_endpoint), "%s", override);
    snprintf(out_info->bind_endpoint, sizeof(out_info->bind_endpoint), "%s", override);
    if (parse_tcp_endpoint(override, host, sizeof(host), &port) == 0) {
      if (host_is_wildcard(host) && detect_local_identity(out_info->publish_host, sizeof(out_info->publish_host)) != 0) {
        out_info->publish_host[0] = '\0';
      } else if (!host_is_wildcard(host)) {
        snprintf(out_info->publish_host, sizeof(out_info->publish_host), "%s", host);
      }
      out_info->port = port;
    }
    return 0;
  }

  const char *domain = getenv("ZCMDOMAIN");
  if (!domain || !*domain) return -1;

  char host[ZCM_DOMAIN_NAME_MAX] = {0};
  int port = 0;
  if (resolve_domains_file_path(out_info->domains_file, sizeof(out_info->domains_file)) != 0) return -1;
  if (load_domain_endpoint(domain, out_info->domains_file, host, sizeof(host), &port) != 0) return -1;

  snprintf(out_info->domain, sizeof(out_info->domain), "%s", domain);
  snprintf(out_info->query_endpoint, sizeof(out_info->query_endpoint), "tcp://%s:%d", host, port);
  if (detect_local_identity(out_info->publish_host, sizeof(out_info->publish_host)) != 0 || !out_info->publish_host[0]) {
    snprintf(out_info->publish_host, sizeof(out_info->publish_host), "%s", host);
  }
  snprintf(out_info->bind_endpoint, sizeof(out_info->bind_endpoint),
           "tcp://%s:%d", out_info->publish_host, port);
  out_info->port = port;
  out_info->has_domain_mapping = 1;
  return 0;
}

int zcm_domain_info_update_published_host(const zcm_domain_info_t *info) {
  if (!info || !info->has_domain_mapping) return -1;
  return update_domain_host_in_file(info->domains_file, info->domain,
                                    info->publish_host, info->port);
}
