/*======================================================
  myshell.c - Minimal C Shell
  Features:
   - System-Wide $PATH Command Auto-completion
   - Modular Keybindings Dispatcher (Easy to extend)
   - Ctrl+Backspace / Ctrl+U to Clear Entire Line
   - Live Fish-like Autosuggestions (Right Arrow)
   - Tab-Completion Cycling (Alphabetical Files & Dirs)
   - Strict `cd` context filtering for Directories
   - Up/Down Arrow Command History Traversal
   - Native Tilde (~) Expansion for paths
======================================================*/

#include <ctype.h>
#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define MAX_LINE 1024
#define MAX_ARGS 64
#define MAX_HISTORY 100
#define MAX_MATCHES                                                            \
  1024 /* Increased to safely handle large /usr/bin directories */

/*======================================================
   MODULAR KEYBINDING DEFINITIONS (ASCII Codes)
======================================================*/
#define KEY_CTRL_C 3
#define KEY_CTRL_D 4
#define KEY_CTRL_BS 8 /* Ctrl+Backspace (Sends ASCII 8 on most terminals) */
#define KEY_TAB 9
#define KEY_ENTER 10
#define KEY_CTRL_L 12 /* Clear Screen */
#define KEY_RETURN 13
#define KEY_CTRL_U 21 /* UNIX standard terminal clear line shortcut */
#define KEY_CTRL_W 23 /* UNIX delete word (fallback for some terminals) */
#define KEY_ESC 27
#define KEY_BACKSPACE 127 /* Standard Backspace */

/*--- ANSI Escape Colors ---*/
#define COLOR_RESET "\033[0m"
#define COLOR_GRAY "\033[90m"
#define COLOR_RED "\033[31m"
#define COLOR_GREEN "\033[32m"
#define COLOR_YELLOW "\033[33m"
#define COLOR_BLUE "\033[34m"
#define COLOR_MAGENTA "\033[35m"
#define COLOR_CYAN "\033[36m"
#define COLOR_WHITE "\033[37m"

/*--- THEME SWITCH: 1 (Ocean), 2 (Hacker), 3 (Sunset) ---*/
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

/*--- Globals ---*/
struct termios orig_termios;
char history[MAX_HISTORY][MAX_LINE];
int history_count = 0;

/*--- Restore Original Terminal Settings ---*/
void disable_raw_mode() { tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios); }

