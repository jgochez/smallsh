#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <err.h>
#include <errno.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/wait.h>

// CONSTANTS
#ifndef MAX_WORDS
#define MAX_WORDS 512
#endif
#define MAX_STRING_LENGTH 256
#define MAX_BG_PROCESSES 256
// VARIABLES
char *words[MAX_WORDS] = {0};
pid_t latest_bg_pid = -1;
pid_t spawnPid;
int latest_exit_status = 0;
int bg_status = 0;
int should_exit = 0;
int CURRENT_EXIT_STATUS = 0;
int is_reading_input = 0;
struct sigaction SIGINT_action = {0}, OLD_ACT_INT;
struct sigaction SIGTSTP_action = {0}, OLD_ACT_TSTP;
// FUNCTIONS
size_t wordsplit(char const *line);
char * expand(char const *word);
void run_command(char *words[], size_t nwords, int bg_status);
void bg_proc();
void handle_SIGINT(int signo);
void handle_SIGTSTP(int signo);
void reset_signals();
void handle_signals(int reading_input);

/*
*
*
*
*
*
*
*
*
*/

/* Handling Background Processes */
void bg_proc() {
  int proc_stat;
  pid_t pid;
  pid = waitpid(latest_bg_pid, &proc_stat, WNOHANG | WUNTRACED);
  // Loop through all child processes
  while (pid > 0) {
    if (WIFEXITED(proc_stat)) {
      // Process exited normally
      fprintf(stderr, "Child process %jd done. Exit status %d.\n", (intmax_t)pid, WEXITSTATUS(proc_stat));
    } else if (WIFSIGNALED(proc_stat)) {
      // Process was terminated by a signal
      fprintf(stderr, "Child process %jd done. Signaled %d.\n", (intmax_t)pid, WTERMSIG(proc_stat));
    } else if (WIFSTOPPED(proc_stat)) {
      // Process was stopped, now continue it
      fprintf(stderr, "Child process %jd stopped. Continuing.\n", (intmax_t)pid);
      kill(pid, SIGCONT); // Continue the stopped process
    }
    pid = waitpid(latest_bg_pid, &proc_stat, WNOHANG | WUNTRACED);
  }
}

/* Handle Signals */
void handle_signals(int reading_input){
  is_reading_input = reading_input;
  // handle ctrl-C
	if (reading_input) {
    SIGINT_action.sa_handler = handle_SIGINT; 
    sigfillset(&SIGINT_action.sa_mask);
	  SIGINT_action.sa_flags = 0;
    sigaction(SIGINT, &SIGINT_action, NULL);
  } else {
    SIGINT_action.sa_handler = SIG_IGN;
    sigfillset(&SIGINT_action.sa_mask);
	  SIGINT_action.sa_flags = 0;
    sigaction(SIGINT, &SIGINT_action, &OLD_ACT_INT);
  }
	
  // handle ctrl-Z
	SIGTSTP_action.sa_handler = SIG_IGN;
	sigfillset(&SIGTSTP_action.sa_mask);
	SIGTSTP_action.sa_flags = 0;
  sigaction(SIGTSTP, &SIGTSTP_action, &OLD_ACT_TSTP);
}

/* Reset Signals */
void reset_signals(){
 // reset ctrl-C
  sigaction(SIGINT, &OLD_ACT_INT, NULL);

  // reset ctrl-Z
  sigaction(SIGTSTP, &OLD_ACT_TSTP, NULL);
}

/* Handling SIGINT  */
void handle_SIGINT(int signo){}

/* Handling SIGTSTP */
void handle_SIGTSTP(int signo){}

/* Helper function that calls wordsplit and expand functions */
size_t wordsplit_and_expand(char *line) {
  size_t nwords = wordsplit(line);
  for (size_t i = 0; i < nwords; ++i) { 
    char *exp_word = expand(words[i]);
    free(words[i]);
    words[i] = exp_word;
  }
  return nwords;
}

/* Splits a string into words delimited by whitespace. Recognizes
 * comments as '#' at the beginning of a word, and backslash escapes.
 *
 * Returns number of words parsed, and updates the words[] array
 * with pointers to the words, each as an allocated string.
 */
