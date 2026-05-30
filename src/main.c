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

#define DEBUG_CHAR '-'

void __print_debug(char* cmd, int n ){
  // debug
  char debug[n];
  memset(debug,DEBUG_CHAR , (n-1));
  debug[n-1]='\0';

  printf("%s\n", debug);
  printf("cmd: %s\n", cmd);  
  printf("%s\n", debug);

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
  
  FILE* fd = popen(cmd, mode);

  if(!fd){
    fprintf(stderr, "Some error occured!");
    return -1;
  }


  char buffer[1024];
  while (fgets(buffer, sizeof(buffer), fd) != NULL) {
      printf("%s", buffer);
  }

  int status = pclose(fd);
  if (status == -1) {
      perror("pclose failed");
      return -1;
  }

  return 0;

}


