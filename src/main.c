#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <signal.h>
#include <unistd.h>

#include <curses.h>
#include <getopt.h>

/* how much extra memory to allocate at a time if needed */
#define MEM_CHUNK 1024
#define PATTERN_MATCH_PAIR 1 

static volatile sig_atomic_t running = 1;
static volatile sig_atomic_t resized = 0;

/*
 * By any future mentions of
 *
 * `cmd` or
 * `base_cmd` or
 * `executable`
 *
 * we refer to the filter whose output you want to display
 * eg: in `./fdired grep -rni -L js ~/some_path`
 * we refer to grep.
 *
 * */

/* fd returned by popen to run cmd */
FILE*  proc_fd= NULL;

/* one parsed line of output from the underlying utility */
typedef struct {
  char *display;      /* raw line from popen, owned (strdup'd) */
  char *file;         /* points into display where the filepath starts */
  char *matched_line; /* points into display where the matched line start if present */
  int   file_end;     /* index of ':' after filepath (grep/rg), or end of string (fd/find) */
  int   line_num;     /* line number from grep/rg output, -1 otherwise */
} SearchResult;

/* dynamic array holding every parsed line returned by the command */
typedef struct {
  SearchResult *items;
  size_t        count;
  size_t        capacity;
} UtilityOutput;


/*
 * I stole this idea from nob.h
 * I impl a very stripped version of it
 * all we need is a dynamically allocable
 * array to capture cmd and all its flags
 * Could we get away with allocating on the stack?
 * Absolutely!
 * but.
 */
typedef struct {
  const char **items;
  size_t count;
  size_t capacity;
} Cmd;

/*
 * validate cmd
 * since the point of this tool is to render
 * a dired like text buffer, we can only support
 * cmd which return a list of files seperated by newlines
 * either natively or with a flag
 * at this point we only support these
 * obv this is infinitely extensible
 */
typedef enum {
  FIND,      
  FD,        
  GREP,      
  RG,        
} AVAILABLE_CMDS;



/* viewport describes the terminal area used for the result list */
typedef struct {
  int height;     /* usable rows between header and status bar */
  int width;      /* terminal columns */
  int top_row;    /* first visible result index */
  int total_rows; /* total fd results */
  int curr_row;   /* selected result index */
} viewport;



/*
 * Since I want to keep this project limited to one file
 * we should forward decalre all functions
 */

/* signal handlers */
static void finish(int sig);
static void handle_resize(int sig);

/* add arg to a base cmd; this can be done infinitely */
int cmd_append(Cmd *cmd, const char *arg);

/* match the base cmd with enum and inject the required flags */
void inject_required_flags(Cmd* cmd, AVAILABLE_CMDS cmd_type);

/* parse each line of the output by the utility one by one */ 
SearchResult parse_single_output(char *line, AVAILABLE_CMDS cmd); 

/* append a parsed result into UtilityOutput, growing as needed */
int output_append(UtilityOutput *out, SearchResult r);

static void draw_entry(int screen_y, int col, SearchResult *r, int is_sel, int width, AVAILABLE_CMDS cmd_type); 
void render(viewport* v, UtilityOutput *out, AVAILABLE_CMDS cmd_type);





SearchResult parse_single_output(char *line, AVAILABLE_CMDS cmd)
{
  SearchResult result = {
    .display      = line,
    .file         = line,  /* default: whole line is the file */
    .matched_line = NULL, 
    .file_end     = -1,
    .line_num     = -1,
  };

  switch (cmd) {
  case GREP:
  case RG: {
    /* format: filepath:linenum:matched_text */
    char *first_colon = strchr(line, ':'); /* points to the end of filepath*/
    if (!first_colon) return result;

    result.file_end = (int)(first_colon - line); /* idx to end of filepath 
                                                    probably we can just store the pointer instead*/ 

    char *num_start = first_colon + 1; 
    char *second_colon = strchr(num_start, ':');
    if (!second_colon) return result;

    /* temporarily null-terminate to parse line number */
    *second_colon         = '\0';
    result.line_num       = atoi(num_start);
    *second_colon         = ':';

    result.matched_line = second_colon + 1;
    while (*result.matched_line == ' ' || *result.matched_line == '\t')
      result.matched_line++;

    return result;
  }

  case FD:
  case FIND: {
    /* strip trailing newline; file_end points past last real char */
    int len = strlen(line);
    if (len > 0 && line[len - 1] == '\n') len--;
    result.file_end = len;
    return result;
  }

  default:
    return result;
  }
}

