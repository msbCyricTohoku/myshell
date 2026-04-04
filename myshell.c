#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define MAX_LINE 4096
#define MAX_ARGS 512
#define MAX_HISTORY 1000
#define MAX_MATCHES 2048
#define MAX_ALIASES 128
#define MAX_JOBS 64
#define MAX_DIR_STACK 128

/* Theme Colors */
#define COLOR_RESET "\033[0m"
#define COLOR_GRAY "\033[90m"
#define COLOR_RED "\033[31m"
#define COLOR_GREEN "\033[32m"
#define COLOR_YELLOW "\033[33m"
#define COLOR_CYAN "\033[36m"
#define COLOR_WHITE "\033[37m"
#define COLOR_MAGENTA "\033[35m"

#define PROMPT_COLOR COLOR_MAGENTA
#define DIR_COLOR COLOR_YELLOW
#define SUGG_COLOR COLOR_GRAY
#define ERR_COLOR COLOR_RED

extern char **environ;

/* Structures */
typedef struct {
  char insert[1024];
  char display[1024];
  int is_dir;
  int is_exec;
} Completion;

typedef struct {
  char name[64];
  char val[MAX_LINE];
} Alias;

typedef struct {
  int id;
  pid_t pid;
  char cmd[MAX_LINE];
  int active;
  int stopped;
} Job;

/* Globals */
pid_t shell_pgid;
struct termios shell_tmodes;
struct termios orig_termios;
int shell_terminal;
int shell_is_interactive;

char history[MAX_HISTORY][MAX_LINE];
char history_file[1024] = "";
char prev_dir[1024] = "";

char dir_stack[MAX_DIR_STACK][1024];
int dir_stack_count = 0;
static int rc_depth = 0; /* Infinite recursion protector */

Alias aliases[MAX_ALIASES];
Job jobs[MAX_JOBS];

int alias_count = 0;
int history_count = 0;
int next_job_id = 1;
int last_status = 0;
int last_duration = 0;

volatile sig_atomic_t window_resized = 0;

/* Forward Declarations */
void tokenize(char *line, char **args);
void load_rc(const char *filepath);
void process_command(char *line);

/*======================================================
  SYSTEM & JOB ENGINE
======================================================*/

void sigwinch_handler(int sig) { window_resized = 1; }

void init_shell() {
  shell_terminal = STDIN_FILENO;
  shell_is_interactive = isatty(shell_terminal);
  if (shell_is_interactive) {
    while (tcgetpgrp(shell_terminal) != (shell_pgid = getpgrp()))
      kill(-shell_pgid, SIGTTIN);

    struct sigaction sa;
    sa.sa_handler = sigwinch_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGWINCH, &sa, NULL);

    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);

    shell_pgid = getpid();
    if (setpgid(shell_pgid, shell_pgid) < 0) {
      perror("myshell: setpgid");
      exit(1);
    }
    tcsetpgrp(shell_terminal, shell_pgid);
    tcgetattr(shell_terminal, &shell_tmodes);
    orig_termios = shell_tmodes;
  }
}

void add_job(pid_t pid, const char *cmd, int stopped) {
  for (int i = 0; i < MAX_JOBS; i++) {
    if (!jobs[i].active) {
      jobs[i].id = next_job_id++;
      jobs[i].pid = pid;
      snprintf(jobs[i].cmd, MAX_LINE, "%s", cmd);
      jobs[i].active = 1;
      jobs[i].stopped = stopped;
      if (!stopped)
        printf("[%d] %d\n", jobs[i].id, pid);
      return;
    }
  }
}

void check_jobs() {
  int status;
  pid_t pid;
  while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED)) > 0) {
    for (int i = 0; i < MAX_JOBS; i++) {
      if (jobs[i].active && jobs[i].pid == pid) {
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
          printf("\r\033[K[%d] Done       %s\n", jobs[i].id, jobs[i].cmd);
          jobs[i].active = 0;
        } else if (WIFSTOPPED(status)) {
          jobs[i].stopped = 1;
          printf("\r\033[K[%d] Stopped    %s\n", jobs[i].id, jobs[i].cmd);
        } else if (WIFCONTINUED(status)) {
          jobs[i].stopped = 0;
        }
        break;
      }
    }
  }
}

/*======================================================
  HISTORY & ALIAS ENGINE
======================================================*/

void init_history() {
  char *home = getenv("HOME");
  if (!home)
    return;
  snprintf(history_file, sizeof(history_file), "%s/.myshell_history", home);
  FILE *f = fopen(history_file, "r");
  if (!f)
    return;

  char line[MAX_LINE];
  while (fgets(line, sizeof(line), f)) {
    line[strcspn(line, "\r\n")] = '\0';
    if (!*line)
      continue;
    if (history_count >= MAX_HISTORY) {
      memmove(history[0], history[1], (MAX_HISTORY - 1) * MAX_LINE);
      history_count = MAX_HISTORY - 1;
    }
    snprintf(history[history_count++], MAX_LINE, "%s", line);
  }
  fclose(f);
}

void save_history() {
  if (!history_file[0])
    return;
  FILE *f = fopen(history_file, "w");
  if (!f)
    return;
  for (int i = 0; i < history_count; i++)
    fprintf(f, "%s\n", history[i]);
  fclose(f);
}

void add_history(const char *line) {
  if (!*line || line[0] == ' ' ||
      (history_count && !strcmp(history[history_count - 1], line)))
    return;
  if (history_count < MAX_HISTORY)
    snprintf(history[history_count++], MAX_LINE, "%s", line);
  else {
    memmove(history[0], history[1], (MAX_HISTORY - 1) * MAX_LINE);
    snprintf(history[MAX_HISTORY - 1], MAX_LINE, "%s", line);
  }
  if (history_file[0]) {
    FILE *f = fopen(history_file, "a");
    if (f) {
      fprintf(f, "%s\n", line);
      fclose(f);
    }
  }
}

void get_suggestion(const char *in, char *out) {
  out[0] = '\0';
  size_t len = strlen(in);
  if (!len)
    return;
  for (int i = history_count - 1; i >= 0; i--) {
    if (!strncmp(history[i], in, len) && strlen(history[i]) > len) {
      snprintf(out, MAX_LINE, "%s", history[i] + len);
      return;
    }
  }
}

void get_git_branch(char *buf, size_t max_size) {
  buf[0] = '\0';
  char path[1024];
  if (getcwd(path, sizeof(path)) == NULL)
    return;
  while (1) {
    char head_path[2048];
    snprintf(head_path, sizeof(head_path), "%s/.git/HEAD", path);
    FILE *f = fopen(head_path, "r");
    if (f) {
      char line[256];
      if (fgets(line, sizeof(line), f)) {
        char *ref = strstr(line, "ref: refs/heads/");
        if (ref) {
          snprintf(buf, max_size, "%s", ref + 16);
          buf[strcspn(buf, "\r\n")] = '\0';
        } else
          snprintf(buf, max_size, "%.7s", line);
      }
      fclose(f);
      return;
    }
    char *last_slash = strrchr(path, '/');
    if (!last_slash || last_slash == path)
      break;
    *last_slash = '\0';
  }
}

void set_alias(const char *name, const char *val) {
  for (int i = 0; i < alias_count; i++) {
    if (!strcmp(aliases[i].name, name)) {
      snprintf(aliases[i].val, MAX_LINE, "%s", val);
      return;
    }
  }
  if (alias_count < MAX_ALIASES) {
    snprintf(aliases[alias_count].name, 64, "%s", name);
    snprintf(aliases[alias_count++].val, MAX_LINE, "%s", val);
  }
}

void remove_alias(const char *name) {
  for (int i = 0; i < alias_count; i++) {
    if (!strcmp(aliases[i].name, name)) {
      for (int j = i; j < alias_count - 1; j++)
        aliases[j] = aliases[j + 1];
      alias_count--;
      return;
    }
  }
}

void init_default_aliases() {
  set_alias("ls", "ls --color=auto");
  set_alias("ll", "ls -alF --color=auto");
  set_alias("grep", "grep --color=auto");
}

