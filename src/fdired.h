#ifndef FDIRED_H
#define FDIRED_H

#include "logger.h"
// #include <cstring>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <signal.h>
#include <unistd.h>

#include <curses.h>
#include <regex.h>

/* temp buffer that reads output of utility line by line */
#define BUFFER_SIZE 2048
/* how much extra memory to allocate at a time if needed */
#define MEM_CHUNK 1024
/* color for highlighting matched line */
#define PATTERN_MATCH_PAIR 1
/* color for highlighting line number */
#define LINE_NUM_PAIR 2
#define CONFIG_FILE_PATH "~/.config/fdired.toml"

static volatile sig_atomic_t running = 1;
static volatile sig_atomic_t resized = 0;
static inline int min(int a, int b) { return (a < b) ? a : b; }
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
static FILE *proc_fd = NULL;

/* handling configs */
typedef enum {
  FT_PDF,
  FT_IMAGE,
  FT_ASCII,
  FT_UNKNOWN,
} FileType;

typedef struct {
  FileType type;
  const char *cmd;
} FileTypeHandler;

static FileTypeHandler file_type_handlers[] = {
    {FT_PDF, "open"},
    {FT_IMAGE, "open"},
    {FT_ASCII, "nvim"},
};

/* Currently just stores the positional args */
typedef struct {
  char *pattern;     /* pattern to match in case of grep/rg */
  char *search_path; /* path where to search */
} InputArgs;

/* one parsed line of output from the underlying utility */
typedef struct {
  char *display;       /* raw line from popen, owned (strdup'd) */
  char *file;          /* points into display where the filepath starts */
  char *relative_file; /* points into file but relative to the input filepath */
  char *matched_line;  /* points into display where the matched line start if
                          present */
  char *matched_substr;     /* points into matched_line where the exact patterrn
                               match exists */
  char *matched_substr_end; /* points into matched_line where the exact patterrn
                               match ends */
  int file_end; /* end of filepath (grep/rg)/ also end of string (fd/find) */
  int line_num; /* line number from grep/rg output, -1 otherwise */
} SearchResult;

/* dynamic array holding every parsed line returned by the command */
typedef struct {
  SearchResult *items;
  size_t count;
  size_t capacity;
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
} PATH_STATE;

/* viewport describes the terminal area used for the result list */
typedef struct {
  int height;     /* usable rows between header and status bar */
  int width;      /* terminal columns */
  int top_row;    /* first visible result index */
  int total_rows; /* total fd results */
  int curr_row;   /* selected result index */
} viewport;

/*
 * Since this header is only ever included once (by main.c)
 * we forward decalre all functions the same way the old single
 * main.c file did
 */

/* signal handlers */
static void finish(int sig);
static void handle_resize(int sig);

/* add arg to a base cmd; this can be done infinitely */
static int cmd_append(Cmd *cmd, const char *arg);

/* match the base cmd with enum and inject the required flags */
static void inject_required_flags(Cmd *cmd, AVAILABLE_CMDS cmd_type);

/* parse each line of the output by the utility one by one */
static SearchResult parse_single_output(char *line, char *pattern,
                                        InputArgs input_args,
                                        AVAILABLE_CMDS cmd);

/* append a parsed result into UtilityOutput, growing as needed */
static int output_append(UtilityOutput *out, SearchResult r);

/* draw a single row */
static void draw_entry(int screen_y, int col, SearchResult *r, char *pattern,
                       int is_sel, int width, AVAILABLE_CMDS cmd_type,
                       PATH_STATE path_state);

/* render state: calls draw_entry over all search results stored in
 * UtilityOutput */
static void render(viewport *v, UtilityOutput *out, char *pattern,
                   AVAILABLE_CMDS cmd_type, PATH_STATE path_state);

/* match the file into FileType to know how to open it */
static FileType match_file_type(char *file_path);
static const char *get_file_type_handler(FileType file_type);

