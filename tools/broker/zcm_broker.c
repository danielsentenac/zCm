#include "zcm/zcm.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <arpa/inet.h>

static volatile sig_atomic_t g_stop = 0;

static void handle_sig(int sig) {
  (void)sig;
  g_stop = 1;
}

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
  if (!domain || !*domain || !file_name || !*file_name || !out_host || out_host_size == 0 || !out_port) {
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

static int parse_tcp_endpoint_port(const char *endpoint, int *out_port) {
  if (!endpoint || !out_port) return -1;
  if (strncmp(endpoint, "tcp://", 6) != 0) return -1;
  const char *addr = endpoint + 6;
  const char *colon = strrchr(addr, ':');
  if (!colon || !colon[1]) return -1;
  char *end = NULL;
  long port = strtol(colon + 1, &end, 10);
  if (!end || *end != '\0' || port < 1 || port > 65535) return -1;
  *out_port = (int)port;
  return 0;
}

static int detect_local_ipv4(char *out_host, size_t out_host_size) {
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

  char host[256] = {0};
  if (gethostname(host, sizeof(host) - 1) == 0 && host[0]) {
    snprintf(out_host, out_host_size, "%s", host);
    return 0;
  }
  return -1;
}

static int update_domain_host_in_file(const char *file_name, const char *domain,
                                      const char *new_host, int new_port) {
  if (!file_name || !*file_name || !domain || !*domain || !new_host || !*new_host) return -1;
  if (new_port < 1 || new_port > 65535) return -1;

  FILE *in = fopen(file_name, "r");
  if (!in) return -1;

  char tmp_name[1024];
  if (snprintf(tmp_name, sizeof(tmp_name), "%s.tmpXXXXXX", file_name) >= (int)sizeof(tmp_name)) {
    fclose(in);
    return -1;
  }
  int fd = mkstemp(tmp_name);
  if (fd < 0) {
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
  return 0;
}

static int sync_domain_broker_host(const char *endpoint) {
  if (!endpoint || !*endpoint) return -1;

  const char *domain = getenv("ZCMDOMAIN");
  if (!domain || !*domain) return -1;

  char file_name[512];
  if (resolve_domains_file_path(file_name, sizeof(file_name)) != 0) return -1;

  int port = 0;
  if (parse_tcp_endpoint_port(endpoint, &port) != 0) return -1;

  char local_host[256] = {0};
  if (detect_local_ipv4(local_host, sizeof(local_host)) != 0) return -1;

  if (update_domain_host_in_file(file_name, domain, local_host, port) != 0) return -1;
  printf("zcm_broker: updated %s domain=%s host=%s port=%d\n",
         file_name, domain, local_host, port);
  fflush(stdout);
  return 0;
}

static char *load_endpoint_from_config(void) {
  const char *override = getenv("ZCMBROKER");
  if (!override || !*override) override = getenv("ZCMBROKER_ENDPOINT");
  if (override && *override) {
    char *endpoint = strdup(override);
    if (!endpoint) return NULL;
    return endpoint;
  }

  const char *domain = getenv("ZCMDOMAIN");
  if (!domain || !*domain) return NULL;

  char file_name[512];
  char host[256] = {0};
  int port = 0;
  if (resolve_domains_file_path(file_name, sizeof(file_name)) != 0) return NULL;
  if (load_domain_endpoint(domain, file_name, host, sizeof(host), &port) != 0) return NULL;
  {
    char local_host[256] = {0};
    if (detect_local_ipv4(local_host, sizeof(local_host)) == 0 && local_host[0]) {
      snprintf(host, sizeof(host), "%s", local_host);
    }
  }

  char *endpoint = malloc(512);
  if (!endpoint) return NULL;
  snprintf(endpoint, 512, "tcp://%s:%d", host, port);
  return endpoint;
}

int main(void) {
  int rc = 1;
  char *endpoint = load_endpoint_from_config();
  zcm_context_t *ctx = NULL;
  zcm_broker_t *broker = NULL;

  if (!endpoint) {
    fprintf(stderr, "zcm_broker: missing ZCMDOMAIN or ZCmDomains entry\n");
    goto out;
  }

  ctx = zcm_context_new();
  if (!ctx) goto out;

  broker = zcm_broker_start(ctx, endpoint);
  if (!broker) {
    fprintf(stderr, "zcm_broker: start failed\n");
    goto out;
  }

  if (sync_domain_broker_host(endpoint) != 0) {
    fprintf(stderr,
            "zcm_broker: warning: could not update ZCmDomains "
            "(check ZCMDOMAIN and write permissions)\n");
  }

  signal(SIGINT, handle_sig);
  signal(SIGTERM, handle_sig);

  printf("zcm_broker listening on %s (Ctrl+C to stop)\n", endpoint);
  fflush(stdout);

  while (!g_stop && zcm_broker_is_running(broker)) {
    sleep(1);
  }

  rc = 0;

out:
  if (broker) zcm_broker_stop(broker);
  if (ctx) zcm_context_free(ctx);
  free(endpoint);
  return rc;
}
