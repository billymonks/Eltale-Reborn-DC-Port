// Controller Pak file system on port 1, kept in RAM for now. Saves don't survive a power cycle
// until the VMU backend replaces this storage.

#include <stdlib.h>
#include <string.h>

#include "ultra.h"

#define PAK_MAX_FILES  16
#define PAK_PAGE_SIZE  256
#define PAK_FREE_PAGES 123

#define PFS_READ  0
#define PFS_WRITE 1

#define PFS_ERR_NOPACK    1
#define PFS_ERR_INVALID   5
#define PFS_ERR_DATA_FULL 7
#define PFS_ERR_DIR_FULL  8
#define PFS_ERR_EXIST     9

// OSPfs fields.
#define PFS_STATUS  0x00
#define PFS_QUEUE   0x04
#define PFS_CHANNEL 0x08

typedef struct {
    bool used;
    uint16_t company_code;
    uint32_t game_code;
    uint8_t game_name[16];
    uint8_t ext_name[4];
    uint32_t size;
    uint8_t* data;
} PakFile;

static PakFile files[PAK_MAX_FILES];

static void read_bytes(uint32_t vram, uint8_t* out, int n) {
    for (int i = 0; i < n; i++) {
        out[i] = vram != 0 ? MEM_BU(i, vram) : 0;
    }
}

static int pages_used(void) {
    int pages = 0;
    for (int i = 0; i < PAK_MAX_FILES; i++) {
        if (files[i].used) {
            pages += (files[i].size + PAK_PAGE_SIZE - 1) / PAK_PAGE_SIZE;
        }
    }
    return pages;
}

static int find_file(uint16_t company, uint32_t game, const uint8_t* name, const uint8_t* ext) {
    for (int i = 0; i < PAK_MAX_FILES; i++) {
        if (files[i].used && files[i].company_code == company && files[i].game_code == game &&
            memcmp(files[i].game_name, name, 16) == 0 && memcmp(files[i].ext_name, ext, 4) == 0) {
            return i;
        }
    }
    return -1;
}

static bool valid_file_no(int32_t file_no) {
    return file_no >= 0 && file_no < PAK_MAX_FILES && files[file_no].used;
}

void osPfsInitPak_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint32_t pfs = ctx->r5;
    int32_t channel = (int32_t)ctx->r6;
    if (channel != 0) {
        ctx->r2 = PFS_ERR_NOPACK;
        return;
    }
    MEM_W(PFS_STATUS, pfs) = 1;
    MEM_W(PFS_QUEUE, pfs) = (int32_t)ctx->r4;
    MEM_W(PFS_CHANNEL, pfs) = channel;
    ctx->r2 = 0;
}

void osPfsFindFile_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint8_t name[16], ext[4];
    read_bytes(ctx->r7, name, 16);
    read_bytes(STACK_ARG(ctx, 4), ext, 4);
    uint32_t file_no_out = STACK_ARG(ctx, 5);

    int i = find_file((uint16_t)ctx->r5, ctx->r6, name, ext);
    if (i < 0) {
        ctx->r2 = PFS_ERR_INVALID;
        return;
    }
    MEM_W(0, file_no_out) = i;
    ctx->r2 = 0;
}

void osPfsAllocateFile_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint16_t company = (uint16_t)ctx->r5;
    uint32_t game = ctx->r6;
    uint8_t name[16], ext[4];
    read_bytes(ctx->r7, name, 16);
    read_bytes(STACK_ARG(ctx, 4), ext, 4);
    uint32_t length = STACK_ARG(ctx, 5);
    uint32_t file_no_out = STACK_ARG(ctx, 6);

    if (company == 0 || game == 0) {
        ctx->r2 = PFS_ERR_INVALID;
        return;
    }
    if (find_file(company, game, name, ext) >= 0) {
        ctx->r2 = PFS_ERR_EXIST;
        return;
    }
    int pages = (length + PAK_PAGE_SIZE - 1) / PAK_PAGE_SIZE;
    if (pages_used() + pages > PAK_FREE_PAGES) {
        ctx->r2 = PFS_ERR_DATA_FULL;
        return;
    }
    for (int i = 0; i < PAK_MAX_FILES; i++) {
        if (files[i].used) {
            continue;
        }
        files[i].data = calloc(1, pages * PAK_PAGE_SIZE);
        if (files[i].data == NULL) {
            runtime_fatal("[q64dc] out of memory for a Controller Pak file");
        }
        files[i].used = true;
        files[i].company_code = company;
        files[i].game_code = game;
        memcpy(files[i].game_name, name, 16);
        memcpy(files[i].ext_name, ext, 4);
        files[i].size = pages * PAK_PAGE_SIZE;
        MEM_W(0, file_no_out) = i;
        ctx->r2 = 0;
        return;
    }
    ctx->r2 = PFS_ERR_DIR_FULL;
}

void osPfsDeleteFile_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint8_t name[16], ext[4];
    read_bytes(ctx->r7, name, 16);
    read_bytes(STACK_ARG(ctx, 4), ext, 4);

    int i = find_file((uint16_t)ctx->r5, ctx->r6, name, ext);
    if (i < 0) {
        ctx->r2 = PFS_ERR_INVALID;
        return;
    }
    free(files[i].data);
    memset(&files[i], 0, sizeof(files[i]));
    ctx->r2 = 0;
}

void osPfsReadWriteFile_recomp(uint8_t* rdram, recomp_context* ctx) {
    int32_t file_no = (int32_t)ctx->r5;
    uint32_t flag = ctx->r6;
    uint32_t offset = ctx->r7;
    uint32_t size = STACK_ARG(ctx, 4);
    uint32_t data = STACK_ARG(ctx, 5);

    if (!valid_file_no(file_no) || offset + size > files[file_no].size) {
        ctx->r2 = PFS_ERR_INVALID;
        return;
    }
    uint8_t* contents = files[file_no].data + offset;
    for (uint32_t i = 0; i < size; i++) {
        if (flag == PFS_WRITE) {
            contents[i] = MEM_BU(i, data);
        } else {
            MEM_BU(i, data) = contents[i];
        }
    }
    ctx->r2 = 0;
}

void osPfsFileState_recomp(uint8_t* rdram, recomp_context* ctx) {
    int32_t file_no = (int32_t)ctx->r5;
    uint32_t state = ctx->r6;
    if (!valid_file_no(file_no)) {
        ctx->r2 = PFS_ERR_INVALID;
        return;
    }
    const PakFile* f = &files[file_no];
    MEM_W(0x00, state) = (int32_t)f->size;
    MEM_W(0x04, state) = (int32_t)f->game_code;
    MEM_HU(0x08, state) = f->company_code;
    for (int i = 0; i < 4; i++) {
        MEM_BU(0x0A + i, state) = f->ext_name[i];
    }
    for (int i = 0; i < 16; i++) {
        MEM_BU(0x0E + i, state) = f->game_name[i];
    }
    ctx->r2 = 0;
}

void osPfsFreeBlocks_recomp(uint8_t* rdram, recomp_context* ctx) {
    MEM_W(0, ctx->r5) = (PAK_FREE_PAGES - pages_used()) * PAK_PAGE_SIZE;
    ctx->r2 = 0;
}

void osPfsNumFiles_recomp(uint8_t* rdram, recomp_context* ctx) {
    int used = 0;
    for (int i = 0; i < PAK_MAX_FILES; i++) {
        used += files[i].used;
    }
    MEM_W(0, ctx->r5) = PAK_MAX_FILES;
    MEM_W(0, ctx->r6) = used;
    ctx->r2 = 0;
}
