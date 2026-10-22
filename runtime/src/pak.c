// Controller Pak file system on port 1, kept on a VMU. The game's pak files (names, codes and
// every byte of their contents) live in RAM and are mirrored to one VMU file, QUEST64.PAK, on
// the first VMU found. The VMU is read when the game first opens the pak and written only after
// the game changes something, once it has stopped writing for a moment.
//
// Without a VMU the game is told there's no Controller Pak, as on the N64. If the VMU has no room
// for a new save, the game gets the pak-full error before anything is written.

#include <kos.h>
#include <dc/vmu_pkg.h>
#include <dc/vmufs.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "ultra.h"

#define PAK_MAX_FILES  16
#define PAK_PAGE_SIZE  256
#define PAK_FREE_PAGES 123

#define PFS_READ  0
#define PFS_WRITE 1

#define PFS_ERR_NOPACK    1
#define PFS_ERR_CONTRFAIL 4
#define PFS_ERR_INVALID   5
#define PFS_ERR_DATA_FULL 7
#define PFS_ERR_DIR_FULL  8
#define PFS_ERR_EXIST     9

// OSPfs fields.
#define PFS_STATUS  0x00
#define PFS_QUEUE   0x04
#define PFS_CHANNEL 0x08

#define VMU_FILE_NAME  "QUEST64.PAK"
#define VMU_BLOCK_SIZE 512
#define IMAGE_MAGIC    "Q64PAK01"
// Writes wait until the game has left the pak alone this long, so one save is one VMU write.
#define FLUSH_DELAY_MS 300

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

static maple_device_t* vmu;       // the VMU the files were loaded from
static uint8_t vmu_port, vmu_unit;
static bool loaded;
static bool dirty;
static uint64_t last_change_ms;
static int vmu_file_blocks;       // size of QUEST64.PAK on the VMU, 0 if there's none

// --- Image format: magic, file count, then per file its slot, header and contents (big-endian). ---

static uint8_t* put32(uint8_t* p, uint32_t v) {
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
    return p + 4;
}

static uint32_t get32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

#define FILE_HEADER_SIZE (4 + 4 + 4 + 16 + 4 + 4)

static uint32_t image_size(void) {
    uint32_t size = 8 + 4;
    for (int i = 0; i < PAK_MAX_FILES; i++) {
        if (files[i].used) {
            size += FILE_HEADER_SIZE + files[i].size;
        }
    }
    return size;
}

static uint8_t* build_image(uint32_t* out_size) {
    uint32_t size = image_size();
    uint8_t* buf = malloc(size);
    if (buf == NULL) {
        return NULL;
    }
    uint8_t* p = buf;
    memcpy(p, IMAGE_MAGIC, 8);
    p += 8;
    uint32_t count = 0;
    for (int i = 0; i < PAK_MAX_FILES; i++) {
        count += files[i].used;
    }
    p = put32(p, count);
    for (int i = 0; i < PAK_MAX_FILES; i++) {
        const PakFile* f = &files[i];
        if (!f->used) {
            continue;
        }
        p = put32(p, i);
        p = put32(p, f->company_code);
        p = put32(p, f->game_code);
        memcpy(p, f->game_name, 16);
        p += 16;
        memcpy(p, f->ext_name, 4);
        p += 4;
        p = put32(p, f->size);
        memcpy(p, f->data, f->size);
        p += f->size;
    }
    *out_size = size;
    return buf;
}

static void free_files(void) {
    for (int i = 0; i < PAK_MAX_FILES; i++) {
        free(files[i].data);
        memset(&files[i], 0, sizeof(files[i]));
    }
}

static bool parse_image(const uint8_t* p, uint32_t size) {
    const uint8_t* end = p + size;
    if (size < 12 || memcmp(p, IMAGE_MAGIC, 8) != 0) {
        return false;
    }
    uint32_t count = get32(p + 8);
    p += 12;
    for (uint32_t n = 0; n < count; n++) {
        if (end - p < FILE_HEADER_SIZE) {
            return false;
        }
        uint32_t slot = get32(p);
        uint32_t file_size = get32(p + 32);
        if (slot >= PAK_MAX_FILES || files[slot].used || file_size > PAK_FREE_PAGES * PAK_PAGE_SIZE ||
            (uint32_t)(end - p) < FILE_HEADER_SIZE + file_size) {
            return false;
        }
        PakFile* f = &files[slot];
        f->data = malloc(file_size);
        if (f->data == NULL) {
            return false;
        }
        f->used = true;
        f->company_code = (uint16_t)get32(p + 4);
        f->game_code = get32(p + 8);
        memcpy(f->game_name, p + 12, 16);
        memcpy(f->ext_name, p + 28, 4);
        f->size = file_size;
        memcpy(f->data, p + FILE_HEADER_SIZE, file_size);
        p += FILE_HEADER_SIZE + file_size;
    }
    return true;
}

