/*======================================================
  myshell.c - Advanced Minimal C Shell
  Features:
   - Persistent History (~/.myshell_history)
   - Zsh-Style Prefix History Search (Up/Down Arrows)
   - Live Git Branch Prompt Indicator (Native & Fast)
   - Command Execution Time Tracking ([Xs])
   - Ctrl+Left/Right Word Jumping
   - Startup Script Auto-Loader (~/.myshellrc)
   - Built-ins: export, source, cd, exit, clear, history
   - Infinite Pipeline Chaining (ls | grep | wc)
   - Logical Operators (&&, ||, ;) & Backgrounds (&)
   - Wildcard Globbing Expansion (*.c, ?)
   - Quote-Safe Tokenizer ("text | inside") & # Comments
   - Grid-Style Tab Autocompletion Menu
======================================================*/

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <glob.h>
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

#define MAX_LINE 1024
#define MAX_ARGS 512
#define MAX_HISTORY 1000
#define MAX_MATCHES 2048

/* Theme Colors */
#define COLOR_RESET "\033[0m"
#define COLOR_GRAY "\033[90m"
#define COLOR_RED "\033[31m"
#define COLOR_GREEN "\033[32m"
#define COLOR_YELLOW "\033[33m"
#define COLOR_CYAN "\033[36m"
#define COLOR_WHITE "\033[37m"

#define PROMPT_COLOR "\033[35m" /* Magenta */
#define DIR_COLOR "\033[33m"    /* Yellow */
#define SUGG_COLOR COLOR_GRAY
#define ERR_COLOR COLOR_RED

typedef struct {
  char insert[MAX_LINE];
  char display[MAX_LINE];
  int is_dir;
  int is_exec;
} Completion;

/* Globals */
struct termios orig_termios;
char history[MAX_HISTORY][MAX_LINE];
char history_file[1024] = "";
int history_count = 0;
int last_status = 0;
int last_duration = 0;
char prev_dir[1024] = "";

/*======================================================
  HISTORY & SYSTEM ENGINE
======================================================*/

void init_history() {
  char *home = getenv("HOME"); if (!home) return;
  snprintf(history_file, sizeof(history_file), "%s/.myshell_history", home);
  FILE *f = fopen(history_file, "r"); if (!f) return;
  
  char line[MAX_LINE];
  while (fgets(line, sizeof(line), f)) {
    line[strcspn(line, "\r\n")] = '\0';
    if (!*line) continue;
    if (history_count >= MAX_HISTORY) {
      memmove(history[0], history[1], (MAX_HISTORY - 1) * MAX_LINE);
      history_count = MAX_HISTORY - 1;
    }
    strcpy(history[history_count++], line);
  } fclose(f);
}

void save_history() { /* Truncates and saves cleanly on exit */
  if (!history_file[0]) return;
  FILE *f = fopen(history_file, "w"); if (!f) return;
  for (int i = 0; i < history_count; i++) fprintf(f, "%s\n", history[i]);
  fclose(f);
}

void add_history(const char *line) {
  if (!*line || (history_count && !strcmp(history[history_count - 1], line))) return;
  if (history_count < MAX_HISTORY) strcpy(history[history_count++], line);
  else { memmove(history[0], history[1], (MAX_HISTORY - 1) * MAX_LINE); strcpy(history[MAX_HISTORY - 1], line); }
  if (history_file[0]) { FILE *f = fopen(history_file, "a"); if (f) { fprintf(f, "%s\n", line); fclose(f); } }
}

void get_suggestion(const char *in, char *out) {
  out[0] = '\0'; int len = strlen(in); if (!len) return;
  for (int i = history_count - 1; i >= 0; i--) {
    if (!strncmp(history[i], in, len) && strlen(history[i]) > len) { strcpy(out, history[i] + len); return; }
  }
}

/* Zero-latency Git Branch Detection purely in C */
void get_git_branch(char *buf, size_t max_size) {
  buf[0] = '\0'; char path[1024]; if (!getcwd(path, sizeof(path))) return;
  while (1) {
    char head_path[1024]; snprintf(head_path, sizeof(head_path), "%s/.git/HEAD", path);
    FILE *f = fopen(head_path, "r");
    if (f) {
      char line[256];
      if (fgets(line, sizeof(line), f)) {
        char *ref = strstr(line, "ref: refs/heads/");
        if (ref) { strncpy(buf, ref + 16, max_size); buf[strcspn(buf, "\r\n")] = '\0'; }
        else { strncpy(buf, line, 7); buf[7] = '\0'; } /* Detached HEAD */
      } fclose(f); return;
    }
    char *last_slash = strrchr(path, '/'); if (!last_slash || last_slash == path) break; *last_slash = '\0';
  }
}