static char *get_file_path_relative_to_input(char *line,
                                             InputArgs *input_args) {
  if (strlen(input_args->search_path) == 0)
    return NULL;
  char *base = input_args->search_path;
  char *relative_file = strstr(line, base);

  if (relative_file && relative_file == line) {
    relative_file += strlen(base);
    relative_file--; /* to include the trailing `/` */
  } else
    relative_file = NULL;

  return relative_file;
}

static FileType match_file_type(char *file_path) {
  char *ext = strrchr(file_path, '.');
  if (!ext) {
    return FT_UNKNOWN;
  }
  if (strcmp(ext, ".pdf") == 0) {
    return FT_PDF;
  }

  if (strcmp(ext, ".png") == 0 || strcmp(ext, ".jpg") == 0 ||
      strcmp(ext, ".jpeg") == 0) {
    return FT_IMAGE;
  }
  return FT_ASCII;
}

static const char *get_file_type_handler(FileType file_type) {
  if (file_type == FT_UNKNOWN) {
    exit(1);
  }
  for (size_t i = 0;
       i < sizeof(file_type_handlers) / sizeof(file_type_handlers[0]); i++) {
    if (file_type_handlers[i].type == file_type) {
      return file_type_handlers[i].cmd;
    }
  }

  return NULL;
}

static SearchResult parse_single_output(char *line, char *pattern,
                                        InputArgs input_args,
                                        AVAILABLE_CMDS cmd) {
  SearchResult result = {
      .display = line,
      .file = line, /* default: whole line is the file */
      .relative_file = get_file_path_relative_to_input(line, &input_args),
      .matched_line = NULL,
      .matched_substr = NULL,
      .file_end = -1,
      .line_num = -1,
  };

  switch (cmd) {
  case GREP:
  case RG: {
    regex_t re;
    regmatch_t matches[1];
    regcomp(
        &re, pattern,
        REG_EXTENDED); /* look at which cflag to use
                          https://www.man7.org/linux/man-pages/man3/regcomp.3.html#LIBRARY
                        */
    if (regexec(&re, line, 1, matches, 0) == 0) {

      /* format: filepath:linenum:matched_text */
      char *first_colon = strchr(line, ':'); /* points to the end of filepath*/
      if (!first_colon)
        return result;

      result.file_end =
          (int)(first_colon -
                line); /* idx to end of filepath
                          probably we can just store the pointer instead*/

      char *num_start = first_colon + 1;
      char *second_colon = strchr(num_start, ':');
      if (!second_colon)
        return result;

      /* temporarily null-terminate to parse line number */
      *second_colon = '\0';
      result.line_num = atoi(num_start);
      *second_colon = ':';

      result.matched_line = second_colon + 1;
      while (*result.matched_line == ' ' || *result.matched_line == '\t')
        result.matched_line++;

      result.matched_substr = strcasestr(result.matched_line, pattern);
      return result;
    }

  case FD:
  case FIND: {
    /* strip trailing newline; file_end points past last real char */
    int len = strlen(line);
    if (len > 0 && line[len - 1] == '\n')
      len--;
    result.file_end = len;
    return result;
  }

  default:
    return result;
  }
  }
}