void expand_aliases(char **args) {
  if (!args[0])
    return;
  for (int i = 0; i < alias_count; i++) {
    if (!strcmp(args[0], aliases[i].name)) {
      static char a_buf[MAX_LINE];
      snprintf(a_buf, sizeof(a_buf), "%s", aliases[i].val);
      char *a_toks[MAX_ARGS];
      tokenize(a_buf, a_toks);
      int a_cnt = 0, p_cnt = 0;
      while (a_toks[a_cnt])
        a_cnt++;
      while (args[p_cnt])
        p_cnt++;
      if (p_cnt + a_cnt >= MAX_ARGS)
        break;
      for (int x = p_cnt; x >= 1; x--)
        args[x + a_cnt - 1] = args[x];
      for (int x = 0; x < a_cnt; x++)
        args[x] = a_toks[x];
      break;
    }
  }
}

/*======================================================
  TERMINAL RENDERING & UI ENGINE
======================================================*/

void disable_raw_mode() { tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios); }
void enable_raw_mode() {
  struct termios raw = orig_termios;
  raw.c_lflag &= ~(ECHO | ICANON | ISIG);
  raw.c_cc[VMIN] = 1;
  raw.c_cc[VTIME] = 0;
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

int cmp_comp(const void *a, const void *b) {
  return strcmp(((Completion *)a)->display, ((Completion *)b)->display);
}

int get_visible_len(const char *str) {
  int len = 0, in_esc = 0;
  for (int i = 0; str[i]; i++) {
    if (str[i] == '\033') {
      in_esc = 1;
      continue;
    }
    if (in_esc) {
      if (isalpha((unsigned char)str[i]))
        in_esc = 0;
      continue;
    }
    if ((unsigned char)str[i] >= 32 && (((unsigned char)str[i] & 0xC0) != 0x80))
      len++;
  }
  return len;
}

int is_builtin(const char *cmd) {
  if (!cmd)
    return 0;
  const char *blt[] = {
      "cd",    "exit",    "clear", "history", "export", "source",     ".",
      "alias", "unalias", "echo",  "pwd",     "pushd",  "popd",       "dirs",
      "jobs",  "fg",      "bg",    "true",    "false",  "deactivate", NULL};
  for (int i = 0; blt[i]; i++)
    if (!strcmp(cmd, blt[i]))
      return 1;
  return 0;
}

int is_command_valid(const char *cmd) {
  if (!cmd || !*cmd)
    return 0;
  if (is_builtin(cmd))
    return 1;
  for (int i = 0; i < alias_count; i++)
    if (!strcmp(cmd, aliases[i].name))
      return 1;
  if (strchr(cmd, '/'))
    return !access(cmd, X_OK);

  static char cache_cmd[1024] = "";
  static int cache_res = 0;
  static char *cache_path = NULL;
  char *path = getenv("PATH");
  if (!path)
    path = "";

  /* Dynamic Cache Invalidation ensures Syntax Highlighting respects VENV
   * changes instantly */
  if (cache_path && !strcmp(cmd, cache_cmd) && !strcmp(path, cache_path))
    return cache_res;

  snprintf(cache_cmd, sizeof(cache_cmd), "%s", cmd);
  if (cache_path)
    free(cache_path);
  cache_path = strdup(path);

  char *p_copy = strdup(path);
  if (!p_copy)
    return 0;
  for (char *dir = strtok(p_copy, ":"); dir; dir = strtok(NULL, ":")) {
    char fp[1024];
    snprintf(fp, sizeof(fp), "%s/%s", dir, cmd);
    if (!access(fp, X_OK)) {
      free(p_copy);
      return (cache_res = 1);
    }
  }
  free(p_copy);
  return (cache_res = 0);
}

void print_highlighted(const char *line, int len) {
  int in_sq = 0, in_dq = 0, word_start = 1, is_cmd = 1;
  const char *color = COLOR_WHITE;

  for (int i = 0; i < len; i++) {
    char c = line[i];
    if (c == '\'') {
      in_sq = !in_sq;
      printf("%s%c", COLOR_YELLOW, c);
      word_start = 0;
      continue;
    }
    if (c == '"') {
      in_dq = !in_dq;
      printf("%s%c", COLOR_YELLOW, c);
      word_start = 0;
      continue;
    }
    if (in_sq || in_dq) {
      if (in_dq && c == '$')
        printf("%s%c%s", COLOR_MAGENTA, c, COLOR_YELLOW);
      else
        printf("%s%c", COLOR_YELLOW, c);
      continue;
    }
    if (c == '\n') {
      printf("\r\n%s> %s", COLOR_GRAY, COLOR_RESET);
      word_start = 1;
      is_cmd = 1;
      continue;
    }
    if (c == ' ' || c == '\t') {
      printf("%s%c", COLOR_RESET, c);
      word_start = 1;
      continue;
    }
    if (strchr("|;<>&", c)) {
      printf("%s%c", COLOR_RED, c);
      word_start = 1;
      is_cmd = 1;
      continue;
    }
    if (c == '=') {
      printf("%s%c", COLOR_MAGENTA, c);
      word_start = 1;
      continue;
    }

    if (word_start) {
      if (is_cmd) {
        char cur_word[1024] = "";
        int cw_len = 0;
        for (int j = i; j < len; j++) {
          if (line[j] == ' ' || line[j] == '\t' || line[j] == '\n' ||
              strchr("|;<>&'\"=", line[j]))
            break;
          if (cw_len < 1023)
            cur_word[cw_len++] = line[j];
        }
        cur_word[cw_len] = '\0';
        color = is_command_valid(cur_word) ? COLOR_GREEN : ERR_COLOR;
        is_cmd = 0;
      } else if (c == '-')
        color = COLOR_CYAN;
      else if (c == '$')
        color = COLOR_MAGENTA;
      else
        color = COLOR_WHITE;
      word_start = 0;
    }
    printf("%s%c", color, c);
  }
  printf("%s", COLOR_RESET);
}

int read_input(char *line) {
  int len = 0, pos = 0, tab_mode = 0, match_count = 0, match_idx = 0;
  int last_cursor_row = 0, history_pos = history_count, word_pos = 0;
  char suggestion[MAX_LINE] = "", cwd[1024], cur_in[MAX_LINE] = "",
       saved_tail[MAX_LINE] = "";
  static Completion matches[MAX_MATCHES];

  char host[256] = "local";
  gethostname(host, sizeof(host));
  char *user = getenv("USER");
  if (!user)
    user = "anon";

  char *venv = getenv("VIRTUAL_ENV");
  char venv_str[256] = "";
  if (venv) {
    char *vname = strrchr(venv, '/');
    snprintf(venv_str, sizeof(venv_str), "%s(%s) ", COLOR_CYAN,
             vname ? vname + 1 : venv);
  }

  if (getcwd(cwd, sizeof(cwd)) == NULL)
    strcpy(cwd, "?");
  char *home = getenv("HOME"), d_cwd[1024];
  if (home && !strncmp(cwd, home, strlen(home)))
    snprintf(d_cwd, sizeof(d_cwd), "~%s", cwd + strlen(home));
  else
    strcpy(d_cwd, cwd);

  char git_branch[64] = "", git_prompt[128] = "";
  get_git_branch(git_branch, sizeof(git_branch));
  if (git_branch[0])
    snprintf(git_prompt, sizeof(git_prompt), " %sgit:(%s%s%s)", COLOR_GRAY,
             COLOR_RED, git_branch, COLOR_GRAY);

  char time_str[32] = "";
  if (last_duration > 0) {
    if (last_duration > 59)
      snprintf(time_str, sizeof(time_str), "%s[%dm %ds] ", COLOR_YELLOW,
               last_duration / 60, last_duration % 60);
    else
      snprintf(time_str, sizeof(time_str), "%s[%ds] ", COLOR_YELLOW,
               last_duration);
  }
  const char *status_color = (last_status == 0) ? PROMPT_COLOR : ERR_COLOR;

  char prompt[2048];
  snprintf(prompt, sizeof(prompt), "%s%s%s%s@%s%s:%s%s%s %s❯%s ", venv_str,
           time_str, COLOR_CYAN, user, host, COLOR_RESET, DIR_COLOR, d_cwd,
           git_prompt, status_color, COLOR_RESET);
  int prompt_len = get_visible_len(prompt);

  line[0] = '\0';
  enable_raw_mode();
  int reading = 1, first_draw = 1;

  while (reading) {
    if (window_resized) {
      window_resized = 0;
      first_draw = 1;
    }
    struct winsize w;
    int t_cols = 80;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) != -1 && w.ws_col > 0)
      t_cols = w.ws_col;

    printf("\033[?25l"); /* Hide cursor natively */

    if (!first_draw) {
      if (last_cursor_row > 0)
        printf("\033[%dA", last_cursor_row);
      printf("\r\033[J");
    } else {
      printf("\r\033[2K");
      first_draw = 0;
    }

    /* Rendering Engine Setup */
    printf("%s", prompt);
    print_highlighted(line, len);

    int g_len = 0;
    if (!tab_mode && pos == len) {
      get_suggestion(line, suggestion);
      if (suggestion[0]) {
        printf("%s%s%s", SUGG_COLOR, suggestion, COLOR_RESET);
        g_len = get_visible_len(suggestion);
      }
    } else
      suggestion[0] = '\0';

    /* Exact Terminal Row/Col Geometry Tracker */
    int target_row = 0, target_col = 0, r = prompt_len / t_cols,
        c = prompt_len % t_cols;
    for (int i = 0; i <= len; i++) {
      if (i == pos) {
        target_row = r;
        target_col = c;
      }
      if (i < len) {
        if (line[i] == '\n') {
          r++;
          c = 0;
        } else if (((unsigned char)line[i] & 0xC0) != 0x80) {
          c++;
          if (c >= t_cols) {
            r++;
            c = 0;
          }
        }
      }
    }
    for (int i = 0; i < g_len; i++) {
      c++;
      if (c >= t_cols) {
        r++;
        c = 0;
      }
    }
    int total_rows = r, menu_lines = 0;

    /* Draw Tab Autocompletion Grid Menu */
    if (tab_mode && match_count > 0) {
      printf("\r\n\033[K");
      menu_lines++;
      int max_len = 0;
      for (int i = 0; i < match_count; i++) {
        int l = strlen(matches[i].display) + matches[i].is_dir;
        if (l > max_len)
          max_len = l;
      }
      int col_w = max_len + 2, num_cols = t_cols / col_w;
      if (!num_cols)
        num_cols = 1;
      int m_show = num_cols * 5, s_idx = (match_idx / m_show) * m_show,
          e_idx = (s_idx + m_show > match_count) ? match_count : s_idx + m_show;

      for (int i = s_idx; i < e_idx; i++) {
        if ((i - s_idx) % num_cols == 0 && i != s_idx) {
          printf("\r\n\033[K");
          menu_lines++;
        }
        if (i == match_idx)
          printf("\033[7m");
        printf("%s%s%s",
               matches[i].is_dir
                   ? DIR_COLOR
                   : (matches[i].is_exec ? COLOR_GREEN : COLOR_WHITE),
               matches[i].display, matches[i].is_dir ? "/" : "");
        for (int p = 0;
             p < col_w - (int)strlen(matches[i].display) - matches[i].is_dir &&
             (i + 1 - s_idx) % num_cols != 0;
             p++)
          printf(" ");
        printf("\033[0m");
      }
      if (match_count > e_idx) {
        printf("\r\n\033[K %s... %d more%s", SUGG_COLOR, match_count - e_idx,
               COLOR_RESET);
        menu_lines++;
      }
    }

    /* Target Re-Snap */
    int current_row = total_rows + menu_lines, go_up = current_row - target_row;
    if (go_up > 0)
      printf("\033[%dA", go_up);
    printf("\r");
    if (target_col > 0)
      printf("\033[%dC", target_col);

    printf("\033[?25h");
    fflush(stdout);
    last_cursor_row = target_row;

    char c_in;
    ssize_t nread = read(STDIN_FILENO, &c_in, 1);
    if (nread < 0 && errno == EINTR)
      continue;
    if (nread <= 0) {
      disable_raw_mode();
      return 0;
    }

    switch (c_in) {
    case 10:
    case 13: {
      int sq = 0, dq = 0;
      for (int i = 0; i < len; i++) {
        if (line[i] == '\'' && !dq)
          sq = !sq;
        else if (line[i] == '"' && !sq)
          dq = !dq;
      }
      if (sq || dq || (len > 0 && line[len - 1] == '\\')) {
        if (len > 0 && line[len - 1] == '\\') {
          pos--;
          len--;
        }
        if (len < MAX_LINE - 1) {
          memmove(&line[pos + 1], &line[pos], len - pos + 1);
          line[pos] = '\n';
          pos++;
          len++;
          history_pos = history_count;
        }
      } else {
        int down = total_rows - target_row;
        if (down > 0)
          printf("\033[%dB", down);
        printf("\r\n\033[J");
        reading = 0;
      }
      break;
    }
    case 1: /* Ctrl+A (Home) */
      pos = 0;
      tab_mode = 0;
      break;
    case 5: /* Ctrl+E (End) */
      pos = len;
      tab_mode = 0;
      break;
    case 3: /* Ctrl+C */
      printf("\r\n\033[J^C\n");
      pos = len = 0;
      line[0] = '\0';
      last_status = 130;
      reading = 0;
      break;
    case 4: /* Ctrl+D */
      if (!len) {
        disable_raw_mode();
        printf("\n");
        exit(0);
      }
      if (pos < len) {
        memmove(&line[pos], &line[pos + 1], len - pos);
        len--;
      }
      break;
    case 12: /* Ctrl+L */
      printf("\033[H\033[2J");
      last_cursor_row = 0;
      first_draw = 1;
      break;
    case 11: /* Ctrl+K */
      line[pos] = '\0';
      len = pos;
      tab_mode = 0;
      break;
    case 21: /* Ctrl+U */
      memmove(&line[0], &line[pos], len - pos + 1);
      len -= pos;
      pos = 0;
      tab_mode = 0;
      history_pos = history_count;
      break;
    case 23: /* Ctrl+W */
      if (pos > 0) {
        int s = pos - 1;
        while (s > 0 && line[s] == ' ')
          s--;
        while (s > 0 && line[s - 1] != ' ' && !strchr("/-=_", line[s - 1]))
          s--;
        memmove(&line[s], &line[pos], len - pos + 1);
        len -= (pos - s);
        pos = s;
      }
      tab_mode = 0;
      history_pos = history_count;
      break;
    case 8:
    case 127: /* Backspace */
      if (pos > 0) {
        memmove(&line[pos - 1], &line[pos], len - pos + 1);
        pos--;
        len--;
      }
      tab_mode = 0;
      history_pos = history_count;
      break;

    case 27: {
      char seq[8];
      struct termios t_raw = orig_termios;
      t_raw.c_lflag &= ~(ECHO | ICANON | ISIG);
      t_raw.c_cc[VMIN] = 0;
      t_raw.c_cc[VTIME] = 1;
      tcsetattr(STDIN_FILENO, TCSANOW, &t_raw);
      if (read(STDIN_FILENO, &seq[0], 1) == 1) {
        if (seq[0] == '[') {
          if (read(STDIN_FILENO, &seq[1], 1) == 1) {
            if (seq[1] == 'A') { /* Up */
              if (history_pos == history_count)
                snprintf(cur_in, MAX_LINE, "%s", line);
              while (history_pos > 0) {
                history_pos--;
                if (!strncmp(history[history_pos], cur_in, strlen(cur_in))) {
                  snprintf(line, MAX_LINE, "%s", history[history_pos]);
                  pos = len = strlen(line);
                  break;
                }
              }
            } else if (seq[1] == 'B') { /* Down */
              while (history_pos < history_count) {
                history_pos++;
                if (history_pos == history_count) {
                  snprintf(line, MAX_LINE, "%s", cur_in);
                  pos = len = strlen(line);
                  break;
                }
                if (!strncmp(history[history_pos], cur_in, strlen(cur_in))) {
                  snprintf(line, MAX_LINE, "%s", history[history_pos]);
                  pos = len = strlen(line);
                  break;
                }
              }
            } else if (seq[1] == 'C') { /* Right */
              if (pos < len)
                pos++;
              else if (suggestion[0]) {
                snprintf(line + len, MAX_LINE - len, "%s", suggestion);
                pos = len = strlen(line);
                suggestion[0] = '\0';
              }
            } else if (seq[1] == 'D') {
              if (pos > 0)
                pos--;
            } else if (seq[1] == 'H')
              pos = 0;
            else if (seq[1] == 'F')
              pos = len;
            else if (seq[1] == '3' && read(STDIN_FILENO, &seq[2], 1) == 1 &&
                     seq[2] == '~' && pos < len) {
              memmove(&line[pos], &line[pos + 1], len - pos);
              len--;
            } else if (seq[1] == '1' && read(STDIN_FILENO, &seq[2], 1) == 1 &&
                       seq[2] == ';') {
              if (read(STDIN_FILENO, &seq[3], 1) == 1 &&
                  read(STDIN_FILENO, &seq[4], 1) == 1) {
                if (seq[3] == '5' && seq[4] == 'C') {
                  while (pos < len && line[pos] != ' ')
                    pos++;
                  while (pos < len && line[pos] == ' ')
                    pos++;
                } else if (seq[3] == '5' && seq[4] == 'D') {
                  while (pos > 0 && line[pos - 1] == ' ')
                    pos--;
                  while (pos > 0 && line[pos - 1] != ' ')
                    pos--;
                }
              }
            }
          }
        } else if (seq[0] == 'O') {
          if (read(STDIN_FILENO, &seq[1], 1) == 1) {
            if (seq[1] == 'H')
              pos = 0;
            else if (seq[1] == 'F')
              pos = len;
          }
        } else if (seq[0] == 'b') { /* Alt+B */
          while (pos > 0 && line[pos - 1] == ' ')
            pos--;
          while (pos > 0 && line[pos - 1] != ' ')
            pos--;
        } else if (seq[0] == 'f') { /* Alt+F */
          while (pos < len && line[pos] != ' ')
            pos++;
          while (pos < len && line[pos] == ' ')
            pos++;
        }
      }
      tcsetattr(STDIN_FILENO, TCSANOW, &t_raw);
      enable_raw_mode();
      tab_mode = 0;
      break;
    }

    case 9: { /* Space Escaped Autocompletion */
      if (!tab_mode) {
        match_count = match_idx = 0;
        word_pos = pos;
        while (word_pos > 0 && line[word_pos - 1] != ' ' &&
               line[word_pos - 1] != '"' && line[word_pos - 1] != '\n')
          word_pos--;
        char w[MAX_LINE] = "";
        if (pos > word_pos)
          snprintf(w, sizeof(w), "%.*s", pos - word_pos, &line[word_pos]);
        snprintf(saved_tail, sizeof(saved_tail), "%s", &line[pos]);

        int is_first = 1;
        for (int i = word_pos - 1; i >= 0; i--) {
          if (strchr("|&;(\n", line[i]))
            break;
          if (line[i] != ' ' && line[i] != '"' && line[i] != '\'') {
            is_first = 0;
            break;
          }
        }

        if (w[0] == '$') {
          int pref_len = strlen(w) - 1;
          for (char **env = environ; *env && match_count < MAX_MATCHES; env++) {
            char *eq = strchr(*env, '=');
            if (eq) {
              int len_var = eq - *env;
              if (pref_len == 0 || !strncmp(*env, w + 1, pref_len)) {
                char var_name[1024];
                snprintf(var_name, len_var + 2, "$%s", *env);
                int dup = 0;
                for (int k = 0; k < match_count; k++)
                  if (!strcmp(matches[k].display, var_name))
                    dup = 1;
                if (!dup) {
                  snprintf(matches[match_count].insert, 1024, "%s", var_name);
                  snprintf(matches[match_count].display, 1024, "%s", var_name);
                  matches[match_count].is_exec = 0;
                  matches[match_count].is_dir = 0;
                  match_count++;
                }
              }
            }
          }
        } else if (is_first && !strchr(w, '/') && strlen(w) > 0) {
          const char *blt[] = {
              "cd",    "exit",       "clear",   "history", "export", "source",
              ".",     "alias",      "unalias", "echo",    "pwd",    "pushd",
              "popd",  "dirs",       "jobs",    "fg",      "bg",     "true",
              "false", "deactivate", NULL};
          for (int i = 0; blt[i]; i++)
            if (!strncmp(blt[i], w, strlen(w))) {
              snprintf(matches[match_count].insert, 1024, "%s", blt[i]);
              snprintf(matches[match_count].display, 1024, "%s", blt[i]);
              matches[match_count].is_exec = 1;
              matches[match_count].is_dir = 0;
              match_count++;
            }

          for (int i = 0; i < alias_count; i++)
            if (!strncmp(aliases[i].name, w, strlen(w))) {
              snprintf(matches[match_count].insert, 1024, "%s",
                       aliases[i].name);
              snprintf(matches[match_count].display, 1024, "%s",
                       aliases[i].name);
              matches[match_count].is_exec = 1;
              matches[match_count].is_dir = 0;
              match_count++;
            }

          char *p_env = getenv("PATH");
          if (p_env) {
            char *p_copy = strdup(p_env);
            if (p_copy) {
              for (char *dir_t = strtok(p_copy, ":");
                   dir_t && match_count < MAX_MATCHES;
                   dir_t = strtok(NULL, ":")) {
                DIR *dir = opendir(dir_t);
                if (!dir)
                  continue;
                struct dirent *ent;
                while ((ent = readdir(dir)) && match_count < MAX_MATCHES) {
                  if (!strncmp(ent->d_name, w, strlen(w)) &&
                      strcmp(ent->d_name, ".") && strcmp(ent->d_name, "..")) {
                    int dup = 0;
                    for (int k = 0; k < match_count; k++)
                      if (!strcmp(matches[k].display, ent->d_name))
                        dup = 1;
                    if (!dup) {
                      char fp[2048];
                      snprintf(fp, sizeof(fp), "%s/%s", dir_t, ent->d_name);
                      struct stat st;
                      if (stat(fp, &st) == 0 && S_ISREG(st.st_mode) &&
                          !access(fp, X_OK)) {
                        snprintf(matches[match_count].insert, 1024, "%s",
                                 ent->d_name);
                        snprintf(matches[match_count].display, 1024, "%s",
                                 ent->d_name);
                        matches[match_count].is_exec = 1;
                        matches[match_count].is_dir = 0;
                        match_count++;
                      }
                    }
                  }
                }
                closedir(dir);
              }
              free(p_copy);
            }
          }
        } else {
          char s_dir[MAX_LINE] = ".", pref[MAX_LINE] = "",
               d_path[MAX_LINE] = "";
          char *slash = strrchr(w, '/');
          if (slash) {
            snprintf(d_path, sizeof(d_path), "%.*s", (int)(slash - w + 1), w);
            snprintf(pref, sizeof(pref), "%s", slash + 1);
            if (slash > w)
              snprintf(s_dir, sizeof(s_dir), "%.*s", (int)(slash - w), w);
            else
              snprintf(s_dir, sizeof(s_dir), "/");
          } else
            snprintf(pref, sizeof(pref), "%s", w);

          if (s_dir[0] == '~') {
            char *h = getenv("HOME");
            if (h) {
              char t[MAX_LINE];
              snprintf(t, MAX_LINE, "%s%s", h, s_dir + 1);
              snprintf(s_dir, sizeof(s_dir), "%s", t);
            }
          }
          DIR *dir = opendir(s_dir);
          if (dir) {
            struct dirent *ent;
            while ((ent = readdir(dir)) && match_count < MAX_MATCHES) {
              if (ent->d_name[0] == '.' && pref[0] != '.')
                continue;
              if (!strncmp(ent->d_name, pref, strlen(pref)) &&
                  strcmp(ent->d_name, ".") && strcmp(ent->d_name, "..")) {
                char f_path[MAX_LINE];
                snprintf(f_path, MAX_LINE, "%s/%s",
                         strcmp(s_dir, "/") ? s_dir : "", ent->d_name);
                struct stat st;
                int is_d = (stat(f_path, &st) == 0 && S_ISDIR(st.st_mode));
                char *src = ent->d_name, *dst = matches[match_count].insert;
                snprintf(dst, 1024, "%s", d_path);
                dst += strlen(d_path);
                while (*src && (dst - matches[match_count].insert) < 1022) {
                  if (*src == ' ' || *src == '(' || *src == ')')
                    *dst++ = '\\';
                  *dst++ = *src++;
                }
                *dst = '\0';
                snprintf(matches[match_count].display, 1024, "%s", ent->d_name);
                matches[match_count].is_dir = is_d;
                matches[match_count].is_exec = (!is_d && !access(f_path, X_OK));
                match_count++;
              }
            }
            closedir(dir);
          }
        }
        if (match_count > 0) {
          qsort(matches, match_count, sizeof(Completion), cmp_comp);
          tab_mode = match_count > 1;
          match_idx = -1;
          if (match_count == 1) {
            snprintf(&line[word_pos], MAX_LINE - word_pos, "%s%s%s",
                     matches[0].insert, matches[0].is_dir ? "/" : " ",
                     saved_tail);
            pos = len = strlen(line) - strlen(saved_tail);
            tab_mode = 0;
          } else {
            char lcp[1024];
            snprintf(lcp, sizeof(lcp), "%s", matches[0].insert);
            for (int i = 1; i < match_count; i++) {
              int j = 0;
              while (lcp[j] && matches[i].insert[j] &&
                     lcp[j] == matches[i].insert[j])
                j++;
              lcp[j] = '\0';
            }
            snprintf(&line[word_pos], MAX_LINE - word_pos, "%s%s", lcp,
                     saved_tail);
            pos = word_pos + strlen(lcp);
            len = strlen(line);
          }
        } else if (suggestion[0] && pos == len) {
          snprintf(line + len, MAX_LINE - len, "%s", suggestion);
          pos = len = strlen(line);
          suggestion[0] = '\0';
        }
      } else if (match_count > 0) {
        match_idx = (match_idx + 1) % match_count;
        snprintf(&line[word_pos], MAX_LINE - word_pos, "%s%s%s",
                 matches[match_idx].insert,
                 matches[match_idx].is_dir ? "/" : "", saved_tail);
        pos = word_pos + strlen(matches[match_idx].insert) +
              matches[match_idx].is_dir;
        len = strlen(line);
      }
      break;
    }
    default:
      if (isprint(c_in) && len < MAX_LINE - 1) {
        memmove(&line[pos + 1], &line[pos], len - pos + 1);
        line[pos++] = c_in;
        len++;
        history_pos = history_count;
      }
      tab_mode = 0;
      break;
    }
  }
  disable_raw_mode();
  return 1;
}