// --- The VMU file ---

// A 32x32 icon: a pearl in a gold ring, like the one on the title screen.
static void draw_icon(uint8_t* icon, uint16_t* pal) {
    static const uint16_t colors[] = {
        0x0000,                                  // transparent
        0xF430, 0xF750, 0xFA80, 0xFDB2,          // gold, dark to light
        0xF211, 0xF433, 0xF755, 0xFA88, 0xFDCC,  // pearl, dark to light
        0xFFFF,                                  // highlight
    };
    memcpy(pal, colors, sizeof(colors));
    for (int y = 0; y < 32; y++) {
        for (int x = 0; x < 32; x++) {
            float dx = x - 15.5f, dy = y - 15.5f;
            float r = sqrtf(dx * dx + dy * dy);
            int c = 0;
            if (r < 10.5f) {
                // Lit from the top left.
                float lx = dx + 4.0f, ly = dy + 4.0f;
                float d = sqrtf(lx * lx + ly * ly);
                c = d < 2.5f ? 10 : 9 - (int)(d / 3.5f);
                if (c < 5) {
                    c = 5;
                }
            } else if (r < 15.5f) {
                float shade = (dx + dy) / 22.0f;  // -1 top left .. 1 bottom right
                c = 3 - (int)(shade * 2.0f + (r < 12.5f || r > 14.5f ? 1.0f : 0.0f));
                if (c < 1) {
                    c = 1;
                } else if (c > 4) {
                    c = 4;
                }
            }
            uint8_t* b = &icon[(y * 32 + x) / 2];
            if (x & 1) {
                *b = (*b & 0xF0) | c;
            } else {
                *b = (*b & 0x0F) | (c << 4);
            }
        }
    }
}

static maple_device_t* find_vmu(void) {
    return maple_enum_type(0, MAPLE_FUNC_MEMCARD);
}

static int blocks_for(int bytes) {
    return (bytes + VMU_BLOCK_SIZE - 1) / VMU_BLOCK_SIZE;
}

static void load_from_vmu(maple_device_t* dev) {
    free_files();
    vmu = dev;
    vmu_port = dev->port;
    vmu_unit = dev->unit;
    loaded = true;
    dirty = false;
    vmu_file_blocks = 0;

    void* buf = NULL;
    int size = 0;
    if (vmufs_read(dev, VMU_FILE_NAME, &buf, &size) < 0) {
        printf("[q64dc] pak: VMU %c%d has no %s yet (%d blocks free)\n", 'A' + vmu_port, vmu_unit, VMU_FILE_NAME,
               vmufs_free_blocks(dev));
        return;
    }
    vmu_file_blocks = blocks_for(size);
    vmu_pkg_t pkg;
    if (vmu_pkg_parse(buf, (size_t)size, &pkg) < 0 || !parse_image(pkg.data, (uint32_t)pkg.data_len)) {
        printf("[q64dc] pak: %s on VMU %c%d is damaged; starting with an empty pak\n", VMU_FILE_NAME, 'A' + vmu_port,
               vmu_unit);
        free_files();
    } else {
        int count = 0;
        for (int i = 0; i < PAK_MAX_FILES; i++) {
            count += files[i].used;
        }
        printf("[q64dc] pak: loaded %d file(s) from VMU %c%d\n", count, 'A' + vmu_port, vmu_unit);
    }
    free(buf);
}

static uint8_t* build_package(int* out_size) {
    uint32_t data_size;
    uint8_t* data = build_image(&data_size);
    if (data == NULL) {
        return NULL;
    }
    static uint8_t icon[512];
    vmu_pkg_t pkg;
    memset(&pkg, 0, sizeof(pkg));
    strcpy(pkg.desc_short, "Quest 64");
    strcpy(pkg.desc_long, "Quest 64 saves");
    strcpy(pkg.app_id, "Quest64-DC-Port");
    pkg.icon_cnt = 1;
    pkg.icon_anim_speed = 0;
    pkg.eyecatch_type = VMUPKG_EC_NONE;
    pkg.data_len = (int)data_size;
    draw_icon(icon, pkg.icon_pal);
    pkg.icon_data = icon;
    pkg.data = data;
    uint8_t* out = NULL;
    int size = 0;
    int err = vmu_pkg_build(&pkg, &out, &size);
    free(data);
    if (err < 0) {
        return NULL;
    }
    *out_size = size;
    return out;
}

