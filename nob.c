#define NOB_IMPLEMENTATION
#include "nob.h"
#include <string.h>

#define BUILD_FOLDER "./"
#define SRC_FOLDER "src/"

int main(int argc, char **argv) {
  NOB_GO_REBUILD_URSELF(argc, argv);
  // if (!nob_mkdir_if_not_exists(BUILD_FOLDER)) return 1;
  // setenv("MallocNanoZone", "0", 1);
  Nob_Cmd cmd={0};
  nob_cmd_append(&cmd, "cc","-Wall", "-Wextra", "-o", BUILD_FOLDER"fdired", SRC_FOLDER"main.c","-lncurses");

  /* `./nob debug` builds with logging (writes debug.txt); prod builds omit -DFDIRED_DEBUG */
  if (argc > 1 && strcmp(argv[1], "debug") == 0)
    nob_cmd_append(&cmd, "-DFDIRED_DEBUG");

  if (!nob_cmd_run(&cmd)) return 1;
  return 0;
}