/*======================================================
  PARSER & EXECUTION ENGINE
======================================================*/

void capture_output(const char *cmd, char *out, size_t max_len) {
  out[0] = '\0';
  int fd[2];
  if (pipe(fd) < 0)
    return;
  pid_t pid = fork();
  if (pid == 0) {
    shell_is_interactive = 0;
    close(fd[0]);
    dup2(fd[1], STDOUT_FILENO);
    close(fd[1]);
    char cmd_copy[MAX_LINE];
    snprintf(cmd_copy, sizeof(cmd_copy), "%s", cmd);
    process_command(cmd_copy);
    exit(0);
  }
  close(fd[1]);
  int pos = 0;
  char buf[256];
  ssize_t n;
  while ((n = read(fd[0], buf, sizeof(buf))) > 0 ||
         (n == -1 && errno == EINTR)) {
    if (n == -1 && errno == EINTR)
      continue;
    for (ssize_t i = 0; i < n && pos < (int)max_len - 1; i++)
      out[pos++] = (buf[i] == '\n') ? ' ' : buf[i];
  }
  out[pos] = '\0';
  while (pos > 0 && out[pos - 1] == ' ')
    out[--pos] = '\0';
  close(fd[0]);
  pid_t res;
  do {
    res = waitpid(pid, NULL, 0);
  } while (res == -1 && errno == EINTR);
}

