#include <_stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <signal.h>
#include <unistd.h>

#include <curses.h>
#include <getopt.h>
#include <locale.h>

#define BUFFER_SIZE 2048 /* temp buffer that reads output of utility line by line */
#define MEM_CHUNK 1024 /* how much extra memory to allocate at a time if needed */
#define PATTERN_MATCH_PAIR 1 /* color for highlighting matched line */
#define LINE_NUM_PAIR 2 /* color for highlighting line number */

static volatile sig_atomic_t running = 1;
static volatile sig_atomic_t resized = 0;
int min(int a, int b) { return (a < b)? a: b; } 
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

/* Currently just stores the positional args */
typedef struct {
  char *pattern;     /* pattern to match in case of grep/rg */
  char *search_path; /* path where to search */
}InputArgs; 

/* one parsed line of output from the underlying utility */
typedef struct {
  char *display;        /* raw line from popen, owned (strdup'd) */
  char *file;           /* points into display where the filepath starts */
  char *relative_file;  /* points into file but relative to the input filepath */ 
  char *matched_line;   /* points into display where the matched line start if present */
  char *matched_substr; /* points into matched_line where the exact patterrn match exists */
  int   file_end;       /* end of filepath (grep/rg)/ also end of string (fd/find) */
  int   line_num;       /* line number from grep/rg output, -1 otherwise */
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

/* render full file path for path relative to the input */ 
typedef enum {
  PATH_FULL, 
  PATH_REL_TO_INPUT,
}PATH_STATE; 

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
SearchResult parse_single_output(char *line,char *pattern, InputArgs input_args, AVAILABLE_CMDS cmd); 

/* append a parsed result into UtilityOutput, growing as needed */
int output_append(UtilityOutput *out, SearchResult r);

/* draw a single row */ 
static void draw_entry(int screen_y, int col, SearchResult *r,char *pattern, int is_sel, int width, AVAILABLE_CMDS cmd_type, PATH_STATE path_state); 

/* render state: calls draw_entry over all search results stored in UtilityOutput */ 
void render(viewport* v, UtilityOutput *out, char *pattern, AVAILABLE_CMDS cmd_type, PATH_STATE path_state);



char* get_file_path_relative_to_input(char *line, InputArgs *input_args){
  if(strlen(input_args->search_path) == 0 ) return NULL; 
  char *base = input_args->search_path;
  char *relative_file = strstr(line, base);

  if (relative_file && relative_file == line){
    relative_file += strlen(base);
    relative_file-- ; /* to include the trailing `/` */ 
  }
  else
    relative_file = NULL;

  return relative_file;
}

SearchResult parse_single_output(char *line,char *pattern, InputArgs input_args, AVAILABLE_CMDS cmd)
{
  SearchResult result = {
    .display        = line,
    .file           = line,  /* default: whole line is the file */
    .relative_file  = get_file_path_relative_to_input(line, &input_args),
    .matched_line   = NULL, 
    .matched_substr = NULL,
    .file_end       = -1,
    .line_num       = -1,
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

    result.matched_substr= strcasestr(result.matched_line, pattern);
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
static void draw_entry(
    int screen_y, int col, SearchResult *result, char *pattern, int is_sel, int width, AVAILABLE_CMDS cmd_type, PATH_STATE path_state) 
{
  int avail = width - col;
  if (avail <= 0) return;

  if (is_sel) attron(A_REVERSE);
  /*
   * for grep/rg: path portion is display[0..file_end], suffix is rest
   * for fd/find: file_end is end of string, no suffix
   */
  const char *raw_display_str = (path_state == PATH_REL_TO_INPUT && result->relative_file != NULL)
                                  ? result->relative_file : result->display;
  size_t path_len;
  if (path_state == PATH_REL_TO_INPUT && result->relative_file != NULL) {
    if (result->file_end >= 0) {
      int offset = (int)(result->relative_file - result->display);
      path_len = result->file_end - offset;
    } else {
      path_len = strlen(result->relative_file);
      while (path_len > 0 && raw_display_str[path_len - 1] == '\n') path_len--;
    }
  } else {
    path_len = (result->file_end >= 0) ? result->file_end : strlen(result->display);
  }
  int linenum_str_len = 0;
  int pre_match_len = 0;
  int match_len = 0;
  int post_match_len = 0;

  if ((cmd_type == GREP || cmd_type == RG) && result->line_num >= 0) {
    linenum_str_len = snprintf(NULL, 0, ":%d: ", result->line_num);
    pre_match_len  = result->matched_substr - result->matched_line;
    match_len      = strlen(pattern);
    post_match_len = strlen(result->matched_line) - pre_match_len - match_len;

  }
  char* suffix=NULL;
  int suffix_len=0;
  
  if ((cmd_type == GREP || cmd_type == RG) && result->file_end >= 0) {
    /* suffix= pre match substr + match substr + post match substr
     * dim it to separate from path */
    suffix     = result->matched_line;
    suffix_len = strlen(suffix);
    /* strip trailing newline from suffix length */
    while (suffix_len > 0 && suffix[suffix_len - 1] == '\n') suffix_len--;
  }

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
      if (!is_sel) {
        attron(COLOR_PAIR(LINE_NUM_PAIR));
      }
      if(cmd_type== GREP || cmd_type== RG){
        printw(":%d: ", result->line_num);
      }
      if (!is_sel) {
        attroff(COLOR_PAIR(LINE_NUM_PAIR));
      }

      /* append suffix dimmed if room */
      if (suffix && suffix_len > 0 ) {
        int suffix_budget = avail - (int)path_len - linenum_str_len;
        if (suffix_budget > 0) {
          int draw_len = ((int)suffix_len <= suffix_budget) ? (int)suffix_len : suffix_budget;
          int remaining = draw_len;

          int n = min(pre_match_len, remaining);
          addnstr(suffix, n);
          remaining -= n;

          if (remaining > 0) {
            // if (!is_sel) {
            //   attron(COLOR_PAIR(PATTERN_MATCH_PAIR));
            // }
            attron(COLOR_PAIR(PATTERN_MATCH_PAIR));
            n = min(match_len, remaining);
            addnstr(suffix + pre_match_len, n);
            remaining -= n;
            attroff(COLOR_PAIR(PATTERN_MATCH_PAIR));
            // if (!is_sel) {
            //   attroff(COLOR_PAIR(PATTERN_MATCH_PAIR));
            // }
          }

          if (remaining > 0) {
            n = min(post_match_len, remaining);
            addnstr(suffix + pre_match_len + match_len, n);
            remaining -= n;
          }

          if (draw_len < (int)suffix_len) {
            addnstr("...", min(3, avail));
          }
        } else if (suffix_budget == 0) {
          if (!is_sel) attron(COLOR_PAIR(PATTERN_MATCH_PAIR));
          addnstr("...", 3);
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
    int show = (int)path_len < avail ? path_len : avail;
    mvaddnstr(screen_y, col, raw_display_str, show);
    if ((cmd_type == GREP || cmd_type == RG) && result->line_num >= 0) {
      if (!is_sel) attron(COLOR_PAIR(LINE_NUM_PAIR));
      printw(":%d: ", result->line_num);
      if (!is_sel) attroff(COLOR_PAIR(LINE_NUM_PAIR));
    }
    /* render suffix if exists */
    if (suffix && suffix_len > 0) {
      int suffix_budget = avail - (int)path_len - linenum_str_len;
      if (suffix_budget > 0) {
        int draw_len = ((int)suffix_len <= suffix_budget) ? (int)suffix_len : suffix_budget;
        int remaining = draw_len;

        int n = min(pre_match_len, remaining);
        addnstr(suffix, n);
        remaining -= n;

        if (remaining > 0) {
          attron(COLOR_PAIR(PATTERN_MATCH_PAIR));
          n = min(match_len, remaining);
          addnstr(suffix + pre_match_len, n);
          remaining -= n;
          attroff(COLOR_PAIR(PATTERN_MATCH_PAIR));
        }

        if (remaining > 0) {
          n = min(post_match_len, remaining);
          addnstr(suffix + pre_match_len + match_len, n);
          remaining -= n;
        }

        if (draw_len < (int)suffix_len) {
          addnstr("...", min(3, avail));
        }
      } else if (suffix_budget == 0) {
        if (!is_sel) attron(COLOR_PAIR(PATTERN_MATCH_PAIR));
        addnstr("...", 3);
        if (!is_sel) attroff(COLOR_PAIR(PATTERN_MATCH_PAIR));
      }
    }
    int drawn = (suffix && (int)suffix_len > 0 && (int)suffix_len <= avail - (int)path_len - linenum_str_len)
                  ? show + linenum_str_len + suffix_len : show + linenum_str_len;
    for (int p = drawn; p < avail; p++) addch(' ');
  }
  if (is_sel) attroff(A_REVERSE);
}

/* rendering logic */
void render(viewport *v, UtilityOutput *out, char *pattern, AVAILABLE_CMDS cmd_type, PATH_STATE path_state) {
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
      draw_entry(sy + 1, 1, &out->items[idx],pattern, is_sel, v->width, cmd_type, path_state);
    }
  }

  /* footer status bar */
  char status[1024];
  int slen = snprintf(status, sizeof(status),
                      " [%d/%d]  j:↓  k:↑  gg: top  G: end  enter: open  tab: show relative path q: quit",
                      v->total_rows > 0 ? v->curr_row + 1 : 0,
                      v->total_rows);
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
  AVAILABLE_CMDS cmd_type;

  if      (strcmp(argv[1], "fd")   == 0) cmd_type = FD;
  else if (strcmp(argv[1], "find") == 0) cmd_type = FIND;
  else if (strcmp(argv[1], "grep") == 0) cmd_type = GREP;
  else if (strcmp(argv[1], "rg")   == 0) cmd_type = RG;
  else {
      fprintf(stderr, "Unknown command: %s.\n We currently only support the following:\n\
          FIND\n\
          FD\n\
          GREP\n\
          RG\n", argv[1]);
      return 1;
  }

  /* (1) utility name (2) positional arg #1 pattern (3) positional arg #2 search_path*/
  InputArgs input_args = {
    .pattern = "", 
    .search_path = "", 
  };

  char *pos_args[3];
  int n_pos = 0;
  FILE *f = fopen("debug.txt", "w");

  /* for debugging purposes */ 
  // if (f == NULL)
  // {
  //   printf("Error opening file!\n");
  //   exit(1);
  // }

  /* first arg is `./fdired`; no need to capture */
  for (int i = 1; i < argc; ++i) {
    /* capture the positional arg for some reason */ 
    if (argv[i][0] != '-' && n_pos < 3) {
      pos_args[n_pos++] = argv[i]; // two pointers to same data?? 
      // fprintf(f,"pos arg #%d, %s\n",n_pos, argv[i]);
    }
    cmd_append(&cmd, argv[i]);
  }
  fclose(f);

  /* TODO! need to audit: umm this is super hacky */
  switch (cmd_type){
    case FIND:
    case FD: 
      if (n_pos != 2) break;
      input_args.search_path = pos_args[1]; 
      break; 
    case GREP:
    case RG: 
      if (n_pos != 3) break; 
      input_args.pattern = pos_args[1]; 
      input_args.search_path = pos_args[2];
      break; 
  }

  inject_required_flags(&cmd, cmd_type);

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


  setlocale(LC_ALL, "");
  initscr();
  start_color();
  init_pair(PATTERN_MATCH_PAIR, COLOR_RED, COLOR_BLACK);
  init_pair(LINE_NUM_PAIR, COLOR_GREEN, COLOR_BLACK);
  keypad(stdscr, TRUE);
  noecho();
  cbreak();
  halfdelay(1);

  // FILE *f = fopen("debug.txt", "w");
  // if (f == NULL)
  // {
  //     printf("Error opening file!\n");
  //     exit(1);
  // }
 
  /* capture output of proc_cmd */
  // char buffer[BUFFER_SIZE];

  char *line = NULL;
  size_t size = 0;
  ssize_t nread;

  while ((nread = getline(&line, &size, proc_fd)) != -1) {
    char *temp_line = strdup(line); 
    SearchResult r = parse_single_output(temp_line, pos_args[1], input_args, cmd_type);
    output_append(&output, r);
    // free(temp_line);
    temp_line = NULL;
  }

  if (ferror(proc_fd))
      perror("getline");
  if (feof(proc_fd))
      puts("EOF");

  free(line);
  line = NULL; 

  // while (fgets(buffer, sizeof(buffer), proc_fd) != NULL) {
  //   char *line = strdup(buffer);
  //   // fprintf(f,"%s\n",line);
  //   SearchResult r = parse_single_output(line, pos_args[1], input_args, cmd_type);
  //   output_append(&output, r);
  // }
  // fclose(f);

  v.total_rows = (int)output.count;
  v.top_row    = 0;
  v.curr_row   = 0;
  v.width      = COLS;
  /* row 0 = header, LINES-1 = status bar */
  v.height     = LINES - 2;

  char last_key = ' ';
  PATH_STATE path_state = PATH_FULL;

  render(&v, &output, pos_args[1], cmd_type, path_state);

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
      render(&v, &output, pos_args[1], cmd_type, path_state);
    }

    int key = getch();

    switch (key) {
    case 'j':
      last_key = ' ';
      if (v.curr_row + 1 < v.total_rows) {
        v.curr_row++;
        if (v.curr_row >= v.top_row + v.height) v.top_row++;
      }
      render(&v, &output, pos_args[1], cmd_type, path_state);
      break;

    case 'k':
      last_key = ' ';
      if (v.curr_row > 0) {
        v.curr_row--;
        if (v.curr_row < v.top_row) v.top_row--;
      }
      render(&v, &output, pos_args[1], cmd_type, path_state);
      break;

    case 'G':
      last_key   = ' ';
      v.curr_row = v.total_rows - 1;
      v.top_row  = v.total_rows - v.height;
      if (v.top_row < 0) v.top_row = 0;
      render(&v, &output, pos_args[1], cmd_type, path_state);
      break;

    case 'g':
      if (last_key == 'g') {
        v.curr_row = 0;
        v.top_row  = 0;
        last_key   = ' ';
      render(&v, &output, pos_args[1], cmd_type, path_state);
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
      render(&v, &output, pos_args[1], cmd_type, path_state);
      break;
    }

    case '\t':
      last_key   = ' ';
      path_state = (path_state == PATH_FULL) ? PATH_REL_TO_INPUT : PATH_FULL;
      render(&v, &output, pos_args[1], cmd_type, path_state);
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
  for (size_t i = 0; i < output.count; i++) free(output.items[i].display);
  free(output.items);
  endwin();

  return 0;
}

static void finish(int sig)       { (void)sig; running = 0; }
static void handle_resize(int sig){ (void)sig; resized = 1; }
