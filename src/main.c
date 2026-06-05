#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <getopt.h>
#include <curses.h>
#include <signal.h>
#include <stdint.h>
#include <signal.h>
#include <stdarg.h>

static volatile sig_atomic_t running = 1;
static volatile sig_atomic_t resized = 0;

FILE*  proc_fd= NULL;          // fd returned by popen to run cmd
char** proc_result= NULL;     // caputre result from proc_fd
int    proc_result_count = 0; // # of files returned

typedef struct {
  int height;     // usable rows between header and status bar
  int width;      // terminal columns
  int top_row;    // first visible result index
  int total_rows; // total fd results
  int curr_row;   // selected result index
} viewport;

typedef enum {
  FD,        // returns newline sep files
  GREP,      // needs the -L flag 
  RG,        // needs the -l flag 
} AVAILABLE_CMDS;


/* 
 * Since I want to keep this project limited to one file
 * forward decalre all fns
 * */

// signal handlers
static void finish(int sig);
static void handle_resize(int sig);

// validate cmd 
// since the point of this tool is to render 
// a dired like text buffer, we can only support 
// cmd which return a list of files seperated by newlines
// either natively or with a flag  
//
//
// rendering logic
static void draw_entry(int screen_y, int col, const char *raw, int raw_len, int is_sel, int width); 
void render(viewport* v);


// Draw a single result row
static void draw_entry(int screen_y, int col, const char *raw, int raw_len, int is_sel, int width) {

  int avail = width - col;
  if (avail <= 0) return;

  if (is_sel) attron(A_REVERSE);

  // locate last '/' 
  int slash = raw_len - 1;
  while (slash > 0 && raw[slash] != '/') slash--;

  if (slash > 0) {
    int dir_len  = slash + 1;   // split into dir and base
    int base_len = raw_len - dir_len;
    int total    = dir_len + base_len;

    if (total <= avail) {
      // everything fits
      if (!is_sel) attron(A_DIM);
      mvaddnstr(screen_y, col, raw, dir_len);
      if (!is_sel) attroff(A_DIM);
      addnstr(raw + dir_len, base_len);
      for (int p = total; p < avail; p++) addch(' ');

    } else if (base_len + 4 <= avail) {
      // dir too long: ".../[tail]basename"
      int tail = avail - base_len - 4; // 4 = strlen("…/")
      if (!is_sel) attron(A_DIM);
      mvaddnstr(screen_y, col, ".../", 4);
      if (tail > 0) addnstr(raw + dir_len - tail, tail);
      if (!is_sel) attroff(A_DIM);
      addnstr(raw + dir_len, base_len);

    } else {
      // basename alone too long: truncate it
      mvaddnstr(screen_y, col, raw + dir_len, avail);
    }
  } else {
    int show = raw_len < avail ? raw_len : avail;
    mvaddnstr(screen_y, col, raw, show);
    for (int p = show; p < avail; p++) addch(' ');
  }

  if (is_sel) attroff(A_REVERSE);
}

void render(viewport *v) {
  clear();

  // list # of result by fd
  char header[1024];
  snprintf(header, sizeof(header), " fdired  %d result%s",
           v->total_rows, v->total_rows == 1 ? "" : "s");
  attron(A_BOLD);
  mvaddstr(0, 0, header);
  attroff(A_BOLD);

  if (v->total_rows == 0) {
    attron(A_DIM);
    mvaddstr(2, 2, "no results");
    attroff(A_DIM);
  } else {
    for (int sy = 0; sy < v->height && v->top_row + sy < v->total_rows; sy++) {
      int idx    = v->top_row + sy;
      int is_sel = (idx == v->curr_row);
      char *raw  = proc_result[idx];
      int   rlen = strlen(raw);
      if (rlen > 0 && raw[rlen - 1] == '\n') rlen--;

      draw_entry(sy + 1, 1, raw, rlen, is_sel, v->width);
    }
  }

  // footer status bar
  char status[1024];
  int slen = snprintf(status, sizeof(status),
                      " [%d/%d]  j%c  k%c  gg top  G end  enter open  q quit",
                      v->total_rows > 0 ? v->curr_row + 1 : 0,
                      v->total_rows,
                      (char)40, (char)38);
  // right-pad to full width
  while (slen < v->width && slen < (int)sizeof(status) - 1) status[slen++] = ' ';
  status[slen] = '\0';

  attron(A_REVERSE);
  mvaddstr(LINES - 1, 0, status);
  attroff(A_REVERSE);

  if (v->total_rows > 0)
    move(v->curr_row - v->top_row + 1, 1);

  refresh();
}