void expand_substitutions(char *line) {
  char *temp = malloc(MAX_LINE * 4);
  if (!temp)
    return;
  temp[0] = '\0';
  char *r = line, *w = temp;
  int in_sq = 0;
  while (*r && (w - temp) < (MAX_LINE * 4) - 1) {
    if (*r == '\'') {
      in_sq = !in_sq;
      *w++ = *r++;
      continue;
    }
    if (!in_sq && *r == '$' && *(r + 1) == '(') {
      r += 2;
      char cmd[MAX_LINE] = "";
      int c_idx = 0, depth = 1;
      while (*r) {
        if (*r == '$' && *(r + 1) == '(') {
          depth++;
          if (c_idx < MAX_LINE - 2) {
            cmd[c_idx++] = *r++;
            cmd[c_idx++] = *r;
          }
        } else if (*r == ')') {
          depth--;
          if (depth == 0) {
            r++;
            break;
          }
        } else {
          if (depth > 0 && c_idx < MAX_LINE - 1)
            cmd[c_idx++] = *r;
        }
        if (depth > 0 && *r != '\0')
          r++;
        if (*r == '\0')
          break;
      }
      cmd[c_idx] = '\0';
      char out[MAX_LINE];
      capture_output(cmd, out, sizeof(out));
      for (char *s = out; *s && (w - temp) < (MAX_LINE * 4) - 1; s++)
        *w++ = *s;
    } else if (!in_sq && *r == '`') {
      r++;
      char cmd[MAX_LINE] = "";
      int c_idx = 0;
      while (*r && *r != '`') {
        if (c_idx < MAX_LINE - 1)
          cmd[c_idx++] = *r;
        r++;
      }
      if (*r == '`')
        r++;
      cmd[c_idx] = '\0';
      char out[MAX_LINE];
      capture_output(cmd, out, sizeof(out));
      for (char *s = out; *s && (w - temp) < (MAX_LINE * 4) - 1; s++)
        *w++ = *s;
    } else
      *w++ = *r++;
  }
  *w = '\0';
  snprintf(line, MAX_LINE * 4, "%s", temp);
  free(temp);
}

