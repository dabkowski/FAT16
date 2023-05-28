#include "file_reader.h"
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>

#define BYTES_PER_SECTOR 512
#define ATTR_READ_ONLY 0x01
#define ATTR_HIDDEN 0x02
#define ATTR_SYSTEM 0x04
#define ATTR_VOLUME_ID 0x08
#define ATTR_DIRECTORY 0x10
#define ATTR_ARCHIVE 0x20

struct disk_t *disk_open_from_file(const char *volume_file_name) {
    if (volume_file_name == NULL) {
        errno = EFAULT;
        return NULL;
    }

    FILE *fp = fopen(volume_file_name, "rb");
    if (fp == NULL) {
        return NULL;
    }

    struct disk_t *disk = malloc(sizeof(struct disk_t));
    if (disk == NULL) {
        errno = ENOMEM;
        return NULL;
    }

    disk->fp = fp;

    return disk;
}

int disk_read(struct disk_t *pdisk, int32_t first_sector, void *buffer, int32_t sectors_to_read) {
    if (pdisk == NULL || pdisk->fp == NULL || buffer == NULL) {
        errno = EFAULT;
        return -1;
    }

    int32_t total_sectors = 0;
    fseek(pdisk->fp, 0, SEEK_END);
    int32_t file_size = ftell(pdisk->fp);
    if (file_size < 0) {
        errno = EIO;
        return -1;
    }
    total_sectors = file_size / 512;

    if (first_sector + sectors_to_read > total_sectors) {
        errno = ERANGE;
        return -1;
    }

    fseek(pdisk->fp, first_sector * 512, SEEK_SET);

    size_t bytes_read = fread(buffer, 512, sectors_to_read, pdisk->fp);
    if ((int32_t) bytes_read != sectors_to_read) {
        errno = EIO;
        return -1;
    }

    return sectors_to_read;
}

int disk_close(struct disk_t *pdisk) {
    if (pdisk == NULL) {
        errno = EFAULT;
        return -1;
    }
    fclose(pdisk->fp);
    free(pdisk);
    return 0;
}

struct volume_t *fat_open(struct disk_t *pdisk, uint32_t first_sector) {
    if (pdisk == NULL || pdisk->fp == NULL) {
        errno = EFAULT;
        return NULL;
    }

    struct super_t super;
    int32_t res = disk_read(pdisk, first_sector, &super, 1);
    if (res != 1) {
        return NULL;
    }

    if (super.bytes_per_sector != BYTES_PER_SECTOR) {
        errno = EINVAL;
        return NULL;
    }

    struct volume_t *volume = malloc(sizeof(struct volume_t));
    if (volume == NULL) {
        errno = ENOMEM;
        return NULL;
    }

    volume->super = malloc(sizeof(struct super_t));
    if (volume->super == NULL) {
        free(volume);
        errno = ENOMEM;
        return NULL;
    }
    memcpy(volume->super, &super, sizeof(struct super_t));

    volume->disk = pdisk;

    volume->total_sectors = super.logical_sectors16;
    if (volume->total_sectors == 0) {
        volume->total_sectors = super.logical_sectors32;
    }

    volume->fat_size = super.sectors_per_fat * super.bytes_per_sector;
    volume->data_sectors = volume->total_sectors - (super.reserved_sectors + (super.fat_count * super.sectors_per_fat) +
                                                    volume->root_dir_sectors);
    volume->first_fat_sector = super.reserved_sectors;
    volume->root_dir_sectors = ((super.root_dir_capacity * 32) + (super.bytes_per_sector - 1)) / super.bytes_per_sector;
    volume->first_data_sector =
            super.reserved_sectors + (super.fat_count * super.sectors_per_fat) + volume->root_dir_sectors;
    volume->first_root_dir_sector = volume->first_data_sector - volume->root_dir_sectors;

    volume->fat = malloc(volume->fat_size);
    if (volume->fat == NULL) {
        free(volume->super);
        free(volume);
        errno = ENOMEM;
        return NULL;
    }

