// imspatch: find and flip the incoming-call 100rel decision in a running IMS process.
//
// Target (Pixel IMS stack, libimsstack.so), inside IdleState::OnAttached():
//
//     adrp xN, <page>                ; \  load &MtcExtensionSet::OPTION_TAG_RPR ("100rel")
//     ldr  x1, [xN, #<off>]          ; /  from its GOT slot
//     bl   <IsAvailableOnBoth>       ; MtcExtensionSet::IsAvailableOnBoth("100rel")
//     tbz  w0, #0, <ready_to_alert>  ; <- patch site
//
// "patch" rewrites the tbz into "b <ready_to_alert>", so an incoming INVITE always
// takes the plain-180 path and the SDP answer goes into the 200 OK.
//
// Nothing is located by fixed offset. The site is found from exported symbols and
// verified three ways before anything is written:
//   - the instruction pattern above, read from the library file on disk;
//   - the GOT slot loaded into x1 holds the runtime address of OPTION_TAG_RPR;
//   - the bl goes (directly or through a PLT stub) to IsAvailableOnBoth.
// Exactly one site must match. The word in process memory must be either the
// original tbz (from the file) or the patched b; anything else is left alone.
//
// Usage: imspatch <pid> <status|patch|revert> [library-name]
// Exit:  0 ok, 1 library not mapped, 2 site not found or ambiguous,
//        3 I/O error, 4 memory does not match original or patched code, 64 usage.

#define _GNU_SOURCE
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#define SYM_ONATTACHED "_ZN9IdleState10OnAttachedEv"
#define SYM_ISAVAIL "_ZNK15MtcExtensionSet17IsAvailableOnBothERK7AString"
#define SYM_TAG_RPR "_ZN15MtcExtensionSet14OPTION_TAG_RPRE"

static int g_mem = -1;  // /proc/<pid>/mem

static int read_mem(uint64_t addr, void *buf, size_t len) {
    return pread(g_mem, buf, len, (off_t)addr) == (ssize_t)len ? 0 : -1;
}

static int write_mem(uint64_t addr, const void *buf, size_t len) {
    return pwrite(g_mem, buf, len, (off_t)addr) == (ssize_t)len ? 0 : -1;
}

// ---- AArch64 decoding -------------------------------------------------------

static int64_t sext(uint64_t v, int bits) {
    uint64_t m = 1ULL << (bits - 1);
    return (int64_t)((v ^ m) - m);
}

static int is_adrp(uint32_t i) { return (i & 0x9F000000u) == 0x90000000u; }
static int adrp_rd(uint32_t i) { return i & 0x1f; }
static uint64_t adrp_target(uint32_t i, uint64_t pc) {
    uint64_t immlo = (i >> 29) & 3, immhi = (i >> 5) & 0x7ffff;
    return (pc & ~0xfffULL) + (uint64_t)(sext((immhi << 2) | immlo, 21) << 12);
}

// LDR Xt, [Xn, #imm] (64-bit, unsigned offset)
static int is_ldr64(uint32_t i) { return (i & 0xFFC00000u) == 0xF9400000u; }
static int ldr_rt(uint32_t i) { return i & 0x1f; }
static int ldr_rn(uint32_t i) { return (i >> 5) & 0x1f; }
static uint64_t ldr_off(uint32_t i) { return ((i >> 10) & 0xfff) * 8; }

static int is_bl(uint32_t i) { return (i & 0xFC000000u) == 0x94000000u; }
static uint64_t b_target(uint32_t i, uint64_t pc) { return pc + (uint64_t)(sext(i & 0x3ffffff, 26) * 4); }
static uint32_t enc_b(uint64_t pc, uint64_t target) {
    return 0x14000000u | (uint32_t)(((int64_t)(target - pc) >> 2) & 0x3ffffff);
}

// TBZ w0, #0, <target>: b5 = 0, op = 0, b40 = 0, Rt = 0
static int is_tbz_w0_b0(uint32_t i) { return (i & 0xFFF8001Fu) == 0x36000000u; }
static uint64_t tbz_target(uint32_t i, uint64_t pc) { return pc + (uint64_t)(sext((i >> 5) & 0x3fff, 14) * 4); }

// ---- ELF from the mapped file -------------------------------------------------

typedef struct {
    int fd;
    Elf64_Ehdr eh;
    Elf64_Phdr *ph;
    Elf64_Shdr *sh;
} elf_t;

static int file_read(int fd, uint64_t off, void *buf, size_t len) {
    return pread(fd, buf, len, (off_t)off) == (ssize_t)len ? 0 : -1;
}

