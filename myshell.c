/*======================================================
  myshell.c - Minimal Feature-Rich C Shell
  Features:
   - Quote Parsing (Fixes Git commits: "msg")
   - Pipelining (|) & I/O Redirection (>, >>, <)
   - Left/Right Cursor Native Navigation
   - Background Tasks (&) & Env Vars ($VAR)
   - Bottom-Grid Colored Tab Autocompletion
   - Dynamic Exit Status Prompt Formatting
======================================================*/

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define MAX_LINE 1024
#define MAX_ARGS 64
#define MAX_HISTORY 100
#define MAX_MATCHES 4096

/* Keybindings */
#define KEY_CTRL_A 1
#define KEY_CTRL_C 3
#define KEY_CTRL_D 4
#define KEY_CTRL_E 5
#define KEY_CTRL_BS 8
#define KEY_TAB 9
#define KEY_ENTER 10
#define KEY_CTRL_L 12
#define KEY_RETURN 13
#define KEY_CTRL_U 21
#define KEY_CTRL_W 23
#define KEY_ESC 27
#define KEY_BACKSPACE 127

/* Colors */
#define COLOR_RESET "\033[0m"
#define COLOR_GRAY "\033[90m"
#define COLOR_RED "\033[31m"
#define COLOR_GREEN "\033[32m"
#define COLOR_YELLOW "\033[33m"
#define COLOR_BLUE "\033[34m"
#define COLOR_MAGENTA "\033[35m"
#define COLOR_CYAN "\033[36m"
#define COLOR_WHITE "\033[37m"

/* Theme (1=Ocean, 2=Hacker, 3=Sunset) */
#define THEME 3
#if THEME == 1
#define PROMPT_COLOR COLOR_CYAN
#define DIR_COLOR COLOR_BLUE
#define SUGG_COLOR COLOR_GRAY
#define ERR_COLOR COLOR_RED
#elif THEME == 2
#define PROMPT_COLOR COLOR_GREEN
#define DIR_COLOR COLOR_GREEN
#define SUGG_COLOR COLOR_GRAY
#define ERR_COLOR COLOR_GREEN
#elif THEME == 3
#define PROMPT_COLOR COLOR_MAGENTA
#define DIR_COLOR COLOR_YELLOW
#define SUGG_COLOR COLOR_GRAY
#define ERR_COLOR COLOR_RED
#endif

typedef struct {
  char insert[MAX_LINE];
  char display[MAX_LINE];
  int is_dir;
  int is_exec;
} Completion;

/* Globals */
struct termios orig_termios;
char history[MAX_HISTORY][MAX_LINE];
int history_count = 0;
int last_status = 0;

