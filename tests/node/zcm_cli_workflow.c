#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef ZCM_SOURCE_DIR
#define ZCM_SOURCE_DIR "."
#endif

typedef struct cmd_result {
  int exit_code;
  char *output;
} cmd_result_t;

static int append_buf(char **dst, size_t *len, size_t *cap, const char *src, size_t n) {
  if (!dst || !len || !cap || (!src && n > 0)) return -1;
  size_t need = *len + n + 1;
  if (need > *cap) {
    size_t new_cap = (*cap == 0) ? 1024 : *cap;
    while (new_cap < need) new_cap *= 2;
    char *p = (char *)realloc(*dst, new_cap);
    if (!p) return -1;
    *dst = p;
    *cap = new_cap;
  }
  if (n > 0) memcpy(*dst + *len, src, n);
  *len += n;
  (*dst)[*len] = '\0';
  return 0;
}

static int run_capture_argv(const char *const argv[], int timeout_ms, cmd_result_t *out) {
  if (!argv || !argv[0] || !out) return -1;
  out->exit_code = -1;
  out->output = NULL;

  int pipefd[2];
  if (pipe(pipefd) != 0) return -1;

  pid_t pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    return -1;
  }

  if (pid == 0) {
    close(pipefd[0]);
    dup2(pipefd[1], STDOUT_FILENO);
    dup2(pipefd[1], STDERR_FILENO);
    close(pipefd[1]);

    execv(argv[0], (char *const *)argv);
    perror("execv");
    _exit(127);
  }

  close(pipefd[1]);
  int flags = fcntl(pipefd[0], F_GETFL, 0);
  if (flags >= 0) (void)fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

  char *buf = NULL;
  size_t len = 0;
  size_t cap = 0;
  int elapsed = 0;
  int status = 0;
  int child_done = 0;

  while (1) {
    struct pollfd pfd;
    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = pipefd[0];
    pfd.events = POLLIN;

    int wait_ms = 100;
    if (!child_done && timeout_ms > 0 && elapsed >= timeout_ms) {
      kill(pid, SIGKILL);
      waitpid(pid, &status, 0);
      child_done = 1;
    }

    int prc = poll(&pfd, 1, wait_ms);
    if (prc > 0 && (pfd.revents & POLLIN)) {
      char tmp[1024];
      ssize_t n = read(pipefd[0], tmp, sizeof(tmp));
      if (n > 0) {
        if (append_buf(&buf, &len, &cap, tmp, (size_t)n) != 0) {
          close(pipefd[0]);
          free(buf);
          return -1;
        }
      }
    }

    if (!child_done) {
      pid_t r = waitpid(pid, &status, WNOHANG);
      if (r == pid) child_done = 1;
    }

    if (child_done) {
      char tmp[1024];
      while (1) {
        ssize_t n = read(pipefd[0], tmp, sizeof(tmp));
        if (n > 0) {
          if (append_buf(&buf, &len, &cap, tmp, (size_t)n) != 0) {
            close(pipefd[0]);
            free(buf);
            return -1;
          }
        } else {
          break;
        }
      }
      break;
    }

    elapsed += wait_ms;
  }

  close(pipefd[0]);
  if (!buf) {
    buf = (char *)calloc(1, 1);
    if (!buf) return -1;
  }

  out->output = buf;
  if (WIFEXITED(status)) out->exit_code = WEXITSTATUS(status);
  else out->exit_code = 255;
  return 0;
}

static int start_daemon_argv(const char *const argv[], const char *log_path, pid_t *out_pid) {
  if (!argv || !argv[0] || !out_pid) return -1;

  pid_t pid = fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    int out_fd = -1;
    if (log_path && *log_path) {
      out_fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    }
    if (out_fd < 0) out_fd = open("/dev/null", O_WRONLY);
    if (out_fd >= 0) {
      dup2(out_fd, STDOUT_FILENO);
      dup2(out_fd, STDERR_FILENO);
      close(out_fd);
    }
    execv(argv[0], (char *const *)argv);
    _exit(127);
  }

  *out_pid = pid;
  return 0;
}

static void stop_pid(pid_t *pid) {
  if (!pid || *pid <= 0) return;
  pid_t p = *pid;

  kill(p, SIGTERM);
  for (int i = 0; i < 20; i++) {
    int st = 0;
    pid_t r = waitpid(p, &st, WNOHANG);
    if (r == p) {
      *pid = -1;
      return;
    }
    usleep(100 * 1000);
  }

  kill(p, SIGKILL);
  (void)waitpid(p, NULL, 0);
  *pid = -1;
}

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