void pad_operators(const char *in, char *out) {
  int in_sq = 0, in_dq = 0;
  while (*in) {
    if (*in == '\'' && !in_dq)
      in_sq = !in_sq;
    else if (*in == '"' && !in_sq)
      in_dq = !in_dq;
    if (!in_sq && !in_dq) {
      if (!strncmp(in, "2>&1", 4)) {
        *out++ = ' ';
        *out++ = '2';
        *out++ = '>';
        *out++ = '&';
        *out++ = '1';
        *out++ = ' ';
        in += 4;
        continue;
      }
      if (!strncmp(in, "2>>", 3)) {
        *out++ = ' ';
        *out++ = '2';
        *out++ = '>';
        *out++ = '>';
        *out++ = ' ';
        in += 3;
        continue;
      }
      if (!strncmp(in, "&>", 2) || !strncmp(in, ">>", 2) ||
          !strncmp(in, "2>", 2) || !strncmp(in, "&&", 2) ||
          !strncmp(in, "||", 2)) {
        *out++ = ' ';
        *out++ = in[0];
        *out++ = in[1];
        *out++ = ' ';
        in += 2;
        continue;
      }
      if (strchr("|;<>&", *in)) {
        *out++ = ' ';
        *out++ = *in;
        *out++ = ' ';
        in++;
        continue;
      }
    }
    *out++ = *in++;
  }
  *out = '\0';
}

void tokenize(char *line, char **args) {
  int count = 0, sq = 0, dq = 0;
  char *p = line;
  while (*p) {
    while (isspace((unsigned char)*p))
      p++;
    if (!*p || (*p == '#' && !sq && !dq))
      break;
    args[count++] = p;
    if (count >= MAX_ARGS - 1)
      break;
    while (*p) {
      if (*p == '\'')
        sq = !sq;
      else if (*p == '"')
        dq = !dq;
      else if (*p == '\\' && *(p + 1) && !sq && !dq)
        p++;
      else if (isspace((unsigned char)*p) && !sq && !dq) {
        *p = '\0';
        p++;
        break;
      }
      p++;
    }
  }
  args[count] = NULL;
}