static int elf_open(elf_t *e, const char *path) {
    memset(e, 0, sizeof(*e));
    e->fd = open(path, O_RDONLY | O_CLOEXEC);
    if (e->fd < 0) return -1;
    if (file_read(e->fd, 0, &e->eh, sizeof(e->eh)) || memcmp(e->eh.e_ident, ELFMAG, SELFMAG) ||
        e->eh.e_ident[EI_CLASS] != ELFCLASS64 || e->eh.e_machine != EM_AARCH64 || e->eh.e_shnum == 0)
        return -1;
    e->ph = calloc(e->eh.e_phnum, sizeof(Elf64_Phdr));
    e->sh = calloc(e->eh.e_shnum, sizeof(Elf64_Shdr));
    if (!e->ph || !e->sh) return -1;
    if (file_read(e->fd, e->eh.e_phoff, e->ph, e->eh.e_phnum * sizeof(Elf64_Phdr)) ||
        file_read(e->fd, e->eh.e_shoff, e->sh, e->eh.e_shnum * sizeof(Elf64_Shdr)))
        return -1;
    return 0;
}

// Convert a virtual address to a file offset through the PT_LOAD headers.
static int elf_vaddr_to_off(const elf_t *e, uint64_t va, uint64_t *off) {
    for (int i = 0; i < e->eh.e_phnum; i++) {
        const Elf64_Phdr *p = &e->ph[i];
        if (p->p_type == PT_LOAD && va >= p->p_vaddr && va < p->p_vaddr + p->p_filesz) {
            *off = va - p->p_vaddr + p->p_offset;
            return 0;
        }
    }
    return -1;
}

// Look up a defined symbol in .dynsym. Returns 0 and fills value/size.
static int elf_dynsym(const elf_t *e, const char *name, uint64_t *value, uint64_t *size) {
    for (int s = 0; s < e->eh.e_shnum; s++) {
        const Elf64_Shdr *sym = &e->sh[s];
        if (sym->sh_type != SHT_DYNSYM || sym->sh_link >= e->eh.e_shnum) continue;
        const Elf64_Shdr *str = &e->sh[sym->sh_link];
        size_t n = sym->sh_size / sizeof(Elf64_Sym);
        size_t want = strlen(name);
        char buf[256];
        for (size_t k = 0; k < n; k++) {
            Elf64_Sym es;
            if (file_read(e->fd, sym->sh_offset + k * sizeof(es), &es, sizeof(es))) return -1;
            if (es.st_shndx == SHN_UNDEF || es.st_name == 0) continue;
            if (want + 1 > sizeof(buf)) return -1;
            if (file_read(e->fd, str->sh_offset + es.st_name, buf, want + 1)) continue;
            if (memcmp(buf, name, want) == 0 && buf[want] == '\0') {
                *value = es.st_value;
                *size = es.st_size;
                return 0;
            }
        }
    }
    return -1;
}

// ---- Locate the library in the target process -------------------------------

// Finds the mapping of <lib> at file offset 0. Fills base (load bias, assuming the
// first PT_LOAD is at vaddr 0, which is checked later) and a path under
// /proc/<pid>/root that opens the mapped file in the process's own mount namespace.
// (/proc/<pid>/map_files is not used: on some kernels the read-only first segment's
// entry cannot be opened.)
static int find_lib(pid_t pid, const char *lib, uint64_t *base, char *mapfile, size_t mapfile_len) {
    char path[64], line[1024];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    FILE *f = fopen(path, "re");
    if (!f) return -1;
    size_t liblen = strlen(lib);
    int found = -1;
    while (fgets(line, sizeof(line), f)) {
        uint64_t start, end, off;
        char perms[8];
        int pos = 0;
        if (sscanf(line, "%" SCNx64 "-%" SCNx64 " %7s %" SCNx64 " %*s %*s %n", &start, &end, perms, &off, &pos) < 4)
            continue;
        char *p = line + pos;
        p[strcspn(p, "\n")] = '\0';
        size_t plen = strlen(p);
        if (off != 0 || plen <= liblen || p[plen - liblen - 1] != '/' || strcmp(p + plen - liblen, lib) != 0)
            continue;
        (void)end;
        *base = start;
        snprintf(mapfile, mapfile_len, "/proc/%d/root%s", pid, p);
        found = 0;
        break;
    }
    fclose(f);
    return found;
}