/*======================================================
  TERMINAL & UI ENGINE
======================================================*/

void disable_raw_mode() { tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios); }
void enable_raw_mode() {
  struct termios raw = orig_termios; raw.c_lflag &= ~(ECHO | ICANON | ISIG);
  raw.c_cc[VMIN] = 1; raw.c_cc[VTIME] = 0; tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

int cmp_comp(const void *a, const void *b) { return strcmp(((Completion *)a)->display, ((Completion *)b)->display); }

/* Interactive Key Loop */
int read_input(char *line) {
  int len = 0, pos = 0, tab_mode = 0, match_count = 0, match_idx = 0;
  int menu_lines = 0, last_menu_lines = 0, history_pos = history_count, word_pos = 0;
  char suggestion[MAX_LINE] = "", cwd[1024], prompt[2048], cur_in[MAX_LINE] = "", saved_tail[MAX_LINE] = "";
  static Completion matches[MAX_MATCHES];

  char host[256] = "local"; gethostname(host, sizeof(host));
  char *user = getenv("USER"); if (!user) user = "anon";

  line[0] = '\0'; enable_raw_mode(); int reading = 1;

  while (reading) {
    if (!getcwd(cwd, sizeof(cwd))) strcpy(cwd, "?");
    char *home = getenv("HOME"), d_cwd[1024];
    if (home && !strncmp(cwd, home, strlen(home))) snprintf(d_cwd, sizeof(d_cwd), "~%s", cwd + strlen(home)); else strcpy(d_cwd, cwd);

    char git_branch[64] = ""; get_git_branch(git_branch, sizeof(git_branch));
    char git_prompt[128] = ""; if (git_branch[0]) snprintf(git_prompt, sizeof(git_prompt), " %sgit:(%s%s%s)", COLOR_GRAY, COLOR_RED, git_branch, COLOR_GRAY);

    char time_str[32] = ""; if (last_duration > 0) snprintf(time_str, sizeof(time_str), "%s[%ds] ", COLOR_YELLOW, last_duration);
    const char *status_color = (last_status == 0) ? PROMPT_COLOR : ERR_COLOR;
    snprintf(prompt, sizeof(prompt), "\r\033[2K%s%s%s@%s%s:%s%s%s %s❯%s ", time_str, COLOR_CYAN, user, host, COLOR_RESET, DIR_COLOR, d_cwd, git_prompt, status_color, COLOR_RESET);
    
    printf("%s", prompt); for (int i = 0; i < len; i++) putchar(line[i]);

    int g_len = 0;
    if (!tab_mode && pos == len) {
      get_suggestion(line, suggestion);
      if (suggestion[0]) { printf("%s%s%s", SUGG_COLOR, suggestion, COLOR_RESET); g_len = strlen(suggestion); }
    } else suggestion[0] = '\0';

    menu_lines = 0;
    if (tab_mode && match_count > 0) {
      struct winsize w; int t_cols = (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) != -1 && w.ws_col > 0) ? w.ws_col : 80;
      int max_len = 0; for (int i = 0; i < match_count; i++) { int l = strlen(matches[i].display) + matches[i].is_dir; if (l > max_len) max_len = l; }
      int col_w = max_len + 2, cols = t_cols / col_w; if (!cols) cols = 1;
      int m_show = cols * 5, s_idx = (match_idx / m_show) * m_show, e_idx = (s_idx + m_show > match_count) ? match_count : s_idx + m_show;

      for (int i = s_idx; i < e_idx; i++) {
        if ((i - s_idx) % cols == 0) { printf("\r\n\033[K"); menu_lines++; }
        if (i == match_idx) printf("\033[7m"); 
        printf("%s%s%s", matches[i].is_dir ? DIR_COLOR : (matches[i].is_exec ? COLOR_GREEN : COLOR_WHITE), matches[i].display, matches[i].is_dir ? "/" : "");
        for (int p = 0; p < col_w - (int)strlen(matches[i].display) - matches[i].is_dir && (i + 1 - s_idx) % cols != 0; p++) printf(" ");
        printf("\033[0m");
      }
      if (match_count > e_idx) { printf("\r\n\033[K %s... %d more%s", SUGG_COLOR, match_count - e_idx, COLOR_RESET); menu_lines++; }
    }
    for (int i = menu_lines; i < last_menu_lines; i++) printf("\r\n\033[K");
    
    int down = menu_lines > last_menu_lines ? menu_lines : last_menu_lines;
    if (down > 0) { printf("\033[%dA\r\033[2K%s", down, prompt); for (int i = 0; i < len; i++) putchar(line[i]); if (g_len) printf("%s%s%s", SUGG_COLOR, suggestion, COLOR_RESET); }
    last_menu_lines = menu_lines; int left = (len - pos) + g_len; if (left > 0) printf("\033[%dD", left); fflush(stdout);

    char c; if (read(STDIN_FILENO, &c, 1) != 1) { disable_raw_mode(); return 0; }

    switch (c) {
      case 10: case 13: 
        if (last_menu_lines > 0) { for (int i = 0; i < last_menu_lines; i++) printf("\r\n\033[K"); printf("\033[%dA\r\033[2K%s", last_menu_lines, prompt); for (int i = 0; i < len; i++) putchar(line[i]); }
        else if (len > pos) { printf("\r\033[2K%s", prompt); for (int i = 0; i < len; i++) putchar(line[i]); }
        printf("\n"); reading = 0; break;
      case 3: pos = len = 0; line[0] = '\0'; reading = 0; printf("^C\n"); break; 
      case 4: if (!len) { disable_raw_mode(); printf("\n"); exit(0); } if (pos < len) { memmove(&line[pos], &line[pos + 1], len - pos); len--; } break;
      case 12: printf("\033[H\033[2J"); break; 
      case 21: memmove(&line[0], &line[pos], len - pos + 1); len -= pos; pos = 0; tab_mode = 0; history_pos = history_count; break;
      case 23: if (pos > 0) { int s = pos - 1; while (s > 0 && line[s] == ' ') s--; while (s > 0 && line[s - 1] != ' ') s--; memmove(&line[s], &line[pos], len - pos + 1); len -= (pos - s); pos = s; } tab_mode = 0; history_pos = history_count; break;
      case 8: case 127: if (pos > 0) { memmove(&line[pos - 1], &line[pos], len - pos + 1); pos--; len--; } tab_mode = 0; history_pos = history_count; break; 
      
      case 27: {
        char seq[4]; struct termios t_raw = orig_termios; t_raw.c_lflag &= ~(ECHO|ICANON|ISIG); t_raw.c_cc[VMIN]=0; t_raw.c_cc[VTIME]=1; tcsetattr(STDIN_FILENO, TCSANOW, &t_raw);
        if (read(STDIN_FILENO, &seq[0], 1) == 1 && seq[0] == '[' && read(STDIN_FILENO, &seq[1], 1) == 1) {
          if (seq[1] == 'A') { /* Zsh-Style Smart Prefix History Search (Up) */
            if (history_pos == history_count) strcpy(cur_in, line);
            while (history_pos > 0) { history_pos--; if (!strncmp(history[history_pos], cur_in, strlen(cur_in))) { strcpy(line, history[history_pos]); pos = len = strlen(line); break; } }
          } else if (seq[1] == 'B') { /* Zsh-Style Smart Prefix History Search (Down) */
            while (history_pos < history_count) {
              history_pos++; if (history_pos == history_count) { strcpy(line, cur_in); pos = len = strlen(line); break; }
              if (!strncmp(history[history_pos], cur_in, strlen(cur_in))) { strcpy(line, history[history_pos]); pos = len = strlen(line); break; }
            }
          } else if (seq[1] == 'C') { if (pos < len) pos++; else if (suggestion[0]) { strcat(line, suggestion); pos = len = strlen(line); suggestion[0]='\0'; } }
          else if (seq[1] == 'D') { if (pos > 0) pos--; }
          else if (seq[1] == 'H') pos = 0; else if (seq[1] == 'F') pos = len;
          else if (seq[1] == '3' && read(STDIN_FILENO, &seq[2], 1) == 1 && seq[2] == '~' && pos < len) { memmove(&line[pos], &line[pos + 1], len - pos); len--; }
          else if (seq[1] == '1' && read(STDIN_FILENO, &seq[2], 1) == 1 && seq[2] == ';') { /* Ctrl+Arrows Jump Word */
            if (read(STDIN_FILENO, &seq[3], 1) == 1 && read(STDIN_FILENO, &seq[4], 1) == 1) {
              if (seq[3] == '5' && seq[4] == 'C') { while (pos < len && line[pos] != ' ') pos++; while (pos < len && line[pos] == ' ') pos++; }
              else if (seq[3] == '5' && seq[4] == 'D') { while (pos > 0 && line[pos - 1] == ' ') pos--; while (pos > 0 && line[pos - 1] != ' ') pos--; }
            }
          }
        } tcsetattr(STDIN_FILENO, TCSANOW, &t_raw); enable_raw_mode(); tab_mode = 0; break;
      }
      
      case 9: { /* Autocompletion Engine */
        if (!tab_mode) {
          match_count = match_idx = 0; word_pos = pos;
          while (word_pos > 0 && line[word_pos - 1] != ' ' && line[word_pos - 1] != '"') word_pos--;
          char w[MAX_LINE]; strncpy(w, &line[word_pos], pos - word_pos); w[pos - word_pos] = '\0'; strcpy(saved_tail, &line[pos]);
          
          int is_first = 1; 
          for (int i = word_pos - 1; i >= 0; i--) { if (strchr("|&;(", line[i])) break; if (line[i] != ' ' && line[i] != '"' && line[i] != '\'') { is_first = 0; break; } }

          if (is_first && !strchr(w, '/') && strlen(w) > 0) { 
            const char *blt[] = {"cd", "exit", "clear", "history", "export", "source", "ll", NULL};
            for (int i = 0; blt[i]; i++) if (!strncmp(blt[i], w, strlen(w))) { strcpy(matches[match_count].insert, blt[i]); strcpy(matches[match_count].display, blt[i]); matches[match_count].is_exec = 1; matches[match_count].is_dir = 0; match_count++; }
            char *p_env = getenv("PATH"), p_copy[8192];
            if (p_env) {
              strncpy(p_copy, p_env, 8191);
              for (char *dir_t = strtok(p_copy, ":"); dir_t && match_count < MAX_MATCHES; dir_t = strtok(NULL, ":")) {
                DIR *dir = opendir(dir_t); if (!dir) continue; struct dirent *ent;
                while ((ent = readdir(dir)) && match_count < MAX_MATCHES) {
                  if (!strncmp(ent->d_name, w, strlen(w)) && strcmp(ent->d_name, ".") && strcmp(ent->d_name, "..")) {
                    int dup = 0; for (int k = 0; k < match_count; k++) if (!strcmp(matches[k].display, ent->d_name)) dup = 1;
                    if (!dup) { char fp[MAX_LINE * 2]; snprintf(fp, sizeof(fp), "%s/%s", dir_t, ent->d_name); struct stat st; if (stat(fp, &st) == 0 && S_ISREG(st.st_mode) && !access(fp, X_OK)) {
                        strcpy(matches[match_count].insert, ent->d_name); strcpy(matches[match_count].display, ent->d_name); matches[match_count].is_exec = 1; matches[match_count].is_dir = 0; match_count++;
                    } }
                  }
                } closedir(dir);
              }
            }
          } else { 
            char s_dir[MAX_LINE] = ".", pref[MAX_LINE] = "", d_path[MAX_LINE] = ""; char *slash = strrchr(w, '/');
            if (slash) { strncpy(d_path, w, slash - w + 1); d_path[slash - w + 1] = '\0'; strcpy(pref, slash + 1); if (slash > w) { strncpy(s_dir, w, slash - w); s_dir[slash - w] = '\0'; } else strcpy(s_dir, "/"); } else strcpy(pref, w);
            if (s_dir[0] == '~') { char *h = getenv("HOME"); if (h) { char t[MAX_LINE]; snprintf(t, MAX_LINE, "%s%s", h, s_dir + 1); strcpy(s_dir, t); } }
            DIR *dir = opendir(s_dir); if (dir) { struct dirent *ent;
              while ((ent = readdir(dir)) && match_count < MAX_MATCHES) {
                if (ent->d_name[0] == '.' && pref[0] != '.') continue;
                if (!strncmp(ent->d_name, pref, strlen(pref)) && strcmp(ent->d_name, ".") && strcmp(ent->d_name, "..")) {
                  char f_path[MAX_LINE]; snprintf(f_path, MAX_LINE, "%s/%s", strcmp(s_dir, "/") ? s_dir : "", ent->d_name);
                  struct stat st; int is_d = (stat(f_path, &st) == 0 && S_ISDIR(st.st_mode));
                  snprintf(matches[match_count].insert, MAX_LINE, "%s%s", d_path, ent->d_name); strcpy(matches[match_count].display, ent->d_name); matches[match_count].is_dir = is_d; matches[match_count].is_exec = (!is_d && !access(f_path, X_OK)); match_count++;
                }
              } closedir(dir);
            }
          }
          if (match_count > 0) {
            qsort(matches, match_count, sizeof(Completion), cmp_comp); tab_mode = match_count > 1; match_idx = -1;
            if (match_count == 1) { snprintf(&line[word_pos], MAX_LINE - word_pos, "%s%s%s", matches[0].insert, matches[0].is_dir ? "/" : " ", saved_tail); pos = len = strlen(line) - strlen(saved_tail); tab_mode = 0; }
            else { char lcp[MAX_LINE]; strcpy(lcp, matches[0].insert); for (int i = 1; i < match_count; i++) { int j = 0; while (lcp[j] && matches[i].insert[j] && lcp[j] == matches[i].insert[j]) j++; lcp[j] = '\0'; }
              snprintf(&line[word_pos], MAX_LINE - word_pos, "%s%s", lcp, saved_tail); pos = word_pos + strlen(lcp); len = strlen(line); }
          } else if (suggestion[0] && pos == len) { strcat(line, suggestion); pos = len = strlen(line); suggestion[0] = '\0'; }
        } else if (match_count > 0) { 
          match_idx = (match_idx + 1) % match_count; snprintf(&line[word_pos], MAX_LINE - word_pos, "%s%s%s", matches[match_idx].insert, matches[match_idx].is_dir ? "/" : "", saved_tail);
          pos = word_pos + strlen(matches[match_idx].insert) + matches[match_idx].is_dir; len = strlen(line);
        } break;
      }
      default: if (isprint(c) && len < MAX_LINE - 1) { memmove(&line[pos + 1], &line[pos], len - pos + 1); line[pos++] = c; len++; history_pos = history_count; } tab_mode = 0; break;
    }
  } disable_raw_mode(); return 1;
}