void disable_raw_mode() { tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios); }
void enable_raw_mode() {
  struct termios raw = orig_termios;
  raw.c_lflag &= ~(ECHO | ICANON | ISIG);
  raw.c_cc[VMIN] = 1;
  raw.c_cc[VTIME] = 0;
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

void add_history(const char *line) {
  if (!*line || (history_count && !strcmp(history[history_count - 1], line)))
    return;
  if (history_count < MAX_HISTORY)
    strcpy(history[history_count++], line);
  else {
    memmove(history[0], history[1], (MAX_HISTORY - 1) * MAX_LINE);
    strcpy(history[MAX_HISTORY - 1], line);
  }
}

void get_suggestion(const char *in, char *out) {
  out[0] = '\0';
  int len = strlen(in);
  if (!len)
    return;
  for (int i = history_count - 1; i >= 0; i--) {
    if (!strncmp(history[i], in, len) && strlen(history[i]) > len) {
      strcpy(out, history[i] + len);
      return;
    }
  }
}

int cmp_comp(const void *a, const void *b) {
  return strcmp(((Completion *)a)->display, ((Completion *)b)->display);
}

/* Handles routing standard out/in cleanly via open & dup2 */
void handle_redirections(char **args) {
  int j = 0;
  for (int i = 0; args[i]; i++) {
    if (!strcmp(args[i], ">") && args[i + 1]) {
      int fd = open(args[i + 1], O_WRONLY | O_CREAT | O_TRUNC, 0644);
      dup2(fd, STDOUT_FILENO);
      close(fd);
      i++;
    } else if (!strcmp(args[i], ">>") && args[i + 1]) {
      int fd = open(args[i + 1], O_WRONLY | O_CREAT | O_APPEND, 0644);
      dup2(fd, STDOUT_FILENO);
      close(fd);
      i++;
    } else if (!strcmp(args[i], "<") && args[i + 1]) {
      int fd = open(args[i + 1], O_RDONLY);
      dup2(fd, STDIN_FILENO);
      close(fd);
      i++;
    } else
      args[j++] = args[i];
  }
  args[j] = NULL;
}

/* Pipeline Execution Engine */
void execute_cmd(char **args, int bg) {
  int p_idx = -1;
  for (int i = 0; args[i]; i++)
    if (!strcmp(args[i], "|")) {
      p_idx = i;
      break;
    }

  /* Automatically inject colors for standard tooling */
  if (args[0] && (!strcmp(args[0], "ls") || !strcmp(args[0], "grep"))) {
    int k = 0;
    while (args[k])
      k++;
    if (k < MAX_ARGS - 2) {
      for (int x = k; x >= 1; x--)
        args[x + 1] = args[x];
      args[1] = "--color=auto";
    }
  }

  if (p_idx != -1) {
    args[p_idx] = NULL;
    char **args2 = &args[p_idx + 1];
    int fd[2];
    pipe(fd);
    if (fork() == 0) {
      dup2(fd[1], STDOUT_FILENO);
      close(fd[0]);
      close(fd[1]);
      handle_redirections(args);
      execvp(args[0], args);
      exit(1);
    }
    if (fork() == 0) {
      dup2(fd[0], STDIN_FILENO);
      close(fd[0]);
      close(fd[1]);
      handle_redirections(args2);
      execvp(args2[0], args2);
      exit(1);
    }
    close(fd[0]);
    close(fd[1]);
    wait(NULL);
    if (!bg)
      wait(NULL);
  } else {
    pid_t pid = fork();
    if (pid == 0) {
      signal(SIGINT, SIG_DFL);
      handle_redirections(args);
      if (execvp(args[0], args) == -1)
        fprintf(stderr, "%smyshell: command not found: %s%s\n", ERR_COLOR,
                args[0], COLOR_RESET);
      exit(1);
    }
    if (!bg) {
      int status;
      waitpid(pid, &status, 0);
      last_status = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    } else {
      printf("[%d] running in background\n", pid);
      last_status = 0;
    }
  }
}

/* Interactive Raw UI Loop */
int read_input(char *line) {
  int len = 0, pos = 0, tab_mode = 0, match_count = 0, match_idx = 0;
  int menu_lines = 0, last_menu_lines = 0, history_pos = history_count,
      word_start_pos = 0;
  char suggestion[MAX_LINE] = "", cwd[1024], prompt[2048],
       current_input[MAX_LINE] = "", saved_tail[MAX_LINE] = "";
  static Completion matches[MAX_MATCHES];

  line[0] = '\0';
  enable_raw_mode();
  int reading = 1;

  while (reading) {
    if (!getcwd(cwd, sizeof(cwd)))
      strcpy(cwd, "?");
    char *home = getenv("HOME"), d_cwd[1024];
    if (home && !strncmp(cwd, home, strlen(home)))
      snprintf(d_cwd, sizeof(d_cwd), "~%s", cwd + strlen(home));
    else
      strcpy(d_cwd, cwd);

    const char *status_color = (last_status == 0) ? PROMPT_COLOR : ERR_COLOR;
    snprintf(prompt, sizeof(prompt), "\r\033[2K%s%s %s❯%s ", DIR_COLOR, d_cwd,
             status_color, COLOR_RESET);
    printf("%s", prompt);
    for (int i = 0; i < len; i++)
      putchar(line[i]);

    int ghost_len = 0;
    if (!tab_mode && pos == len) {
      get_suggestion(line, suggestion);
      if (suggestion[0]) {
        printf("%s%s%s", SUGG_COLOR, suggestion, COLOR_RESET);
        ghost_len = strlen(suggestion);
      }
    } else
      suggestion[0] = '\0';

    /* Tab Completion Grid Engine */
    menu_lines = 0;
    if (tab_mode && match_count > 0) {
      struct winsize w;
      int t_cols = (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) != -1 && w.ws_col > 0)
                       ? w.ws_col
                       : 80;
      int max_len = 0;
      for (int i = 0; i < match_count; i++) {
        int l = strlen(matches[i].display) + matches[i].is_dir;
        if (l > max_len)
          max_len = l;
      }
      int col_w = max_len + 2, cols = t_cols / col_w;
      if (!cols)
        cols = 1;
      int max_show = cols * 5, start_idx = (match_idx / max_show) * max_show;
      int end_idx = (start_idx + max_show > match_count) ? match_count
                                                         : start_idx + max_show;

      for (int i = start_idx; i < end_idx; i++) {
        if ((i - start_idx) % cols == 0) {
          printf("\r\n\033[K");
          menu_lines++;
        }
        if (i == match_idx)
          printf("\033[7m"); /* Invert color */
        printf("%s%s%s",
               matches[i].is_dir
                   ? DIR_COLOR
                   : (matches[i].is_exec ? COLOR_GREEN : COLOR_WHITE),
               matches[i].display, matches[i].is_dir ? "/" : "");
        int pad = col_w - strlen(matches[i].display) - matches[i].is_dir;
        for (int p = 0; p < pad && (i + 1 - start_idx) % cols != 0; p++)
          printf(" ");
        printf("\033[0m");
      }
      if (match_count > end_idx) {
        printf("\r\n\033[K %s... %d more%s", SUGG_COLOR, match_count - end_idx,
               COLOR_RESET);
        menu_lines++;
      }
    }

    for (int i = menu_lines; i < last_menu_lines; i++)
      printf("\r\n\033[K");
    int down = menu_lines > last_menu_lines ? menu_lines : last_menu_lines;

    /* Decoupled cursor anchoring safely retreats position natively */
    if (down > 0) {
      printf("\033[%dA\r\033[2K%s", down, prompt);
      for (int i = 0; i < len; i++)
        putchar(line[i]);
      if (ghost_len)
        printf("%s%s%s", SUGG_COLOR, suggestion, COLOR_RESET);
    }
    last_menu_lines = menu_lines;
    int left = (len - pos) + ghost_len;
    if (left > 0)
      printf("\033[%dD", left);
    fflush(stdout);

    /* Read Stream Keystroke */
    char c;
    if (read(STDIN_FILENO, &c, 1) != 1) {
      disable_raw_mode();
      return 0;
    }

    switch (c) {
    case KEY_ENTER:
    case KEY_RETURN:
      if (last_menu_lines > 0) {
        for (int i = 0; i < last_menu_lines; i++)
          printf("\r\n\033[K");
        printf("\033[%dA\r\033[2K%s", last_menu_lines, prompt);
        for (int i = 0; i < len; i++)
          putchar(line[i]);
      } else if (len > pos) {
        printf("\r\033[2K%s", prompt);
        for (int i = 0; i < len; i++)
          putchar(line[i]);
      }
      printf("\n");
      reading = 0;
      break;
    case KEY_CTRL_C:
      pos = len = 0;
      line[0] = '\0';
      reading = 0;
      printf("^C\n");
      break;
    case KEY_CTRL_D:
      if (!len)
        return 0;
      if (pos < len) {
        memmove(&line[pos], &line[pos + 1], len - pos);
        len--;
      }
      break;
    case KEY_CTRL_L:
      printf("\033[H\033[2J");
      break;
    case KEY_CTRL_A:
      pos = 0;
      tab_mode = 0;
      break;
    case KEY_CTRL_E:
      pos = len;
      tab_mode = 0;
      break;
    case KEY_CTRL_U:
      memmove(&line[0], &line[pos], len - pos + 1);
      len -= pos;
      pos = 0;
      tab_mode = 0;
      break;
    case KEY_CTRL_W:
      if (pos > 0) {
        int s = pos - 1;
        while (s > 0 && line[s] == ' ')
          s--;
        while (s > 0 && line[s - 1] != ' ')
          s--;
        memmove(&line[s], &line[pos], len - pos + 1);
        len -= (pos - s);
        pos = s;
      }
      tab_mode = 0;
      break;
    case KEY_BACKSPACE:
    case KEY_CTRL_BS:
      if (pos > 0) {
        memmove(&line[pos - 1], &line[pos], len - pos + 1);
        pos--;
        len--;
      }
      tab_mode = 0;
      break;

    case KEY_TAB: {
      if (!tab_mode) {
        match_count = match_idx = 0;
        word_start_pos = pos;
        while (word_start_pos > 0 && line[word_start_pos - 1] != ' ' &&
               line[word_start_pos - 1] != '"' &&
               line[word_start_pos - 1] != '\'')
          word_start_pos--;
        char cur_w[MAX_LINE];
        strncpy(cur_w, &line[word_start_pos], pos - word_start_pos);
        cur_w[pos - word_start_pos] = '\0';
        strcpy(saved_tail, &line[pos]);

        int is_first = 1;
        for (int i = 0; i < word_start_pos; i++)
          if (line[i] != ' ' && line[i] != '"') {
            is_first = 0;
            break;
          }

        if (is_first && !strchr(cur_w, '/') && strlen(cur_w) > 0) {
          const char *blt[] = {"cd", "exit", "clear", NULL};
          for (int i = 0; blt[i]; i++) {
            if (!strncmp(blt[i], cur_w, strlen(cur_w))) {
              strcpy(matches[match_count].insert, blt[i]);
              strcpy(matches[match_count].display, blt[i]);
              matches[match_count].is_exec = 1;
              matches[match_count].is_dir = 0;
              match_count++;
            }
          }
          char *p_env = getenv("PATH"), p_copy[8192];
          if (p_env) {
            strncpy(p_copy, p_env, 8191);
            for (char *dir_t = strtok(p_copy, ":");
                 dir_t && match_count < MAX_MATCHES;
                 dir_t = strtok(NULL, ":")) {
              DIR *dir = opendir(dir_t);
              if (dir) {
                struct dirent *ent;
                while ((ent = readdir(dir)) && match_count < MAX_MATCHES) {
                  if (!strncmp(ent->d_name, cur_w, strlen(cur_w)) &&
                      strcmp(ent->d_name, ".") && strcmp(ent->d_name, "..")) {
                    int dup = 0;
                    for (int k = 0; k < match_count; k++)
                      if (!strcmp(matches[k].display, ent->d_name))
                        dup = 1;
                    if (!dup) {
                      char fpath[MAX_LINE * 2];
                      snprintf(fpath, sizeof(fpath), "%s/%s", dir_t,
                               ent->d_name);
                      struct stat st;
                      if (stat(fpath, &st) == 0 && S_ISREG(st.st_mode) &&
                          !access(fpath, X_OK)) {
                        strcpy(matches[match_count].insert, ent->d_name);
                        strcpy(matches[match_count].display, ent->d_name);
                        matches[match_count].is_exec = 1;
                        matches[match_count].is_dir = 0;
                        match_count++;
                      }
                    }
                  }
                }
                closedir(dir);
              }
            }
          }
        } else {
          char search_dir[MAX_LINE] = ".", pref[MAX_LINE] = "",
               d_path[MAX_LINE] = "";
          char *slash = strrchr(cur_w, '/');
          if (slash) {
            strncpy(d_path, cur_w, slash - cur_w + 1);
            d_path[slash - cur_w + 1] = '\0';
            strcpy(pref, slash + 1);
            if (slash > cur_w) {
              strncpy(search_dir, cur_w, slash - cur_w);
              search_dir[slash - cur_w] = '\0';
            } else
              strcpy(search_dir, "/");
          } else
            strcpy(pref, cur_w);

          if (search_dir[0] == '~') {
            char *h = getenv("HOME");
            if (h) {
              char tmp[MAX_LINE];
              snprintf(tmp, MAX_LINE, "%s%s", h, search_dir + 1);
              strcpy(search_dir, tmp);
            }
          }
          DIR *dir = opendir(search_dir);
          if (dir) {
            struct dirent *ent;
            while ((ent = readdir(dir)) && match_count < MAX_MATCHES) {
              if (ent->d_name[0] == '.' && pref[0] != '.')
                continue;
              if (!strncmp(ent->d_name, pref, strlen(pref)) &&
                  strcmp(ent->d_name, ".") && strcmp(ent->d_name, "..")) {
                char f_path[MAX_LINE];
                snprintf(f_path, MAX_LINE, "%s/%s",
                         strcmp(search_dir, "/") ? search_dir : "",
                         ent->d_name);
                struct stat st;
                int is_d = (stat(f_path, &st) == 0 && S_ISDIR(st.st_mode));
                snprintf(matches[match_count].insert, MAX_LINE, "%s%s", d_path,
                         ent->d_name);
                strcpy(matches[match_count].display, ent->d_name);
                matches[match_count].is_dir = is_d;
                matches[match_count].is_exec = !is_d && !access(f_path, X_OK);
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
            snprintf(&line[word_start_pos], MAX_LINE - word_start_pos, "%s%s%s",
                     matches[0].insert, matches[0].is_dir ? "/" : " ",
                     saved_tail);
            pos = len = strlen(line) - strlen(saved_tail);
            tab_mode = 0;
          } else {
            char lcp[MAX_LINE];
            strcpy(lcp, matches[0].insert);
            for (int i = 1; i < match_count; i++) {
              int j = 0;
              while (lcp[j] && matches[i].insert[j] &&
                     lcp[j] == matches[i].insert[j])
                j++;
              lcp[j] = '\0';
            }
            snprintf(&line[word_start_pos], MAX_LINE - word_start_pos, "%s%s",
                     lcp, saved_tail);
            pos = word_start_pos + strlen(lcp);
            len = strlen(line);
          }
        } else if (suggestion[0] && pos == len) {
          strcat(line, suggestion);
          pos = len = strlen(line);
          suggestion[0] = '\0';
        }
      } else if (match_count > 0) {
        match_idx = (match_idx + 1) % match_count;
        snprintf(&line[word_start_pos], MAX_LINE - word_start_pos, "%s%s%s",
                 matches[match_idx].insert,
                 matches[match_idx].is_dir ? "/" : "", saved_tail);
        pos = word_start_pos + strlen(matches[match_idx].insert) +
              matches[match_idx].is_dir;
        len = strlen(line);
      }
      break;
    }
    case KEY_ESC: {
      char seq[3];
      struct termios temp_raw = orig_termios;
      temp_raw.c_lflag &= ~(ECHO | ICANON | ISIG);
      temp_raw.c_cc[VMIN] = 0;
      temp_raw.c_cc[VTIME] = 1;
      tcsetattr(STDIN_FILENO, TCSANOW, &temp_raw);
      if (read(STDIN_FILENO, &seq[0], 1) == 1 && seq[0] == '[' &&
          read(STDIN_FILENO, &seq[1], 1) == 1) {
        if (seq[1] == 'A') {
          if (history_pos > 0) {
            if (history_pos == history_count)
              strcpy(current_input, line);
            strcpy(line, history[--history_pos]);
            pos = len = strlen(line);
          }
        } else if (seq[1] == 'B') {
          if (history_pos < history_count) {
            strcpy(line, ++history_pos == history_count ? current_input
                                                        : history[history_pos]);
            pos = len = strlen(line);
          }
        } else if (seq[1] == 'C') {
          if (pos < len)
            pos++;
          else if (suggestion[0]) {
            strcat(line, suggestion);
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
        }
      }
      tcsetattr(STDIN_FILENO, TCSANOW, &temp_raw);
      enable_raw_mode();
      tab_mode = 0;
      break;
    }
    default:
      if (isprint(c) && len < MAX_LINE - 1) {
        memmove(&line[pos + 1], &line[pos], len - pos + 1);
        line[pos++] = c;
        len++;
      }
      tab_mode = 0;
      break;
    }
  }
  disable_raw_mode();
  return 1;
}

/* Quote-Aware Tokenizer - Natively supports spaces inside " " or ' '  */
int parse_args(char *line, char **args, char expanded[][MAX_LINE]) {
  int argc = 0;
  char *p = line;
  while (*p && argc < MAX_ARGS - 1) {
    while (isspace((unsigned char)*p))
      p++;
    if (!*p)
      break;

    char *start = p, *w = p;
    int sq = 0, dq = 0;
    while (*p) {
      if (*p == '\'' && !dq) {
        sq = !sq;
        p++;
      } else if (*p == '"' && !sq) {
        dq = !dq;
        p++;
      } else if (isspace((unsigned char)*p) && !sq && !dq) {
        p++;
        break;
      } else
        *w++ = *p++;
    }
    *w = '\0';

    /* Expand Tildes and System Env Variables natively */
    if (start[0] == '~' && (!start[1] || start[1] == '/')) {
      char *h = getenv("HOME");
      snprintf(expanded[argc], MAX_LINE, "%s%s", h ? h : "", start + 1);
      args[argc] = expanded[argc];
    } else if (start[0] == '$' && start[1]) {
      char *v = getenv(start + 1);
      snprintf(expanded[argc], MAX_LINE, "%s", v ? v : "");
      args[argc] = expanded[argc];
    } else
      args[argc] = start;

    argc++;
  }
  args[argc] = NULL;
  return argc;
}

int main() {
  tcgetattr(STDIN_FILENO, &orig_termios);
  char line[MAX_LINE], *args[MAX_ARGS];
  static char exp_buf[MAX_ARGS][MAX_LINE];
  char prev_dir[1024] = "";

  signal(SIGINT, SIG_IGN);

  while (1) {
    /* Auto Reap Background Zombies */
    while (waitpid(-1, NULL, WNOHANG) > 0)
      ;

    if (!read_input(line))
      break;
    add_history(line);

    if (parse_args(line, args, exp_buf) == 0)
      continue;

    /* Background Execution Flags */
    int bg = 0, i = 0;
    while (args[i])
      i++;
    if (i > 0 && !strcmp(args[i - 1], "&")) {
      bg = 1;
      args[i - 1] = NULL;
    }

    if (!args[0])
      continue;

    /* Built-in Handling */
    if (!strcmp(args[0], "exit"))
      break;
    if (!strcmp(args[0], "clear")) {
      printf("\033[H\033[2J");
      last_status = 0;
      continue;
    }
    if (!strcmp(args[0], "cd")) {
      char *t = args[1] ? args[1] : getenv("HOME");
      if (args[1] && !strcmp(args[1], "-"))
        t = prev_dir[0] ? prev_dir : ".";

      char current[1024];
      getcwd(current, sizeof(current));
      if (t && chdir(t) != 0) {
        fprintf(stderr, "%smyshell: cd: %s: No such directory%s\n", ERR_COLOR,
                t, COLOR_RESET);
        last_status = 1;
      } else {
        strcpy(prev_dir, current);
        last_status = 0;
      }
      continue;
    }

    execute_cmd(args, bg);
  }
  return 0;
}
