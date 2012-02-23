/* Ultra Simple Partitionning library written from:

http://en.wikipedia.org/wiki/Master_boot_record
http://en.wikipedia.org/wiki/Extended_boot_record

to test:
 gcc -D TEST -I ../../../../bootable/recovery microfdisk.c -o microfdisk -g -Wall
*/
#define _LARGEFILE64_SOURCE
#include <linux/fs.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/types.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <ctype.h>
#include <linux/hdreg.h>

#include "droidboot_fstab.h"
#include "droidboot_ui.h"
#ifdef TEST
#define ui_print printf
#endif
/*
  useful ioctls:
 BLKRRPART: re-read partition table
 BLKGETSIZE: return device size /512 (long *arg)
*/
#define MAX_PART 32
struct block_dev {
	int fd;
	long lba_start[MAX_PART];
	long lba_count[MAX_PART];
	int type[MAX_PART];
	int num_partitions;
	int max_lba;
	int sectors;
	int heads;
	int extended_partition_offset;
};
#define MBR_PART_TABLE_START 0x1BE

/* partition types
   http://en.wikipedia.org/wiki/Partition_type
*/
#define EMPTY_PART 0x00
#define EXT_PART 0x05
#define VFAT_PART 0x0C
#define HIDDEN_PART -1
#define LINUX_PART 0x83

#define MB_TO_LBA(x) (x*1024*(1024/512))
#define LBA_TO_MB(x) (x/(1024*(1024/512)))

#define DEVICE_CREATION_TIMEOUT		5

/* gap reserved for partition table. (one track)
   http://en.wikipedia.org/wiki/Extended_boot_record#cite_note-fn_2-3
*/
#define EBR_GAP (s->sectors)

struct mbr_part {
	__u8 status;
	__u8 C1;
	__u8 H1;
	__u8 S1;
	__u8 type;
	__u8 C2;
	__u8 H2;
	__u8 S2;
	__u32 lba_start;
	__u32 lba_count;
};
static unsigned char buf[512];

/* write  the MBRs
   CHS is not supported. It is ignored anyway by linux
 */
static void write_mbr(struct block_dev *s)
{
	struct mbr_part *parts = (struct mbr_part *)(buf + MBR_PART_TABLE_START);
	int i;
	lseek(s->fd, 0, SEEK_SET);
	read(s->fd, buf, 512);
	for (i = 0; i < 3; i++) {
		memset(&parts[i], 0, sizeof(parts[i]));
		parts[i].type = s->type[i];
		parts[i].lba_start = s->lba_start[i];
		parts[i].lba_count = s->lba_count[i];
	}
	memset(&parts[3], 0, sizeof(parts[3]));
	if(s->num_partitions > 3)
	{
		parts[3].type = EXT_PART;
		parts[3].lba_start = s->lba_start[3] - EBR_GAP;
		s->extended_partition_offset = parts[3].lba_start;
		parts[3].lba_count = s->lba_start[s->num_partitions-1]
				     + s->lba_count[s->num_partitions-1]
				     - parts[3].lba_start;
	}
	lseek(s->fd, 0, SEEK_SET);
	write(s->fd, buf, 512);
}
/* write one of the EBRs
   CHS is not supported. It is ignored anyway by linux
 */
static void write_ebr(struct block_dev *s, int i)
{
	struct mbr_part *parts = (struct mbr_part *)(buf + MBR_PART_TABLE_START);
	lseek64(s->fd, (long long)(s->lba_start[i]-EBR_GAP)*512, SEEK_SET);
	read(s->fd, buf, 512);
	memset(buf, 0, 512);
	parts[0].status = 0;
	parts[0].type = s->type[i];
	parts[0].lba_start = EBR_GAP;
	parts[0].lba_count = s->lba_count[i];
	if (i + 1 < s->num_partitions) {
		parts[1].status = 0;
		parts[1].type = EXT_PART;
		parts[1].lba_start = s->lba_start[i+1]
				     - s->extended_partition_offset
				     - EBR_GAP;
		parts[1].lba_count = s->lba_count[i+1] + EBR_GAP;
	}
	buf[0x1FE]=0x55;
	buf[0x1FF]=0xAA;
	lseek64(s->fd, (long long)(s->lba_start[i]-EBR_GAP)*512, SEEK_SET);
	write(s->fd, buf, 512);
}
/*
   declare a new partition
   @param: size in MB
   @param: type: the filesystem type as defined in:
   http://en.wikipedia.org/wiki/Partition_type
*/
static int new_partition(struct block_dev *s, long size, int type)
{
	static int gap = 0;
	if (type == HIDDEN_PART) {
		gap += MB_TO_LBA(size);
	} else {
		int i = s->num_partitions++;
		s->lba_count[i] = MB_TO_LBA(size);
		s->type[i] = type;
		if (i == 0)
			s->lba_start[i] = EBR_GAP;
		else if (i<3)
			s->lba_start[i] = s->lba_start[i-1]
				+ s->lba_count[i-1];
		else
			s->lba_start[i] = s->lba_start[i-1]
				+ s->lba_count[i-1]
				+ EBR_GAP;
		s->lba_start[i] += gap;
		if ( s->max_lba < s->lba_start[i] + s->lba_count[i]) {
			pr_error("no space left..\n");
			return -ENOSPC;
		}
		gap = 0;
	}
	return 0;
}
/* open the disk, and fetch its info
   only 512b sector disk is supported...
*/
static struct block_dev *open_disk(char *disk)
{
	int fd = open(disk,O_RDWR);
	struct block_dev *ret;
	int sec_size, r, num_sec;
        struct hd_geometry geo;
	if (fd < 0) {
		pr_error("unable to open file");
		return NULL;
	}
	r = ioctl(fd,BLKSSZGET,&sec_size);
	if (r < 0 || sec_size != 512)
	{
		pr_error("sec_size != 512 ? what kind of disk is this?");
		return NULL;
	}
	r = ioctl(fd,BLKGETSIZE, &num_sec);
	if (r < 0)
	{
		pr_error("unable to get disk size");
		return NULL;
	}