/*======================================================
  PARSER & EXECUTION ENGINE
======================================================*/

void pad_operators(const char *in, char *out) {
  int in_sq = 0, in_dq = 0;
  while (*in) {
    if (*in == '\'' && !in_dq) in_sq = !in_sq; else if (*in == '"' && !in_sq) in_dq = !in_dq;
    if (!in_sq && !in_dq && strchr("|;<>&", *in)) {
      *out++ = ' '; *out++ = *in; if ((in[0] == '>' && in[1] == '>') || (in[0] == '&' && in[1] == '&') || (in[0] == '|' && in[1] == '|')) { in++; *out++ = *in; } *out++ = ' ';
    } else *out++ = *in; in++;
  } *out = '\0';
}

void tokenize(char *line, char **args) {
  int count = 0, sq = 0, dq = 0; char *p = line;
  while (*p) {
    while (isspace(*p)) p++; if (!*p) break;
    if (*p == '#' && !sq && !dq) break; /* Natively skips comments */
    args[count++] = p;
    while (*p) {
      if (*p == '\'') sq = !sq; else if (*p == '"') dq = !dq; else if (isspace(*p) && !sq && !dq) { *p = '\0'; p++; break; } p++;
    }
  } args[count] = NULL;
}

void expand_args(char **args, char **new_args, char buf[][MAX_LINE]) {
  int n = 0;
  for (int i = 0; args[i] && n < MAX_ARGS - 1; i++) {
    char temp[MAX_LINE]; int has_quote = (strchr(args[i], '"') || strchr(args[i], '\''));
    char *r = args[i], *w = temp; int sq = 0, dq = 0;
    while (*r) {
      if (*r == '\'' && !dq) { sq = !sq; r++; } else if (*r == '"' && !sq) { dq = !dq; r++; } else if (*r == '$' && !sq) {
        r++; if (*r == '?') { char st[16]; snprintf(st, 16, "%d", last_status); for (char *s = st; *s; s++) *w++ = *s; r++; }
        else { char var[64]; int v = 0; while (isalnum(*r) || *r == '_') var[v++] = *r++; var[v] = '\0'; char *val = getenv(var); if (val) { for (char *s = val; *s; s++) *w++ = *s; } }
      } else *w++ = *r++;
    } *w = '\0';

    if (temp[0] == '~' && (!temp[1] || temp[1] == '/')) { char *h = getenv("HOME"); char t2[MAX_LINE]; snprintf(t2, MAX_LINE, "%s%s", h ? h : "", temp + 1); strcpy(temp, t2); }
    if (!has_quote && strpbrk(temp, "*?")) {
      glob_t g; if (glob(temp, GLOB_NOCHECK, NULL, &g) == 0) { for (size_t j = 0; j < g.gl_pathc && n < MAX_ARGS - 1; j++) { strncpy(buf[n], g.gl_pathv[j], MAX_LINE); new_args[n++] = buf[n]; } } globfree(&g);
    } else { strncpy(buf[n], temp, MAX_LINE); new_args[n++] = buf[n]; }
  } new_args[n] = NULL;
}

