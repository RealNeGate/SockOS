// This program is responsible for creating the ramdisk which Beans uses to
// hold the initial set of drivers.
//
// bake_initrd [out_path] [in_path...]
//
#define _CRT_SECURE_NO_WARNINGS
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#define fileno _fileno
#define fstat _fstat64
#define stat _stat64
#endif

typedef struct {
    uint32_t data_len;
    char path[24];
    char data[];
} FileEntry;

int main(int argc, char** argv) {
    if (argc <= 1) {
        fprintf(stderr, "error: there's no output path, much less an input");
        return 1;
    }

    FILE* out_fp = fopen(argv[1], "wb");
    if (out_fp == NULL) {
        fprintf(stderr, "error: bad output path: %s", argv[1]);
        return 1;
    }

    size_t file_len = 0;
    for (int i = 2; i < argc; i++) {
        const char* name = strrchr(argv[i], '/');
        if (name == NULL) { name = argv[i]; }
        else { name += 1; }

        FILE* file = fopen(argv[i], "rb");
        if (file == NULL) {
            fprintf(stderr, "error: bad input path: %s", argv[i]);
            return 1;
        }

        int descriptor = fileno(file);
        struct stat file_stats;
        if (fstat(descriptor, &file_stats) == -1) {
            fprintf(stderr, "error: bad input path: %s (can't fstat?)", argv[i]);
            return 1;
        }

        size_t len = file_stats.st_size;
        size_t cap = (len + 16) & -16ull; // round_up(len + 1, 16)
        char* data = malloc(cap);

        fseek(file, 0, SEEK_SET);
        fread(data, 1, len, file);
        memset(&data[len], 0, cap - len);
        fclose(file);

        FileEntry header = { .data_len = len };
        header.path[0] = '/';
        strncpy(header.path+1, name, 23);

        fwrite(&header, sizeof(FileEntry), 1, out_fp);
        fwrite(data, cap, 1, out_fp);
        free(data);

        file_len += sizeof(FileEntry) + cap;
        printf("Added '%s' (%zu bytes)\n", name, len);
    }

    // NULL file
    FileEntry header = { .data_len = 0 };
    fwrite(&header, sizeof(FileEntry), 1, out_fp);
    file_len += sizeof(FileEntry);

    static const char zero_page[4096];
    fwrite(zero_page, 4096 - (file_len & 4095), 1, out_fp);
    fclose(out_fp);
    return 0;
}