// ---- Main -------------------------------------------------------------------

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <pid> <status|patch|revert> [library]\n", argv[0]);
        return 64;
    }
    pid_t pid = (pid_t)atoi(argv[1]);
    const char *mode = argv[2];
    const char *lib = argc > 3 ? argv[3] : "libimsstack.so";
    if (strcmp(mode, "status") && strcmp(mode, "patch") && strcmp(mode, "revert")) {
        fprintf(stderr, "unknown mode: %s\n", mode);
        return 64;
    }

    uint64_t base;
    char mapfile[1100];
    if (find_lib(pid, lib, &base, mapfile, sizeof(mapfile))) {
        printf("%s not mapped in pid %d\n", lib, pid);
        return 1;
    }

    elf_t e;
    if (elf_open(&e, mapfile)) {
        printf("cannot read ELF from %s: %s\n", mapfile, strerror(errno));
        return 3;
    }
    // The load bias equals the mapping start only if the first PT_LOAD is at vaddr 0.
    for (int i = 0; i < e.eh.e_phnum; i++) {
        if (e.ph[i].p_type != PT_LOAD) continue;
        if (e.ph[i].p_vaddr != 0 || e.ph[i].p_offset != 0) {
            printf("unsupported layout: first PT_LOAD not at 0\n");
            return 2;
        }
        break;
    }

    uint64_t fn, fn_size, isavail, isavail_size, tag, tag_size;
    if (elf_dynsym(&e, SYM_ONATTACHED, &fn, &fn_size) || fn_size < 16 ||
        elf_dynsym(&e, SYM_ISAVAIL, &isavail, &isavail_size) ||
        elf_dynsym(&e, SYM_TAG_RPR, &tag, &tag_size)) {
        printf("required symbols not exported by %s\n", lib);
        return 2;
    }

    char mempath[64];
    snprintf(mempath, sizeof(mempath), "/proc/%d/mem", pid);
    g_mem = open(mempath, O_RDWR | O_CLOEXEC);
    if (g_mem < 0) {
        printf("cannot open %s: %s\n", mempath, strerror(errno));
        return 3;
    }

    // Pristine code of OnAttached, from the file.
    uint64_t fn_off;
    size_t n = fn_size / 4;
    uint32_t *code = calloc(n, 4);
    if (!code || elf_vaddr_to_off(&e, fn, &fn_off) || file_read(e.fd, fn_off, code, n * 4)) {
        printf("cannot read %s from file\n", SYM_ONATTACHED);
        return 3;
    }

    int matches = 0;
    uint64_t site = 0, target = 0;
    uint32_t orig = 0;
    for (size_t k = 0; k + 1 < n; k++) {
        uint64_t pc = fn + k * 4;
        if (!is_bl(code[k]) || !is_tbz_w0_b0(code[k + 1])) continue;

        // The bl must reach IsAvailableOnBoth, directly or through a PLT stub
        // (adrp x16 / ldr x17, [x16, #off]) whose GOT slot holds its runtime address.
        uint64_t t = b_target(code[k], pc);
        int call_ok = (t == isavail);
        if (!call_ok) {
            uint64_t stub_off;
            uint32_t stub[2];
            if (!elf_vaddr_to_off(&e, t, &stub_off) && !file_read(e.fd, stub_off, stub, sizeof(stub)) &&
                is_adrp(stub[0]) && adrp_rd(stub[0]) == 16 && is_ldr64(stub[1]) && ldr_rn(stub[1]) == 16 &&
                ldr_rt(stub[1]) == 17) {
                uint64_t slot = adrp_target(stub[0], t) + ldr_off(stub[1]), val;
                call_ok = !read_mem(base + slot, &val, 8) && val == base + isavail;
            }
        }
        if (!call_ok) continue;

        // Within the 4 preceding instructions: adrp xN + ldr x1, [xN, #off], with the
        // slot holding the runtime address of OPTION_TAG_RPR.
        int arg_ok = 0;
        for (size_t j = (k >= 4 ? k - 4 : 0); j < k && !arg_ok; j++) {
            if (!is_ldr64(code[j]) || ldr_rt(code[j]) != 1) continue;
            for (size_t a = (j >= 4 ? j - 4 : 0); a < j; a++) {
                if (!is_adrp(code[a]) || adrp_rd(code[a]) != ldr_rn(code[j])) continue;
                uint64_t slot = adrp_target(code[a], fn + a * 4) + ldr_off(code[j]), val;
                if (!read_mem(base + slot, &val, 8) && val == base + tag) arg_ok = 1;
            }
        }
        if (!arg_ok) continue;

        matches++;
        site = pc + 4;
        orig = code[k + 1];
        target = tbz_target(orig, site);
    }

    if (matches != 1) {
        printf("patch site not found uniquely (%d matches)\n", matches);
        return 2;
    }

    uint32_t patched = enc_b(site, target), cur;
    if (read_mem(base + site, &cur, 4)) {
        printf("cannot read process memory: %s\n", strerror(errno));
        return 3;
    }
    const char *state = cur == orig ? "original" : cur == patched ? "patched" : NULL;
    if (!state) {
        printf("site 0x%" PRIx64 ": unexpected word %08x (original %08x, patched %08x); not touching\n", site, cur,
               orig, patched);
        return 4;
    }

    if (!strcmp(mode, "status")) {
        printf("site 0x%" PRIx64 " target 0x%" PRIx64 " state %s\n", site, target, state);
        return 0;
    }

    uint32_t want = !strcmp(mode, "patch") ? patched : orig;
    if (cur != want) {
        if (write_mem(base + site, &want, 4) || read_mem(base + site, &cur, 4) || cur != want) {
            printf("write to 0x%" PRIx64 " failed: %s\n", base + site, strerror(errno));
            return 3;
        }
    }
    printf("site 0x%" PRIx64 " target 0x%" PRIx64 " state %s\n", site, target, want == patched ? "patched" : "original");
    return 0;
}