void handle_redirections(char **args) {
  int j = 0;
  for (int i = 0; args[i]; i++) {
    if (!strcmp(args[i], ">") && args[i + 1]) { int f = open(args[i + 1], O_WRONLY | O_CREAT | O_TRUNC, 0644); if (f >= 0) { dup2(f, STDOUT_FILENO); close(f); } i++; }
    else if (!strcmp(args[i], ">>") && args[i + 1]) { int f = open(args[i + 1], O_WRONLY | O_CREAT | O_APPEND, 0644); if (f >= 0) { dup2(f, STDOUT_FILENO); close(f); } i++; }
    else if (!strcmp(args[i], "<") && args[i + 1]) { int f = open(args[i + 1], O_RDONLY); if (f >= 0) { dup2(f, STDIN_FILENO); close(f); } else { perror(args[i + 1]); exit(1); } i++; }
    else args[j++] = args[i];
  } args[j] = NULL;
}

void load_rc(const char *filepath); /* Forward declaration */

int execute_pipeline(char **args, int bg) {
  if (!args[0]) return 0;
  int num_cmds = 1; for (int i = 0; args[i]; i++) if (!strcmp(args[i], "|")) num_cmds++;

  if (num_cmds == 1) {
    if (!strcmp(args[0], "exit")) exit(last_status);
    if (!strcmp(args[0], "clear")) { printf("\033[H\033[2J"); return 0; }
    if (!strcmp(args[0], "history")) { for (int h = 0; h < history_count; h++) printf("%d  %s\n", h + 1, history[h]); return 0; }
    if (!strcmp(args[0], "cd")) {
      char *t = args[1] ? args[1] : getenv("HOME");
      if (args[1] && !strcmp(args[1], "-")) t = prev_dir[0] ? prev_dir : ".";
      char cur[1024]; getcwd(cur, sizeof(cur));
      if (t && chdir(t) != 0) { fprintf(stderr, "%smyshell: cd: %s: No such directory%s\n", ERR_COLOR, t, COLOR_RESET); return 1; }
      strcpy(prev_dir, cur); return 0;
    }
    if (!strcmp(args[0], "export")) {
      for (int i = 1; args[i]; i++) { char *eq = strchr(args[i], '='); if (eq) { *eq = '\0'; setenv(args[i], eq + 1, 1); } } return 0;
    }
    if (!strcmp(args[0], "source")) {
      if (args[1]) load_rc(args[1]); else fprintf(stderr, "%smyshell: source: missing filename%s\n", ERR_COLOR, COLOR_RESET); return 0;
    }
  }

  int pipes[MAX_ARGS][2]; pid_t pids[MAX_ARGS]; char **cmd_curr = args;

  for (int c = 0; c < num_cmds; c++) {
    char **cmd_end = cmd_curr; while (*cmd_end && strcmp(*cmd_end, "|")) cmd_end++; *cmd_end = NULL;
    char *pipe_args[MAX_ARGS]; int p_idx = 0; for (char **p = cmd_curr; p < cmd_end; p++) pipe_args[p_idx++] = *p; pipe_args[p_idx] = NULL;

    if (pipe_args[0]) {
      if (!strcmp(pipe_args[0], "ll")) { pipe_args[0] = "ls"; for (int x = p_idx; x >= 1; x--) pipe_args[x + 2] = pipe_args[x]; pipe_args[1] = "-al"; pipe_args[2] = "--color=auto"; }
      else if (!strcmp(pipe_args[0], "ls") || !strcmp(pipe_args[0], "grep")) { for (int x = p_idx; x >= 1; x--) pipe_args[x + 1] = pipe_args[x]; pipe_args[1] = "--color=auto"; }
    }

    if (c < num_cmds - 1) pipe(pipes[c]);
    pids[c] = fork();
    if (pids[c] == 0) {
      signal(SIGINT, SIG_DFL);
      if (c > 0) dup2(pipes[c - 1][0], STDIN_FILENO);
      if (c < num_cmds - 1) dup2(pipes[c][1], STDOUT_FILENO);
      for (int i = 0; i < c; i++) { close(pipes[i][0]); close(pipes[i][1]); }
      if (c < num_cmds - 1) { close(pipes[c][0]); close(pipes[c][1]); }

      handle_redirections(pipe_args);
      if (pipe_args[0]) {
        if (!strcmp(pipe_args[0], "cd") || !strcmp(pipe_args[0], "exit") || !strcmp(pipe_args[0], "clear") || !strcmp(pipe_args[0], "export") || !strcmp(pipe_args[0], "source")) exit(0);
        if (!strcmp(pipe_args[0], "history")) { for (int h = 0; h < history_count; h++) printf("%d  %s\n", h + 1, history[h]); exit(0); }
        execvp(pipe_args[0], pipe_args); fprintf(stderr, "%smyshell: command not found: %s%s\n", ERR_COLOR, pipe_args[0], COLOR_RESET);
      } exit(127);
    }
    if (c > 0) { close(pipes[c - 1][0]); close(pipes[c - 1][1]); }
    cmd_curr = cmd_end + 1;
  }

  int status = 0, ret = 0;
  if (!bg) {
    for (int c = 0; c < num_cmds; c++) { waitpid(pids[c], &status, 0); if (c == num_cmds - 1) ret = WIFEXITED(status) ? WEXITSTATUS(status) : (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 127); }
    return ret;
  } else { printf("[%d] background task running\n", pids[num_cmds - 1]); return 0; }
}

