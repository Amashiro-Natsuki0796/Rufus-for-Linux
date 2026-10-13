/*
 * rf_badblocks.c -- 坏块检测的 Linux 移植
 *
 * 上游 src/badblocks.c 源自 e2fsprogs 的 badblocks，Rufus 在其上：
 *   - 使用 512 KiB 的逻辑块（适配闪存页/擦除块粒度）
 *   - 一次处理 64 个块（BB_BLOCKS_AT_ONCE）
 *   - 按闪存类型选择 1..4 轮互补模式（0x55/0xaa/0x00/0xff 等）
 *   - 写完后立刻读回比对，区分"读错误/写错误/数据损坏"
 *
 * 移植时把 Win32 的 SetFilePointerEx/ReadFile/WriteFile 换成 pread/pwrite，
 * 把 SetTimer 状态刷新换成绩效回调，其余算法逻辑（含逐块收敛定位）保持同构。
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "rf_badblocks.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* 与上游 rufus.h 完全一致的 5 组模式表 */
static const unsigned int bb_patterns[RF_FLASH_TYPE_COUNT][4] = {
    { 0x55,       0x00,       0x00,       0x00       },  /* ONE_PASS   */
    { 0x55,       0xaa,       0x00,       0x00       },  /* TWO_PASSES */
    { 0x00,       0xff,       0x55,       0xaa       },  /* SLC        */
    { 0x00,       0xff,       0x33,       0xcc       },  /* MLC        */
    { 0x00,       0xff,       0x1c71c7,   0xe38e38   },  /* TLC        */
};

const char *rf_flash_type_name(rf_flash_type t)
{
    switch (t) {
    case RF_FLASH_ONE_PASS:   return "单次写入(1 轮)";
    case RF_FLASH_TWO_PASSES: return "双次写入(2 轮)";
    case RF_FLASH_SLC:        return "SLC(4 轮)";
    case RF_FLASH_MLC:        return "MLC(4 轮)";
    case RF_FLASH_TLC:        return "TLC(4 轮)";
    default:                  return "未知";
    }
}

/*
 * 按上游 pattern_fill() 的规则填充缓冲区。
 * 注意其"反转重复"语义：把 pattern 的字节按低到高写入，然后反向重复，
 * 使得缓冲区形如 b[nb] b[nb-1] ... b[1] b[0] 循环。
 */
static void pattern_fill(unsigned char *buffer, unsigned int pattern, size_t n)
{
    unsigned int i, nb;
    unsigned char bpattern[sizeof(pattern)];
    unsigned char *ptr;

    if (pattern == 0xFFFFFFFFu) {
        /* 随机模式 */
        srand((unsigned int)(rf_monotonic() * 1000.0));
        for (ptr = buffer; ptr < buffer + n; ptr++)
            *ptr = (unsigned char)(rand() & 0xFF);
        return;
    }

    bpattern[0] = 0;
    for (i = 0; i < sizeof(bpattern); i++) {
        if (pattern == 0)
            break;
        bpattern[i] = (unsigned char)(pattern & 0xFF);
        pattern >>= 8;
    }
    nb = i ? (i - 1) : 0;
    for (ptr = buffer, i = nb; ptr < buffer + n; ptr++) {
        *ptr = bpattern[i];
        if (i == 0)
            i = nb;
        else
            i--;
    }
}

/* 统计性数据结构 */
typedef struct {
    unsigned char *buf;          /* 写缓冲区 */
    unsigned char *rbuf;         /* 读缓冲区 */
    int            fd;
    uint64_t       first_block;
    uint64_t       last_block;
    size_t         blocks_at_once;
    size_t         block_size;
    rf_badblocks_report *rep;
    rf_progress_cb progress;
    void          *user;
    int            pass_index;
    int            num_passes;
    double         t0;
    uint64_t       total_ops;    /* 用于进度 */
    uint64_t       done_ops;
    uint32_t       bb;
} bb_ctx;

/*
 * 尝试读写一组块。返回成功处理的块数。
 * op = 0 读，1 写。
 */