#define MEM_CHUNK 1024

typedef struct {
  const char **items;
  size_t count;
  size_t capacity;
} Cmd;

int cmd_append(Cmd *cmd, const char *arg) {
  if (cmd->count >= cmd->capacity) {
    cmd->capacity += MEM_CHUNK;
    cmd->items = realloc(cmd->items, cmd->capacity * sizeof(char *));
    if (!cmd->items) {
      fprintf(stderr, "Some error with memory allocation\n");
      return 1;
    }
  }
  cmd->items[cmd->count++] = arg;
  return 0;
}

int main(int argc, char **argv) {
  viewport v;
  Cmd cmd = {0};

  // first arg is `./fdired`; no need to capture
  for (int i = 1; i < argc; ++i) {
    cmd_append(&cmd, argv[i]);
  }

  // calc total size needed for proc_cmd
  size_t total_len = 0;
  for (size_t i = 0; i < cmd.count; ++i)
    total_len += strlen(cmd.items[i]) + 1;

  // init proc_cmd with that size
  char proc_cmd[total_len];
  proc_cmd[0] = '\0';
  
  // capture cmd to run with popen
  for (size_t i = 0; i < cmd.count; ++i) {
    strcat(proc_cmd, cmd.items[i]);
    if (i < cmd.count - 1)
      strcat(proc_cmd, " ");
  }

  free(cmd.items);

  proc_fd = popen(proc_cmd, "r");
  if (!proc_fd) {
    fprintf(stderr, "fdired: failed to run fd\n");
    return 1;
  }

  // handle quit
  signal(SIGINT,   finish);
  // handle win resizing 
  signal(SIGWINCH, handle_resize);

  initscr();
  keypad(stdscr, TRUE);
  noecho();
  cbreak();
  halfdelay(1);

  char buffer[2048];
  while (fgets(buffer, sizeof(buffer), proc_fd) != NULL) {
    proc_result = realloc(proc_result, sizeof(char*) * (proc_result_count + 1));
    proc_result[proc_result_count++] = strdup(buffer);
  }

  v.total_rows = proc_result_count;
  v.top_row    = 0;
  v.curr_row   = 0;
  v.width      = COLS;
  v.height     = LINES - 2; // row 0 = header, LINES-1 = status bar

  render(&v);

  char last_key = ' ';

  for (;;) {
    if (!running) break;

    // handle terminal resize
    if (resized) {
      resized  = 0;
      endwin();
      refresh();
      v.width  = COLS;
      v.height = LINES - 2;
      if (v.top_row + v.height <= v.curr_row)
        v.top_row = v.curr_row - v.height + 1;
      render(&v);
    }

    int key = getch();

    switch (key) {
    case 'j':
      last_key = ' ';
      if (v.curr_row + 1 < v.total_rows) {
        v.curr_row++;
        if (v.curr_row >= v.top_row + v.height) v.top_row++;
      }
      render(&v);
      break;

    case 'k':
      last_key = ' ';
      if (v.curr_row > 0) {
        v.curr_row--;
        if (v.curr_row < v.top_row) v.top_row--;
      }
      render(&v);
      break;

    case 'G':
      last_key   = ' ';
      v.curr_row = v.total_rows - 1;
      v.top_row  = v.total_rows - v.height;
      if (v.top_row < 0) v.top_row = 0;
      render(&v);
      break;

    case 'g':
      if (last_key == 'g') {
        v.curr_row = 0;
        v.top_row  = 0;
        last_key   = ' ';
        render(&v);
      } else {
        last_key = 'g';
      }
      break;

    case '\n':
    case '\r':
    case KEY_ENTER:
      if (v.total_rows == 0) break;
      char file[1024];
      snprintf(file, sizeof(file), "%s", proc_result[v.curr_row]);
      file[strcspn(file, "\n")] = '\0';

      char open_cmd[1200];
      snprintf(open_cmd, sizeof(open_cmd), "nvim \"%s\"", file);

      def_prog_mode();
      endwin();
      system(open_cmd);
      reset_prog_mode();
      refresh();
      render(&v);
      break;

    case 'q':
      running = 0;
      break;

    default:
      if (key != ERR) last_key = ' ';
      break;
    }
  }

  if (proc_fd) pclose(proc_fd);
  for (int i = 0; i < proc_result_count; i++) free(proc_result[i]);
  free(proc_result);
  endwin();

  return 0;
}

static void finish(int sig)       { (void)sig; running = 0; }
static void handle_resize(int sig){ (void)sig; resized = 1; }
