#ifndef UTIL_H
#define UTIL_H

#include <diskconfig/diskconfig.h>

int named_file_write(const char *filename, const unsigned char *what,
		size_t sz);
int kexec_linux(char *kernel, char *initrd, char *cmdline);
int is_valid_blkdev(const char *node);
int execute_command(const char *cmd);
int unmount_partition(char *mountpoint);
int mount_partition(struct part_info *ptn);
void die(void);

#endif