size_t wordsplit(char const *line) {
  size_t wlen = 0;
  size_t wind = 0;

  char const *c = line;
  for (;*c && isspace(*c); ++c); /* discard leading space */

  for (; *c;) {
    if (wind == MAX_WORDS) break;
    /* read a word */
    if (*c == '#') break;
    for (;*c && !isspace(*c); ++c) {
      if (*c == '\\') ++c;
      void *tmp = realloc(words[wind], sizeof **words * (wlen + 2));
      if (!tmp) err(1, "realloc");
      words[wind] = tmp;
      words[wind][wlen++] = *c; 
      words[wind][wlen] = '\0';
    }
    ++wind;
    wlen = 0;
    for (;*c && isspace(*c); ++c);
  }
  return wind;
}

/* Find next instance of a parameter within a word. Sets
 * start and end pointers to the start and end of the parameter
 * token.
 */
char
param_scan(char const *word, char const **start, char const **end)
{
  static char const *prev;
  if (!word) word = prev;
  
  char ret = 0;
  *start = 0;
  *end = 0;
  for (char const *s = word; *s && !ret; ++s) {
    s = strchr(s, '$');
    if (!s) break;
    switch (s[1]) {
    case '$':
    case '!':
    case '?':
      ret = s[1];
      *start = s;
      *end = s + 2;
      break;
    case '{':;
      char *e = strchr(s + 2, '}');
      if (e) {
        ret = s[1];
        *start = s;
        *end = e + 1;
      }
      break;
    }
  }
  prev = *end;
  return ret;
}

/* Simple string-builder function. Builds up a base
 * string by appending supplied strings/character ranges
 * to it.
 */
char *
build_str(char const *start, char const *end)
{
  static size_t base_len = 0;
  static char *base = 0;

  if (!start) {
    /* Reset; new base string, return old one */
    char *ret = base;
    base = NULL;
    base_len = 0;
    return ret;
  }
  /* Append [start, end) to base string 
   * If end is NULL, append whole start string to base string.
   * Returns a newly allocated string that the caller must free.
   */
  size_t n = end ? end - start : strlen(start);
  size_t newsize = sizeof *base *(base_len + n + 1);
  void *tmp = realloc(base, newsize);
  if (!tmp) err(1, "realloc");
  base = tmp;
  memcpy(base + base_len, start, n);
  base_len += n;
  base[base_len] = '\0';

  return base;
}

/* Expands all instances of $! $$ $? and ${param} in a string 
 * Returns a newly allocated string that the caller must free
 */
char *
expand(char const *word){
  char const *pos = word;
  char const *start, *end;
  char c = param_scan(pos, &start, &end);
  build_str(NULL, NULL);
  build_str(pos, start);

  // handle expansions
  while (c) {
    if (c == '!') {  // obtain value of latest background process ID 
      char bgpid_str[MAX_STRING_LENGTH];
      snprintf(bgpid_str, sizeof(bgpid_str), "%d", latest_bg_pid);
      build_str(bgpid_str, NULL);
    }else if (c == '$') {  // obtain process ID of current process
      char pid_str[MAX_STRING_LENGTH];
      snprintf(pid_str, sizeof(pid_str), "%d", getpid());
      build_str(pid_str, NULL);
    }else if (c == '?') {   // obtain the latest exit status of process
      char stat_str[MAX_STRING_LENGTH];
      snprintf(stat_str, sizeof(stat_str), "%d", latest_exit_status);
      build_str(stat_str, NULL);
    }else if (c == '{') {    // obtain value of environment variable 
      char environ_str[MAX_STRING_LENGTH];
      int envNameLength = end - start - 3; // -3 to remove '${}' 
      char environment[envNameLength + 1]; // +1 for null terminator
      snprintf(environment, sizeof(environment), "%.*s", envNameLength, start + 2);
      char* envValue = getenv(environment);
      if (envValue != NULL) {
        snprintf(environ_str, sizeof(environ_str), "%s", envValue);
      } else {
        environ_str[0] = '\0'; // if environment not found
      }
      build_str(environ_str, NULL);
    }
    pos = end;
    c = param_scan(pos, &start, &end);
    build_str(pos, start);
  }
  return build_str(start, NULL);
}


/* Handle redirection: "<", ">", & ">>"
* 1) Extract file name
* 2) Remove redirection and file name
* 3) Manage file descriptors: read/write -> duplicate -> close
 */
