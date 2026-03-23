/*======================================================
  myshell.c - Advanced Minimal C Shell
  Features:
   - System-Wide $PATH Command Auto-completion
   - Left/Right Arrow Free Cursor Movement & Insertion
   - Modular Keybindings Dispatcher (Easy to extend)
   - Ctrl+W / Ctrl+U to Clear Words or Entire Line
   - Live Fish-like Autosuggestions (Right Arrow to accept)
   - Tab-Completion Grid Menu (Paginated like Zsh/Fish)
   - Tab-Completion Inline Cycling & Mid-line support
   - Syntax Colored Files, Dirs, and Executables
   - Up/Down Arrow Command History Traversal
   - Native Tilde (~) Expansion for paths
======================================================*/

#include <ctype.h>
#include <dirent.h>
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
#define MAX_MATCHES                                                            \
  4096 /* Scaled up to handle massive /usr/bin/ directories safely */

/*======================================================
   MODULAR KEYBINDING DEFINITIONS (ASCII Codes)
======================================================*/
#define KEY_CTRL_A 1 /* Home Shortcut */
#define KEY_CTRL_C 3
#define KEY_CTRL_D 4
#define KEY_CTRL_E 5  /* End Shortcut */
#define KEY_CTRL_BS 8 /* Ctrl+Backspace */
#define KEY_TAB 9
#define KEY_ENTER 10
#define KEY_CTRL_L 12 /* Clear Screen */
#define KEY_RETURN 13
#define KEY_CTRL_U 21 /* Unix line clear to start */
#define KEY_CTRL_W 23 /* Unix delete previous word */
#define KEY_ESC 27
#define KEY_BACKSPACE 127

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

/*--- Struct for Tab Matches ---*/
typedef struct {
  char insert[MAX_LINE];
  char display[MAX_LINE];
  int is_dir;
  int is_exec;
} Completion;

/*--- Globals ---*/
struct termios orig_termios;
char history[MAX_HISTORY][MAX_LINE];
int history_count = 0;

/*--- Restore Original Terminal Settings ---*/
void disable_raw_mode() { tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios); }

