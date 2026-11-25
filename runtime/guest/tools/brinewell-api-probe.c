// Brinewell: classify a Windows game as Neptune, Mullion, or Venus.
//
// The probe reads PE import and delay-import tables from a bounded set of
// executables and looks for D3D11/D3D12 DLL dependencies. A short bounded
// string scan catches games that load those DLLs dynamically. It does not
// execute anything and does not need Wine.
//
// Exit codes: 0 = neptune, 1 = venus, 2 = usage error, 3 = mullion.
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PROBE_MAX_FILE_BYTES (512ull * 1024 * 1024)
#define PROBE_MAX_EVIDENCE 64
#define PROBE_MAX_SCAN_FILES 512
#define PROBE_MAX_SCAN_DEPTH 6

struct buffer {
    unsigned char *data;
    size_t size;
};

struct evidence {
    char items[PROBE_MAX_EVIDENCE][PATH_MAX + 128];
    size_t count;
};

struct pe_info {
    const struct buffer *b;
    uint32_t size_of_headers;
    uint16_t num_sections;
    const unsigned char *sections; /* first IMAGE_SECTION_HEADER */
    uint32_t import_rva;
    uint32_t delay_import_rva;
};

static int add_evidence(struct evidence *e, const char *path, const char *what) {
    if (e->count >= PROBE_MAX_EVIDENCE) return 0;
    size_t plen = strlen(path);
    size_t wlen = strlen(what);
    if (plen > PATH_MAX) plen = PATH_MAX;
    if (wlen > 48) wlen = 48;
    if (plen + 1 + wlen + 1 > sizeof(e->items[0])) return 0;
    memcpy(e->items[e->count], path, plen);
    e->items[e->count][plen] = ':';
    memcpy(e->items[e->count] + plen + 1, what, wlen);
    e->items[e->count][plen + 1 + wlen] = '\0';
    e->count++;
    return 1;
}

static uint16_t rd16(const unsigned char *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int read_file(int fd, struct buffer *out) {
    memset(out, 0, sizeof(*out));
    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return -1; }
    if (!S_ISREG(st.st_mode) || st.st_size <= 0 || (uint64_t)st.st_size > PROBE_MAX_FILE_BYTES)
        { close(fd); return -1; }
    FILE *f = fdopen(fd, "rb");
    if (!f) { close(fd); return -1; }
    size_t size = (size_t)st.st_size;
    unsigned char *data = malloc(size ? size : 1);
    if (!data) {
        fclose(f);
        return -1;
    }
    size_t got = fread(data, 1, size, f);
    int failed = ferror(f) || got != size;
    fclose(f);
    if (failed) {
        free(data);
        return -1;
    }
    out->data = data;
    out->size = size;
    return 0;
}

static void free_file(struct buffer *b) {
    free(b->data);
    memset(b, 0, sizeof(*b));
}

static int rva_to_offset(const struct pe_info *pe, uint32_t rva, size_t *offset) {
    if (rva < pe->size_of_headers) {
        if (rva >= pe->b->size) return -1;
        *offset = rva;
        return 0;
    }
    for (uint16_t i = 0; i < pe->num_sections; i++) {
        const unsigned char *s = pe->sections + (size_t)i * 40;
        uint32_t virtual_size = rd32(s + 8);
        uint32_t virtual_address = rd32(s + 12);
        uint32_t raw_size = rd32(s + 16);
        uint32_t raw_pointer = rd32(s + 20);
        uint32_t span = virtual_size > raw_size ? virtual_size : raw_size;
        if (rva < virtual_address) continue;
        uint32_t delta = rva - virtual_address;
        if (delta >= span) continue;
        if (delta >= raw_size) return -1;
        uint64_t file_offset = (uint64_t)raw_pointer + delta;
        if (file_offset >= pe->b->size) return -1;
        *offset = (size_t)file_offset;
        return 0;
    }
    return -1;
}