void handle_redirection(char **words, size_t *nwords) {
  char *input_redirect = NULL;
  char *output_redirect = NULL;
  int flag_mode = 0; // ">" -> TRUNC | ">>" -> APPEND

  // Iterate to handle multiple redirections
  for (size_t curr = 0; curr < *nwords; ++curr) {
    int next;
    if (strcmp(words[curr], "<") == 0) {
      next = curr + 1;
      if (next < *nwords) {            // check if next index is valid
        input_redirect = words[next];  // if yes, then assign as redirect 
        // Manage file descriptors
        if (input_redirect != NULL) {
          int input_fd = open(input_redirect, O_RDONLY); // read the file name
          if (input_fd == -1) {
            // perror("open input file");
            exit(EXIT_FAILURE);
          }
          if (dup2(input_fd, 0) == -1) {    // duplicate input file descriptor
            // perror("dup2 input");
            exit(EXIT_FAILURE);
          }
          if (close(input_fd) == -1) {     // close file descriptor
            // perror("close input file");
            exit(EXIT_FAILURE);
          }  
        }
      }
    } else if (strcmp(words[curr], ">") == 0) {
      next = curr + 1;
      if (next< *nwords) {              // check if next index is valid
        output_redirect = words[next];  // if yes, then assign as redirect
        flag_mode = 0;                  // 0 = O_TRUNC   
        // Manage file descriptors
        if (output_redirect != NULL) {
          int output_fd;
          // handle flag selection: 1 = O_APPEND | 0 = O_TRUNC
          int flags;
          if (flag_mode == 1){flags = O_WRONLY | O_CREAT | O_APPEND;}
          if(flag_mode == 0){flags = O_WRONLY | O_CREAT | O_TRUNC;}
          
          output_fd = open(output_redirect, flags, 0777);  // write to file name
          if (output_fd == -1) {
            // perror("open output file");
            exit(EXIT_FAILURE);
          }
          if (dup2(output_fd, 1) == -1) {    // duplicate output file descriptor
            // perror("dup2 output");
            exit(EXIT_FAILURE);
          }
          if (close(output_fd) == -1) {     // close output file descritor
            // perror("close output file");
            exit(EXIT_FAILURE);
          }
        }
      }
    } else if (strcmp(words[curr], ">>") == 0) {
      next = curr + 1;
      if (next < *nwords) {             // check if next index is valid
        output_redirect = words[next];  // if yes, then assign as redirect
        flag_mode = 1;                  // 1 = O_APPEND
        // Manage file descriptors
        if (output_redirect != NULL) {
          int output_fd;
          // handle flag selection: 1 = O_APPEND | 0 = O_TRUNC
          int flags;
          if (flag_mode == 1){flags = O_WRONLY | O_CREAT | O_APPEND;}
          if(flag_mode == 0){flags = O_WRONLY | O_CREAT | O_TRUNC;}
          
          output_fd = open(output_redirect, flags, 0777);  // write to file name
          if (output_fd == -1) {
            // perror("open output file");
            exit(EXIT_FAILURE);
          }
          if (dup2(output_fd, 1) == -1) {    // duplicate output file descriptor
            // perror("dup2 output");
            exit(EXIT_FAILURE);
          }
          if (close(output_fd) == -1) {     // close output file descritor
            // perror("close output file");
            exit(EXIT_FAILURE);
          }
        }
      }
    }
  }

  // Remove redirection and file name
  size_t iter_arr = 0;
  size_t new_index = 0; 

  while (iter_arr < *nwords) {
    if (strcmp(words[iter_arr], "<") == 0 || strcmp(words[iter_arr], ">") == 0 || strcmp(words[iter_arr], ">>") == 0) {
      // Skip this symbol and the associated filename
      if (iter_arr + 1 < *nwords) { // Make sure there is a next word (filename) to skip
        iter_arr += 2; // Skip the redirection symbol and the filename
      } else {
       exit(EXIT_FAILURE); // ERROR if no word follows the redirection symbol
      }
    } else {
      // Copy the word to the updated position if necessary
      if (new_index != iter_arr) { // Avoid unnecessary self-assignment
        words[new_index] = words[iter_arr];
      }
      new_index++;
      iter_arr++;
    }
  }
  // Update the number of words to the new, compacted size
  *nwords = new_index;
  // Null-terminate the indexes where values were shifted away from
  for (size_t i = *nwords; i < MAX_WORDS; i++){
    words[i] = NULL;
  }
}

/* Run all commands in this function:
* 1) empty command
* 2) exit commandhandle
* 3) cd command
* 4) non built-in command
 */