        r = ioctl(fd, HDIO_GETGEO, &geo);
	if (r < 0)
	{
		pr_error("unable to get disk geometry");
		return NULL;
	}
	ret = calloc(1, sizeof(struct block_dev));
	ret->fd = fd;
	ret->max_lba = num_sec;
	ret->sectors = geo.sectors;
	ret->heads = geo.heads;
	return ret;
}
/* write the mbr and ebrs, and ask linux to reload the partitions table */
static int write_partitions(struct block_dev *dev)
{
	int i, r = 0;
	if ( dev->max_lba < dev->lba_start[dev->num_partitions-1]
		+ dev->lba_count[dev->num_partitions-1]){
		pr_error("no space left..\n");
		return -ENOSPC;
	}
	write_mbr(dev);
	for (i=3; i<dev->num_partitions; i++)
		write_ebr(dev, i);

	r = ioctl(dev->fd,BLKRRPART,NULL);
	if (r < 0)
		pr_error("unable to load partition table");
	return r;
}
extern int num_volumes;
extern Volume* device_volumes;
#define _EMMC_BASEDEVICE "/dev/block/mmcblk0"

#ifdef TEST
char *EMMC_BASEDEVICE;
#else
#define EMMC_BASEDEVICE _EMMC_BASEDEVICE
#endif

int is_emmc(Volume *v)
{
	if (v->device == 0)
		return 0;
	if (memcmp(v->device, EMMC_BASEDEVICE, sizeof(EMMC_BASEDEVICE)-1) != 0)
		return 0;
	return 1;

}
void ufdisk_umount_all(void)
{
	int i;
	/* ensure everybody is unmounted */
	for (i = 0; i < num_volumes; ++i) {
		Volume* v = device_volumes+i;
		if (!is_emmc(v))
			continue;
		ensure_path_unmounted(v->mount_point);
	}
}

static int wait_device_creation_timeout(const char *device, int times)
{
       int i, ret = -1;

       for (i = 0; i < times; i++) {
               if (access(device, R_OK) == 0) {
                       ret = 0;
                       break;
               }
               sleep(1);
       }

       return ret;
}


int ufdisk_ensure_partition_created(void)
{
	int i;
	int need_create = 0;
	int num_auto_part = 0;
	int allocated_space = 0;
	int max_space;
	int auto_size = 1;
	int r;
	struct block_dev *dev;

	dev = open_disk(EMMC_BASEDEVICE);
	if (dev == NULL)
		return -ENODEV;

	/* first pass to calculate remaining space,
	   and check if we need to partition */
	for (i = 0; i < num_volumes; ++i) {
		Volume* v = device_volumes+i;
		if (!is_emmc(v))
			continue;

		if (v->length > 0) /* +1 for EBR_GAPS */
			allocated_space += v->length + 1;
		else if (v->length == 0)
			num_auto_part++;
		else if (v->length < 0)
			continue;
		if (strcmp(v->fs_type, "hidden") == 0)
			continue;
		if (access(v->device, R_OK) != 0)
		    need_create = 1;

	}
	if (!need_create) {
		pr_info("no need to create partition...\n");
		return 0;
	}


	pr_info("emmc empty. Lets partition it!\n");
	ufdisk_umount_all();

	max_space = LBA_TO_MB(dev->max_lba);
	if (max_space < allocated_space) {
		pr_error("emmc is too small for this partition table! %dM VS %dM\n",
			 max_space, allocated_space);
		return -EINVAL;
	}
	if (num_auto_part)
		auto_size = ((max_space - allocated_space) / num_auto_part);
	/* start allocating */
	for (i = 0; i < num_volumes; ++i) {
		Volume* v = device_volumes+i;
		int type = LINUX_PART;
		if (!is_emmc(v))
			continue;
		if (v->length == 0)
			v->length = auto_size;
		if (strcmp(v->fs_type, "vfat") == 0)
			type = VFAT_PART;
		if (strcmp(v->fs_type, "hidden") == 0)
			type = HIDDEN_PART;
		r = new_partition(dev, v->length, type);
		if (r)
			return r;
	}
	r = write_partitions(dev);
	if (r)
		return r;
	/* check if everything is good */
	for (i = 0; i < num_volumes; ++i) {
		Volume* v = device_volumes+i;
		if (!is_emmc(v))
			continue;
		if (strcmp(v->fs_type, "hidden") == 0)
			continue;
		if (wait_device_creation_timeout(v->device, DEVICE_CREATION_TIMEOUT) != 0) {
			pr_error("fatal: unable to create partition: %s\n", v->device);
			return -ENODEV;
		}
	}

	/* format every body */
	for (i = 0; i < num_volumes; ++i) {
		Volume* v = device_volumes+i;
		if (!is_emmc(v))
			continue;
		if (strcmp(v->fs_type, "ext4") == 0) {
			pr_info("formatting %s\n", v->mount_point);
			if(format_volume(v->mount_point))
				r = -EINVAL;
		}
	}
	ui_print("PARTITION EMMC COMPLETE.\n");
	return r;
}

