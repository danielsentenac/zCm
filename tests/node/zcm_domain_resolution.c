#include "zcm/zcm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int write_text_file(const char *path, const char *text) {
  FILE *f = fopen(path, "w");
  if (!f) return -1;
  if (fputs(text, f) == EOF) {
    fclose(f);
    return -1;
  }
  fclose(f);
  return 0;
}

static int read_first_line(const char *path, char *out, size_t out_size) {
  FILE *f = NULL;
  char *nl = NULL;

  if (!path || !out || out_size == 0) return -1;
  out[0] = '\0';

  f = fopen(path, "r");
  if (!f) return -1;
  if (!fgets(out, (int)out_size, f)) {
    fclose(f);
    return -1;
  }
  fclose(f);

  nl = strchr(out, '\n');
  if (nl) *nl = '\0';
  return 0;
}

int main(void) {
  int rc = 1;
  char tmp_dir[] = "/tmp/zcm-domain-resolution-XXXXXX";
  char db_path[512] = {0};
  char expected_query[512] = {0};
  char expected_bind[512] = {0};
  char line[1024] = {0};
  const char *domain = "singleton-test";
  const char *remote_host = "remote-broker.invalid";
  const int port = 48555;
  zcm_domain_info_t info;
  zcm_domain_info_t override_info;
  zcm_domain_info_t updated_info;

  if (!mkdtemp(tmp_dir)) {
    perror("mkdtemp");
    return 1;
  }

  snprintf(db_path, sizeof(db_path), "%s/ZCmDomains", tmp_dir);
  if (write_text_file(db_path,
                      "singleton-test remote-broker.invalid 48555 61234 64 repo\n") != 0) {
    perror("write_text_file");
    goto done;
  }

  (void)unsetenv("ZCMBROKER");
  (void)unsetenv("ZCMBROKER_ENDPOINT");
  (void)unsetenv("ZCMMGR");
  (void)setenv("ZCMDOMAIN", domain, 1);
  (void)setenv("ZCMDOMAIN_DATABASE", tmp_dir, 1);

  memset(&info, 0, sizeof(info));
  if (zcm_domain_info_load(&info) != 0) {
    fprintf(stderr, "zcm_domain_resolution: load from domain database failed\n");
    goto done;
  }

  snprintf(expected_query, sizeof(expected_query), "tcp://%s:%d", remote_host, port);
  snprintf(expected_bind, sizeof(expected_bind), "tcp://%s:%d", info.publish_host, port);

  if (!info.has_domain_mapping) {
    fprintf(stderr, "zcm_domain_resolution: expected domain mapping\n");
    goto done;
  }
  if (strcmp(info.domain, domain) != 0) {
    fprintf(stderr, "zcm_domain_resolution: wrong domain: %s\n", info.domain);
    goto done;
  }
  if (strcmp(info.domains_file, db_path) != 0) {
    fprintf(stderr, "zcm_domain_resolution: wrong domains file: %s\n", info.domains_file);
    goto done;
  }
  if (strcmp(info.query_endpoint, expected_query) != 0) {
    fprintf(stderr, "zcm_domain_resolution: wrong query endpoint: %s\n", info.query_endpoint);
    goto done;
  }
  if (!info.publish_host[0]) {
    fprintf(stderr, "zcm_domain_resolution: missing local publish host\n");
    goto done;
  }
  if (strcmp(info.publish_host, remote_host) == 0) {
    fprintf(stderr, "zcm_domain_resolution: publish host did not switch away from remote host\n");
    goto done;
  }
  if (strcmp(info.bind_endpoint, expected_bind) != 0) {
    fprintf(stderr, "zcm_domain_resolution: wrong bind endpoint: %s\n", info.bind_endpoint);
    goto done;
  }
  if (info.port != port) {
    fprintf(stderr, "zcm_domain_resolution: wrong port: %d\n", info.port);
    goto done;
  }

  updated_info = info;
  snprintf(updated_info.publish_host, sizeof(updated_info.publish_host), "%s", "127.0.0.1");
  if (zcm_domain_info_update_published_host(&updated_info) != 0) {
    fprintf(stderr, "zcm_domain_resolution: failed to rewrite ZCmDomains\n");
    goto done;
  }
  if (read_first_line(db_path, line, sizeof(line)) != 0) {
    fprintf(stderr, "zcm_domain_resolution: failed to reread ZCmDomains\n");
    goto done;
  }
  if (strcmp(line, "singleton-test 127.0.0.1 48555 61234 64 repo") != 0) {
    fprintf(stderr, "zcm_domain_resolution: unexpected rewritten line: %s\n", line);
    goto done;
  }

  (void)setenv("ZCMBROKER", "tcp://127.0.0.1:57575", 1);
  memset(&override_info, 0, sizeof(override_info));
  if (zcm_domain_info_load(&override_info) != 0) {
    fprintf(stderr, "zcm_domain_resolution: override load failed\n");
    goto done;
  }
  if (override_info.has_domain_mapping) {
    fprintf(stderr, "zcm_domain_resolution: override should bypass domain mapping\n");
    goto done;
  }
  if (strcmp(override_info.query_endpoint, "tcp://127.0.0.1:57575") != 0 ||
      strcmp(override_info.bind_endpoint, "tcp://127.0.0.1:57575") != 0) {
    fprintf(stderr, "zcm_domain_resolution: override endpoints not unified\n");
    goto done;
  }
  if (strcmp(override_info.publish_host, "127.0.0.1") != 0) {
    fprintf(stderr, "zcm_domain_resolution: override publish host mismatch: %s\n",
            override_info.publish_host);
    goto done;
  }
  if (override_info.port != 57575) {
    fprintf(stderr, "zcm_domain_resolution: override port mismatch: %d\n", override_info.port);
    goto done;
  }

  printf("zcm_domain_resolution: PASS\n");
  rc = 0;

done:
  (void)unsetenv("ZCMBROKER");
  (void)unsetenv("ZCMBROKER_ENDPOINT");
  (void)unsetenv("ZCMDOMAIN");
  (void)unsetenv("ZCMDOMAIN_DATABASE");
  (void)unsetenv("ZCMMGR");
  if (db_path[0]) unlink(db_path);
  rmdir(tmp_dir);
  return rc;
}