static int64_t bb_do(bb_ctx *c, int op, uint64_t current_block, size_t nblocks)
{
    size_t bytes = nblocks * c->block_size;
    off_t off = (off_t)(current_block * c->block_size);
    unsigned char *p = (op == 0) ? c->rbuf : c->buf;
    size_t done = 0;

    while (done < bytes) {
        ssize_t r;
        if (op == 0)
            r = pread(c->fd, p + done, bytes - done, off + (off_t)done);
        else
            r = pwrite(c->fd, p + done, bytes - done, off + (off_t)done);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (r == 0)
            break;
        done += (size_t)r;
    }
    if (done % c->block_size)
        rf_log(RF_LOG_DEBUG, "非整块 I/O: %zu 字节", done);
    return (int64_t)(done / c->block_size);
}

static void bb_status(bb_ctx *c, const char *phase, uint64_t cur_block)
{
    rf_progress p;
    double el = rf_monotonic() - c->t0;

    if (!c->progress)
        return;
    memset(&p, 0, sizeof(p));
    p.done = c->done_ops;
    p.total = c->total_ops;
    p.speed_bps = el > 0.001 ? (double)c->done_ops * c->block_size / el : 0.0;
    p.eta_sec = p.speed_bps > 0 ?
        (double)(c->total_ops - c->done_ops) * c->block_size / p.speed_bps : -1.0;
    p.percent = c->total_ops ? (int)((double)c->done_ops * 100.0 / (double)c->total_ops) : -1;
    p.phase = phase;
    (void)cur_block;
    if (c->progress(&p, c->user))
        rf_request_cancel();
}

/* 记录一个坏块 */
static void bb_mark(bb_ctx *c, uint64_t block)
{
    if (c->bb < 0xFFFFFFFFu)
        c->bb++;
    c->rep->bb_count = c->bb;
    rf_log(RF_LOG_WARN, "发现坏块 @ 块 %llu (偏移 %llu, %s)",
           (unsigned long long)block,
           (unsigned long long)(block * c->block_size),
           "该区域读写不一致");
}

/*
 * 对一组块做"写-读-比对"。
 * 若整组不一致或出错，则退化为逐块测试以精确定位（与上游策略一致）。
 */
static void bb_test_group(bb_ctx *c, uint64_t first, size_t nblocks, int is_write_op)
{
    int64_t wrote, got;
    size_t i;

    wrote = bb_do(c, 1, first, nblocks);
    if ((size_t)wrote != nblocks) {
        c->rep->num_write_errors++;
        rf_log(RF_LOG_WARN, "写入错误 @ 块 %llu（%d/%zu 块成功）",
               (unsigned long long)first, (int)wrote, nblocks);
        /* 逐块定位 */
        for (i = 0; i < nblocks; i++) {
            if (bb_do(c, 1, first + i, 1) != 1) {
                c->rep->num_write_errors++;
                bb_mark(c, first + i);
            }
        }
        c->done_ops += nblocks;
        bb_status(c, is_write_op ? "写入测试模式" : "校验", first);
        return;
    }

    /* 立刻读回比对 */
    memset(c->rbuf, 0, nblocks * c->block_size);
    got = bb_do(c, 0, first, nblocks);
    if ((size_t)got != nblocks) {
        c->rep->num_read_errors++;
        rf_log(RF_LOG_WARN, "读取错误 @ 块 %llu（%d/%zu 块成功）",
               (unsigned long long)first, (int)got, nblocks);
        for (i = 0; i < nblocks; i++) {
            if (bb_do(c, 0, first + i, 1) != 1) {
                c->rep->num_read_errors++;
                bb_mark(c, first + i);
            }
        }
        c->done_ops += nblocks;
        bb_status(c, "读取测试模式", first);
        return;
    }

    if (memcmp(c->buf, c->rbuf, nblocks * c->block_size) != 0) {
        /* 整组不一致：逐块定位 */
        size_t nbad = 0;
        for (i = 0; i < nblocks; i++) {
            size_t off = i * c->block_size;
            if (bb_do(c, 0, first + i, 1) != 1)
                continue;
            if (memcmp(c->buf + off, c->rbuf, c->block_size) != 0) {
                c->rep->num_corruption_errors++;
                bb_mark(c, first + i);
                nbad++;
            }
        }
        if (nbad == 0) {
            /* 组级比对失败但逐块正常 → 视为一次性读抖动 */
            rf_log(RF_LOG_DEBUG, "组级比对失败但逐块正常，忽略 (块 %llu)",
                   (unsigned long long)first);
        }
    }

    c->done_ops += nblocks;
    bb_status(c, "写入并校验", first);
}

