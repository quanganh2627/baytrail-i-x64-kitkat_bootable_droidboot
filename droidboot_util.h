#ifndef DROIDBOOT_UTIL_H
#define DROIDBOOT_UTIL_H

#include <diskconfig/diskconfig.h>

int named_file_write(const char *filename, const unsigned char *what,
		size_t sz);
int kexec_linux(char *kernel, char *initrd, char *cmdline);
int is_valid_blkdev(const char *node);
int execute_command(const char *cmd);
int mount_partition(struct part_info *ptn);
void die(void);
int erase_partition(struct part_info *ptn);
void apply_sw_update(const char *location, int send_fb_ok);
int check_ext_superblock(struct part_info *ptn, int *sb_present);
int unmount_partition(struct part_info *ptn);
int mount_partition_device(const char *device, const char *type, char *mountpoint);
int ext4_filesystem_checks(const char *device);

#endif