void expand_args(char **args, char **new_args, char buf[][MAX_LINE]) {
  int n = 0;
  for (int i = 0; args[i] && n < MAX_ARGS - 1; i++) {
    char temp[MAX_LINE];
    int has_quote = (strchr(args[i], '"') || strchr(args[i], '\''));
    char *r = args[i], *w = temp;
    int sq = 0, dq = 0;
    while (*r && (w - temp < MAX_LINE - 1)) {
      if (*r == '\\' && *(r + 1) && !sq && !dq) {
        r++;
        *w++ = *r++;
      } else if (*r == '\\' && dq &&
                 (*(r + 1) == '"' || *(r + 1) == '$' || *(r + 1) == '\\')) {
        r++;
        *w++ = *r++;
      } else if (*r == '\'' && !dq) {
        sq = !sq;
        r++;
      } else if (*r == '"' && !sq) {
        dq = !dq;
        r++;
      } else if (*r == '$' && !sq) {
        r++;
        if (*r == '?') {
          char st[16];
          snprintf(st, 16, "%d", last_status);
          for (char *s = st; *s && w - temp < MAX_LINE - 1; s++)
            *w++ = *s;
          r++;
        } else {
          int braced = (*r == '{');
          if (braced)
            r++;
          char var[64];
          int v = 0;
          while ((isalnum((unsigned char)*r) || *r == '_') && v < 63)
            var[v++] = *r++;
          var[v] = '\0';
          if (braced && *r == '}')
            r++;
          char *val = getenv(var);
          if (val)
            for (char *s = val; *s && w - temp < MAX_LINE - 1; s++)
              *w++ = *s;
        }
      } else
        *w++ = *r++;
    }
    *w = '\0';

    for (int j = 0; temp[j]; j++) {
      if (temp[j] == '~' &&
          (j == 0 || temp[j - 1] == '=' || temp[j - 1] == ':')) {
        if (!temp[j + 1] || temp[j + 1] == '/' || temp[j + 1] == ':') {
          char *h = getenv("HOME");
          if (h) {
            char t2[MAX_LINE];
            snprintf(t2, sizeof(t2), "%.*s%s%s", j, temp, h, temp + j + 1);
            snprintf(temp, MAX_LINE, "%s", t2);
            j += strlen(h) - 1;
          }
        }
      }
    }
    if (!has_quote && strpbrk(temp, "*?")) {
      glob_t g;
      if (glob(temp, GLOB_NOCHECK, NULL, &g) == 0) {
        for (size_t j = 0; j < g.gl_pathc && n < MAX_ARGS - 1; j++) {
          snprintf(buf[n], MAX_LINE, "%s", g.gl_pathv[j]);
          new_args[n] = buf[n];
          n++;
        }
      }
      globfree(&g);
    } else {
      snprintf(buf[n], MAX_LINE, "%s", temp);
      new_args[n] = buf[n];
      n++;
    }
  }
  new_args[n] = NULL;
}