static int pick_free_tcp_port(void) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(0);

  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    close(fd);
    return -1;
  }

  socklen_t len = sizeof(addr);
  if (getsockname(fd, (struct sockaddr *)&addr, &len) != 0) {
    close(fd);
    return -1;
  }

  int port = (int)ntohs(addr.sin_port);
  close(fd);
  return port;
}

static int pick_distinct_ports(int *broker_port, int *first_port) {
  if (!broker_port || !first_port) return -1;
  for (int i = 0; i < 64; i++) {
    int b = pick_free_tcp_port();
    int f = pick_free_tcp_port();
    if (b <= 0 || f <= 0) continue;
    if (b == f) continue;
    if (f > b && f < (b + 200)) continue;
    if (b > f && b < (f + 200)) continue;
    *broker_port = b;
    *first_port = f;
    return 0;
  }
  return -1;
}

static int compute_build_dir(char *out, size_t out_size) {
  if (!out || out_size == 0) return -1;
  char exe[1024];
  ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
  if (n <= 0 || n >= (ssize_t)sizeof(exe)) return -1;
  exe[n] = '\0';

  char *p = strrchr(exe, '/');
  if (!p) return -1;
  *p = '\0'; /* .../build/tests */
  p = strrchr(exe, '/');
  if (!p) return -1;
  *p = '\0'; /* .../build */

  if (snprintf(out, out_size, "%s", exe) >= (int)out_size) return -1;
  return 0;
}

static int program_in_path(const char *prog) {
  if (!prog || !*prog) return 0;
  if (strchr(prog, '/')) return access(prog, X_OK) == 0;

  const char *path = getenv("PATH");
  if (!path || !*path) return 0;
  char *copy = strdup(path);
  if (!copy) return 0;

  int found = 0;
  char *saveptr = NULL;
  for (char *dir = strtok_r(copy, ":", &saveptr); dir; dir = strtok_r(NULL, ":", &saveptr)) {
    char candidate[2048];
    if (!*dir) dir = ".";
    if (snprintf(candidate, sizeof(candidate), "%s/%s", dir, prog) >= (int)sizeof(candidate)) continue;
    if (access(candidate, X_OK) == 0) {
      found = 1;
      break;
    }
  }

  free(copy);
  return found;
}

static void dump_file_with_header(const char *header, const char *path) {
  if (!header || !path || !*path) return;
  fprintf(stderr, "--- %s (%s) ---\n", header, path);
  FILE *f = fopen(path, "r");
  if (!f) {
    fprintf(stderr, "<unavailable>\n");
    return;
  }
  char line[512];
  int lines = 0;
  while (fgets(line, sizeof(line), f) && lines < 120) {
    fputs(line, stderr);
    lines++;
  }
  if (!feof(f)) fprintf(stderr, "... (truncated)\n");
  fclose(f);
}

static int names_has_name(const char *text, const char *name) {
  if (!text || !name || !*name) return 0;

  char *copy = strdup(text);
  if (!copy) return 0;

  int found = 0;
  char *saveptr = NULL;
  for (char *line = strtok_r(copy, "\n", &saveptr);
       line;
       line = strtok_r(NULL, "\n", &saveptr)) {
    if (!*line) continue;
    if (strncmp(line, "NAME", 4) == 0) continue;
    if (line[0] == '-') continue;

    char tok[128] = {0};
    if (sscanf(line, "%127s", tok) != 1) continue;
    if (strcmp(tok, name) == 0) {
      found = 1;
      break;
    }
  }

  free(copy);
  return found;
}

static int wait_cmd_ok(const char *const argv[], const char *must_contain, int timeout_ms) {
  int waited = 0;
  while (waited < timeout_ms) {
    cmd_result_t r;
    if (run_capture_argv(argv, 6000, &r) != 0) return -1;
    int ok = (r.exit_code == 0);
    if (ok && must_contain && *must_contain) {
      ok = (strstr(r.output, must_contain) != NULL);
    }
    if (ok) {
      free(r.output);
      return 0;
    }
    free(r.output);
    usleep(200 * 1000);
    waited += 200;
  }
  return -1;
}