/* Draw a single result row */
static void draw_entry(int screen_y, int col, SearchResult *result, int is_sel, int width, AVAILABLE_CMDS cmd_type) {
  int avail = width - col;
  if (avail <= 0) return;
  if (is_sel) attron(A_REVERSE);
  /*
   * for grep/rg: path portion is display[0..file_end], suffix is rest
   * for fd/find: file_end is end of string, no suffix
   */
  size_t   path_len   = (result->file_end >= 0) ? result->file_end : strlen(result->display);
  char *suffix     = NULL;
  size_t  suffix_len = 0;

  if ((cmd_type == GREP || cmd_type == RG) && result->file_end >= 0) {
    /* suffix = "... :linenum:matched_text"
     * dim it to separate from path */
    // suffix     = result->display + result->file_end;
    suffix     = result->matched_line;
    suffix_len = strlen(suffix);
    /* strip trailing newline from suffix length */
    /* THIS COULD BE THE CULPRIT */ 
    while (suffix_len > 0 && suffix[suffix_len - 1] == '\n') suffix_len--;
  }

  int linenum_str_len = 0;
  if ((cmd_type == GREP || cmd_type == RG) && result->line_num >= 0) {
    linenum_str_len = snprintf(NULL, 0, ":%d:", result->line_num);
  }

  int total_display_str_len = (int)path_len + linenum_str_len + (int)suffix_len;
  const char *raw_display_str = result->display;

  /* locate last '/' */
  int slash_idx= path_len - 1;
  while (slash_idx > 0 && raw_display_str[slash_idx] != '/') slash_idx--;
  int total_path_len = path_len;

  if (slash_idx > 0) {
    int dir_len  = slash_idx + 1;   /* split into dir and base */
    int base_len = path_len - dir_len;
    if (total_path_len <= avail) {

      /* everything fits */
      if (!is_sel) attron(A_DIM);
      mvaddnstr(screen_y, col, raw_display_str, dir_len);
      if (!is_sel) attroff(A_DIM);
      addnstr(raw_display_str + dir_len, base_len);
      printw(":%d:", result->line_num);

      /* append suffix dimmed if room */
      if (suffix && suffix_len > 0 ) {
        int suffix_budget = avail - (int)path_len - linenum_str_len;
        if (suffix_budget > 0) {
          int draw_len = ((int)suffix_len <= suffix_budget) ? (int)suffix_len : suffix_budget;
          if (!is_sel) {
            // attron(A_DIM);
            attron(COLOR_PAIR(PATTERN_MATCH_PAIR));
          }
          // printw(" ");
          if (draw_len < (int)suffix_len) {
            addnstr(suffix, draw_len - 3);
            addnstr("...", 3);
          } else {
            addnstr(suffix, draw_len);
          }
          if (!is_sel) {
            // attroff(A_DIM);
            attroff(COLOR_PAIR(PATTERN_MATCH_PAIR));
          }
        } else if (suffix_budget == 0) {
            if (!is_sel) attron(COLOR_PAIR(PATTERN_MATCH_PAIR));
            addnstr("...",3);
            if (!is_sel) attroff(COLOR_PAIR(PATTERN_MATCH_PAIR));
        }
      }

      /* pad remainder */
      int drawn = (int)path_len + linenum_str_len;
      if (suffix && suffix_len > 0) {
        int added = ((int)suffix_len <= avail - drawn) ? (int)suffix_len : avail - drawn;
        if (added > 0) drawn += added;
      }
      for (int p = drawn; p < avail; p++) addch(' ');

    } else if (base_len + 4 <= avail) {
      /* dir too long: ".../[tail]basename" */
      int tail = avail - base_len - 4; /* 4 = strlen("…/") */
      if (!is_sel) attron(A_DIM);
      mvaddnstr(screen_y, col, ".../", 4);
      if (tail > 0) addnstr(raw_display_str + dir_len - tail, tail);
      if (!is_sel) attroff(A_DIM);
      addnstr(raw_display_str+ dir_len, base_len);
    } else {
      /* basename alone too long: truncate it */
      mvaddnstr(screen_y, col, raw_display_str + dir_len, avail);
    }
  } else {
    int show = path_len < avail ? path_len : avail;
    mvaddnstr(screen_y, col, raw_display_str, show);
    /* render suffix if exists */ 
    if (suffix && suffix_len > 0 && show + suffix_len <= avail) {
      if (!is_sel) {
        attron(A_DIM);
        attron(COLOR_PAIR(PATTERN_MATCH_PAIR));
      }
      addnstr(suffix, suffix_len);
      if (!is_sel) {
        attroff(A_DIM);
        attroff(COLOR_PAIR(PATTERN_MATCH_PAIR));
      }
    }
    int drawn = (suffix && suffix_len > 0 && show + suffix_len <= avail)
                  ? show + suffix_len : show;
    for (int p = drawn; p < avail; p++) addch(' ');
  }
  if (is_sel) attroff(A_REVERSE);
}

