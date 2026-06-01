#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <getopt.h>
#include <curses.h>
#include <signal.h>
#include <stdint.h>
#include <signal.h>

static volatile sig_atomic_t running = 1;
static volatile sig_atomic_t resized = 0;
static void finish(int sig);
static void handle_resize(int sig);
static FILE* proc_fd = NULL;

char** proc_result   = NULL;
int    proc_result_count = 0;

typedef struct {
  int height;     // usable rows between header and status bar
  int width;      // terminal columns
  int top_row;    // first visible result index
  int total_rows; // total fd results
  int curr_row;   // selected result index
} viewport;

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

int main(int argc, char** argv)
{
  viewport v;
  char flags[512]    = "--absolute-path";
  const char *pattern = "";
  const char *path    = ".";
  char cmd[1024];
  int  c;

  static struct option long_options[] = {
      {"hidden",    no_argument,       0, 'H'},
      {"no-ignore", no_argument,       0, 'I'},
      {"glob",      no_argument,       0, 'g'},
      {"type",      required_argument, 0, 't'},
      {"extension", required_argument, 0, 'e'},
      {"max-depth", required_argument, 0, 'd'},
      {0, 0, 0, 0}
  };
  while ((c = getopt_long(argc, argv, "HIt:e:d:g", long_options, NULL)) != -1) {
      switch (c) {
      case 'H': strncat(flags, " --hidden",     sizeof(flags) - strlen(flags) - 1); break;
      case 'I': strncat(flags, " --no-ignore",  sizeof(flags) - strlen(flags) - 1); break;
      case 't': strncat(flags, " --type ",      sizeof(flags) - strlen(flags) - 1);
                strncat(flags, optarg,           sizeof(flags) - strlen(flags) - 1); break;
      case 'e': strncat(flags, " --extension ", sizeof(flags) - strlen(flags) - 1);
                strncat(flags, optarg,           sizeof(flags) - strlen(flags) - 1); break;
      case 'd': strncat(flags, " --max-depth ", sizeof(flags) - strlen(flags) - 1);
                strncat(flags, optarg,           sizeof(flags) - strlen(flags) - 1); break;
      case 'g': strncat(flags, " --glob",       sizeof(flags) - strlen(flags) - 1); break;
      case '?': return 1;
      }
  }

  if (optind < argc) pattern = argv[optind++];
  if (optind < argc) path    = argv[optind];

  snprintf(cmd, sizeof(cmd), "fd %s \"%s\" %s", flags, pattern, path);

  proc_fd = popen(cmd, "r");
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