static int read_cstring_rva(const struct pe_info *pe, uint32_t rva, char *out, size_t cap) {
    size_t offset = 0;
    if (cap == 0 || rva_to_offset(pe, rva, &offset) != 0) return -1;
    size_t n = 0;
    while (offset + n < pe->b->size && n + 1 < cap) {
        unsigned char c = pe->b->data[offset + n];
        if (c == 0) {
            out[n] = '\0';
            return n ? 0 : -1;
        }
        out[n++] = (char)c;
    }
    out[cap - 1] = '\0';
    return -1;
}

static void lower_basename(const char *name, char *out, size_t cap) {
    const char *base = name;
    for (const char *p = name; *p; p++) {
        if (*p == '\\' || *p == '/') base = p + 1;
    }
    size_t n = 0;
    for (const char *p = base; *p && n + 1 < cap; p++) {
        unsigned char c = (unsigned char)*p;
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
        out[n++] = (char)c;
    }
    out[n] = '\0';
}

static int is_d3d_dependency(const char *name) {
    char lower[128];
    lower_basename(name, lower, sizeof(lower));
    if (strcmp(lower, "d3d11.dll") == 0 || strcmp(lower, "d3d12.dll") == 0 ||
        strcmp(lower, "d3d12core.dll") == 0 || strcmp(lower, "d3d11on12.dll") == 0)
        return 2;
    if (strcmp(lower, "d3d10.dll") == 0 || strcmp(lower, "d3d10_1.dll") == 0 ||
        strcmp(lower, "d3d10core.dll") == 0) return 1;
    return 0;
}

static int parse_pe(const char *path, const struct buffer *b, struct pe_info *pe) {
    memset(pe, 0, sizeof(*pe));
    pe->b = b;
    if (b->size < 0x40 || b->data[0] != 'M' || b->data[1] != 'Z') return -1;
    uint32_t pe_offset = rd32(b->data + 0x3c);
    if ((uint64_t)pe_offset + 24 > b->size) return -1;
    const unsigned char *peh = b->data + pe_offset;
    if (peh[0] != 'P' || peh[1] != 'E' || peh[2] != 0 || peh[3] != 0) return -1;
    uint16_t num_sections = rd16(peh + 6);
    uint16_t optional_size = rd16(peh + 20);
    if (num_sections == 0 || num_sections > 96) return -1;
    uint64_t optional_offset = (uint64_t)pe_offset + 24;
    if (optional_offset + optional_size > b->size || optional_size < 64) return -1;
    const unsigned char *opt = b->data + optional_offset;
    uint16_t magic = rd16(opt);
    size_t data_dir_offset;
    if (magic == 0x10b) {
        data_dir_offset = 96;
    } else if (magic == 0x20b) {
        data_dir_offset = 112;
    } else {
        return -1;
    }
    if (optional_size < data_dir_offset + 16) return -1;
    uint32_t num_dirs = rd32(opt + data_dir_offset - 4);
    if (num_dirs > 16) num_dirs = 16;
    if (num_dirs > 1) pe->import_rva = rd32(opt + data_dir_offset + 8);
    if (num_dirs > 13 && optional_size >= data_dir_offset + 14 * 8)
        pe->delay_import_rva = rd32(opt + data_dir_offset + 13 * 8);
    pe->size_of_headers = rd32(opt + 60);
    pe->num_sections = num_sections;
    uint64_t sections_offset = optional_offset + optional_size;
    if (sections_offset + (uint64_t)num_sections * 40 > b->size) return -1;
    pe->sections = b->data + sections_offset;
    (void)path;
    return 0;
}

static int check_import_directory(const struct pe_info *pe, uint32_t dir_rva, int delay,
                                  const char *path, struct evidence *evidence) {
    if (dir_rva == 0) return 0;
    size_t dir_offset = 0;
    if (rva_to_offset(pe, dir_rva, &dir_offset) != 0) return 0;
    const size_t stride = delay ? 32 : 20;
    int found = 0;
    for (size_t i = 0; i < 4096; i++) {
        size_t offset = dir_offset + i * stride;
        if (offset + stride > pe->b->size) break;
        const unsigned char *entry = pe->b->data + offset;
        uint32_t name_rva;
        if (delay) {
            uint32_t attributes = rd32(entry);
            name_rva = rd32(entry + 4);
            /* Old-style delay descriptors store VAs. Normalize when possible. */
            if (attributes == 0 && name_rva > pe->size_of_headers) {
                uint32_t image_base = 0x10000000u;
                if (name_rva > image_base) name_rva -= image_base;
            }
        } else {
            uint32_t original_thunk = rd32(entry);
            uint32_t name = rd32(entry + 12);
            uint32_t first_thunk = rd32(entry + 16);
            if (!original_thunk && !name && !first_thunk) break;
            name_rva = name;
        }
        if (name_rva == 0) continue;
        char name[256];
        if (read_cstring_rva(pe, name_rva, name, sizeof(name)) != 0) continue;
        int tier = is_d3d_dependency(name);
        if (tier > found) {
            char basename[128];
            lower_basename(name, basename, sizeof(basename));
            add_evidence(evidence, path, basename);
            found = tier;
            if (found == 2) break;
        }
    }
    return found;
}