/*--- Enable Raw Mode to Intercept Keystrokes ---*/
void enable_raw_mode() {
  tcgetattr(STDIN_FILENO, &orig_termios);
  struct termios raw = orig_termios;
  raw.c_lflag &= ~(ECHO | ICANON | ISIG);
  raw.c_cc[VMIN] = 1;
  raw.c_cc[VTIME] = 0;
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

/*--- Check if a path is a directory via sys/stat.h ---*/
int is_dir(const char *path) {
  struct stat statbuf;
  if (stat(path, &statbuf) != 0)
    return 0;
  return S_ISDIR(statbuf.st_mode);
}

/*--- Add Command to History ---*/
void add_history(const char *line) {
  if (strlen(line) == 0)
    return;
  if (history_count > 0 && strcmp(history[history_count - 1], line) == 0)
    return;

  if (history_count < MAX_HISTORY) {
    strcpy(history[history_count++], line);
  } else {
    for (int i = 1; i < MAX_HISTORY; i++) {
      strcpy(history[i - 1], history[i]);
    }
    strcpy(history[MAX_HISTORY - 1], line);
  }
}

/*--- Get Ghost Autosuggestion (From History Only for Speed) ---*/
void get_suggestion(const char *input, char *suggestion) {
  suggestion[0] = '\0';
  int len = strlen(input);
  if (len == 0)
    return;

  for (int i = history_count - 1; i >= 0; i--) {
    if (strncmp(history[i], input, len) == 0 && strlen(history[i]) > len) {
      strcpy(suggestion, history[i] + len);
      return;
    }
  }
}

/*--- Sort function for Tab completion alphabetically ---*/
int cmp_str(const void *a, const void *b) {
  return strcmp((const char *)a, (const char *)b);
}

/*--- Read Input (Handles Keys, Tabs, Arrows) ---*/
int read_input(char *line) {
  int pos = 0;
  char suggestion[MAX_LINE] = "";
  line[0] = '\0';

  char cwd[1024];
  char prompt[2048];

  /*--- History & Tab Cycle States ---*/
  int history_pos = history_count;
  char current_input[MAX_LINE] = "";

  int tab_mode = 0;
  int match_count = 0;
  int match_idx = 0;
  static char matches[MAX_MATCHES][MAX_LINE];
  int word_start_pos = 0;

  enable_raw_mode();
  int reading = 1;

  while (reading) {
    if (getcwd(cwd, sizeof(cwd)) == NULL)
      strcpy(cwd, "?");

    /*--- Replace HOME path with ~ safely ---*/
    char *home = getenv("HOME");
    char display_cwd[1024];
    if (home && strncmp(cwd, home, strlen(home)) == 0) {
      snprintf(display_cwd, sizeof(display_cwd), "~%s", cwd + strlen(home));
    } else {
      strcpy(display_cwd, cwd);
    }

    /*--- \r moves to start, \033[K clears the rest of the line visually ---*/
    snprintf(prompt, sizeof(prompt), "\r\033[K%s%s %s❯%s ", DIR_COLOR,
             display_cwd, PROMPT_COLOR, COLOR_RESET);

    if (!tab_mode)
      get_suggestion(line, suggestion);
    else
      suggestion[0] = '\0'; /* Hide suggestion ghost while cycling tabs */

    /*--- Render Line ---*/
    printf("%s%s", prompt, line);

    /*--- Render Ghost Suggestion ---*/
    if (suggestion[0] != '\0') {
      printf("%s%s%s\033[%dD", SUGG_COLOR, suggestion, COLOR_RESET,
             (int)strlen(suggestion));
    }

    fflush(stdout);

    /*--- Read single keystroke ---*/
    char c;
    if (read(STDIN_FILENO, &c, 1) != 1) {
      disable_raw_mode();
      return 0;
    }

    /*======================================================
       MODULAR KEYBINDING DISPATCHER
    ======================================================*/
    switch (c) {
    case KEY_ENTER:
    case KEY_RETURN:
      printf("\n");
      reading = 0;
      break;

    case KEY_CTRL_C:
      printf("^C\n");
      pos = 0;
      line[0] = '\0';
      reading = 0;
      break;

    case KEY_CTRL_D:
      if (pos == 0) {
        printf("\n");
        disable_raw_mode();
        return 0;
      }
      break;

    case KEY_CTRL_L:
      printf("\033[H\033[2J");
      break;

    case KEY_CTRL_BS:
    case KEY_CTRL_U:
    case KEY_CTRL_W:
      pos = 0;
      line[pos] = '\0';
      tab_mode = 0;
      history_pos = history_count;
      break;

    case KEY_BACKSPACE:
      if (pos > 0) {
        pos--;
        line[pos] = '\0';
      }
      tab_mode = 0;
      break;

    case KEY_TAB: {
      if (!tab_mode) {
        match_count = 0;
        match_idx = 0;

        /* Isolate the current word being typed */
        word_start_pos = pos;
        while (word_start_pos > 0 && line[word_start_pos - 1] != ' ')
          word_start_pos--;

        char current_word[MAX_LINE];
        strcpy(current_word, &line[word_start_pos]);

        /* Determine if this is the first word (Command Context) */
        int is_first_word = 1;
        for (int i = 0; i < word_start_pos; i++) {
          if (line[i] != ' ') {
            is_first_word = 0;
            break;
          }
        }

        /*--- 1. COMMAND COMPLETION (Scans $PATH & Built-ins) ---*/
        if (is_first_word && strchr(current_word, '/') == NULL &&
            strlen(current_word) > 0) {

          /* Include Built-ins */
          const char *builtins[] = {"cd", "exit", "clear", NULL};
          for (int i = 0; builtins[i] != NULL; i++) {
            if (strncmp(builtins[i], current_word, strlen(current_word)) == 0) {
              snprintf(matches[match_count++], MAX_LINE, "%s", builtins[i]);
            }
          }

          /* Scan OS $PATH for Executables */
          char *path_env = getenv("PATH");
          if (path_env) {
            /* Copy so strtok doesn't ruin the real PATH pointer */
            char path_copy[8192];
            strncpy(path_copy, path_env, sizeof(path_copy) - 1);
            path_copy[sizeof(path_copy) - 1] = '\0';

            char *dir_token = strtok(path_copy, ":");
            while (dir_token != NULL && match_count < MAX_MATCHES) {
              DIR *dir = opendir(dir_token);
              if (dir) {
                struct dirent *entry;
                while ((entry = readdir(dir)) != NULL &&
                       match_count < MAX_MATCHES) {
                  if (strncmp(entry->d_name, current_word,
                              strlen(current_word)) == 0) {
                    if (strcmp(entry->d_name, ".") == 0 ||
                        strcmp(entry->d_name, "..") == 0)
                      continue;

                    /* Ensure no duplicates (e.g., duplicate program in /bin and
                     * /usr/bin) */
                    int is_dup = 0;
                    for (int k = 0; k < match_count; k++) {
                      if (strcmp(matches[k], entry->d_name) == 0) {
                        is_dup = 1;
                        break;
                      }
                    }

                    if (!is_dup && match_count < MAX_MATCHES) {
                      char full_path[MAX_LINE * 2];
                      snprintf(full_path, sizeof(full_path), "%s/%s", dir_token,
                               entry->d_name);

                      /* Strictly verify it is an executable file and not a
                       * directory/log file */
                      struct stat statbuf;
                      if (stat(full_path, &statbuf) == 0 &&
                          S_ISREG(statbuf.st_mode) &&
                          (access(full_path, X_OK) == 0)) {
                        snprintf(matches[match_count++], MAX_LINE, "%s",
                                 entry->d_name);
                      }
                    }
                  }
                }
                closedir(dir);
              }
              dir_token = strtok(NULL, ":");
            }
          }
        }
        /*--- 2. FILE & DIRECTORY COMPLETION (Fallback) ---*/
        else {
          /* Safely isolate the very first word in the line to check for "cd" */
          char first_word[MAX_LINE] = "";
          int fw_len = 0;
          while (line[fw_len] != '\0' && line[fw_len] != ' ' &&
                 fw_len < MAX_LINE - 1) {
            first_word[fw_len] = line[fw_len];
            fw_len++;
          }
          first_word[fw_len] = '\0';
          int is_cd = (strcmp(first_word, "cd") == 0);

          char dir_path[MAX_LINE] = "", file_prefix[MAX_LINE] = "",
               search_dir[MAX_LINE] = ".";
          char *last_slash = strrchr(current_word, '/');

          if (last_slash) {
            int dir_len = last_slash - current_word + 1;
            strncpy(dir_path, current_word, dir_len);
            dir_path[dir_len] = '\0';
            strcpy(file_prefix, last_slash + 1);
            if (dir_len > 1) {
              strncpy(search_dir, current_word, dir_len - 1);
              search_dir[dir_len - 1] = '\0';
            } else
              strcpy(search_dir, "/");
          } else {
            strcpy(file_prefix, current_word);
            strcpy(search_dir, ".");
            strcpy(dir_path, "");
          }

          char expanded_search_dir[MAX_LINE];
          if (search_dir[0] == '~' &&
              (search_dir[1] == '/' || search_dir[1] == '\0')) {
            char *home = getenv("HOME");
            if (home)
              snprintf(expanded_search_dir, sizeof(expanded_search_dir), "%s%s",
                       home, search_dir + 1);
            else
              strcpy(expanded_search_dir, search_dir);
          } else
            strcpy(expanded_search_dir, search_dir);

          DIR *dir = opendir(expanded_search_dir);
          if (dir) {
            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL) {
              if (entry->d_name[0] == '.' && file_prefix[0] != '.')
                continue;
              if (strcmp(entry->d_name, ".") == 0 ||
                  strcmp(entry->d_name, "..") == 0)
                continue;

              if (strncmp(entry->d_name, file_prefix, strlen(file_prefix)) ==
                  0) {
                char full_path[MAX_LINE];
                if (strcmp(expanded_search_dir, "/") == 0)
                  snprintf(full_path, sizeof(full_path), "/%s", entry->d_name);
                else
                  snprintf(full_path, sizeof(full_path), "%s/%s",
                           expanded_search_dir, entry->d_name);

                int is_directory = is_dir(full_path);
                if (is_cd && !is_directory)
                  continue;

                if (match_count < MAX_MATCHES) {
                  snprintf(matches[match_count], MAX_LINE, "%s%s%s", dir_path,
                           entry->d_name, is_directory ? "/" : "");
                  match_count++;
                }
              }
            }
            closedir(dir);
          }
        }

        /* Inject First Tab Match into the Prompt */
        if (match_count > 0) {
          qsort(matches, match_count, MAX_LINE, cmp_str);
          tab_mode = 1;
          line[word_start_pos] = '\0';
          strcat(line, matches[0]);
          pos = strlen(line);
        } else if (suggestion[0] != '\0') {
          if (pos + strlen(suggestion) < MAX_LINE - 1) {
            strcat(line, suggestion);
            pos += strlen(suggestion);
          }
        }
      } else {
        /* Cycle Inline Results on Subsequent Tab hits */
        if (match_count > 0) {
          match_idx = (match_idx + 1) % match_count;
          line[word_start_pos] = '\0';
          strcat(line, matches[match_idx]);
          pos = strlen(line);
        }
      }
      break;
    }

    case KEY_ESC: {
      char seq[2];
      if (read(STDIN_FILENO, &seq[0], 1) == 0 ||
          read(STDIN_FILENO, &seq[1], 1) == 0)
        break;

      if (seq[0] == '[') {
        if (seq[1] == 'A') { /* Up Arrow */
          if (history_pos > 0) {
            if (history_pos == history_count)
              strcpy(current_input, line);
            history_pos--;
            strcpy(line, history[history_pos]);
            pos = strlen(line);
          }
        } else if (seq[1] == 'B') { /* Down Arrow */
          if (history_pos < history_count) {
            history_pos++;
            if (history_pos == history_count)
              strcpy(line, current_input);
            else
              strcpy(line, history[history_pos]);
            pos = strlen(line);
          }
        } else if (seq[1] == 'C') { /* Right Arrow (Accept Autosuggest) */
          if (suggestion[0] != '\0') {
            if (pos + strlen(suggestion) < MAX_LINE - 1) {
              strcat(line, suggestion);
              pos += strlen(suggestion);
            }
            suggestion[0] = '\0';
          }
        }
      }
      tab_mode = 0;
      break;
    }

    default:
      if (isprint(c)) {
        if (pos < MAX_LINE - 1) {
          line[pos++] = c;
          line[pos] = '\0';
        }
      }
      tab_mode = 0;
      break;
    }
  }

  disable_raw_mode();
  return 1;
}

