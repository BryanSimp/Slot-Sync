/* See wii_dol.h. */

#include "wii_dol.h"

#include <gccore.h>
#include <malloc.h>
#include <ogcsys.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The DOL header. Seven text sections, eleven data sections, then bss and the
 * entry point. Offsets are from the format, not from memory. */
typedef struct {
    unsigned int text_offset[7];
    unsigned int data_offset[11];
    unsigned int text_address[7];
    unsigned int data_address[11];
    unsigned int text_size[7];
    unsigned int data_size[11];
    unsigned int bss_address;
    unsigned int bss_size;
    unsigned int entry_point;
    unsigned int padding[7];
} dol_header;

/* wii_dol_stub.S. Copied out to MEM2 and called there, never called in place. */
extern void wii_dol_stub(void *plan);
extern void wii_dol_stub_end(void);

/* 7 text + 11 data + 1 bss, each a {dst, src, len} triple, after the two
 * leading words. */
#define PLAN_WORDS (2 + (7 + 11 + 1) * 3)

/* Take memory from the MEM2 arena.
 *
 * MEM1 is where the incoming DOL is going -- homebrew links at 0x80004000 and
 * so does Nintendont, whose sections reach past 0x80200000 -- and the malloc
 * heap sits just above this app's own bss, which is inside that range. Staging
 * the image there would mean the copy destroying its own source partway
 * through. MEM2 is untouched by a DOL's sections, so everything the stub reads
 * lives here: the image, the plan, and the stub itself. */
static void *mem2_take(unsigned int size)
{
    unsigned int lo = (unsigned int)SYS_GetArena2Lo();
    unsigned int hi = (unsigned int)SYS_GetArena2Hi();

    lo = (lo + 31) & ~31u;
    size = (size + 31) & ~31u;
    if (size > hi || lo > hi - size) {
        return NULL;
    }
    SYS_SetArena2Lo((void *)(lo + size));
    return (void *)lo;
}

int wii_dol_run(const char *path, int argc, char **argv)
{
    FILE *file;
    long size;
    unsigned char *image;
    unsigned int *plan;
    unsigned int sections = 0;
    unsigned char *stub;
    unsigned int stub_size;
    dol_header *header;
    int i;
    void (*go)(void *);

    (void)argc;
    (void)argv;

    file = fopen(path, "rb");
    if (file == NULL) {
        return -1;
    }
    fseek(file, 0, SEEK_END);
    size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size < (long)sizeof(dol_header)) {
        fclose(file);
        return -1;
    }

    image = (unsigned char *)mem2_take((unsigned int)size);
    if (image == NULL) {
        fclose(file);
        return -1;
    }
    if (fread(image, 1, (size_t)size, file) != (size_t)size) {
        fclose(file);
        return -1;
    }
    fclose(file);

    stub_size = (unsigned int)((char *)wii_dol_stub_end - (char *)wii_dol_stub);
    stub = (unsigned char *)mem2_take(stub_size);
    plan = (unsigned int *)mem2_take(PLAN_WORDS * 4);
    if (stub == NULL || plan == NULL) {
        return -1;
    }

    header = (dol_header *)image;

    /* Build the whole plan before anything is shut down, so a malformed DOL is
     * a clean -1 with the console still usable rather than a hang halfway
     * through overwriting memory. */
    for (i = 0; i < 7; i++) {
        if (header->text_size[i] == 0 || header->text_address[i] < 0x100) {
            continue;
        }
        plan[2 + sections * 3 + 0] = header->text_address[i];
        plan[2 + sections * 3 + 1] = (unsigned int)(image + header->text_offset[i]);
        plan[2 + sections * 3 + 2] = header->text_size[i];
        sections++;
    }
    for (i = 0; i < 11; i++) {
        if (header->data_size[i] == 0 || header->data_address[i] < 0x100) {
            continue;
        }
        plan[2 + sections * 3 + 0] = header->data_address[i];
        plan[2 + sections * 3 + 1] = (unsigned int)(image + header->data_offset[i]);
        plan[2 + sections * 3 + 2] = header->data_size[i];
        sections++;
    }
    if (header->bss_size != 0 && header->bss_address >= 0x100) {
        plan[2 + sections * 3 + 0] = header->bss_address;
        plan[2 + sections * 3 + 1] = 0; /* src 0: the stub zero-fills */
        plan[2 + sections * 3 + 2] = header->bss_size;
        sections++;
    }
    if (sections == 0) {
        return -1;
    }
    plan[0] = header->entry_point;
    plan[1] = sections;

    memcpy(stub, (const void *)wii_dol_stub, stub_size);

    /* The stub is about to be executed from where it was just written, so it
     * has to reach memory and out of the instruction cache first. */
    DCFlushRange(stub, stub_size);
    ICInvalidateRange(stub, stub_size);
    DCFlushRange(plan, PLAN_WORDS * 4);
    DCFlushRange(image, (unsigned int)size);

    go = (void (*)(void *))stub;

    /* Past here nothing can be reported and there is no way back: IOS is gone,
     * and the stub overwrites this function almost immediately. */
    __IOS_ShutdownSubsystems();
    SYS_ResetSystem(SYS_SHUTDOWN, 0, 0);
    IRQ_Disable();

    go(plan);
    return -1; /* unreachable if the jump worked */
}