/* rendering logic */
void render(viewport *v, UtilityOutput *out, AVAILABLE_CMDS cmd_type) {
  clear();

  /* list # of result by fd */
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
      draw_entry(sy + 1, 1, &out->items[idx], is_sel, v->width, cmd_type);
    }
  }

  /* footer status bar */
  char status[1024];
  int slen = snprintf(status, sizeof(status),
                      " [%d/%d]  j%c  k%c  gg top  G end  enter open  q quit",
                      v->total_rows > 0 ? v->curr_row + 1 : 0,
                      v->total_rows,
                      (char)40, (char)38);
  /* right-pad to full width */
  while (slen < v->width && slen < (int)sizeof(status) - 1) status[slen++] = ' ';
  status[slen] = '\0';

  attron(A_REVERSE);
  mvaddstr(LINES - 1, 0, status);
  attroff(A_REVERSE);

  if (v->total_rows > 0)
    move(v->curr_row - v->top_row + 1, 1);

  refresh();
}

/* add arg to a base cmd */
/* the base cmd will usually be the name of the executable */
/* while args are flags and positional args */
int cmd_append(Cmd *cmd, const char *arg) {
  /* oops need more memory */
  if (cmd->count >= cmd->capacity) {
    cmd->capacity += MEM_CHUNK;
    /* realloc only the items arr with mem increase by 1 MEM_CHUNK */
    cmd->items = realloc(cmd->items, cmd->capacity * sizeof(char *));
    if (!cmd->items) {
      fprintf(stderr, "Some error with memory allocation\n");
      return 1;
    }
  }
  /* add arg to chain of cmds */
  cmd->items[cmd->count++] = arg;
  return 0;
}

/* append a parsed SearchResult into UtilityOutput, growing as needed */
int output_append(UtilityOutput *out, SearchResult r) {
  if (out->count >= out->capacity) {
    out->capacity += MEM_CHUNK;
    out->items = realloc(out->items, out->capacity * sizeof(SearchResult));
    if (!out->items) {
      fprintf(stderr, "Some error with memory allocation\n");
      return 1;
    }
  }
  out->items[out->count++] = r;
  return 0;
}

