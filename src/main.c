#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <signal.h>

#include <curses.h>
#include <locale.h>

#include "fdired.h"
#include "logger.h"

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "Usage: %s <fd|grep|rg> [args...]\n", argv[0]);
    return 1;
  }

  if (log_init("debug.txt")) {
    printf("Error opening file!\n");
    exit(1);
  }

  viewport v;
  Cmd cmd = {0};
  UtilityOutput output = {0};
  AVAILABLE_CMDS cmd_type;

  if (strcmp(argv[1], "fd") == 0)
    cmd_type = FD;
  else if (strcmp(argv[1], "find") == 0)
    cmd_type = FIND;
  else if (strcmp(argv[1], "grep") == 0)
    cmd_type = GREP;
  else if (strcmp(argv[1], "rg") == 0)
    cmd_type = RG;
  else {
    fprintf(stderr,
            "Unknown command: %s.\n We currently only support the following:\n\
        FIND\n\
        FD\n\
        GREP\n\
        RG\n",
            argv[1]);
    log_close();
    return 1;
  }

  /* (1) utility name (2) positional arg #1 pattern (3) positional arg #2
   * search_path*/
  InputArgs input_args = {
      .pattern = "",
      .search_path = "",
  };

  char *pos_args[3];
  int n_pos = 0;

  /* first arg is `./fdired`; no need to capture */
  for (int i = 1; i < argc; ++i) {
    /* capture the positional arg for some reason */
    if (argv[i][0] != '-' && n_pos < 3) {
      pos_args[n_pos++] = argv[i]; // two pointers to same data??
      log_write("pos arg #%d, %s\n", n_pos, argv[i]);
    }
    cmd_append(&cmd, argv[i]);
  }

  /* TODO! need to audit: umm this is super hacky */
  switch (cmd_type) {
  case FIND:
  case FD:
    if (n_pos != 2)
      break;
    input_args.search_path = pos_args[1];
    break;
  case GREP:
  case RG:
    if (n_pos != 3)
      break;
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
  signal(SIGINT, finish);
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

  char *line = NULL;
  size_t size = 0;
  ssize_t nread;

  while ((nread = getline(&line, &size, proc_fd)) != -1) {
    char *temp_line = strdup(line);
    SearchResult r =
        parse_single_output(temp_line, pos_args[1], input_args, cmd_type);
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

  v.total_rows = (int)output.count;
  v.top_row = 0;
  v.curr_row = 0;
  v.width = COLS;
  /* row 0 = header, LINES-1 = status bar */
  v.height = LINES - 2;

  char last_key = ' ';
  PATH_STATE path_state = PATH_FULL;

  render(&v, &output, pos_args[1], cmd_type, path_state);

  for (;;) {
    if (!running)
      break;

    /* handle terminal resize */
    if (resized) {
      resized = 0;
      endwin();
      refresh();
      v.width = COLS;
      v.height = LINES - 2;
      if (v.top_row + v.height <= v.curr_row)
        v.top_row = v.curr_row - v.height + 1;
      render(&v, &output, pos_args[1], cmd_type, path_state);
    }

    int key = getch();

    switch (key) {
    /* go down one row */
    case 'j':
      last_key = ' ';
      if (v.curr_row + 1 < v.total_rows) {
        v.curr_row++;
        if (v.curr_row >= v.top_row + v.height)
          v.top_row++;
      }
      render(&v, &output, pos_args[1], cmd_type, path_state);
      break;

    /* go up one row */
    case 'k':
      last_key = ' ';
      if (v.curr_row > 0) {
        v.curr_row--;
        if (v.curr_row < v.top_row)
          v.top_row--;
      }
      render(&v, &output, pos_args[1], cmd_type, path_state);
      break;

    /* go to the last row */
    case 'G':
      last_key = ' ';
      v.curr_row = v.total_rows - 1;
      v.top_row = v.total_rows - v.height;
      if (v.top_row < 0)
        v.top_row = 0;
      render(&v, &output, pos_args[1], cmd_type, path_state);
      break;

    /* go to the first row (excluding the top status row ) */
    case 'g':
      if (last_key == 'g') {
        v.curr_row = 0;
        v.top_row = 0;
        last_key = ' ';
        render(&v, &output, pos_args[1], cmd_type, path_state);
      } else {
        last_key = 'g';
      }
      break;

    case '\n':
    case '\r':
    /* open file in NVIM and navigate to the line number present in the
     * selected row */
    case KEY_ENTER: {
      if (v.total_rows == 0)
        break;
      SearchResult *r = &output.items[v.curr_row];

      /* temporarily null-terminate at file_end to isolate the path */
      char saved = '\0';
      if (r->file_end >= 0) {
        saved = r->display[r->file_end];
        r->display[r->file_end] = '\0';
      }

      /*TO DO: instead of using NVIM, query the configs*/
      FileType file_type = match_file_type(r->file);
      const char *file_type_handler = get_handler_for_file(file_type);

      char open_cmd[1200];
      if (r->line_num > 0)
        snprintf(open_cmd, sizeof(open_cmd), "%s +%d \"%s\"", file_type_handler,
                 r->line_num, r->file);
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

    /* toggle between fullpath and relative to input path  */
    case '\t':
      last_key = ' ';
      path_state = (path_state == PATH_FULL) ? PATH_REL_TO_INPUT : PATH_FULL;
      render(&v, &output, pos_args[1], cmd_type, path_state);
      break;

    /* copy filepath to current row to clipboard
     * should work with linux/macos at the least */
    case 'y':
      if (v.total_rows == 0)
        break;
      SearchResult *r = &output.items[v.curr_row];

      /* temporarily null-terminate at file_end to isolate the path */
      char saved = '\0';
      if (r->file_end >= 0) {
        saved = r->display[r->file_end];
        r->display[r->file_end] = '\0';
      }
      log_write("[KEYLOG]: y : copy path %s to clipboard \n", r->display);
      FILE *clip = popen(
          "xclip -selection clipboard 2>/dev/null || pbcopy 2>/dev/null", "w");
      if (clip) {
        fputs(r->display, clip);
        pclose(clip);
      }
      /* restore the display string */
      if (r->file_end >= 0)
        r->display[r->file_end] = saved;

      break;

    /* exit */
    case 'q':
      running = 0;
      break;

    /* for capturing two keystroke patterns like `gg` */
    default:
      if (key != ERR)
        last_key = ' ';
      break;
    }
  }

  if (proc_fd)
    pclose(proc_fd);
  for (size_t i = 0; i < output.count; i++)
    free(output.items[i].display);

  free(output.items);
  endwin();
  log_close();

  return 0;
}
