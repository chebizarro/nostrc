#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <stdint.h>

// Declare the fuzzer entry so we can drive it without libFuzzer runtime
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static int process_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return 0; }
    uint8_t *buf = (uint8_t*)malloc((size_t)n);
    if (!buf) { fclose(f); return 0; }
    size_t r = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (r > 0) LLVMFuzzerTestOneInput(buf, r);
    free(buf);
    return 1;
}

static void process_path(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (!d) return;
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
            char sub[4096];
            snprintf(sub, sizeof(sub), "%s/%s", path, de->d_name);
            process_path(sub);
        }
        closedir(d);
    } else if (S_ISREG(st.st_mode)) {
        process_file(path);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <file_or_dir> [more ...]\n", argv[0]);
        return 1;
    }
    for (int i = 1; i < argc; i++) process_path(argv[i]);
    return 0;
}