/* duplicate flags should not be an issue, atleast for these */
void inject_required_flags(Cmd* cmd, AVAILABLE_CMDS cmd_type){
  switch (cmd_type) {
    case FD:
    case FIND: 
      break;
    case GREP:
      cmd_append(cmd, "-n"); /* needs the -n flag to show line numbers */
      cmd_append(cmd, "-I"); /* ignore binary files */ 
      break;
    case RG:
      break;
  }
  return; 
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "Usage: %s <fd|grep|rg> [args...]\n", argv[0]);
    return 1;
  }

  viewport v;
  Cmd cmd = {0};
  UtilityOutput output = {0};
  AVAILABLE_CMDS type;

  if      (strcmp(argv[1], "fd")   == 0) type = FD;
  else if (strcmp(argv[1], "find") == 0) type = FIND;
  else if (strcmp(argv[1], "grep") == 0) type = GREP;
  else if (strcmp(argv[1], "rg")   == 0) type = RG;
  else {
      fprintf(stderr, "Unknown command: %s.\n We currently only support the following:\n\
          FIND\n\
          FD\n\
          GREP\n\
          RG\n", argv[1]);
      return 1;
  }

  /* first arg is `./fdired`; no need to capture */
  for (int i = 1; i < argc; ++i) {
    cmd_append(&cmd, argv[i]);
  }

  inject_required_flags(&cmd, type);

  /* calc total size needed for proc_cmd */
  size_t total_len = 0;
  for (size_t i = 0; i < cmd.count; ++i)
    total_len += strlen(cmd.items[i]) + 1;

  /* init proc_cmd with that size */
  char proc_cmd[total_len];
  proc_cmd[0] = '\0';
  
  /* copy cmd in a text buffer to run */
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

  /* handle quit */
  signal(SIGINT,   finish);
  /* handle win resizing */
  signal(SIGWINCH, handle_resize);

  initscr();
  start_color();
  init_pair(PATTERN_MATCH_PAIR, COLOR_RED, COLOR_BLACK);keypad(stdscr, TRUE);
  noecho();
  cbreak();
  halfdelay(1);

  /* capture output of proc_cmd */
  char buffer[2048];
  FILE *f = fopen("debug.txt", "w");
  if (f == NULL)
  {
      printf("Error opening file!\n");
      exit(1);
  }
  while (fgets(buffer, sizeof(buffer), proc_fd) != NULL) {
    char *line = strdup(buffer);
    fprintf(f,"%s\n",line);
    SearchResult r = parse_single_output(line, type);
    output_append(&output, r);
  }
  fclose(f);

  v.total_rows = (int)output.count;
  v.top_row    = 0;
  v.curr_row   = 0;
  v.width      = COLS;
  /* row 0 = header, LINES-1 = status bar */
  v.height     = LINES - 2;

  render(&v, &output, type);

  char last_key = ' ';

  for (;;) {
    if (!running) break;

    /* handle terminal resize */
    if (resized) {
      resized  = 0;
      endwin();
      refresh();
      v.width  = COLS;
      v.height = LINES - 2;
      if (v.top_row + v.height <= v.curr_row)
        v.top_row = v.curr_row - v.height + 1;
      render(&v, &output, type);
    }

    int key = getch();

    switch (key) {
    case 'j':
      last_key = ' ';
      if (v.curr_row + 1 < v.total_rows) {
        v.curr_row++;
        if (v.curr_row >= v.top_row + v.height) v.top_row++;
      }
      render(&v, &output, type);
      break;

    case 'k':
      last_key = ' ';
      if (v.curr_row > 0) {
        v.curr_row--;
        if (v.curr_row < v.top_row) v.top_row--;
      }
      render(&v, &output, type);
      break;

    case 'G':
      last_key   = ' ';
      v.curr_row = v.total_rows - 1;
      v.top_row  = v.total_rows - v.height;
      if (v.top_row < 0) v.top_row = 0;
      render(&v, &output, type);
      break;

    case 'g':
      if (last_key == 'g') {
        v.curr_row = 0;
        v.top_row  = 0;
        last_key   = ' ';
        render(&v, &output, type);
      } else {
        last_key = 'g';
      }
      break;

    case '\n':
    case '\r':
    case KEY_ENTER: {
      if (v.total_rows == 0) break;
      SearchResult *r = &output.items[v.curr_row];

      /* temporarily null-terminate at file_end to isolate the path */
      char saved = '\0';
      if (r->file_end >= 0) {
        saved                    = r->display[r->file_end];
        r->display[r->file_end] = '\0';
      }

      char open_cmd[1200];
      if (r->line_num > 0)
        snprintf(open_cmd, sizeof(open_cmd), "nvim +%d \"%s\"", r->line_num, r->file);
      else
        snprintf(open_cmd, sizeof(open_cmd), "nvim \"%s\"", r->file);

      /* restore the display string */
      if (r->file_end >= 0)
        r->display[r->file_end] = saved;

      def_prog_mode();
      endwin();
      system(open_cmd);
      reset_prog_mode();
      refresh();
      render(&v, &output, type);
      break;
    }

    case 'q':
      running = 0;
      break;

    default:
      if (key != ERR) last_key = ' ';
      break;
    }
  }

  if (proc_fd) pclose(proc_fd);
  for (size_t i = 0; i < output.count; i++) free(output.items[i].display);
  free(output.items);
  endwin();

  return 0;
}

static void finish(int sig)       { (void)sig; running = 0; }
static void handle_resize(int sig){ (void)sig; resized = 1; }