int execute_logic(char **args) {
  char *chunk[MAX_ARGS]; int c = 0, i = 0, skip = 0;
  while (args[i]) {
    int is_and = !strcmp(args[i], "&&"), is_or = !strcmp(args[i], "||"), is_sem = !strcmp(args[i], ";"), is_bg = !strcmp(args[i], "&");
    if (is_and || is_or || is_sem || is_bg) {
      chunk[c] = NULL;
      if (c > 0 && !skip) last_status = execute_pipeline(chunk, is_bg);
      if (is_and) skip = (last_status != 0); else if (is_or) skip = (last_status == 0); else skip = 0; c = 0;
    } else chunk[c++] = args[i];
    i++;
  }
  chunk[c] = NULL; if (c > 0 && !skip) last_status = execute_pipeline(chunk, 0);
  return last_status;
}

void process_command(char *line) {
  char padded[MAX_LINE * 2], *raw_args[MAX_ARGS], *args[MAX_ARGS]; static char exp_buf[MAX_ARGS][MAX_LINE];
  pad_operators(line, padded); tokenize(padded, raw_args); expand_args(raw_args, args, exp_buf);
  
  time_t start = time(NULL);
  last_status = execute_logic(args);
  time_t end = time(NULL);
  last_duration = (int)(end - start);
}

void load_rc(const char *filepath) {
  FILE *f = fopen(filepath, "r"); if (!f) return;
  char line[MAX_LINE];
  while (fgets(line, sizeof(line), f)) {
    line[strcspn(line, "\r\n")] = '\0';
    if (*line && line[0] != '#') process_command(line);
  } fclose(f);
}

int main() {
  tcgetattr(STDIN_FILENO, &orig_termios);
  atexit(disable_raw_mode); 
  atexit(save_history); /* Cleanly dumps memory to file on exit */

  init_history(); /* Boot history file */

  /* Auto-Load user configurations */
  char *home = getenv("HOME");
  if (home) { char rc[1024]; snprintf(rc, sizeof(rc), "%s/.myshellrc", home); load_rc(rc); }

  char line[MAX_LINE];
  signal(SIGINT, SIG_IGN); 

  while (1) {
    while (waitpid(-1, NULL, WNOHANG) > 0); 
    if (!read_input(line)) break;
    add_history(line);
    process_command(line);
  }
  return 0;
}