/* Draw a single result row */
static void draw_entry(int screen_y, int col, SearchResult *result,
                       char *pattern, int is_sel, int width,
                       AVAILABLE_CMDS cmd_type, PATH_STATE path_state) {
  int avail = width - col;
  if (avail <= 0)
    return;

  if (is_sel)
    attron(A_REVERSE);
  /*
   * for grep/rg: path portion is display[0..file_end], suffix is rest
   * for fd/find: file_end is end of string, no suffix
   */
  const char *raw_display_str =
      (path_state == PATH_REL_TO_INPUT && result->relative_file != NULL)
          ? result->relative_file
          : result->display;
  size_t path_len;
  if (path_state == PATH_REL_TO_INPUT && result->relative_file != NULL) {
    if (result->file_end >= 0) {
      char *start = result->relative_file; /* start of rel filepath */
      char *end = result->display + result->file_end; /* end of filepath */
      path_len = end - start;

    } else {
      char *start = result->relative_file;

      /* find end of line safely
       * this eol could be the end of filepath itself in case of 'find/fd'
       * or not in case or 'grep/rg' since this points into the display cstr*/
      char *end = strchr(start, '\n');
      if (!end)
        end = result->display + strlen(result->display);
      path_len = end - start;
    }

  } else {
    if (result->file_end >= 0) {
      path_len = result->file_end;
    } else {
      char *end = strchr(result->display, '\n');
      if (!end)
        end = result->display + strlen(result->display);

      path_len = end - result->display;
    }
  }

  /* for grep/rg only */
  int linenum_str_len = 0; /* len of line number */
  int pre_match_len = 0;   /* len before pattern found in line */
  int match_len = 0;       /* len of the pattern */
  int post_match_len = 0;  /* len after pattern found in line */

  /* TODO!: we should just store pointers and calc len when we need through
   * pointern arth */
  if ((cmd_type == GREP || cmd_type == RG) && result->line_num >= 0) {
    linenum_str_len = snprintf(NULL, 0, ":%d: ", result->line_num);
    pre_match_len = result->matched_substr - result->matched_line;
    match_len = strlen(pattern);
    post_match_len = strlen(result->matched_line) - pre_match_len - match_len;
  }
  char *suffix = NULL;
  int suffix_len = 0;

  if ((cmd_type == GREP || cmd_type == RG) && result->file_end >= 0) {
    /* suffix= pre match substr + match substr + post match substr
     * dim it to separate from path */
    suffix = result->matched_line;
    suffix_len = strlen(suffix);
    /* strip trailing newline from suffix length */
    while (suffix_len > 0 && suffix[suffix_len - 1] == '\n')
      suffix_len--;
  }

  /* locate last '/' */
  int slash_idx = path_len - 1;
  while (slash_idx > 0 && raw_display_str[slash_idx] != '/')
    slash_idx--;
  int total_path_len = path_len;

  if (slash_idx > 0) {
    int dir_len = slash_idx + 1; /* split into dir and base */
    int base_len = path_len - dir_len;
    if (total_path_len <= avail) {

      /* everything fits */
      if (!is_sel)
        attron(A_DIM);
      mvaddnstr(screen_y, col, raw_display_str, dir_len);
      if (!is_sel)
        attroff(A_DIM);
      addnstr(raw_display_str + dir_len, base_len);
      if (!is_sel) {
        attron(COLOR_PAIR(LINE_NUM_PAIR));
      }
      if (cmd_type == GREP || cmd_type == RG) {
        printw(":%d: ", result->line_num);
      }
      if (!is_sel) {
        attroff(COLOR_PAIR(LINE_NUM_PAIR));
      }

      /* append suffix dimmed if room */
      if (suffix && suffix_len > 0) {
        int suffix_budget = avail - (int)path_len - linenum_str_len;
        if (suffix_budget > 0) {
          int draw_len = ((int)suffix_len <= suffix_budget) ? (int)suffix_len
                                                            : suffix_budget;
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
          if (!is_sel)
            attron(COLOR_PAIR(PATTERN_MATCH_PAIR));
          addnstr("...", 3);
          if (!is_sel)
            attroff(COLOR_PAIR(PATTERN_MATCH_PAIR));
        }
      }
      /* pad remainder */
      int drawn = (int)path_len + linenum_str_len;
      if (suffix && suffix_len > 0) {
        int added = ((int)suffix_len <= avail - drawn) ? (int)suffix_len
                                                       : avail - drawn;
        if (added > 0)
          drawn += added;
      }
      for (int p = drawn; p < avail; p++)
        addch(' ');

    } else if (base_len + 4 <= avail) {
      /* dir too long: ".../[tail]basename" */
      int tail = avail - base_len - 4; /* 4 = strlen("…/") */
      if (!is_sel)
        attron(A_DIM);
      mvaddnstr(screen_y, col, ".../", 4);
      if (tail > 0)
        addnstr(raw_display_str + dir_len - tail, tail);
      if (!is_sel)
        attroff(A_DIM);
      addnstr(raw_display_str + dir_len, base_len);
    } else {
      /* basename alone too long: truncate it */
      mvaddnstr(screen_y, col, raw_display_str + dir_len, avail);
    }
  } else {
    int show = (int)path_len < avail ? path_len : avail;
    mvaddnstr(screen_y, col, raw_display_str, show);
    if ((cmd_type == GREP || cmd_type == RG) && result->line_num >= 0) {
      if (!is_sel)
        attron(COLOR_PAIR(LINE_NUM_PAIR));
      printw(":%d: ", result->line_num);
      if (!is_sel)
        attroff(COLOR_PAIR(LINE_NUM_PAIR));
    }
    /* render suffix if exists */
    if (suffix && suffix_len > 0) {
      int suffix_budget = avail - (int)path_len - linenum_str_len;
      if (suffix_budget > 0) {
        int draw_len = ((int)suffix_len <= suffix_budget) ? (int)suffix_len
                                                          : suffix_budget;
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
        if (!is_sel)
          attron(COLOR_PAIR(PATTERN_MATCH_PAIR));
        addnstr("...", 3);
        if (!is_sel)
          attroff(COLOR_PAIR(PATTERN_MATCH_PAIR));
      }
    }
    int drawn = (suffix && (int)suffix_len > 0 &&
                 (int)suffix_len <= avail - (int)path_len - linenum_str_len)
                    ? show + linenum_str_len + suffix_len
                    : show + linenum_str_len;
    for (int p = drawn; p < avail; p++)
      addch(' ');
  }
  if (is_sel)
    attroff(A_REVERSE);
}

