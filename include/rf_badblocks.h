/*
 * rf_badblocks.h -- 坏块/坏道检测（移植自上游 src/badblocks.c）
 */

#ifndef RF_BADBLOCKS_H
#define RF_BADBLOCKS_H

#include "rufus_linux.h"

/* 闪存类型决定测试模式序列，与上游 rufus.h 的 BADBLOCK_PATTERN_* 一一对应 */
typedef enum {
    RF_FLASH_ONE_PASS = 0,   /* {0x55, 0x00, 0x00, 0x00}            */
    RF_FLASH_TWO_PASSES,     /* {0x55, 0xaa, 0x00, 0x00}            */
    RF_FLASH_SLC,            /* {0x00, 0xff, 0x55, 0xaa}            */
    RF_FLASH_MLC,            /* {0x00, 0xff, 0x33, 0xcc}            */
    RF_FLASH_TLC,            /* {0x00, 0xff, 0x1c71c7, 0xe38e38}    */
    RF_FLASH_TYPE_COUNT
} rf_flash_type;

typedef struct {
    uint32_t bb_count;               /* 坏块数量                       */
    uint32_t num_read_errors;
    uint32_t num_write_errors;
    uint32_t num_corruption_errors;  /* 写进去读出来不一致             */
    uint64_t blocks_tested;
    uint64_t bytes_tested;
} rf_badblocks_report;

/* 与上游一致的基本参数 */
#define RF_BB_BLOCK_SIZE     (512ULL * 1024ULL)   /* BADBLOCK_BLOCK_SIZE */
#define RF_BB_BLOCKS_AT_ONCE 64                   /* BB_BLOCKS_AT_ONCE   */

/*
 * 扫描设备。nb_passes 取 1..4，决定使用多少种模式；
 * flash_type 选择模式表。progress 每秒被回调多次以更新界面。
 * 返回 RF_OK（即便发现坏块）、RF_ERR_CANCELLED 或错误码。
 */
int rf_badblocks(const char *device, int nb_passes, rf_flash_type flash_type,
                 rf_badblocks_report *report,
                 rf_progress_cb progress, void *user);

const char *rf_flash_type_name(rf_flash_type t);

#endif /* RF_BADBLOCKS_H */