/*--- Enable Raw Mode to Intercept Keystrokes ---*/
void enable_raw_mode() {
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

/*--- Get Ghost Autosuggestion ---*/
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
int cmp_completion(const void *a, const void *b) {
  return strcmp(((Completion *)a)->display, ((Completion *)b)->display);
}

/*--- Read Input (Handles Keys, Tabs, Arrows, Menus) ---*/
int read_input(char *line) {
  int len = 0;
  int pos = 0;
  char suggestion[MAX_LINE] = "";
  line[0] = '\0';

  char cwd[1024];
  char prompt[2048];

  int history_pos = history_count;
  char current_input[MAX_LINE] = "";

  int tab_mode = 0;
  int match_count = 0;
  int match_idx = 0;
  int menu_lines = 0;
  int last_menu_lines = 0;

  static Completion matches[MAX_MATCHES];
  int word_start_pos = 0;
  char saved_tail[MAX_LINE] = "";

  enable_raw_mode();
  int reading = 1;

  while (reading) {
    if (getcwd(cwd, sizeof(cwd)) == NULL)
      strcpy(cwd, "?");

    /* Replace HOME path with ~ safely */
    char *home = getenv("HOME");
    char display_cwd[1024];
    if (home && strncmp(cwd, home, strlen(home)) == 0) {
      snprintf(display_cwd, sizeof(display_cwd), "~%s", cwd + strlen(home));
    } else
      strcpy(display_cwd, cwd);

    /* Construct Prompt */
    snprintf(prompt, sizeof(prompt), "\r\033[2K%s%s %s❯%s ", DIR_COLOR,
             display_cwd, PROMPT_COLOR, COLOR_RESET);

    /*======================================================
       RENDER ENGINE (Prevents Screen Tearing & Artifacts)
    ======================================================*/
    printf("%s", prompt);
    for (int i = 0; i < len; i++)
      putchar(line[i]);

    int ghost_len = 0;
    if (!tab_mode && pos == len) {
      get_suggestion(line, suggestion);
      if (suggestion[0] != '\0') {
        printf("%s%s%s", SUGG_COLOR, suggestion, COLOR_RESET);
        ghost_len = strlen(suggestion);
      }
    } else
      suggestion[0] = '\0';

    /* Draw Interactive Bottom Grid Menu */
    menu_lines = 0;
    if (tab_mode && match_count > 0) {
      struct winsize w;
      int term_cols = 80;
      if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) != -1 && w.ws_col > 0)
        term_cols = w.ws_col;

      int max_len = 0;
      for (int i = 0; i < match_count; i++) {
        int l = strlen(matches[i].display);
        if (matches[i].is_dir)
          l++;
        if (l > max_len)
          max_len = l;
      }

      int col_width = max_len + 2;
      int cols = term_cols / col_width;
      if (cols == 0)
        cols = 1;

      int max_show = cols * 5; /* Limit to 5 terminal rows */
      int start_idx = (match_idx / max_show) * max_show;
      if (start_idx < 0)
        start_idx = 0;
      int end_idx = start_idx + max_show;
      if (end_idx > match_count)
        end_idx = match_count;

      for (int i = start_idx; i < end_idx; i++) {
        if ((i - start_idx) % cols == 0) {
          printf("\r\n\033[K");
          menu_lines++;
        }
        if (i == match_idx)
          printf("\033[7m"); /* Invert color for selected item */

        if (matches[i].is_dir)
          printf("%s", DIR_COLOR);
        else if (matches[i].is_exec)
          printf("%s", COLOR_GREEN);
        else
          printf("%s", COLOR_WHITE);

        char display_name[MAX_LINE];
        snprintf(display_name, sizeof(display_name), "%s%s", matches[i].display,
                 matches[i].is_dir ? "/" : "");

        int pad = col_width - strlen(display_name);
        if ((i + 1 - start_idx) % cols == 0)
          pad = 0; /* Prevent wrap break on edge */

        printf("%s", display_name);
        for (int p = 0; p < pad; p++)
          printf(" ");
        printf("\033[0m");
      }

      if (match_count > end_idx) {
        printf("\r\n\033[K %s... and %d more%s", SUGG_COLOR,
               match_count - end_idx, COLOR_RESET);
        menu_lines++;
      }
    }

    /* Wipe lingering menu lines cleanly if the menu shrank or was closed */
    for (int i = menu_lines; i < last_menu_lines; i++) {
      printf("\r\n\033[K");
    }

    /* Snap cursor safely back up to the prompt line */
    int total_down =
        (menu_lines > last_menu_lines) ? menu_lines : last_menu_lines;
    if (total_down > 0) {
      printf("\033[%dA", total_down);
      /* Screen scrolling down re-anchors the terminal. We force a redraw to fix
       * drift. */
      printf("\r\033[2K%s", prompt);
      for (int i = 0; i < len; i++)
        putchar(line[i]);
      if (ghost_len > 0)
        printf("%s%s%s", SUGG_COLOR, suggestion, COLOR_RESET);
    }
    last_menu_lines = menu_lines;

    /* Retreat cursor perfectly backwards behind any ghost text/text ahead of
     * cursor */
    int to_move_left = (len - pos) + ghost_len;
    if (to_move_left > 0)
      printf("\033[%dD", to_move_left);
    fflush(stdout);

    /*======================================================
       INPUT PHASE & DISPATCHER
    ======================================================*/
    char c;
    if (read(STDIN_FILENO, &c, 1) != 1) {
      disable_raw_mode();
      return 0;
    }

    switch (c) {
    case KEY_ENTER:
    case KEY_RETURN:
      if (last_menu_lines > 0) {
        /* Natively destroy the menu from the screen to leave a clean log */
        for (int i = 0; i < last_menu_lines; i++)
          printf("\r\n\033[K");
        printf("\033[%dA\r\033[2K%s", last_menu_lines, prompt);
        for (int i = 0; i < len; i++)
          putchar(line[i]);
        last_menu_lines = 0;
      } else if (len > pos) {
        printf("\r\033[2K%s", prompt);
        for (int i = 0; i < len; i++)
          putchar(line[i]);
      }
      printf("\n");
      reading = 0;
      break;

    case KEY_CTRL_C:
      if (last_menu_lines > 0) {
        for (int i = 0; i < last_menu_lines; i++)
          printf("\r\n\033[K");
        printf("\033[%dA\r\033[2K%s", last_menu_lines, prompt);
        for (int i = 0; i < len; i++)
          putchar(line[i]);
        last_menu_lines = 0;
      }
      printf("^C\n");
      pos = len = 0;
      line[0] = '\0';
      reading = 0;
      break;

    case KEY_CTRL_D:
      if (len == 0) {
        if (last_menu_lines > 0) {
          for (int i = 0; i < last_menu_lines; i++)
            printf("\r\n\033[K");
          printf("\033[%dA", last_menu_lines);
        }
        printf("\n");
        disable_raw_mode();
        return 0;
      } else if (pos < len) { /* Standard UNIX Forward Delete */
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
      if (pos > 0) {
        memmove(&line[0], &line[pos], len - pos + 1);
        len -= pos;
        pos = 0;
      }
      tab_mode = 0;
      break;

    case KEY_CTRL_W:
      if (pos > 0) {
        int start = pos - 1;
        while (start > 0 && line[start] == ' ')
          start--;
        while (start > 0 && line[start - 1] != ' ')
          start--;
        int del_len = pos - start;
        memmove(&line[start], &line[pos], len - pos + 1);
        pos = start;
        len -= del_len;
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
        while (word_start_pos > 0 && line[word_start_pos - 1] != ' ')
          word_start_pos--;

        char current_word[MAX_LINE];
        int w_len = pos - word_start_pos;
        strncpy(current_word, &line[word_start_pos], w_len);
        current_word[w_len] = '\0';
        strcpy(saved_tail, &line[pos]);

        int is_first_word = 1;
        for (int i = 0; i < word_start_pos; i++) {
          if (line[i] != ' ') {
            is_first_word = 0;
            break;
          }
        }

        /*--- 1. COMMAND COMPLETION ---*/
        if (is_first_word && strchr(current_word, '/') == NULL &&
            strlen(current_word) > 0) {
          const char *builtins[] = {"cd", "exit", "clear", NULL};
          for (int i = 0; builtins[i] != NULL; i++) {
            if (strncmp(builtins[i], current_word, strlen(current_word)) == 0) {
              snprintf(matches[match_count].insert, MAX_LINE, "%s",
                       builtins[i]);
              snprintf(matches[match_count].display, MAX_LINE, "%s",
                       builtins[i]);
              matches[match_count].is_dir = 0;
              matches[match_count].is_exec = 1;
              match_count++;
            }
          }

          char *path_env = getenv("PATH");
          if (path_env) {
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

                    /* Block duplicate binaries stored in both bin/ and usr/bin/
                     */
                    int is_dup = 0;
                    for (int k = 0; k < match_count; k++) {
                      if (strcmp(matches[k].display, entry->d_name) == 0) {
                        is_dup = 1;
                        break;
                      }
                    }

                    if (!is_dup) {
                      char full_path[MAX_LINE * 2];
                      snprintf(full_path, sizeof(full_path), "%s/%s", dir_token,
                               entry->d_name);
                      struct stat statbuf;
                      if (stat(full_path, &statbuf) == 0 &&
                          S_ISREG(statbuf.st_mode) &&
                          (access(full_path, X_OK) == 0)) {
                        snprintf(matches[match_count].insert, MAX_LINE, "%s",
                                 entry->d_name);
                        snprintf(matches[match_count].display, MAX_LINE, "%s",
                                 entry->d_name);
                        matches[match_count].is_dir = 0;
                        matches[match_count].is_exec = 1;
                        match_count++;
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
        /*--- 2. FILE & DIRECTORY COMPLETION ---*/
        else {
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
          } else
            strcpy(file_prefix, current_word);

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
            while ((entry = readdir(dir)) != NULL &&
                   match_count < MAX_MATCHES) {
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

                int is_directory = 0;
                int is_exec = 0;
                struct stat statbuf;
                if (stat(full_path, &statbuf) == 0) {
                  is_directory = S_ISDIR(statbuf.st_mode);
                  is_exec = !is_directory && (access(full_path, X_OK) == 0);
                }

                if (is_cd && !is_directory)
                  continue;

                snprintf(matches[match_count].insert, MAX_LINE, "%s%s",
                         dir_path, entry->d_name);
                strcpy(matches[match_count].display, entry->d_name);
                matches[match_count].is_dir = is_directory;
                matches[match_count].is_exec = is_exec;
                match_count++;
              }
            }
            closedir(dir);
          }
        }

        /*--- 3. POPULATE TAB RESULTS ---*/
        if (match_count > 0) {
          qsort(matches, match_count, sizeof(Completion), cmp_completion);

          if (match_count == 1) {
            if (word_start_pos + strlen(matches[0].insert) + 1 +
                    strlen(saved_tail) <
                MAX_LINE - 1) {
              line[word_start_pos] = '\0';
              strcat(line, matches[0].insert);
              if (matches[0].is_dir)
                strcat(line, "/");
              else if (saved_tail[0] != ' ')
                strcat(line, " ");

              pos = strlen(line);
              strcat(line, saved_tail);
              len = strlen(line);
            }
          } else {
            /* Zsh-like Behavior: Expand up to Longest Common Prefix (LCP) */
            char lcp[MAX_LINE];
            strcpy(lcp, matches[0].insert);
            for (int i = 1; i < match_count; i++) {
              int j = 0;
              while (lcp[j] && matches[i].insert[j] &&
                     lcp[j] == matches[i].insert[j])
                j++;
              lcp[j] = '\0';
            }

            if (word_start_pos + strlen(lcp) + strlen(saved_tail) <
                MAX_LINE - 1) {
              line[word_start_pos] = '\0';
              strcat(line, lcp);
              pos = strlen(line);
              strcat(line, saved_tail);
              len = strlen(line);
            }
            tab_mode = 1;
            match_idx = -1; /* First tab triggers menu, subsequent tabs cycle
                               selections */
          }
        } else if (suggestion[0] != '\0' && pos == len) {
          /* Fallback: Auto-accept suggestion if Tab is pressed and no matches
           * exist */
          if (len + strlen(suggestion) < MAX_LINE - 1) {
            strcat(line, suggestion);
            len += strlen(suggestion);
            pos = len;
            suggestion[0] = '\0';
          }
        }
      } else {
        /* Successive Tab presses gracefully cycle items inside the string
         * natively */
        if (match_count > 0) {
          match_idx = (match_idx + 1) % match_count;
          if (word_start_pos + strlen(matches[match_idx].insert) + 1 +
                  strlen(saved_tail) <
              MAX_LINE - 1) {
            line[word_start_pos] = '\0';
            strcat(line, matches[match_idx].insert);
            if (matches[match_idx].is_dir)
              strcat(line, "/");

            pos = strlen(line);
            strcat(line, saved_tail);
            len = strlen(line);
          }
        }
      }
      break;
    }

    case KEY_ESC: {
      char seq[3];
      /* Non-blocking timed read safely interprets Arrow Keys & special bindings
       * without freezing */
      struct termios temp_raw = orig_termios;
      temp_raw.c_lflag &= ~(ECHO | ICANON | ISIG);
      temp_raw.c_cc[VMIN] = 0;
      temp_raw.c_cc[VTIME] = 1; /* 100ms timeout for sequence blocks */
      tcsetattr(STDIN_FILENO, TCSANOW, &temp_raw);

      int r1 = read(STDIN_FILENO, &seq[0], 1);
      int r2 = (r1 == 1) ? read(STDIN_FILENO, &seq[1], 1) : 0;

      if (r1 == 1 && seq[0] == '[') {
        if (r2 == 1) {
          if (seq[1] == 'A') { /* Up Arrow */
            if (history_pos > 0) {
              if (history_pos == history_count)
                strcpy(current_input, line);
              history_pos--;
              strcpy(line, history[history_pos]);
              pos = len = strlen(line);
            }
          } else if (seq[1] == 'B') { /* Down Arrow */
            if (history_pos < history_count) {
              history_pos++;
              if (history_pos == history_count)
                strcpy(line, current_input);
              else
                strcpy(line, history[history_pos]);
              pos = len = strlen(line);
            }
          } else if (seq[1] == 'C') { /* Right Arrow */
            if (pos < len) {
              pos++;
            } else if (pos == len && suggestion[0] != '\0') {
              if (len + strlen(suggestion) < MAX_LINE - 1) {
                strcat(line, suggestion);
                len += strlen(suggestion);
                pos = len;
              }
              suggestion[0] = '\0';
            }
          } else if (seq[1] == 'D') { /* Left Arrow */
            if (pos > 0)
              pos--;
          } else if (seq[1] == 'H') {
            pos = 0;
          } /* Home Key */
          else if (seq[1] == 'F') {
            pos = len;
          } /* End Key */
          else if (seq[1] >= '1' && seq[1] <= '4') {
            if (read(STDIN_FILENO, &seq[2], 1) == 1 && seq[2] == '~') {
              if (seq[1] == '1')
                pos = 0;
              else if (seq[1] == '4')
                pos = len;
              else if (seq[1] == '3') { /* Delete Key (\033[3~) */
                if (pos < len) {
                  memmove(&line[pos], &line[pos + 1], len - pos);
                  len--;
                }
              }
            }
          }
        }
      } else if (r1 == 1 && seq[0] == 'O') {
        if (r2 == 1) {
          if (seq[1] == 'H')
            pos = 0;
          else if (seq[1] == 'F')
            pos = len;
        }
      }

      /* Re-lock terminal stream after sequence clears */
      tcsetattr(STDIN_FILENO, TCSANOW, &temp_raw);
      enable_raw_mode();
      tab_mode = 0;
      break;
    }

    default:
      if (isprint(c)) {
        if (len < MAX_LINE - 1) {
          memmove(&line[pos + 1], &line[pos],
                  len - pos +
                      1); /* Elegantly pushes text to the right natively */
          line[pos] = c;
          pos++;
          len++;
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
  /* Save the core terminal blueprint immediately */
  tcgetattr(STDIN_FILENO, &orig_termios);

  char line[MAX_LINE];
  char *args[MAX_ARGS];
  static char expanded_args[MAX_ARGS][MAX_LINE];

  /* Protect shell from exiting blindly on Ctrl+C */
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

    /* Secretly inject `--color=auto` for modern system outputs */
    if ((strcmp(args[0], "ls") == 0 || strcmp(args[0], "grep") == 0) &&
        i < MAX_ARGS - 2) {
      for (int k = i; k >= 1; k--)
        args[k + 1] = args[k];
      args[1] = "--color=auto";
      i++;
    }

    /*--- 2. EXECUTE SUB-PROCESS ---*/
    pid_t pid = fork();

    if (pid == 0) {
      /* Restore terminal default Ctrl+C behavior for the launched child so
       * ping/nano works */
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
