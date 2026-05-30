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
int main(int argc, char** argv)
{

  char flags[512] = "--absolute-path";  /* always on */
  int c;

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

  // capture pos args [PATTERN] [PATH]
  const char *pattern = "";
  const char *path = ".";

  // if we use --glob, the pattern passed will be the
  // one searched on.
  if (optind < argc) pattern = argv[optind++];
  if (optind < argc) path = argv[optind];

  char cmd[1024];

  snprintf(
      cmd,
      sizeof(cmd),
      "fd %s \"%s\" %s",
      flags,
      pattern,
      path
  );
  
  // debug
  char debug[100];
  memset(debug, '*' ,99);
  debug[99]='\0';
  printf("%s\n", debug);
  printf("cmd: %s\n", cmd);  
  printf("%s\n", debug);

  
  char* mode= "r";
  FILE* fd = popen(cmd, mode);

  if(!fd){
    return -1;
  }

  printf("Fd cmd exec!\n");

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


