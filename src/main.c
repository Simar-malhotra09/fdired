/*
 * MVP: 
 * 1. run fd with with flags and pos args parsed. Ensure --absolute-path for now
 * 2. parse them 
 * 3. render a text buffer with them 
 * 4. support j/k/gg/G nav
 * 5. <enter> $EDITOR file or vim file
 *
 */
#include <_stdio.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <getopt.h>
#include <curses.h>
#include <signal.h>
#include <stdint.h>
#include <signal.h>

#define DEBUG_CHAR '-'
static volatile sig_atomic_t running = 1;
static void finish(int sig);
static FILE* proc_fd = NULL; // process that run fd 
                             
char** proc_result= NULL; // store (ptr to) fd results 
int proc_result_count=0; // fd results count
                         
void __print_debug(char* cmd, int n ){
  // debug
  char debug[n];
  memset(debug,DEBUG_CHAR , (n-1));
  debug[n-1]='\0';

  printf("%s\n", debug);
  printf("cmd: %s\n", cmd);  
  printf("%s\n", debug);

}


typedef struct {
  int height; // curr term height
  int width; // curr term width
  int top_row; // first row idx
  int total_rows; // # of fd results
  int curr_row; // row where cursor is
} viewport;

void __viewport_debug(viewport* v){
  printf("current height:%d and width:%d\n", v->height, v->width);
  return; 
}

void render(viewport* v){
  clear();

  // info row always pinned at y=0
  char info[64];
  snprintf(info, sizeof(info), "fd returned %d results", v->total_rows);
  mvaddstr(0, 0, info);

  int rows_fit = v->height; // height is LINES-1 so we never overwrite the info row

  for (int screen_y = 0;
       screen_y < rows_fit &&
       v->top_row + screen_y < v->total_rows;
       screen_y++) {

    int idx= v->top_row + screen_y; // the idx of the ptr which stored the correct fd ouput entry 
    int padding_left= 10;  // [idx]  padding [entry ]
    int padding_fmt_left= v->width - padding_left > 1 ? v->width - padding_left : 1;

    char fmt_proc_result[1024]; 
    snprintf(
        fmt_proc_result,
        sizeof(fmt_proc_result),
        "[%d] %-*.*s",
        idx,
        padding_fmt_left, // pad to width
        padding_fmt_left, // truncate if too long
        proc_result[idx]
    );
    // +1 to skip the info row sitting at y=0
    mvaddstr(screen_y + 1, 0,
             fmt_proc_result);
  }

  // +1 because the info row shifts everything down by one
  move(v->curr_row - v->top_row + 1, 0);
  refresh();
}

int main(int argc, char** argv)
{
  // create viewport 
  viewport v;
  // fd params fd [FLAG] [PATTERN] [PATH]
  char flags[512] = "--absolute-path";  //always on  
  const char *pattern = "";
  const char *path = ".";

  char cmd[1024]; //store cmd for fd

  int c; // capture argv
  char* mode= "r"; // mode for popen used to run fd cmd 
                    

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

  __print_debug(cmd, 100);
 
  // open process to run fd cmd 
  proc_fd= popen(cmd, mode);

  if(!proc_fd){
    fprintf(stderr, "Some error occured!");
    return -1;
  }
  // close process if open


  // source: https://invisible-island.net/ncurses/ncurses-intro.html
  (void) signal(SIGINT, finish);      /* arrange interrupts to terminate */

  (void) initscr();      /* initialize the curses library */
  keypad(stdscr, TRUE);  /* enable keyboard mapping */
  // (void) nonl();         /* tell curses not to do NL->CR/NL on output */
  (void) noecho();         /* dont echo input */
  (void) cbreak();       /* take input chars one at a time, no wait for \n */
  // scrollok(stdscr, TRUE);
  halfdelay(1);
  v.height=LINES;
  v.width=COLS;



  char buffer[2046]; // read fd output  

  while (fgets(buffer, sizeof(buffer), proc_fd) != NULL) {
    proc_result = realloc(proc_result, sizeof(char*) * (proc_result_count + 1)); // store [ptr0|ptr1..]
    proc_result[proc_result_count] = strdup(buffer); // allocate mem, return ptr to cpy
    proc_result_count++;
  }

  // initialize viewport 
  v.total_rows = proc_result_count;
  v.top_row    = 0;
  v.curr_row   = 0;
  v.height     = LINES - 1; // -1 reserves row 0 for the info line
  render(&v);
             
  char last_key =' '; // to keep track of paired strokes like `gg`

  // keep alive until user presses <ctrl> c 
  for (;;) {
    if (!running) break;
    int c = getch(); // key event 

    // right now we only track keys
    // no user input rendered 
    
    // TBD: fix the retard way of adding
    // last_key at every case
    switch (c) {
    case 'j':
      last_key=' ';
      if (v.curr_row + 1 < v.total_rows) {
        v.curr_row++;
        // scroll down when cursor hits the bottom of the visible window
        if (v.curr_row >= v.top_row + v.height)
          v.top_row++;
      }
      render(&v);
      break;
    case 'k':
      last_key=' ';
      if (v.curr_row > 0) {
        v.curr_row--;
        // scroll up when cursor moves above the visible window
        if (v.curr_row < v.top_row)
          v.top_row--;
      }
      render(&v);
      break;
    case 'G':
      last_key=' ';
      v.curr_row = v.total_rows - 1;
      // clamp top_row so the last page fills the screen
      v.top_row = v.total_rows - v.height;
      if (v.top_row < 0) v.top_row = 0;
      render(&v);
      break;

    case 'g':
      if(last_key == 'g'){
        v.curr_row = 0;
        v.top_row  = 0;
        last_key = ' '; // reset so the next lone 'g' starts a fresh sequence
        render(&v);
        break;
      }
      else{
        last_key='g';
        break;
      }
    
    // on enter keyevent 
    case '\n':
    case '\r':
    case KEY_ENTER:
      char file[1024]; // make copy of filepath 
      snprintf(file, sizeof(file),
               "%s", proc_result[v.curr_row]);

      file[strcspn(file, "\n")] = '\0'; // strip the null term 

      def_prog_mode(); // save state 
      endwin(); 
      
      // open file with nvim 
      char cmd[1200];
      snprintf(cmd, sizeof(cmd),
               "nvim \"%s\"", file);

      system(cmd);

      // on exit callback restore fdired viewport
      reset_prog_mode(); 
      refresh(); 
      render(&v);
      break;

    case 'q':
      // if(proc_open_file) pclose(proc_open_file);
      running=0;
    default:
      // ERR means halfdelay timed out with no keypress — don't break gg sequence
      if (c != ERR) last_key = ' ';
      break;
    }


  }


  // close process if open
  if (proc_fd) pclose(proc_fd);

  // free all memory pointer to by ptr_i in proc_result 
  for (int i = 0; i < proc_result_count; i++) free(proc_result[i]);
  free(proc_result); // free itself
  endwin();

  // __viewport_debug(&v);

  return 0;

}

static void finish(int sig)
{
  (void)sig;
  running = 0;
}