int main() {
  char line[MAX_LINE];
  char *args[MAX_ARGS];
  static char expanded_args[MAX_ARGS][MAX_LINE];

  signal(SIGINT, SIG_IGN);

  while (1) {
    if (!read_input(line))
      break;

    add_history(line);

    /*--- 1. PARSE & EXPAND TILDE (~) ---*/
    int i = 0;
    char *token = strtok(line, " \t\r\n\a");
    while (token != NULL) {
      if (token[0] == '~' && (token[1] == '/' || token[1] == '\0')) {
        char *home = getenv("HOME");
        if (home) {
          snprintf(expanded_args[i], MAX_LINE, "%s%s", home, token + 1);
          args[i] = expanded_args[i];
        } else {
          args[i] = token;
        }
      } else {
        args[i] = token;
      }
      i++;
      token = strtok(NULL, " \t\r\n\a");
    }
    args[i] = NULL;

    if (args[0] == NULL)
      continue;

    /*--- BUILT-IN COMMANDS ---*/
    if (strcmp(args[0], "exit") == 0)
      break;

    if (strcmp(args[0], "clear") == 0) {
      printf("\033[H\033[2J");
      continue;
    }

    if (strcmp(args[0], "cd") == 0) {
      if (args[1] == NULL) {
        char *home = getenv("HOME");
        if (home) {
          if (chdir(home) != 0)
            perror("myshell: cd");
        }
      } else {
        if (chdir(args[1]) != 0) {
          fprintf(stderr, "%smyshell: cd: %s: No such file or directory%s\n",
                  ERR_COLOR, args[1], COLOR_RESET);
        }
      }
      continue;
    }

    /*--- 2. EXECUTE ---*/
    pid_t pid = fork();

    if (pid == 0) {
      signal(SIGINT, SIG_DFL);
      if (execvp(args[0], args) == -1) {
        fprintf(stderr, "%smyshell: command not found: %s%s\n", ERR_COLOR,
                args[0], COLOR_RESET);
      }
      exit(EXIT_FAILURE);
    } else if (pid < 0) {
      perror("myshell: fork");
    } else {
      waitpid(pid, NULL, 0);
    }
  }

  return 0;
}
