#ifndef FAT16_FILE_READER_H
#define FAT16_FILE_READER_H

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

struct super_t {
    uint8_t jump_code[3];
    char oem_name[8];
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t fat_count;
    uint16_t root_dir_capacity;
    uint16_t logical_sectors16;
    uint8_t media_type;
    uint16_t sectors_per_fat;
    uint16_t chs_sectors_per_track;
    uint16_t chs_track_per_cylinder;
    uint32_t hidden_sectors;
    uint32_t logical_sectors32;
    uint8_t media_id;
    uint8_t chs_head;
    uint8_t ext_bpb_signature;
    uint32_t serial_number;
    char volume_label[11];
    char fsid[8];
    uint8_t boot_code[488];
    uint16_t magic;
} __attribute__ (( packed ));

struct disk_t {
    FILE *fp;
};

struct dir_entry_t_ff {
    char name[8];
    char extension[3];
    uint8_t attributes;
    char reserved[10];
    uint16_t time_created;
    uint16_t date_created;
    uint16_t starting_cluster;
    uint32_t file_size;
} __attribute__ (( packed ));

struct dir_entry_t_dd {
    char name[8];
    char extension[3];
    uint8_t attributes;
    char reserved[10];
    uint16_t time_created;
    uint16_t date_created;
    uint16_t starting_cluster;
    uint32_t file_size;
} __attribute__ (( packed ));

struct dir_entry_t {
    char name[32];
} __attribute__ (( packed ));

struct dir_t {
    int nr_of_files;
    struct dir_entry_t *files;
    int current;
};

struct volume_t {
    struct super_t *super;

    struct disk_t *disk;
    uint16_t *fat;

    uint32_t fat_size;
    uint16_t total_sectors;
    uint16_t root_dir_sectors;
    uint16_t first_data_sector;
    uint16_t first_fat_sector;
    uint32_t data_sectors;
    uint32_t total_clusters;
    uint16_t first_root_dir_sector;

} __attribute__ (( packed ));

struct file_t {
    char name[8];
    char extension[3];
    uint32_t size;
    uint16_t starting_cluster;
    uint16_t *cluster_chain;
    int number_of_clusters;
    char *data;
    char *ptr_to_data;
    uint32_t current_position;
} __attribute__ (( packed ));

struct disk_t *disk_open_from_file(const char *volume_file_name);
int disk_read(struct disk_t *pdisk, int32_t first_sector, void *buffer, int32_t sectors_to_read);
int disk_close(struct disk_t *pdisk);
struct volume_t *fat_open(struct disk_t *pdisk, uint32_t first_sector);
int fat_close(struct volume_t *pvolume);
struct file_t *file_open(struct volume_t *pvolume, const char *file_name);
int file_close(struct file_t *stream);
size_t file_read(void *ptr, size_t size, size_t nmemb, struct file_t *stream);
int32_t file_seek(struct file_t *stream, int32_t offset, int whence);
struct dir_t *dir_open(struct volume_t *pvolume, const char *dir_path);
int dir_read(struct dir_t *pdir, struct dir_entry_t *pentry);
int dir_close(struct dir_t *pdir);

#endif //FAT16_FILE_READER_H