    res = disk_read(pdisk, volume->first_fat_sector, volume->fat, super.sectors_per_fat);
    if (res != super.sectors_per_fat) {
        free(volume->fat);
        free(volume->super);
        free(volume);
    }

    uint8_t *fat2 = malloc(volume->fat_size);
    if (fat2 == NULL) {
        free(volume->fat);
        free(volume->super);
        free(volume);
        errno = ENOMEM;
        return NULL;
    }

    res = disk_read(pdisk, volume->first_fat_sector + super.sectors_per_fat, fat2, super.sectors_per_fat);
    if (res != super.sectors_per_fat) {
        free(fat2);
        free(volume->fat);
        free(volume->super);
        free(volume);
        return NULL;
    }

    if (memcmp(volume->fat, fat2, volume->fat_size) != 0) {
        free(fat2);
        free(volume->fat);
        free(volume->super);
        free(volume);
        errno = EINVAL;
        return NULL;
    }
    free(fat2);

    return volume;
}

int fat_close(struct volume_t *pvolume) {
    free(pvolume->super);
    free(pvolume->fat);
    free(pvolume);
    return 0;
}


void split_filename(char *filename, char *name, char *ext) {
    char *token = strtok(filename, ".");
    if (token) {
        strncpy(name, token, 8);
        token = strtok(NULL, ".");
        if (token) {
            strncpy(ext, token, 3);
        }
    }
}

void fill_name_with_spaces(char *name, int size) {
    for (int i = 0; i < size; i++) {
        if (isalpha(name[i]))
            continue;
        name[i] = ' ';
    }
}

int file_directory(struct dir_entry_t_ff *file) {
    if (file->attributes & ATTR_DIRECTORY) {
        errno = EISDIR;
        return 1;
    } else {
        return 0;
    }
}

struct file_t *file_open(struct volume_t *pvolume, const char *file_name) {
    if (pvolume == NULL || file_name == NULL) {
        errno = EFAULT;
        return NULL;
    }

    struct file_t *file = malloc(sizeof(struct file_t));
    if (file == NULL) {
        errno = ENOMEM;
        return NULL;
    }

    struct dir_entry_t_ff *current_entry = calloc(1, sizeof(struct dir_entry_t_ff));
    if (current_entry == NULL) {
        errno = ENOMEM;
        free(file);
        return NULL;
    }

    char *buffer = calloc(sizeof(char), 512);
    if (buffer == NULL) {
        errno = ENOMEM;
        free(file);
        free(current_entry);
        return NULL;
    }

    char name[8] = {0};
    char ext[3] = {0};
    char *filename = strdup(file_name);
    split_filename(filename, name, ext);
    fill_name_with_spaces(name, 8);
    fill_name_with_spaces(ext, 3);

    char *iterator = buffer;
    for (uint16_t i = 0; i < pvolume->root_dir_sectors; i++) {
        disk_read(pvolume->disk, pvolume->first_root_dir_sector, buffer, 1);
        for (int j = 0; j < 512 / 32; j++) {
            memcpy(current_entry, &buffer[j * 32], 32);
            if (memcmp(current_entry->name, name, 8) == 0 && memcmp(current_entry->extension, ext, 3) == 0) {

                if (file_directory(current_entry)) {
                    free(iterator);
                    free(current_entry);
                    free(file);
                    return NULL;
                }

                memcpy(file->name, name, 8);
                memcpy(file->extension, ext, 3);
                file->size = current_entry->file_size;
                file->starting_cluster = current_entry->starting_cluster;

                free(iterator);

                iterator = calloc(sizeof(char), 512);
                if (iterator == NULL) {
                    errno = ENOMEM;
                    free(file);
                    free(current_entry);
                    return NULL;
                }

                file->number_of_clusters = 0;
                for (int k = file->starting_cluster; k < 0xFFF8;) {
                    file->number_of_clusters++;
                    k = pvolume->fat[k];
                }
                file->cluster_chain = calloc(file->number_of_clusters, sizeof(uint16_t));
                if (file->cluster_chain == NULL) {
                    errno = ENOMEM;
                    free(file);
                    free(current_entry);
                    return NULL;
                }
                for (int k = file->starting_cluster, l = 0; k < 0xFFF8; l++) {
                    file->cluster_chain[l] = k;
                    k = pvolume->fat[k];
                }

                file->data = calloc(1, file->size + 1 + 512);
                if (file->data == NULL) {
                    errno = ENOMEM;
                    free(file);
                    free(current_entry);
                    free(file->cluster_chain);
                    return NULL;
                }

                uint32_t sectors_read = 0;
                for (int k = 0; k < file->number_of_clusters && sectors_read * 512 < file->size; k++) {
                    disk_read(pvolume->disk, pvolume->first_data_sector + file->cluster_chain[k] - 2, iterator, 1);
                    memcpy(file->data + k * 512, iterator, 512);
                    sectors_read++;
                }

                free(iterator);
                free(current_entry);

                file->ptr_to_data = file->data;
                file->current_position = 0;

                return file;
            }
        }
    }
    free(file);
    free(current_entry);
    free(buffer);
    errno = ENOENT;
    return NULL;
}