void handle_redirections(char **args) {
  int j = 0;
  for (int i = 0; args[i]; i++) {
    if (!strcmp(args[i], ">") && args[i + 1]) {
      int f = open(args[i + 1], O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (f >= 0) {
        dup2(f, STDOUT_FILENO);
        close(f);
      }
      i++;
    } else if (!strcmp(args[i], ">>") && args[i + 1]) {
      int f = open(args[i + 1], O_WRONLY | O_CREAT | O_APPEND, 0644);
      if (f >= 0) {
        dup2(f, STDOUT_FILENO);
        close(f);
      }
      i++;
    } else if (!strcmp(args[i], "2>") && args[i + 1]) {
      int f = open(args[i + 1], O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (f >= 0) {
        dup2(f, STDERR_FILENO);
        close(f);
      }
      i++;
    } else if (!strcmp(args[i], "2>>") && args[i + 1]) {
      int f = open(args[i + 1], O_WRONLY | O_CREAT | O_APPEND, 0644);
      if (f >= 0) {
        dup2(f, STDERR_FILENO);
        close(f);
      }
      i++;
    } else if (!strcmp(args[i], "&>") && args[i + 1]) {
      int f = open(args[i + 1], O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (f >= 0) {
        dup2(f, STDOUT_FILENO);
        dup2(f, STDERR_FILENO);
        close(f);
      }
      i++;
    } else if (!strcmp(args[i], "2>&1")) {
      dup2(STDOUT_FILENO, STDERR_FILENO);
    } else if (!strcmp(args[i], "<") && args[i + 1]) {
      int f = open(args[i + 1], O_RDONLY);
      if (f >= 0) {
        dup2(f, STDIN_FILENO);
        close(f);
      } else {
        perror(args[i + 1]);
        exit(1);
      }
      i++;
    } else
      args[j++] = args[i];
  }
  args[j] = NULL;
}

int is_assignment(const char *str) {
  if (!str || (!isalpha((unsigned char)*str) && *str != '_'))
    return 0;
  const char *eq = strchr(str, '=');
  if (!eq)
    return 0;
  for (const char *p = str; p < eq; p++)
    if (!isalnum((unsigned char)*p) && *p != '_')
      return 0;
  return 1;
}

int run_builtin(char **args) {
  if (!args[0])
    return 0;

  /* Native Inline Assignment Engine: VAR=VAL */
  if (is_assignment(args[0])) {
    int i = 0;
    while (args[i] && is_assignment(args[i]))
      i++;
    if (!args[i]) {
      for (int j = 0; j < i; j++) {
        char *eq = strchr(args[j], '=');
        *eq = '\0';
        setenv(args[j], eq + 1, 1);
        *eq = '=';
      }
      last_status = 0;
      return 1;
    }
  }

  if (!strcmp(args[0], "echo")) {
    int newline = 1, i = 1;
    if (args[1] && !strcmp(args[1], "-n")) {
      newline = 0;
      i++;
    }
    for (; args[i]; i++)
      printf("%s%s", args[i], args[i + 1] ? " " : "");
    if (newline)
      printf("\n");
    last_status = 0;
    return 1;
  }
  if (!strcmp(args[0], "pwd")) {
    char cwd[4096];
    if (getcwd(cwd, sizeof(cwd)))
      printf("%s\n", cwd);
    last_status = 0;
    return 1;
  }
  if (!strcmp(args[0], "clear")) {
    printf("\033[H\033[2J");
    last_status = 0;
    return 1;
  }
  if (!strcmp(args[0], "history")) {
    for (int h = 0; h < history_count; h++)
      printf("%d  %s\n", h + 1, history[h]);
    last_status = 0;
    return 1;
  }
  if (!strcmp(args[0], "cd")) {
    char *t = args[1] ? args[1] : getenv("HOME");
    if (args[1] && !strcmp(args[1], "-"))
      t = getenv("OLDPWD") ? getenv("OLDPWD") : (prev_dir[0] ? prev_dir : ".");
    char cur[4096];
    if (getcwd(cur, sizeof(cur)) == NULL)
      cur[0] = '\0';
    if (t && chdir(t) != 0) {
      fprintf(stderr, "%smyshell: cd: %s: No such directory%s\n", ERR_COLOR, t,
              COLOR_RESET);
      last_status = 1;
    } else {
      if (cur[0] != '\0') {
        setenv("OLDPWD", cur, 1);
        snprintf(prev_dir, sizeof(prev_dir), "%s", cur);
      }
      char ncur[4096];
      if (getcwd(ncur, sizeof(ncur)))
        setenv("PWD", ncur, 1);
      if (args[1] && !strcmp(args[1], "-"))
        printf("%s\n", t);
      last_status = 0;
    }
    return 1;
  }
  if (!strcmp(args[0], "pushd")) {
    char cur[4096];
    if (getcwd(cur, sizeof(cur))) {
      char *t = args[1] ? args[1] : getenv("HOME");
      if (chdir(t) == 0) {
        if (dir_stack_count < MAX_DIR_STACK)
          snprintf(dir_stack[dir_stack_count++], 1024, "%s", cur);
        printf("%s %s\n", t, cur);
        setenv("PWD", t, 1);
        setenv("OLDPWD", cur, 1);
        last_status = 0;
      } else {
        fprintf(stderr, "myshell: pushd: %s: No such directory\n", t);
        last_status = 1;
      }
    }
    return 1;
  }
  if (!strcmp(args[0], "popd")) {
    if (dir_stack_count > 0) {
      char *t = dir_stack[--dir_stack_count];
      char cur[4096];
      getcwd(cur, sizeof(cur));
      if (chdir(t) == 0) {
        printf("%s\n", t);
        setenv("PWD", t, 1);
        setenv("OLDPWD", cur, 1);
        last_status = 0;
      } else {
        perror("popd");
        last_status = 1;
      }
    } else {
      fprintf(stderr, "myshell: popd: directory stack empty\n");
      last_status = 1;
    }
    return 1;
  }
  if (!strcmp(args[0], "dirs")) {
    char cur[4096];
    if (getcwd(cur, sizeof(cur)))
      printf("%s ", cur);
    for (int i = dir_stack_count - 1; i >= 0; i--)
      printf("%s ", dir_stack[i]);
    printf("\n");
    last_status = 0;
    return 1;
  }
  if (!strcmp(args[0], "jobs")) {
    for (int i = 0; i < MAX_JOBS; i++)
      if (jobs[i].active)
        printf("[%d] %s  %d  %s\n", jobs[i].id,
               jobs[i].stopped ? "Stopped" : "Running", jobs[i].pid,
               jobs[i].cmd);
    last_status = 0;
    return 1;
  }
  if (!strcmp(args[0], "fg")) {
    int target = -1;
    if (args[1]) {
      int id = atoi(args[1]);
      for (int i = 0; i < MAX_JOBS; i++)
        if (jobs[i].active && jobs[i].id == id)
          target = i;
    } else {
      for (int i = MAX_JOBS - 1; i >= 0; i--)
        if (jobs[i].active) {
          target = i;
          break;
        }
    }
    if (target != -1) {
      if (shell_is_interactive)
        tcsetpgrp(shell_terminal, jobs[target].pid);
      printf("%s\n", jobs[target].cmd);
      kill(-jobs[target].pid, SIGCONT);
      int st;
      waitpid(jobs[target].pid, &st, WUNTRACED);
      if (WIFSTOPPED(st)) {
        jobs[target].stopped = 1;
        last_status = 128 + WSTOPSIG(st);
      } else {
        jobs[target].active = 0;
        last_status = WIFEXITED(st)
                          ? WEXITSTATUS(st)
                          : (WIFSIGNALED(st) ? 128 + WTERMSIG(st) : 127);
      }
      if (shell_is_interactive) {
        tcsetpgrp(shell_terminal, shell_pgid);
        tcsetattr(shell_terminal, TCSADRAIN, &shell_tmodes);
      }
    } else {
      fprintf(stderr, "%smyshell: fg: no such job%s\n", ERR_COLOR, COLOR_RESET);
      last_status = 1;
    }
    return 1;
  }
  if (!strcmp(args[0], "bg")) {
    int target = -1;
    if (args[1]) {
      int id = atoi(args[1]);
      for (int i = 0; i < MAX_JOBS; i++)
        if (jobs[i].active && jobs[i].id == id)
          target = i;
    } else {
      for (int i = MAX_JOBS - 1; i >= 0; i--)
        if (jobs[i].active && jobs[i].stopped) {
          target = i;
          break;
        }
    }
    if (target != -1) {
      jobs[target].stopped = 0;
      kill(-jobs[target].pid, SIGCONT);
      printf("[%d] %s &\n", jobs[target].id, jobs[target].cmd);
      last_status = 0;
    } else {
      fprintf(stderr, "%smyshell: bg: no current job%s\n", ERR_COLOR,
              COLOR_RESET);
      last_status = 1;
    }
    return 1;
  }
  if (!strcmp(args[0], "export")) {
    for (int i = 1; args[i]; i++) {
      char *eq = strchr(args[i], '=');
      if (eq) {
        *eq = '\0';
        setenv(args[i], eq + 1, 1);
      }
    }
    last_status = 0;
    return 1;
  }

  /* --- NATIVE PYTHON VENV INTERCEPTOR --- */
  if (!strcmp(args[0], "deactivate")) {
    char *old_path = getenv("_OLD_VIRTUAL_PATH");
    if (old_path) {
      setenv("PATH", old_path, 1);
      unsetenv("_OLD_VIRTUAL_PATH");
    }
    unsetenv("VIRTUAL_ENV");
    printf("%s[myshell] Deactivated Python VENV.%s\n", COLOR_YELLOW,
           COLOR_RESET);
    last_status = 0;
    return 1;
  }

  if (!strcmp(args[0], "source") || !strcmp(args[0], ".")) {
    if (args[1]) {
      /* Instantly catch VENV activate scripts to bypass unparseable Bash syntax
       */
      if (strstr(args[1], "activate")) {
        char rpath[4096];
        if (realpath(args[1], rpath)) {
          char *bin_slash = strstr(rpath, "/bin/activate");
          if (!bin_slash)
            bin_slash = strstr(rpath, "/Scripts/activate");
          if (bin_slash) {
            *bin_slash = '\0'; /* Truncate to locate absolute VENV root */
            setenv("VIRTUAL_ENV", rpath, 1);
            char *old_path = getenv("PATH");
            if (!getenv("_OLD_VIRTUAL_PATH"))
              setenv("_OLD_VIRTUAL_PATH", old_path ? old_path : "", 1);

            /* Secure dynamic path building to avoid buffer limits */
            size_t new_len =
                strlen(rpath) + 6 + (old_path ? strlen(old_path) : 0);
            char *new_path = malloc(new_len);
            if (new_path) {
              snprintf(new_path, new_len, "%s/bin:%s", rpath,
                       old_path ? old_path : "");
              setenv("PATH", new_path, 1);
              free(new_path);
            }
            printf("%s[myshell] Native Python VENV Activated: %s%s\n",
                   COLOR_GREEN, rpath, COLOR_RESET);
            last_status = 0;
            return 1;
          }
        }
      }
      /* Fallback for standard .myshellrc sourcing */
      load_rc(args[1]);
    } else {
      fprintf(stderr, "%smyshell: source: missing filename%s\n", ERR_COLOR,
              COLOR_RESET);
      last_status = 1;
    }
    return 1;
  }

  if (!strcmp(args[0], "alias")) {
    if (!args[1])
      for (int i = 0; i < alias_count; i++)
        printf("alias %s='%s'\n", aliases[i].name, aliases[i].val);
    else
      for (int i = 1; args[i]; i++) {
        char *eq = strchr(args[i], '=');
        if (eq) {
          *eq = '\0';
          set_alias(args[i], eq + 1);
        }
      }
    last_status = 0;
    return 1;
  }
  if (!strcmp(args[0], "unalias")) {
    for (int i = 1; args[i]; i++)
      remove_alias(args[i]);
    last_status = 0;
    return 1;
  }
  if (!strcmp(args[0], "true")) {
    last_status = 0;
    return 1;
  }
  if (!strcmp(args[0], "false")) {
    last_status = 1;
    return 1;
  }
  if (!strcmp(args[0], "exit")) {
    exit(args[1] ? atoi(args[1]) : last_status);
  }
  return 0;
}

int execute_pipeline(char **args, int bg, const char *raw_cmd) {
  if (!args[0])
    return 0;
  int num_cmds = 1;
  for (int i = 0; args[i]; i++)
    if (!strcmp(args[i], "|"))
      num_cmds++;

  int pipes[MAX_ARGS][2];
  pid_t pids[MAX_ARGS];
  char **cmd_curr = args;
  pid_t pipeline_pgid = 0;

  for (int c = 0; c < num_cmds; c++) {
    char **cmd_end = cmd_curr;
    while (*cmd_end && strcmp(*cmd_end, "|"))
      cmd_end++;
    *cmd_end = NULL;
    char *pipe_args[MAX_ARGS];
    int p_idx = 0;
    for (char **p = cmd_curr; p < cmd_end; p++)
      pipe_args[p_idx++] = *p;
    pipe_args[p_idx] = NULL;
    expand_aliases(pipe_args);

    if (num_cmds == 1 && is_builtin(pipe_args[0])) {
      int so = dup(STDOUT_FILENO), se = dup(STDERR_FILENO),
          si = dup(STDIN_FILENO);
      handle_redirections(pipe_args);
      if (pipe_args[0])
        run_builtin(pipe_args);
      dup2(so, STDOUT_FILENO);
      dup2(se, STDERR_FILENO);
      dup2(si, STDIN_FILENO);
      close(so);
      close(se);
      close(si);
      return last_status;
    }

    if (c < num_cmds - 1 && pipe(pipes[c]) < 0) {
      perror("myshell: pipe");
      break;
    }

    pids[c] = fork();
    if (pids[c] == 0) {
      if (shell_is_interactive) {
        pid_t pid = getpid();
        if (pipeline_pgid == 0)
          pipeline_pgid = pid;
        setpgid(pid, pipeline_pgid);
        if (!bg)
          tcsetpgrp(shell_terminal, pipeline_pgid);
      }
      signal(SIGINT, SIG_DFL);
      signal(SIGQUIT, SIG_DFL);
      signal(SIGTSTP, SIG_DFL);
      signal(SIGTTIN, SIG_DFL);
      signal(SIGTTOU, SIG_DFL);

      if (c > 0)
        dup2(pipes[c - 1][0], STDIN_FILENO);
      if (c < num_cmds - 1)
        dup2(pipes[c][1], STDOUT_FILENO);
      for (int i = 0; i < c; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
      }
      if (c < num_cmds - 1) {
        close(pipes[c][0]);
        close(pipes[c][1]);
      }

      handle_redirections(pipe_args);
      if (pipe_args[0]) {
        if (run_builtin(pipe_args))
          exit(last_status);
        execvp(pipe_args[0], pipe_args);
        fprintf(stderr, "%smyshell: command not found: %s%s\n", ERR_COLOR,
                pipe_args[0], COLOR_RESET);
      }
      exit(127);
    } else if (shell_is_interactive) {
      if (pipeline_pgid == 0)
        pipeline_pgid = pids[c];
      setpgid(pids[c], pipeline_pgid);
    }
    if (c > 0) {
      close(pipes[c - 1][0]);
      close(pipes[c - 1][1]);
    }
    cmd_curr = cmd_end + 1;
  }

  int status = 0, ret = 0;
  if (!bg) {
    if (shell_is_interactive)
      tcsetpgrp(shell_terminal, pipeline_pgid);
    for (int c = 0; c < num_cmds; c++) {
      if (pids[c] > 0) {
        pid_t res;
        do {
          res = waitpid(pids[c], &status, WUNTRACED);
        } while (res == -1 && errno == EINTR);
        if (WIFSTOPPED(status) && c == num_cmds - 1) {
          printf("\n[%d] Stopped  %s\n", next_job_id, raw_cmd);
          add_job(pipeline_pgid, raw_cmd, 1);
          ret = 128 + WSTOPSIG(status);
        } else if (c == num_cmds - 1) {
          ret = WIFEXITED(status)
                    ? WEXITSTATUS(status)
                    : (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 127);
        }
      }
    }
    if (shell_is_interactive) {
      tcsetpgrp(shell_terminal, shell_pgid);
      tcsetattr(shell_terminal, TCSADRAIN, &shell_tmodes);
    }
    return ret;
  } else {
    add_job(pipeline_pgid, raw_cmd, 0);
    return 0;
  }
}

int execute_logic(char **args) {
  char *chunk[MAX_ARGS];
  int c = 0, i = 0, skip = 0;
  while (args[i]) {
    int is_and = !strcmp(args[i], "&&"), is_or = !strcmp(args[i], "||"),
        is_sem = !strcmp(args[i], ";"), is_bg = !strcmp(args[i], "&");
    if (is_and || is_or || is_sem || is_bg) {
      chunk[c] = NULL;
      if (c > 0 && !skip) {
        char cmd_str[MAX_LINE] = "";
        for (int k = 0; k < c; k++) {
          strncat(cmd_str, chunk[k], MAX_LINE - strlen(cmd_str) - 1);
          if (k < c - 1)
            strncat(cmd_str, " ", MAX_LINE - strlen(cmd_str) - 1);
        }
        last_status = execute_pipeline(chunk, is_bg, cmd_str);
      }
      if (is_and)
        skip = (last_status != 0);
      else if (is_or)
        skip = (last_status == 0);
      else
        skip = 0;
      c = 0;
    } else
      chunk[c++] = args[i];
    i++;
  }
  chunk[c] = NULL;
  if (c > 0 && !skip) {
    char cmd_str[MAX_LINE] = "";
    for (int k = 0; k < c; k++) {
      strncat(cmd_str, chunk[k], MAX_LINE - strlen(cmd_str) - 1);
      if (k < c - 1)
        strncat(cmd_str, " ", MAX_LINE - strlen(cmd_str) - 1);
    }
    last_status = execute_pipeline(chunk, 0, cmd_str);
  }
  return last_status;
}

void process_command(char *line) {
  /* Dynamically allocate giant arrays to prevent stack overflow/reentrancy when
   * mapping massive directories */
  char *expanded_subst = malloc(MAX_LINE * 4);
  char *padded = malloc(MAX_LINE * 4);
  char **raw_args = malloc(MAX_ARGS * sizeof(char *));
  char **args = malloc(MAX_ARGS * sizeof(char *));
  char(*exp_buf)[MAX_LINE] = malloc(MAX_ARGS * MAX_LINE);

  if (!expanded_subst || !padded || !raw_args || !args || !exp_buf) {
    if (expanded_subst)
      free(expanded_subst);
    if (padded)
      free(padded);
    if (raw_args)
      free(raw_args);
    if (args)
      free(args);
    if (exp_buf)
      free(exp_buf);
    return;
  }

  snprintf(expanded_subst, MAX_LINE * 4, "%s", line);
  expand_substitutions(expanded_subst); /* Execute $(cmd) and `cmd` */

  pad_operators(expanded_subst, padded);
  tokenize(padded, raw_args);
  if (raw_args[0]) {
    expand_args(raw_args, args, exp_buf);
    time_t start = time(NULL);
    last_status = execute_logic(args);
    last_duration = (int)(time(NULL) - start);
  }

  free(expanded_subst);
  free(padded);
  free(raw_args);
  free(args);
  free(exp_buf);
}

void load_rc(const char *filepath) {
  if (rc_depth > 10) { /* Infinite depth-recursion bounds checking */
    fprintf(stderr, "%smyshell: max source depth exceeded%s\n", ERR_COLOR,
            COLOR_RESET);
    return;
  }
  rc_depth++;

  FILE *f = fopen(filepath, "r");
  if (!f) {
    rc_depth--;
    return;
  }
  char line[MAX_LINE];
  while (fgets(line, sizeof(line), f)) {
    line[strcspn(line, "\r\n")] = '\0';
    char *p = line;
    while (isspace((unsigned char)*p))
      p++;
    if (*p && *p != '#')
      process_command(p);
  }
  fclose(f);
  last_duration = 0;
  rc_depth--;
}

int main() {
  init_shell();
  tcgetattr(STDIN_FILENO, &orig_termios);
  atexit(disable_raw_mode);
  atexit(save_history);

  init_history();
  init_default_aliases();

  char *home = getenv("HOME");
  if (home) {
    char rc[1024];
    snprintf(rc, sizeof(rc), "%s/.myshellrc", home);
    load_rc(rc);
  }

  char line[MAX_LINE];

  while (1) {
    if (shell_is_interactive)
      check_jobs();
    if (!read_input(line))
      break;

    char hist_line[MAX_LINE];
    snprintf(hist_line, MAX_LINE, "%s", line);
    for (int i = 0; hist_line[i]; i++)
      if (hist_line[i] == '\n')
        hist_line[i] = ' ';

    int is_empty = 1;
    for (int i = 0; hist_line[i]; i++)
      if (!isspace((unsigned char)hist_line[i]))
        is_empty = 0;
    if (!is_empty) {
      add_history(hist_line);
      process_command(line);
    }
  }
  return 0;
}