static int bb_check_cancel(bb_ctx *c)
{
    if (rf_is_cancelled()) {
        rf_log(RF_LOG_WARN, "坏块检测被取消");
        return 1;
    }
    return 0;
}

int rf_badblocks(const char *device, int nb_passes, rf_flash_type flash_type,
                 rf_badblocks_report *report,
                 rf_progress_cb progress, void *user)
{
    bb_ctx c;
    int fd;
    uint64_t size = 0;
    int pass;
    int rc = RF_OK;

    if (!device || !report)
        return RF_ERR_INVALID_ARG;
    if (nb_passes < 1 || nb_passes > 4)
        return RF_ERR_INVALID_ARG;
    if (flash_type < 0 || flash_type >= RF_FLASH_TYPE_COUNT)
        return RF_ERR_INVALID_ARG;

    memset(report, 0, sizeof(*report));
    memset(&c, 0, sizeof(c));

    fd = open(device, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return (errno == EACCES) ? RF_ERR_PERMISSION :
               (errno == EBUSY) ? RF_ERR_BUSY : RF_ERR_IO;

    {
        struct stat st;
        if (fstat(fd, &st) != 0) {
            close(fd);
            return RF_ERR_IO;
        }
        if (S_ISBLK(st.st_mode)) {
            unsigned long long sz = 0;
            if (ioctl(fd, BLKGETSIZE64, &sz) == 0)
                size = (uint64_t)sz;
        } else {
            size = (uint64_t)st.st_size;
        }
    }
    if (size < RF_BB_BLOCK_SIZE * 2) {
        close(fd);
        rf_log(RF_LOG_ERROR, "设备太小，无法进行坏块检测");
        return RF_ERR_INVALID_ARG;
    }

    c.fd = fd;
    c.first_block = 0;
    c.last_block = size / RF_BB_BLOCK_SIZE;
    c.blocks_at_once = RF_BB_BLOCKS_AT_ONCE;
    c.block_size = RF_BB_BLOCK_SIZE;
    c.rep = report;
    c.progress = progress;
    c.user = user;
    c.num_passes = nb_passes;
    c.t0 = rf_monotonic();

    c.buf = malloc(c.blocks_at_once * c.block_size * 2);
    if (!c.buf) {
        close(fd);
        return RF_ERR_NO_MEMORY;
    }
    c.rbuf = c.buf + c.blocks_at_once * c.block_size;

    {
        char sz[32];
        rf_log(RF_LOG_INFO, "坏块检测: %s，块大小 %s，共 %llu 个块，模式 %s，%d 轮",
               device,
               rf_size_human(RF_BB_BLOCK_SIZE, sz, sizeof(sz)),
               (unsigned long long)c.last_block,
               rf_flash_type_name(flash_type), nb_passes);
    }

    /* total_ops：每轮对每个块做 写+读 计 1 次 done */
    c.total_ops = (uint64_t)nb_passes * c.last_block;

    for (pass = 0; pass < nb_passes; pass++) {
        uint64_t blk;
        unsigned int pat = bb_patterns[flash_type][pass];

        if (bb_check_cancel(&c)) { rc = RF_ERR_CANCELLED; break; }

        rf_log(RF_LOG_INFO, "第 %d/%d 轮：写入测试模式 0x%08X", pass + 1, nb_passes, pat);
        pattern_fill(c.buf, pat, c.blocks_at_once * c.block_size);
        c.pass_index = pass;

        for (blk = c.first_block; blk < c.last_block; ) {
            size_t n = (size_t)(c.last_block - blk);
            if (n > c.blocks_at_once)
                n = c.blocks_at_once;
            if (bb_check_cancel(&c)) { rc = RF_ERR_CANCELLED; break; }
            /* 每轮填充一次缓冲区（模式填充是按缓冲区整体进行的） */
            pattern_fill(c.buf, pat, n * c.block_size);
            bb_test_group(&c, blk, n, 1);
            blk += n;
        }
        if (rc != RF_OK)
            break;
    }

    report->blocks_tested = c.done_ops;
    report->bytes_tested = c.done_ops * c.block_size;

    if (rc == RF_OK)
        fsync(fd);
    free(c.buf);
    close(fd);
    return rc;
}