int file_close(struct file_t *stream) {
    if (stream == NULL) {
        errno = EFAULT;
        return -1;
    }
    free(stream->ptr_to_data);
    free(stream->cluster_chain);
    free(stream);
    return 0;
}

size_t file_read(void *ptr, size_t size, size_t nmemb, struct file_t *stream) {
    if (ptr == NULL || stream == NULL) {
        errno = EFAULT;
        return -1;
    }

    char *out_ptr = ptr;
    int read_bytes = 0;
    for (size_t i = 0; i < size * nmemb && stream->current_position < stream->size; i++) {
        out_ptr[i] = stream->data[stream->current_position];
        read_bytes++;
        stream->current_position++;
    }

    return read_bytes / size;
}

int32_t file_seek(struct file_t *stream, int32_t offset, int whence) {
    if (stream == NULL) {
        errno = EFAULT;
        return -1;
    }

    uint32_t new_position;
    switch (whence) {
        case SEEK_SET:
            new_position = offset;
            break;
        case SEEK_CUR:
            new_position = stream->current_position + offset;
            break;
        case SEEK_END:
            new_position = stream->size + offset;
            break;
        default:
            errno = EINVAL;
            return -1;
    }

    if (new_position > stream->size) {
        errno = ENXIO;
        return -1;
    }

    stream->current_position = new_position;
    return new_position;
}

void remove_spaces_from_name(struct dir_entry_t_dd *dir, struct dir_entry_t *out) {

    if (dir->extension[0] == ' ') {
        char *iterator = dir->name;
        int i = 0;
        while (*iterator && isalpha(*iterator)) {
            out->name[i++] = *iterator++;
        }
        out->name[i] = '\0';
    } else {
        char *iterator = dir->name;
        int i = 0;
        while (*iterator && isalpha(*iterator) && i < 8) {
            out->name[i++] = *iterator++;
        }
        out->name[i] = '.';
        i++;
        iterator = dir->extension;
        int how_many_in_ext = 0;
        for (int j = 0; j < 3; j++) {
            if (isalpha(dir->extension[j]))
                how_many_in_ext++;
        }
        for (int j = 0; j < how_many_in_ext; j++) {
            out->name[i++] = *iterator++;
        }
        out->name[i] = '\0';
    }
}

int dir_or_file(struct dir_entry_t_dd *dir) {
    if (dir->attributes & ATTR_VOLUME_ID) {
        return 0;
    }
    return 1;
}

struct dir_t *dir_open(struct volume_t *pvolume, const char *dir_path) {
    if (pvolume == NULL || dir_path == NULL) {
        errno = EFAULT;
        return NULL;
    }

    struct dir_t *dir = malloc(sizeof(struct dir_t));
    if (dir == NULL) {
        errno = ENOMEM;
        return NULL;
    }