// The size of the VMU file once the pak holds `extra` more bytes.
static int package_blocks(uint32_t extra) {
    // vmu_pkg header (128 bytes) and one icon (512 bytes) before the data.
    return blocks_for(128 + 512 + (int)(image_size() + extra));
}

static void flush(void) {
    if (!dirty) {
        return;
    }
    dirty = false;
    maple_device_t* dev = maple_enum_dev(vmu_port, vmu_unit);
    if (dev == NULL || dev != vmu || !(dev->info.functions & MAPLE_FUNC_MEMCARD)) {
        printf("[q64dc] pak: the VMU was removed; the last save is only in memory\n");
        return;
    }
    int size = 0;
    uint8_t* pkg = build_package(&size);
    if (pkg == NULL) {
        printf("[q64dc] pak: couldn't build the VMU file\n");
        return;
    }
    uint64_t t0 = timer_ms_gettime64();
    int err = vmufs_write(dev, VMU_FILE_NAME, pkg, size, VMUFS_OVERWRITE);
    free(pkg);
    if (err < 0) {
        printf("[q64dc] pak: writing %s to VMU %c%d failed\n", VMU_FILE_NAME, 'A' + vmu_port, vmu_unit);
        return;
    }
    vmu_file_blocks = blocks_for(size);
    printf("[q64dc] pak: saved %d blocks to VMU %c%d in %u ms\n", vmu_file_blocks, 'A' + vmu_port, vmu_unit,
           (unsigned)(timer_ms_gettime64() - t0));
}

static void changed(void) {
    dirty = true;
    last_change_ms = timer_ms_gettime64();
}

void pak_flush_idle(void) {
    if (dirty && timer_ms_gettime64() - last_change_ms >= FLUSH_DELAY_MS) {
        flush();
    }
}

void pak_init(void) {
    maple_device_t* dev = find_vmu();
    if (dev == NULL) {
        printf("[q64dc] pak: no VMU found; saving needs one\n");
        return;
    }
    load_from_vmu(dev);
}

// Checks the VMU is still there; a different VMU (or the first one after none) is loaded.
static bool pak_present(void) {
    maple_device_t* dev = find_vmu();
    if (dev == NULL) {
        return false;
    }
    if (!loaded || dev != vmu || dev->port != vmu_port || dev->unit != vmu_unit) {
        flush();
        load_from_vmu(dev);
    }
    return true;
}

// --- libultra ---

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

// initPFSPaks(file, last error) checks the pak of the current controller: 0 when it's ready.
void pak_check(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = pak_present() ? 0 : PFS_ERR_NOPACK;
}

void osPfsInitPak_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint32_t pfs = ctx->r5;
    int32_t channel = (int32_t)ctx->r6;
    if (channel != 0 || !pak_present()) {
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

    if (!pak_present()) {
        ctx->r2 = PFS_ERR_NOPACK;
        return;
    }
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
    // The VMU file grows by the new file; the old one is replaced, so its blocks count as free.
    int needed = package_blocks(FILE_HEADER_SIZE + pages * PAK_PAGE_SIZE);
    int available = vmufs_free_blocks(vmu) + vmu_file_blocks;
    if (needed > available) {
        printf("[q64dc] pak: the VMU is full (%d blocks needed, %d free)\n", needed, available);
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
        changed();
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
    changed();
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
    if (flag == PFS_WRITE) {
        if (find_vmu() == NULL) {
            ctx->r2 = PFS_ERR_NOPACK;
            return;
        }
        for (uint32_t i = 0; i < size; i++) {
            contents[i] = MEM_BU(i, data);
        }
        changed();
    } else {
        for (uint32_t i = 0; i < size; i++) {
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
    int pages = PAK_FREE_PAGES - pages_used();
    // Not more than the VMU can take, in pak pages.
    if (pak_present()) {
        int vmu_bytes = (vmufs_free_blocks(vmu) + vmu_file_blocks - package_blocks(0)) * VMU_BLOCK_SIZE;
        int vmu_pages = vmu_bytes > 0 ? vmu_bytes / PAK_PAGE_SIZE : 0;
        if (vmu_pages < pages) {
            pages = vmu_pages;
        }
    }
    MEM_W(0, ctx->r5) = pages * PAK_PAGE_SIZE;
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
