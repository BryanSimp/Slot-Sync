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

int wii_dol_run(const char *path, int argc, char **argv)
{
    FILE *file;
    long size;
    unsigned char *image;
    dol_header *header;
    int i;
    void (*entry)(void);

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

    image = (unsigned char *)memalign(32, (size_t)size);
    if (image == NULL) {
        fclose(file);
        return -1;
    }
    if (fread(image, 1, (size_t)size, file) != (size_t)size) {
        fclose(file);
        free(image);
        return -1;
    }
    fclose(file);

    header = (dol_header *)image;

    /* Everything below this point runs with interrupts off and the sections
     * being copied over whatever is already at those addresses -- including,
     * potentially, us. There is no path back from a failure here, which is why
     * every check that can be made is made above. */
    __IOS_ShutdownSubsystems();
    SYS_ResetSystem(SYS_SHUTDOWN, 0, 0);

    for (i = 0; i < 7; i++) {
        if (header->text_size[i] == 0 || header->text_address[i] < 0x100) {
            continue;
        }
        memmove((void *)header->text_address[i], image + header->text_offset[i],
                header->text_size[i]);
        DCFlushRange((void *)header->text_address[i], header->text_size[i]);
        ICInvalidateRange((void *)header->text_address[i], header->text_size[i]);
    }

    for (i = 0; i < 11; i++) {
        if (header->data_size[i] == 0 || header->data_address[i] < 0x100) {
            continue;
        }
        memmove((void *)header->data_address[i], image + header->data_offset[i],
                header->data_size[i]);
        DCFlushRange((void *)header->data_address[i], header->data_size[i]);
    }

    if (header->bss_size != 0) {
        memset((void *)header->bss_address, 0, header->bss_size);
        DCFlushRange((void *)header->bss_address, header->bss_size);
    }

    entry = (void (*)(void))header->entry_point;

    (void)argc;
    (void)argv;

    entry();
    return -1; /* unreachable if the jump worked */
}
