/*
 * MVP: 
 * 1. run fd with with flags and pos args parsed. Ensure --absolute-path for now
 * 2. parse them 
 * 3. render a text buffer with them 
 * 4. support j/k/gg/G nav
 * 5. <enter> $EDITOR file or vim file
 *
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <getopt.h>
#include <curses.h>
#include <signal.h>

#define DEBUG_CHAR '-'

static void finish(int sig);
static int num_rows= 0; // keep track of all rows rendered
static int cursor_row = 0; // keep track of curr row

void __print_debug(char* cmd, int n ){
  // debug
  char debug[n];
  memset(debug,DEBUG_CHAR , (n-1));
  debug[n-1]='\0';

  printf("%s\n", debug);
  printf("cmd: %s\n", cmd);  
  printf("%s\n", debug);

}

// helper; y:row, x:col; I have no idea
// why it isn't the other way round 
void write_and_refresh(int y, int x , char* str){
  mvaddstr(y, x, str);
  refresh();
  num_rows++;
}

void move_cursor_up(int* row) {
    if (*row <= 0) return;
    (*row)--;
    move(*row, 0);
    refresh();
}

void move_cursor_down(int* row) {
    if (*row + 1 >= num_rows) return;
    (*row)++;
    move(*row, 0);
    refresh();
}

int main(int argc, char** argv)
{
  // fd params fd [FLAG] [PATTERN] [PATH]
  char flags[512] = "--absolute-path";  //always on  
  const char *pattern = "";
  const char *path = ".";

  char cmd[1024]; //store cmd for fd

  int c; // capture argv
  char* mode= "r"; // mode for popen used to run fd cmd 
                    
  // source: https://invisible-island.net/ncurses/ncurses-intro.html
  (void) signal(SIGINT, finish);      /* arrange interrupts to terminate */

  (void) initscr();      /* initialize the curses library */
  keypad(stdscr, TRUE);  /* enable keyboard mapping */
  // (void) nonl();         /* tell curses not to do NL->CR/NL on output */
  (void) noecho();         /* dont echo input */
  (void) cbreak();       /* take input chars one at a time, no wait for \n */
  // curs_set(0);  

  // parse args for fdired
  static struct option long_options[] = {
      {"hidden",      no_argument,       0, 'H'},
      {"no-ignore",   no_argument,       0, 'I'},
      {"glob",        no_argument,       0, 'g'},
      {"type",        required_argument, 0, 't'},
      {"extension",   required_argument, 0, 'e'},
      {"max-depth",   required_argument, 0, 'd'},
      {0, 0, 0, 0}
  };
  while ((c = getopt_long(argc, argv, "HIt:e:d:g", long_options, NULL)) != -1) {
      switch (c) {
      case 'H': strncat(flags, " --hidden",    sizeof(flags) - strlen(flags) - 1); break;
      case 'I': strncat(flags, " --no-ignore", sizeof(flags) - strlen(flags) - 1); break;
      case 't': strncat(flags, " --type ",     sizeof(flags) - strlen(flags) - 1);
                strncat(flags, optarg,          sizeof(flags) - strlen(flags) - 1); break;
      case 'e': strncat(flags, " --extension ",sizeof(flags) - strlen(flags) - 1);
                strncat(flags, optarg,          sizeof(flags) - strlen(flags) - 1); break;
      case 'd': strncat(flags, " --max-depth ",sizeof(flags) - strlen(flags) - 1);
                strncat(flags, optarg,          sizeof(flags) - strlen(flags) - 1); break;
      case 'g': strncat(flags, " --glob",       sizeof(flags) - strlen(flags) - 1);
                break;
      case '?': return 1;
      }
  }


  // if we use --glob, the pattern passed will be the
  // one searched on.
  if (optind < argc) pattern = argv[optind++];
  if (optind < argc) path = argv[optind];


  // build cmd for fd
  snprintf(
      cmd,
      sizeof(cmd),
      "fd %s \"%s\" %s",
      flags,
      pattern,
      path
  );

  // __print_debug(cmd, 100);
 
  // open process to run fd cmd 
  FILE* fd = popen(cmd, mode);

  if(!fd){
    fprintf(stderr, "Some error occured!");
    return -1;
  }


  // render some text 
  write_and_refresh(0, 0,"Hello, ncurses!\n");

  int row = 1; // track row
  char buffer[1024]; // store fd output 

  while (fgets(buffer, sizeof(buffer), fd) != NULL) {
    write_and_refresh(row++,0,buffer);
  }

  move(cursor_row, 0); // move cursor to top row
  refresh();
             
  // keep alive until user presses <ctrl> c 
  for (;;) {
      int c = getch(); // key event 

      // right now we only track keys
      // no user input rendered 
      switch (c) {
      case 'j':
        move_cursor_down(&cursor_row);
        // write_and_refresh(row++, 0, "[KEY]J pressed");
        break;
      case 'k':
        move_cursor_up(&cursor_row);
        // write_and_refresh(row++, 0, "[KEY]K pressed");
        break;
      }
  }

  // verify that process closes correctly 
  int status = pclose(fd);
  if (status == -1) {
      perror("pclose failed");
      return -1;
  }

  endwin(); // close window
  return 0;

}

static void finish(int sig)
{
  (void)sig;
  endwin();
  exit(0);
}
