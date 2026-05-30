#define NOB_IMPLEMENTATION
#include "../nob.h/nob.h"

#define BUILD_FOLDER "./"
#define SRC_FOLDER "src/"

int main(int argc, char **argv) {
  NOB_GO_REBUILD_URSELF(argc, argv);
  // if (!nob_mkdir_if_not_exists(BUILD_FOLDER)) return 1;

  Nob_Cmd cmd={0};
  nob_cmd_append(&cmd, "cc", "-Wall", "-Wextra", "-o", BUILD_FOLDER"fdired", SRC_FOLDER"main.c");
  
  // nob_cc(&cmd);
  // nob_cc_flags(&cmd);
  // nob_cc_output(&cmd, BUILD_FOLDER "");
  // nob_cc_inputs(&cmd, SRC_FOLDER "fook
  if (!nob_cmd_run(&cmd)) return 1;
  return 0;
}