#ifdef TEST
int num_volumes;
Volume* device_volumes;
int ensure_path_unmounted(const char* path)
{
	printf("would ensure %s is unmounted\n", path);
	return 0;
}
int format_volume(const char* volume)
{
	printf("would format %s\n", volume);
	return 0;
}
/* stub version from roots. replaces /dev/block/mmcblk0 by /dev/sdb
   for testing purpose */
void load_volume_table(char *filename) {
    int alloc = 2;
    device_volumes = malloc(alloc * sizeof(Volume));

    // Insert an entry for /tmp, which is the ramdisk and is always mounted.
    device_volumes[0].mount_point = "/tmp";
    device_volumes[0].fs_type = "ramdisk";
    device_volumes[0].device = NULL;
    device_volumes[0].device2 = NULL;
    device_volumes[0].length = 0;
    num_volumes = 1;

    FILE* fstab = fopen(filename, "r");
    if (fstab == NULL) {
        pr_error("failed to open /etc/recovery.fstab (%s)\n", strerror(errno));
        return;
    }

    char buffer[1024];
    int i;
    while (fgets(buffer, sizeof(buffer)-1, fstab)) {
        for (i = 0; buffer[i] && isspace(buffer[i]); ++i);
        if (buffer[i] == '\0' || buffer[i] == '#') continue;

        char* original = strdup(buffer);

        char* mount_point = strtok(buffer+i, " \t\n");
        char* fs_type = strtok(NULL, " \t\n");
        char* device = strtok(NULL, " \t\n");
        // lines may optionally have a second device, to use if
        // mounting the first one fails.
        char* device2 = strtok(NULL, " \t\n");
        // lines may optionally have a partition size (in MB) hint to
	// be used by the partitionner
	// if ommitted, the remaining space will be evenly affected
        char* length = strtok(NULL, " \t\n");

        if (mount_point && fs_type && device) {
            while (num_volumes >= alloc) {
                alloc *= 2;
                device_volumes = realloc(device_volumes, alloc*sizeof(Volume));
            }
            device_volumes[num_volumes].mount_point = strdup(mount_point);
            device_volumes[num_volumes].fs_type = strdup(fs_type);
	    if (memcmp(device, _EMMC_BASEDEVICE, sizeof(_EMMC_BASEDEVICE)-1) == 0) {
		     char *buf = malloc(100);
		     snprintf(buf, 100,
			     "%s%s",EMMC_BASEDEVICE, device + sizeof(_EMMC_BASEDEVICE));
		     device_volumes[num_volumes].device = buf;
	    } else
		     device_volumes[num_volumes].device = strdup(device);
            device_volumes[num_volumes].device2 =
                device2 ? strdup(device2) : NULL;
            device_volumes[num_volumes].length =
                length ? atoi(length) : 0;
            ++num_volumes;
        } else {
            pr_error("skipping malformed recovery.fstab line: %s\n", original);
        }
        free(original);
    }

    fclose(fstab);

    printf("recovery filesystem table\n");
    printf("=========================\n");
    for (i = 0; i < num_volumes; ++i) {
        Volume* v = &device_volumes[i];
        printf("  %d %s %s %s %s\n", i, v->mount_point, v->fs_type,
               v->device, v->device2);
    }
    printf("\n");
}

int main(int argc, char ** argv)
{
	if (argc != 3) {
		printf("usage:\n# sudo %s /dev/sdb /path/to/recovery.fstab\n",argv[0]);
		return 1;
	}
	EMMC_BASEDEVICE = argv[1];
	load_volume_table(argv[2]);
	if (num_volumes == 1)
		return 1;
	ufdisk_ensure_partition_created();
	return 0;
}
#endif