static int wait_name_state(const char *zcm_path, const char *name, int should_exist, int timeout_ms) {
  int waited = 0;
  char *last_output = NULL;
  while (waited < timeout_ms) {
    const char *argv[] = {zcm_path, "names", NULL};
    cmd_result_t r;
    if (run_capture_argv(argv, 6000, &r) != 0) {
      free(last_output);
      return -1;
    }
    free(last_output);
    last_output = strdup(r.output ? r.output : "");
    if (r.exit_code == 0) {
      int has = names_has_name(r.output, name);
      free(r.output);
      if ((has ? 1 : 0) == (should_exist ? 1 : 0)) {
        free(last_output);
        return 0;
      }
    } else {
      free(r.output);
    }
    usleep(200 * 1000);
    waited += 200;
  }
  if (last_output) {
    fprintf(stderr, "zcm_cli_workflow: last `zcm names` output while waiting for %s=%s:\n%s\n",
            name, should_exist ? "present" : "absent", last_output);
    free(last_output);
  }
  return -1;
}

int main(void) {
  int rc = 1;

  char build_dir[1024] = {0};
  if (compute_build_dir(build_dir, sizeof(build_dir)) != 0) {
    fprintf(stderr, "zcm_cli_workflow: unable to determine build dir\n");
    return 1;
  }

  char zcm_path[1200] = {0};
  char broker_path[1200] = {0};
  char proc_path[1200] = {0};
  snprintf(zcm_path, sizeof(zcm_path), "%s/tools/zcm", build_dir);
  snprintf(broker_path, sizeof(broker_path), "%s/tools/zcm_broker", build_dir);
  snprintf(proc_path, sizeof(proc_path), "%s/examples/zcm_proc", build_dir);

  char tmp_dir[] = "/tmp/zcm-cli-workflow-XXXXXX";
  if (!mkdtemp(tmp_dir)) {
    perror("mkdtemp");
    return 1;
  }

  char db_path[1200] = {0};
  char basic_cfg[1200] = {0};
  char publisher_cfg[1200] = {0};
  char subscriber_cfg[1200] = {0};
  char broker_log[1200] = {0};
  char publisher_log[1200] = {0};
  char basic_log[1200] = {0};
  char subscriber_log[1200] = {0};
  char publisher2_log[1200] = {0};
  snprintf(db_path, sizeof(db_path), "%s/ZCmDomains", tmp_dir);
  snprintf(basic_cfg, sizeof(basic_cfg), "%s/basic.cfg", tmp_dir);
  snprintf(publisher_cfg, sizeof(publisher_cfg), "%s/publisher.cfg", tmp_dir);
  snprintf(subscriber_cfg, sizeof(subscriber_cfg), "%s/subscriber.cfg", tmp_dir);
  snprintf(broker_log, sizeof(broker_log), "%s/broker.log", tmp_dir);
  snprintf(publisher_log, sizeof(publisher_log), "%s/publisher.log", tmp_dir);
  snprintf(basic_log, sizeof(basic_log), "%s/basic.log", tmp_dir);
  snprintf(subscriber_log, sizeof(subscriber_log), "%s/subscriber.log", tmp_dir);
  snprintf(publisher2_log, sizeof(publisher2_log), "%s/publisher2.log", tmp_dir);

  if (!program_in_path("xmllint")) {
    printf("zcm_cli_workflow: SKIP (xmllint not found in PATH)\n");
    rc = 0;
    goto done;
  }

  int broker_port = -1;
  int first_port = -1;
  if (pick_distinct_ports(&broker_port, &first_port) != 0) {
    fprintf(stderr, "zcm_cli_workflow: SKIP (no local TCP port allocation available)\n");
    rc = 0;
    goto done;
  }

  char db_line[256] = {0};
  snprintf(db_line, sizeof(db_line), "wf 127.0.0.1 %d %d 128 repo\n", broker_port, first_port);
  if (write_text_file(db_path, db_line) != 0) {
    perror("write ZCmDomains");
    goto done;
  }

  const char *publisher_xml =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<procConfig>\n"
      "  <process name=\"publisher\">\n"
      "    <dataSocket type=\"PUB\" payload=\"publisher\" intervalMs=\"200\"/>\n"
      "    <control timeoutMs=\"200\"/>\n"
      "  </process>\n"
      "</procConfig>\n";
  const char *subscriber_xml =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<procConfig>\n"
      "  <process name=\"subscriber\">\n"
      "    <dataSocket type=\"SUB\" targets=\"basic\" topics=\"basic\"/>\n"
      "    <control timeoutMs=\"200\"/>\n"
      "  </process>\n"
      "</procConfig>\n";
  const char *basic_xml =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<procConfig>\n"
      "  <process name=\"basic\">\n"
      "    <dataSocket type=\"PUB\" payload=\"basic\" intervalMs=\"200\"/>\n"
      "    <dataSocket type=\"SUB\" targets=\"publisher\" topics=\"publisher\"/>\n"
      "    <control timeoutMs=\"200\"/>\n"
      "    <handlers>\n"
      "      <type name=\"QUERY\">\n"
      "        <arg kind=\"double\"/>\n"
      "        <arg kind=\"double\"/>\n"
      "        <arg kind=\"text\"/>\n"
      "        <arg kind=\"double\"/>\n"
      "      </type>\n"
      "    </handlers>\n"
      "  </process>\n"
      "</procConfig>\n";

  if (write_text_file(publisher_cfg, publisher_xml) != 0 ||
      write_text_file(subscriber_cfg, subscriber_xml) != 0 ||
      write_text_file(basic_cfg, basic_xml) != 0) {
    perror("write cfg");
    goto done;
  }

  setenv("ZCMDOMAIN", "wf", 1);
  setenv("ZCMDOMAIN_DATABASE", tmp_dir, 1);
  setenv("ZCM_PROC_REANNOUNCE_MS", "200", 1);

  char schema_path[1200] = {0};
  snprintf(schema_path, sizeof(schema_path), "%s/config/schema/proc-config.xsd", ZCM_SOURCE_DIR);
  setenv("ZCM_PROC_CONFIG_SCHEMA", schema_path, 1);

  pid_t broker_pid = -1;
  pid_t publisher_pid = -1;
  pid_t basic_pid = -1;
  pid_t subscriber_pid = -1;
  pid_t publisher2_pid = -1;

  printf("zcm_cli_workflow: start broker\n");
  {
    const char *argv[] = {broker_path, NULL};
    if (start_daemon_argv(argv, broker_log, &broker_pid) != 0) {
      fprintf(stderr, "zcm_cli_workflow: failed to start broker\n");
      goto done;
    }
  }

  {
    const char *argv[] = {zcm_path, "broker", "ping", NULL};
    if (wait_cmd_ok(argv, NULL, 8000) != 0) {
      fprintf(stderr, "zcm_cli_workflow: broker ping failed\n");
      goto done;
    }
  }

  printf("zcm_cli_workflow: start publisher/basic/subscriber\n");
  {
    const char *argv[] = {proc_path, publisher_cfg, NULL};
    if (start_daemon_argv(argv, publisher_log, &publisher_pid) != 0) goto done;
  }
  {
    const char *argv[] = {proc_path, basic_cfg, NULL};
    if (start_daemon_argv(argv, basic_log, &basic_pid) != 0) goto done;
  }
  {
    const char *argv[] = {proc_path, subscriber_cfg, NULL};
    if (start_daemon_argv(argv, subscriber_log, &subscriber_pid) != 0) goto done;
  }

  if (wait_name_state(zcm_path, "publisher", 1, 20000) != 0 ||
      wait_name_state(zcm_path, "basic", 1, 20000) != 0 ||
      wait_name_state(zcm_path, "subscriber", 1, 20000) != 0) {
    fprintf(stderr, "zcm_cli_workflow: names did not register all procs\n");
    dump_file_with_header("broker log", broker_log);
    dump_file_with_header("publisher log", publisher_log);
    dump_file_with_header("basic log", basic_log);
    dump_file_with_header("subscriber log", subscriber_log);
    goto done;
  }

  printf("zcm_cli_workflow: send QUERY to basic and expect QUERY_RPL\n");
  {
    const char *argv[] = {
      zcm_path, "send", "basic", "-type", "QUERY",
      "-d", "5", "-d", "7", "-t", "action", "-d", "0",
      NULL
    };
    cmd_result_t r;
    if (run_capture_argv(argv, 8000, &r) != 0) goto done;
    int ok = (r.exit_code == 0 && strstr(r.output, "msgType=QUERY_RPL") != NULL);
    if (!ok) {
      fprintf(stderr, "zcm_cli_workflow: send QUERY failed output:\n%s\n", r.output);
      free(r.output);
      goto done;
    }
    free(r.output);
  }

  printf("zcm_cli_workflow: kill publisher and verify it disappears from names\n");
  {
    const char *argv[] = {zcm_path, "kill", "publisher", NULL};
    cmd_result_t r;
    if (run_capture_argv(argv, 6000, &r) != 0) goto done;
    int ok = (r.exit_code == 0);
    free(r.output);
    if (!ok) {
      fprintf(stderr, "zcm_cli_workflow: zcm kill publisher failed\n");
      goto done;
    }
  }
  if (wait_name_state(zcm_path, "publisher", 0, 20000) != 0) {
    fprintf(stderr, "zcm_cli_workflow: publisher still present after kill\n");
    goto done;
  }
  stop_pid(&publisher_pid);

  printf("zcm_cli_workflow: stop broker then verify names has offline output\n");
  {
    const char *argv[] = {zcm_path, "broker", "stop", NULL};
    cmd_result_t r;
    if (run_capture_argv(argv, 8000, &r) != 0) goto done;
    int ok = (r.exit_code == 0);
    free(r.output);
    if (!ok) {
      fprintf(stderr, "zcm_cli_workflow: broker stop failed\n");
      goto done;
    }
  }
  stop_pid(&broker_pid);

  {
    const char *argv[] = {zcm_path, "names", NULL};
    cmd_result_t r;
    if (run_capture_argv(argv, 8000, &r) != 0) goto done;
    int ok = (r.exit_code != 0 && strstr(r.output, "BROKER_OFFLINE") != NULL);
    if (!ok) {
      fprintf(stderr, "zcm_cli_workflow: expected offline names output, got:\n%s\n", r.output);
      free(r.output);
      goto done;
    }
    free(r.output);
  }

  printf("zcm_cli_workflow: restart broker, relaunch publisher, verify recovered workflow\n");
  {
    const char *argv[] = {broker_path, NULL};
    if (start_daemon_argv(argv, broker_log, &broker_pid) != 0) goto done;
  }
  {
    const char *argv[] = {zcm_path, "broker", "ping", NULL};
    if (wait_cmd_ok(argv, NULL, 10000) != 0) {
      fprintf(stderr, "zcm_cli_workflow: broker ping after restart failed\n");
      goto done;
    }
  }

  {
    const char *argv[] = {proc_path, publisher_cfg, NULL};
    if (start_daemon_argv(argv, publisher2_log, &publisher2_pid) != 0) goto done;
  }

  if (wait_name_state(zcm_path, "basic", 1, 20000) != 0 ||
      wait_name_state(zcm_path, "subscriber", 1, 20000) != 0 ||
      wait_name_state(zcm_path, "publisher", 1, 20000) != 0) {
    fprintf(stderr, "zcm_cli_workflow: names not restored after restart\n");
    dump_file_with_header("broker log", broker_log);
    dump_file_with_header("basic log", basic_log);
    dump_file_with_header("subscriber log", subscriber_log);
    dump_file_with_header("publisher2 log", publisher2_log);
    goto done;
  }

  {
    const char *argv[] = {
      zcm_path, "send", "basic", "-type", "QUERY",
      "-d", "1", "-d", "2", "-t", "again", "-d", "3",
      NULL
    };
    cmd_result_t r;
    if (run_capture_argv(argv, 8000, &r) != 0) goto done;
    int ok = (r.exit_code == 0 && strstr(r.output, "msgType=QUERY_RPL") != NULL);
    if (!ok) {
      fprintf(stderr, "zcm_cli_workflow: send QUERY after restart failed output:\n%s\n", r.output);
      free(r.output);
      goto done;
    }
    free(r.output);
  }

  printf("zcm_cli_workflow: PASS\n");
  rc = 0;

done:
  stop_pid(&publisher2_pid);
  stop_pid(&subscriber_pid);
  stop_pid(&basic_pid);
  stop_pid(&publisher_pid);
  stop_pid(&broker_pid);

  if (basic_cfg[0]) unlink(basic_cfg);
  if (publisher_cfg[0]) unlink(publisher_cfg);
  if (subscriber_cfg[0]) unlink(subscriber_cfg);
  if (db_path[0]) unlink(db_path);
  rmdir(tmp_dir);

  return rc;
}