static int scan_strings(const struct buffer *b, const char *path, struct evidence *evidence) {
    static const char *needles[] = {"d3d11.dll", "d3d12.dll", "d3d12core.dll", "d3d11on12.dll",
                                    "d3d10.dll", "d3d10_1.dll", "d3d10core.dll"};
    int found = 0;
    for (size_t n = 0; n < sizeof(needles) / sizeof(needles[0]); n++) {
        const char *needle = needles[n];
        size_t needle_len = strlen(needle);
        /* ASCII, case-insensitive. */
        for (size_t i = 0; i + needle_len <= b->size; i++) {
            size_t j = 0;
            while (j < needle_len && tolower(b->data[i + j]) == needle[j]) j++;
            if (j == needle_len) {
                int tier = is_d3d_dependency(needle);
                if (tier > found) {
                    add_evidence(evidence, path, needle);
                    found = tier;
                }
                if (found == 2) return found;
                break;
            }
        }
        /* UTF-16LE. */
        if (needle_len * 2 <= b->size) {
            for (size_t i = 0; i + needle_len * 2 <= b->size; i++) {
                size_t j = 0;
                while (j < needle_len && b->data[i + j * 2] != 0 &&
                       b->data[i + j * 2 + 1] == 0 &&
                       tolower(b->data[i + j * 2]) == needle[j])
                    j++;
                if (j == needle_len) {
                    int tier = is_d3d_dependency(needle);
                    if (tier > found) {
                        add_evidence(evidence, path, needle);
                        found = tier;
                    }
                    if (found == 2) return found;
                    break;
                }
            }
        }
    }
    return found;
}

static int classify_file(int fd, const char *path, struct evidence *evidence, int *parsed) {
    struct buffer b;
    if (fd < 0 || read_file(fd, &b) != 0) {
        fprintf(stderr, "brinewell-api-probe: cannot read %s: %s\n", path, strerror(errno));
        return -1;
    }
    struct pe_info pe;
    int ok = parse_pe(path, &b, &pe) == 0;
    *parsed = ok;
    int found = 0;
    if (ok) {
        found = check_import_directory(&pe, pe.import_rva, 0, path, evidence);
        if (found < 2) {
            int tier = check_import_directory(&pe, pe.delay_import_rva, 1, path, evidence);
            if (tier > found) found = tier;
        }
    }
    if (found < 2) {
        int tier = scan_strings(&b, path, evidence);
        if (tier > found) found = tier;
    }
    free_file(&b);
    return found;
}

static int has_exe_suffix(const char *name) {
    size_t n = strlen(name);
    return n > 4 && strcasecmp(name + n - 4, ".exe") == 0;
}

static int skip_directory(const char *name) {
    static const char *skip[] = {"redist", "_commonredist", "directx", "dotnet", "vcredist",
                                 "$recycle.bin", "system volume information"};
    for (size_t i = 0; i < sizeof(skip) / sizeof(skip[0]); i++) {
        if (strcasecmp(name, skip[i]) == 0) return 1;
    }
    return 0;
}