/* rendering logic */
static void render(viewport *v, UtilityOutput *out, char *pattern,
                   AVAILABLE_CMDS cmd_type, PATH_STATE path_state) {
  clear();

  /* list # of result by fd */
  char header[1024];
  snprintf(header, sizeof(header), " fdired  %d result%s", v->total_rows,
           v->total_rows == 1 ? "" : "s");
  attron(A_BOLD);
  mvaddstr(0, 0, header);
  attroff(A_BOLD);

  if (v->total_rows == 0) {
    attron(A_DIM);
    mvaddstr(2, 2, "no results");
    attroff(A_DIM);
  } else {
    for (int sy = 0; sy < v->height && v->top_row + sy < v->total_rows; sy++) {
      int idx = v->top_row + sy;
      int is_sel = (idx == v->curr_row);
      draw_entry(sy + 1, 1, &out->items[idx], pattern, is_sel, v->width,
                 cmd_type, path_state);
    }
  }

  /* footer status bar */
  char status[1024];
  int slen = snprintf(status, sizeof(status),
                      " [%d/%d]  j:↓  k:↑  gg: top  G: end  enter: open  tab: "
                      "show relative path y: copy filepath q: quit",
                      v->total_rows > 0 ? v->curr_row + 1 : 0, v->total_rows);
  /* right-pad to full width */
  while (slen < v->width && slen < (int)sizeof(status) - 1)
    status[slen++] = ' ';
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
static int cmd_append(Cmd *cmd, const char *arg) {
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
static int output_append(UtilityOutput *out, SearchResult r) {
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
static void inject_required_flags(Cmd *cmd, AVAILABLE_CMDS cmd_type) {
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

static void finish(int sig) {
  (void)sig;
  running = 0;
}
static void handle_resize(int sig) {
  (void)sig;
  resized = 1;
}

#endif /* FDIRED_H */
