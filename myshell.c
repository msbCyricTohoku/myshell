#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_LINE 1024
#define MAX_ARGS 64

int main() {
  char line[MAX_LINE];
  char *args[MAX_ARGS];

  while (1) {
    printf("myshell> ");

    if (!fgets(line, MAX_LINE, stdin)) break; 
    
    line[strcspn(line, "\n")] = 0;

    int i = 0;
    char *token = strtok(line, " ");

    while (token != NULL) {
      args[i++] = token;
      token = strtok(NULL, " ");
    }
    args[i] = NULL; // execvp requires a null-terminated array

    if (args[0] == NULL) continue;

    if (strcmp(args[0], "exit") == 0) break; 
    
    if (strcmp(args[0], "cd") == 0) {
      if (args[1] == NULL) {
        fprintf(stderr, "myshell: expected argument to \"cd\"\n");
      } else {
        if (chdir(args[1]) != 0) {
          perror("myshell");
        }
      }
      continue;
    }

    pid_t pid = fork();

    if (pid == 0) {
      if (execvp(args[0], args) == -1) {
        perror("myshell");
      }
      exit(EXIT_FAILURE);
    } else if (pid < 0) {
      perror("myshell");
    } else {
      waitpid(pid, NULL, 0);
    }
  }

  return 0;
}