static int scan_directory(int fd, const char *dir, struct evidence *evidence, int depth, int *files) {
    struct stat st;
    if (depth > PROBE_MAX_SCAN_DEPTH || *files >= PROBE_MAX_SCAN_FILES ||
        fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode)) {
        close(fd);
        return 0;
    }
    DIR *d = fdopendir(fd);
    if (!d) { close(fd); return 0; }
    struct dirent *ent;
    int found = 0;
    while (found < 2 && *files < PROBE_MAX_SCAN_FILES && (ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char path[PATH_MAX];
        if (snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name) >= (int)sizeof(path)) continue;
        int child = -1;
        if (!skip_directory(ent->d_name))
            child = openat(dirfd(d), ent->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (child >= 0) {
            int tier = scan_directory(child, path, evidence, depth + 1, files);
            if (tier > found) found = tier;
        } else if (has_exe_suffix(ent->d_name)) {
            child = openat(dirfd(d), ent->d_name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
            if (child < 0) continue;
            (*files)++;
            int parsed = 0;
            int tier = classify_file(child, path, evidence, &parsed);
            if (tier > found) found = tier;
        }
    }
    closedir(d);
    return found;
}

static const char *backend_name(int tier) {
    return tier == 2 ? "neptune" : tier == 1 ? "mullion" : "venus";
}

static void print_json(const struct evidence *evidence, int tier) {
    printf("{\"backend\":\"%s\",\"evidence\":[", backend_name(tier));
    for (size_t i = 0; i < evidence->count; i++) {
        if (i) putchar(',');
        putchar('"');
        const unsigned char *p = (const unsigned char *)evidence->items[i];
        while (*p) {
            unsigned char c = *p;
            if (c == '"' || c == '\\') {
                putchar('\\');
                putchar(c);
                p++;
            } else if (c < 0x20) {
                printf("\\u%04x", c);
                p++;
            } else if (c < 0x80) {
                putchar(c);
                p++;
            } else {
                size_t remaining = strlen((const char *)p);
                size_t n = c >= 0xc2 && c <= 0xdf ? 2 :
                           c >= 0xe0 && c <= 0xef ? 3 :
                           c >= 0xf0 && c <= 0xf4 ? 4 : 0;
                int valid = n && remaining >= n;
                for (size_t j = 1; valid && j < n; j++)
                    if ((p[j] & 0xc0) != 0x80) valid = 0;
                if (valid && ((c == 0xe0 && p[1] < 0xa0) ||
                              (c == 0xed && p[1] >= 0xa0) ||
                              (c == 0xf0 && p[1] < 0x90) ||
                              (c == 0xf4 && p[1] >= 0x90))) valid = 0;
                if (valid) {
                    for (size_t j = 0; j < n; j++) putchar(*p++);
                } else {
                    fputs("\\uFFFD", stdout);
                    p++;
                }
            }
        }
        putchar('"');
    }
    printf("]}\n");
}

static void usage(void) {
    fprintf(stderr,
            "usage: brinewell-api-probe [--json] check FILE...\n"
            "       brinewell-api-probe [--json] scan DIR\n");
}

int main(int argc, char **argv) {
    int json = 0;
    int argi = 1;
    if (argi < argc && strcmp(argv[argi], "--json") == 0) {
        json = 1;
        argi++;
    }
    if (argi + 1 >= argc) {
        usage();
        return 2;
    }
    const char *mode = argv[argi++];
    struct evidence evidence;
    memset(&evidence, 0, sizeof(evidence));
    int found = 0;
    if (strcmp(mode, "check") == 0) {
        for (int i = argi; i < argc; i++) {
            int parsed = 0;
            int fd = open(argv[i], O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
            int r = classify_file(fd, argv[i], &evidence, &parsed);
            if (r < 0) continue;
            if (r > found) found = r;
            if (found == 2) break;
        }
    } else if (strcmp(mode, "scan") == 0) {
        if (argi + 1 != argc) {
            usage();
            return 2;
        }
        int files = 0;
        int fd = open(argv[argi], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (fd >= 0) found = scan_directory(fd, argv[argi], &evidence, 0, &files);
    } else {
        usage();
        return 2;
    }
    if (json) {
        print_json(&evidence, found);
    } else {
        for (size_t i = 0; i < evidence.count; i++) printf("evidence %s\n", evidence.items[i]);
        printf("backend=%s\n", backend_name(found));
    }
    return found == 2 ? 0 : found == 1 ? 3 : 1;
}
