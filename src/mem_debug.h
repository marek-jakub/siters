#ifndef MEM_DEBUG_H
#define MEM_DEBUG_H

#ifdef MEM_DEBUG

#include <stdio.h>
#include <malloc.h>

static long mem_draw_counter = 0;
static long mem_surfaces_created = 0;
static long mem_surfaces_destroyed = 0;
static long mem_total_alloc_bytes = 0;

static long get_vmrss_kb(void) {
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return -1;
    char line[256];
    long rss = -1;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "VmRSS: %ld kB", &rss) == 1) break;
    }
    fclose(f);
    return rss;
}

#define MEM_INIT_DRAW()                         \
    mem_draw_counter++;                         \
    long rss_before = get_vmrss_kb();           \
    int cache_bytes_before = tab->total_cache_bytes

#define MEM_SURFACE_CREATED(nb)                 \
    mem_surfaces_created++;                     \
    mem_total_alloc_bytes += (nb)

#define MEM_SURFACE_DESTROYED()                 \
    mem_surfaces_destroyed++

#define MEM_REPORT_DRAW()                                                      \
    {                                                                          \
        long rss_after = get_vmrss_kb();                                      \
        int cache_delta = tab->total_cache_bytes - cache_bytes_before;        \
        if (mem_draw_counter % 5 == 0) {                                      \
            struct mallinfo2 mi = mallinfo2();                                \
            fprintf(stderr,                                                   \
                "[MEM] draw=#%ld rss=%ld kB (delta%+ld kB) "              \
                "cache=%d kB (delta%+d kB) "                              \
                "arena=%lu kB hblkhd=%lu kB uordblks=%lu kB "             \
                "surf_cr=%ld surf_del=%ld diff=%ld "                      \
                "alloc_mb=%ld\n",                                             \
                mem_draw_counter, rss_after, rss_after - rss_before,          \
                tab->total_cache_bytes / 1024, cache_delta / 1024,            \
                (unsigned long)(mi.arena / 1024),                             \
                (unsigned long)(mi.hblkhd / 1024),                            \
                (unsigned long)(mi.uordblks / 1024),                          \
                mem_surfaces_created, mem_surfaces_destroyed,                 \
                mem_surfaces_created - mem_surfaces_destroyed,                \
                mem_total_alloc_bytes / (1024 * 1024));                       \
        }                                                                      \
    }

#else

#define MEM_INIT_DRAW()
#define MEM_SURFACE_CREATED(nb)
#define MEM_SURFACE_DESTROYED()
#define MEM_REPORT_DRAW()

#endif

#endif