    struct dir_entry_t_dd *current_entry = calloc(1, sizeof(struct dir_entry_t_dd));
    if (current_entry == NULL) {
        errno = ENOMEM;
        free(dir);
        return NULL;
    }

    char *buffer = calloc(sizeof(char), 512);
    if (buffer == NULL) {
        errno = ENOMEM;
        free(dir);
        free(current_entry);
        return NULL;
    }

    dir->nr_of_files = 0;
    int end_flag = 0;

    int is_found = 0;
    if (strcmp(dir_path, "\\") != 0) {
        struct dir_entry_t temporary;
        for (uint16_t i = 0; i < pvolume->root_dir_sectors; i++) {
            if (end_flag) {
                break;
            }
            disk_read(pvolume->disk, pvolume->first_root_dir_sector, buffer, 1);
            for (int j = 0; j < 512 / 32; j++) {
                memcpy(current_entry, &buffer[j * 32], 32);
                remove_spaces_from_name(current_entry, &temporary);
                if (strcmp(temporary.name, dir_path) == 0 && (!current_entry->attributes & ATTR_DIRECTORY)
                    || current_entry->attributes & ATTR_VOLUME_ID) {
                    errno = ENOTDIR;
                    free(buffer);
                    free(current_entry);
                    free(dir);
                    return NULL;
                }
                if (strcmp(temporary.name, dir_path) == 0 && current_entry->attributes & ATTR_DIRECTORY) {
                    is_found = 1;
                }
                if (current_entry->name[0] == 0x00) {
                    end_flag = 1;
                    break;
                }
            }
        }
    }

    if (is_found == 0 && strcmp(dir_path, "\\") != 0) {
        errno = ENOENT;
        free(buffer);
        free(current_entry);
        free(dir);
        return NULL;
    }

    end_flag = 0;
    for (uint16_t i = 0; i < pvolume->root_dir_sectors; i++) {
        if (end_flag) {
            break;
        }
        disk_read(pvolume->disk, pvolume->first_root_dir_sector, buffer, 1);
        for (int j = 0; j < 512 / 32; j++) {
            memcpy(current_entry, &buffer[j * 32], 32);
            if (!dir_or_file(current_entry))
                continue;
            if ((uint8_t) current_entry->name[0] == 0xE5) {
                continue;
            }
            if (current_entry->name[0] == 0x00) {
                end_flag = 1;
                break;
            }
            dir->nr_of_files++;
        }
    }

    dir->files = calloc(dir->nr_of_files, sizeof(struct dir_entry_t));
    if (dir->files == NULL) {
        errno = ENOMEM;
        free(dir);
        free(current_entry);
        return NULL;
    }

    int iter = 0;
    end_flag = 0;
    for (uint16_t i = 0; i < pvolume->root_dir_sectors; i++) {
        if (end_flag) {
            break;
        }
        disk_read(pvolume->disk, pvolume->first_root_dir_sector, buffer, 1);
        for (int j = 0; j < 512 / 32; j++) {
            memcpy(current_entry, &buffer[j * 32], 32);
            if (!dir_or_file(current_entry))
                continue;
            if ((uint8_t) current_entry->name[0] == 0xE5) {
                continue;
            }
            if (current_entry->name[0] == 0x00) {
                end_flag = 1;
                break;
            }
            remove_spaces_from_name(current_entry, &dir->files[iter]);
            iter++;
        }
    }
    free(current_entry);
    free(buffer);
    dir->current = 0;
    return dir;
}

int dir_read(struct dir_t *pdir, struct dir_entry_t *pentry) {
    if (pdir == NULL || pentry == NULL) {
        errno = EFAULT;
        return -1;
    }

    if (pdir->current == pdir->nr_of_files) {
        return 1;
    }

    memcpy(pentry, &pdir->files[pdir->current], sizeof(struct dir_entry_t));
    pdir->current++;

    return 0;
}

int dir_close(struct dir_t *pdir) {
    if (pdir == NULL) {
        errno = EFAULT;
        return -1;
    }
    free(pdir->files);
    free(pdir);
    return 0;
}
