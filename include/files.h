#pragma once

#include <sys/stat.h>

unsigned long get_o_flags(void);

unsigned int setup_fds(void);

int open_files(void);
void close_files(void);
void regenerate_fds(void);
int rand_file_fd(void);

void parse_devices(void);
const char *map_dev(dev_t, mode_t);

extern unsigned int nr_file_fds;
extern char *victim_path;
extern const char **fileindex;
extern unsigned int files_in_index;

#define NR_FILE_FDS 250