void run_command(char *words[], size_t nwords, int bg_status) {
  
  // handles empty input
  if (nwords == 0) {
    return;
  }

  // Check for background command
  if (nwords > 0 && strcmp(words[nwords - 1], "&") == 0) {
    bg_status = 1;          
    words[nwords - 1] = NULL; 
    nwords--;                 
    }   

  // exit command
  if (strcmp(words[0], "exit") == 0) {
    CURRENT_EXIT_STATUS = latest_exit_status;
    should_exit = 1;

    if (nwords == 1) {
      exit(EXIT_SUCCESS);
    } else if (nwords == 2) {
      int val = atoi(words[1]); // assuming second argument is an int
      exit(val);
    } else {
      // Error: More than one argument
      // fprintf(stderr, "exit: too many arguments\n");
      exit(1); 
    }
        

  // cd command
  } else if (strcmp(words[0], "cd") == 0) {
      const char *path;
      if (nwords == 1) {
        // cd alone = HOME
        path = getenv("HOME");
      } else if (nwords == 2) {
        // path corresponding to "cd"
        path = words[1];
      } else {
        // fprintf(stderr, "cd: too many arguments\n");
        exit(1);  
      }
      // Navigate dir: use man 2 chdir
      int dir_navig_status = chdir(path);
      if (dir_navig_status == -1) {
        // perror("cd: No such file or directory");
        exit(1);
      }

  // non built-in command
  } else {
    spawnPid = fork();
    switch(spawnPid){
      case -1: 
        // perror("fork()\n");
        exit(-1);
      case 0: 
        reset_signals(); // reset SIGINT & SIGTSTP
        handle_redirection(words, &nwords);   
        execvp(words[0], words);
        // perror("execvp");
        exit(EXIT_FAILURE);
      default: 
        // Foreground process
        if (bg_status == 0) { 
          int childExitStatus;
          waitpid(spawnPid, &childExitStatus, WUNTRACED); 
          if (WIFEXITED(childExitStatus)) { // Process exited normally
            latest_exit_status = WEXITSTATUS(childExitStatus);
            // printf("Foreground process %d exited, status=%d\n", spawnPid, latest_exit_status);
          } else if (WIFSTOPPED(childExitStatus)) { // Continue the stopped process
            fprintf(stderr, "Child process %jd stopped. Continuing.\n", (intmax_t)spawnPid);
            kill(spawnPid, SIGCONT);
            latest_bg_pid = (intmax_t)spawnPid;
          } else if (WIFSIGNALED(childExitStatus)) { // Process was killed by a signal
            latest_exit_status = 128 + WTERMSIG(childExitStatus);
            // printf("Foreground process %d killed by signal %d\n", spawnPid, latest_exit_status);
          }
        } else {
          // Background process: 
          latest_bg_pid = spawnPid;
        }       
    } 
  }
}

/* Main function that toggles between interactive and non-interactive modes */
  int main(int argc, char *argv[]) {
    FILE *input = stdin; 
    char *line = NULL;
    size_t n = 0;
    int isInteractive; 
    
    // Deciding Mode
    if (argc == 1){
      isInteractive = 1; // ./testscripts -> 1 arguments  
    }else{
      isInteractive = 0; // ./smallsh [command1] [command2] etc.. -> 2+ arguments
    }

    // Non Interactive Mode
    if (!isInteractive) {
      input = fopen(argv[1], "re");
      if (!input) {
        // perror(argv[1]);
        exit(EXIT_FAILURE);
      }
    }

    for (;;) {
      bg_proc();
      if (isInteractive) {handle_signals(1);} // Allow SIGINT while reading input & ignore SIGTSTP
      // Interactive Mode
      if (isInteractive) {
        fprintf(stderr, "%s", getenv("PS1")); // stdout gave errors
        }
      if (getline(&line, &n, input) == -1) {
        if (feof(input)) { // End of file reached
          free(line);
          if (!isInteractive) {
            fclose(input); // Close the script file if it was opened
          }
          exit(EXIT_SUCCESS); // Exit the loop
        }
        if (errno == EINTR) {
          clearerr(input); // Clear error and continue
          continue;
        }
        // perror("Error reading input");
        exit(EXIT_FAILURE);
      }
      if (isInteractive){handle_signals(0);} // Handle SIGINT & ignore SIGTSTP

      // Process the command
      size_t nwords = wordsplit_and_expand(line);
      run_command(words, nwords, bg_status);

      // Clean up
      free(line);
      line = NULL;
      n = 0;
      bg_status = 0;
      for (size_t i = 0; i < MAX_WORDS; i++) {
        free(words[i]);
        words[i] = NULL;
      }
    }
  }

