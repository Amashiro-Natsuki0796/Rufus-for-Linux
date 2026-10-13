/*
 * rf_part.c -- 原生 MBR / GPT 分区表写入
 *
 * Rufus 在 format.c 里先用 IOCTL_DISK_CREATE_DISK 指定分区方案，再用
 * IOCTL_DISK_SET_DRIVE_LAYOUT_EX 提交分区项，由 Windows 内核生成分区表。
 * Linux 无此等价接口（parted/sgdisk 都是用户态实现），所以本文件按
 * UEFI 规范与 MBR 约定直接构造扇区数据：
 *
 *   MBR:  LBA0 = 引导代码(446) + 4×16 分区项 + 0x55AA
 *   GPT:  LBA0 = 保护性 MBR
 *         LBA1 = GPT 头（含 CRC32，含自身校验）
 *         LBA2..LBA33 = 128 × 128 字节分区项
 *         末尾 LBA = 备份 GPT 头，末尾-32..末尾-1 = 备份分区项
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "rf_part.h"
#include "rf_hash.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define GPT_ENTRIES      128
#define GPT_ENTRY_SIZE   128
#define GPT_ENTRIES_LBA  32   /* 128*128/512 */

const char *rf_scheme_name(rf_scheme s)
{
    return (s == RF_SCHEME_GPT) ? "GPT" : "MBR";
}

/* ---------------- 几何 ---------------- */

int rf_part_device_geometry(const char *device, uint32_t *sector_size,
                            uint64_t *total_sectors)
{
    int fd;
    struct stat st;
    unsigned long long sz = 0;
    uint32_t ss = 512;

    fd = open(device, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return (errno == EACCES) ? RF_ERR_PERMISSION : RF_ERR_IO;

    if (fstat(fd, &st) != 0) {
        close(fd);
        return RF_ERR_IO;
    }
    if (S_ISBLK(st.st_mode)) {
        int v = 0;
        if (ioctl(fd, BLKSSZGET, &v) == 0 && v > 0)
            ss = (uint32_t)v;
        if (ioctl(fd, BLKGETSIZE64, &sz) != 0)
            sz = 0;
    } else {
        sz = (unsigned long long)st.st_size;
    }
    close(fd);

    if (sector_size)
        *sector_size = ss;
    if (total_sectors)
        *total_sectors = sz / ss;
    return RF_OK;
}

void rf_part_path(const char *device, int index, char *out, size_t out_len)
{
    const char *base = device;
    if (strncmp(base, "/dev/", 5) == 0)
        base += 5;
    /* NVMe / mmcblk 需要 p 分隔 */
    if (strncmp(base, "nvme", 4) == 0 || strncmp(base, "mmcblk", 6) == 0 ||
        strncmp(base, "loop", 4) == 0)
        snprintf(out, out_len, "%sp%d", device, index);
    else
        snprintf(out, out_len, "%s%d", device, index);
}

/* ---------------- 辅助 ---------------- */

static void wr_le32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}
static void wr_le64(unsigned char *p, uint64_t v)
{
    wr_le32(p, (uint32_t)v);
    wr_le32(p + 4, (uint32_t)(v >> 32));
}

/* GUID 文本 -> 16 字节（前 3 段小端，后 2 段大端，与 UEFI 规范一致） */
static int guid_parse(const char *text, unsigned char out[16])
{
    unsigned int b[16];
    int i;
    if (!text || strlen(text) != 36)
        return -1;
    if (sscanf(text,
               "%2x%2x%2x%2x-%2x%2x-%2x%2x-%2x%2x-%2x%2x%2x%2x%2x%2x",
               &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &b[6], &b[7],
               &b[8], &b[9], &b[10], &b[11], &b[12], &b[13], &b[14], &b[15]) != 16)
        return -1;
    /* Data1 (LE) */
    out[0] = (unsigned char)b[3]; out[1] = (unsigned char)b[2];
    out[2] = (unsigned char)b[1]; out[3] = (unsigned char)b[0];
    /* Data2 (LE) */
    out[4] = (unsigned char)b[5]; out[5] = (unsigned char)b[4];
    /* Data3 (LE) */
    out[6] = (unsigned char)b[7]; out[7] = (unsigned char)b[6];
    /* Data4 (BE) */
    for (i = 8; i < 16; i++)
        out[i] = (unsigned char)b[i];
    return 0;
}

static int gen_guid(unsigned char out[16])
{
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0 || read(fd, out, 16) != 16) {
        if (fd >= 0) close(fd);
        /* 退化方案：用时间与地址混合 */
        srand((unsigned)(rf_monotonic() * 1e6) ^ (unsigned)(uintptr_t)out);
        for (int i = 0; i < 16; i++)
            out[i] = (unsigned char)(rand() & 0xFF);
    } else {
        close(fd);
    }
    out[7] = (unsigned char)((out[7] & 0x0F) | 0x40);  /* 版本 4 */
    out[8] = (unsigned char)((out[8] & 0x3F) | 0x80);  /* 变体 1 */
    return 0;
}

/* UTF-8 -> UTF-16LE（GPT 分区名），返回写入字节数 */
static size_t utf8_to_utf16le(const char *s, unsigned char *out, size_t maxbytes)
{
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)s;
    if (!s)
        return 0;
    while (*p && o + 2 <= maxbytes) {
        uint32_t cp;
        if (p[0] < 0x80) {
            cp = p[0]; p += 1;
        } else if ((p[0] & 0xE0) == 0xC0 && p[1]) {
            cp = (uint32_t)((p[0] & 0x1F) << 6) | (p[1] & 0x3F); p += 2;
        } else if ((p[0] & 0xF0) == 0xE0 && p[1] && p[2]) {
            cp = (uint32_t)((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
            p += 3;
        } else if ((p[0] & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
            cp = (uint32_t)((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) |
                 ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
            p += 4;
        } else {
            break;
        }
        if (cp > 0xFFFF) {
            cp -= 0x10000;
            if (o + 4 > maxbytes) break;
            uint16_t hi = (uint16_t)(0xD800 + (cp >> 10));
            uint16_t lo = (uint16_t)(0xDC00 + (cp & 0x3FF));
            out[o++] = (unsigned char)(hi & 0xFF); out[o++] = (unsigned char)(hi >> 8);
            out[o++] = (unsigned char)(lo & 0xFF); out[o++] = (unsigned char)(lo >> 8);
        } else {
            out[o++] = (unsigned char)(cp & 0xFF);
            out[o++] = (unsigned char)(cp >> 8);
        }
    }
    return o;
}

/* 512 字节扇区的读写 */
static int dev_write(int fd, const void *buf, size_t len, uint64_t offset)
{
    const unsigned char *p = buf;
    while (len) {
        ssize_t n = pwrite(fd, p, len, (off_t)offset);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        p += n; len -= (size_t)n; offset += (uint64_t)n;
    }
    return 0;
}

/* ---------------- MBR ---------------- */

static int write_mbr_table(const char *device, const rf_part_entry *parts, int count,
                           uint32_t disk_sig, int is_protective, uint64_t total_sectors,
                           uint32_t sector_size)
{
    int fd;
    unsigned char mbr[512];
    int i;
    uint64_t first_lba, last_lba;

    fd = open(device, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return (errno == EACCES) ? RF_ERR_PERMISSION : RF_ERR_IO;

    /* 保留已有引导代码（前 446 字节）与磁盘签名 */
    if (pread(fd, mbr, sizeof(mbr), 0) != (ssize_t)sizeof(mbr))
        memset(mbr, 0, sizeof(mbr));

    if (is_protective) {
        /* GPT 保护性 MBR：一个 0xEE 项覆盖除 LBA0 外的所有空间 */
        uint64_t cnt = total_sectors - 1;
        if (cnt > 0xFFFFFFFFULL)
            cnt = 0xFFFFFFFFULL;
        memset(mbr + 446, 0, 4 * 16);
        mbr[446 + 0] = 0x00;
        mbr[446 + 1] = 0x00; mbr[446 + 2] = 0x02; mbr[446 + 3] = 0x00;
        mbr[446 + 4] = 0xEE;
        mbr[446 + 5] = 0xFF; mbr[446 + 6] = 0xFF; mbr[446 + 7] = 0xFF;
        wr_le32(mbr + 446 + 8, 1);
        wr_le32(mbr + 446 + 12, (uint32_t)cnt);
        (void)sector_size;
    } else {
        memset(mbr + 446, 0, 4 * 16);
        if (count > 4)
            count = 4;
        for (i = 0; i < count; i++) {
            unsigned char *e = mbr + 446 + i * 16;
            uint64_t start = parts[i].start_lba;
            uint64_t size = parts[i].size_lba;
            uint64_t end;
            if (size == 0)
                size = total_sectors - start;
            end = start + size - 1;
            /* CHS 字段：对 >8GB 的盘使用 LBA 寻址，CHS 填最大合法值 */
            if (start > 1023 * 255 * 63 || size > 1023 * 255 * 63) {
                e[1] = 0xFE; e[2] = 0xFF; e[3] = 0xFF;
                e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF;
            } else {
                uint32_t cs = (uint32_t)start;
                uint32_t ce = (uint32_t)end;
                uint32_t cyl, head, sec;
                cyl = cs / (255 * 63); head = (cs / 63) % 255; sec = cs % 63 + 1;
                e[1] = (unsigned char)head;
                e[2] = (unsigned char)((sec & 0x3F) | ((cyl >> 2) & 0xC0));
                e[3] = (unsigned char)(cyl & 0xFF);
                cyl = ce / (255 * 63); head = (ce / 63) % 255; sec = ce % 63 + 1;
                e[5] = (unsigned char)head;
                e[6] = (unsigned char)((sec & 0x3F) | ((cyl >> 2) & 0xC0));
                e[7] = (unsigned char)(cyl & 0xFF);
            }
            e[0] = parts[i].bootable ? 0x80 : 0x00;
            e[4] = parts[i].mbr_type;
            wr_le32(e + 8, (uint32_t)(start > 0xFFFFFFFFULL ? 0xFFFFFFFFULL : start));
            wr_le32(e + 12, (uint32_t)(size > 0xFFFFFFFFULL ? 0xFFFFFFFFULL : size));
        }
    }

    /* 磁盘签名 @0x1B8（保护性 MBR 也保留） */
    if (disk_sig)
        wr_le32(mbr + 0x1B8, disk_sig);

    mbr[510] = 0x55;
    mbr[511] = 0xAA;

    if (dev_write(fd, mbr, sizeof(mbr), 0) != 0) {
        close(fd);
        return RF_ERR_IO;
    }
    fsync(fd);
    ioctl(fd, BLKFLSBUF, 0);
    close(fd);

    first_lba = 0; last_lba = 0; (void)first_lba; (void)last_lba;
    return RF_OK;
}

/* ---------------- GPT ---------------- */

static void build_gpt_header(unsigned char *h, uint64_t my_lba, uint64_t alt_lba,
                             uint64_t first_usable, uint64_t last_usable,
                             const unsigned char disk_guid[16],
                             uint64_t entries_lba, uint32_t num_entries,
                             uint32_t entry_size, uint32_t entries_crc)
{
    memset(h, 0, 512);
    memcpy(h, "EFI PART", 8);
    wr_le32(h + 8, 0x00010000);          /* Revision 1.0 */
    wr_le32(h + 12, 92);                 /* HeaderSize */
    /* h+16 HeaderCRC32 稍后计算 */
    wr_le32(h + 20, 0);                  /* Reserved */
    wr_le64(h + 24, my_lba);
    wr_le64(h + 32, alt_lba);
    wr_le64(h + 40, first_usable);
    wr_le64(h + 48, last_usable);
    memcpy(h + 56, disk_guid, 16);
    wr_le64(h + 72, entries_lba);
    wr_le32(h + 80, num_entries);
    wr_le32(h + 84, entry_size);
    wr_le32(h + 88, entries_crc);
    /* 计算头部 CRC32（前 HeaderSize 字节，CRC 字段清零） */
    wr_le32(h + 16, rf_crc32(0, h, 92));
}

static int write_gpt(const char *device, const rf_part_entry *parts, int count,
                     uint64_t total_sectors, uint32_t sector_size,
                     unsigned char disk_guid[16], uint32_t disk_sig)
{
    unsigned char *entries = NULL;
    unsigned char header[512];
    uint64_t entries_lba = 2;
    /* 备份分区项紧邻备份 GPT 头之前：占用 [total-33, total-2]，
     * 备份头位于 total-1。 */
    uint64_t backup_entries_lba = total_sectors - 1 - GPT_ENTRIES_LBA;
    uint64_t backup_header_lba = total_sectors - 1;
    uint64_t first_usable = entries_lba + GPT_ENTRIES_LBA;
    uint64_t last_usable = backup_entries_lba - 1;
    uint32_t entries_crc;
    int fd, i, rc = RF_OK;
    size_t entries_bytes = (size_t)GPT_ENTRIES * GPT_ENTRY_SIZE;

    entries = calloc(1, entries_bytes);
    if (!entries)
        return RF_ERR_NO_MEMORY;

    for (i = 0; i < count && i < GPT_ENTRIES; i++) {
        unsigned char *e = entries + (size_t)i * GPT_ENTRY_SIZE;
        unsigned char guid[16];
        uint64_t start = parts[i].start_lba;
        uint64_t size = parts[i].size_lba;
        uint64_t end;
        if (start < first_usable)
            start = first_usable;
        if (size == 0)
            end = last_usable;
        else
            end = start + size - 1;
        if (end > last_usable)
            end = last_usable;
        /* 末端按 2048 扇区（1 MiB）向下对齐，避免分区跨界告警 */
        if (end == last_usable && end > 2048)
            end = (end / 2048) * 2048 - 1;
        if (guid_parse(parts[i].gpt_type ? parts[i].gpt_type : RF_GPT_MS_BASIC_DATA,
                       guid) != 0) {
            rf_log(RF_LOG_WARN, "无法解析 GPT 类型 GUID，回退为 MS Basic Data");
            guid_parse(RF_GPT_MS_BASIC_DATA, guid);
        }
        memcpy(e + 0, guid, 16);
        gen_guid(e + 16);                        /* 唯一分区 GUID */
        wr_le64(e + 32, start);
        wr_le64(e + 40, end);
        wr_le64(e + 48, parts[i].attributes);    /* Attributes */
        utf8_to_utf16le(parts[i].name, e + 56, 72);
    }

    entries_crc = rf_crc32(0, entries, entries_bytes);

    fd = open(device, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        free(entries);
        return (errno == EACCES) ? RF_ERR_PERMISSION : RF_ERR_IO;
    }

    /* 主 GPT 头 */
    build_gpt_header(header, 1, backup_header_lba, first_usable, last_usable,
                     disk_guid, entries_lba, GPT_ENTRIES, GPT_ENTRY_SIZE, entries_crc);
    if (dev_write(fd, header, 512, 512) != 0 ||
        dev_write(fd, entries, entries_bytes, entries_lba * sector_size) != 0) {
        rc = RF_ERR_IO;
        goto out;
    }

    /* 备份分区项 + 备份头 */
    if (dev_write(fd, entries, entries_bytes, backup_entries_lba * sector_size) != 0) {
        rc = RF_ERR_IO;
        goto out;
    }
    build_gpt_header(header, backup_header_lba, 1, first_usable, last_usable,
                     disk_guid, backup_entries_lba, GPT_ENTRIES, GPT_ENTRY_SIZE, entries_crc);
    if (dev_write(fd, header, 512, backup_header_lba * sector_size) != 0) {
        rc = RF_ERR_IO;
        goto out;
    }

    fsync(fd);
    ioctl(fd, BLKFLSBUF, 0);

out:
    close(fd);
    free(entries);
    if (rc == RF_OK) {
        /* 最后写保护性 MBR（覆盖 LBA0） */
        rc = write_mbr_table(device, NULL, 0, disk_sig, 1, total_sectors, sector_size);
    }
    return rc;
}

/* ---------------- 对外 ---------------- */

int rf_part_write(const char *device, const rf_part_entry *parts, int count,
                  const rf_part_opts *opts)
{
    rf_part_opts def;
    uint32_t ss = 512;
    uint64_t total = 0;
    int rc;

    if (!device || !parts || count <= 0)
        return RF_ERR_INVALID_ARG;
    if (!opts) {
        memset(&def, 0, sizeof(def));
        def.scheme = RF_SCHEME_MBR;
        opts = &def;
    }

    rc = rf_part_device_geometry(device, &ss, &total);
    if (rc != RF_OK)
        return rc;
    if (opts->sector_size)
        ss = opts->sector_size;
    if (total < 2048) {
        rf_log(RF_LOG_ERROR, "设备容量过小，无法创建分区表");
        return RF_ERR_NO_SPACE;
    }

    {
        char sz[32];
        rf_log(RF_LOG_INFO, "创建 %s 分区表: %s（%s，扇区 %u 字节，共 %llu 扇区）",
               rf_scheme_name(opts->scheme), device,
               rf_size_human(total * ss, sz, sizeof(sz)), ss,
               (unsigned long long)total);
    }

    if (opts->scheme == RF_SCHEME_GPT) {
        unsigned char dg[16];
        uint32_t sig = opts->disk_signature;
        if (opts->disk_guid[0]) {
            if (guid_parse(opts->disk_guid, dg) != 0)
                gen_guid(dg);
        } else {
            gen_guid(dg);
        }
        if (!sig) {
            unsigned char r[16];
            gen_guid(r);                       /* 产生 16 字节随机数 */
            memcpy(&sig, r, sizeof(sig));      /* 只取前 4 字节作磁盘签名 */
            if (!sig)
                sig = 0x12345678u;
        }
        return write_gpt(device, parts, count, total, ss, dg, sig);
    }

    /* MBR */
    {
        uint32_t sig = opts->disk_signature;
        if (!sig) {
            unsigned char r[16];
            gen_guid(r);
            memcpy(&sig, r, 4);
            if (!sig)
                sig = 0x12345678u;
        }
        return write_mbr_table(device, parts, count, sig, 0, total, ss);
    }
}

int rf_part_zap(const char *device)
{
    int fd;
    unsigned char *zeros;
    uint64_t total = 0;
    uint32_t ss = 512;
    size_t chunk = 1024 * 1024;
    int rc;

    rc = rf_part_device_geometry(device, &ss, &total);
    if (rc != RF_OK)
        return rc;

    fd = open(device, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return (errno == EACCES) ? RF_ERR_PERMISSION : RF_ERR_IO;

    zeros = calloc(1, chunk);
    if (!zeros) {
        close(fd);
        return RF_ERR_NO_MEMORY;
    }

    /* 首 1 MiB */
    if (dev_write(fd, zeros, chunk, 0) != 0) {
        free(zeros); close(fd);
        return RF_ERR_IO;
    }
    /* 末 1 MiB（若容量足够） */
    if (total * ss > chunk * 2) {
        if (dev_write(fd, zeros, chunk, total * ss - chunk) != 0) {
            free(zeros); close(fd);
            return RF_ERR_IO;
        }
    }
    fsync(fd);
    ioctl(fd, BLKFLSBUF, 0);
    free(zeros);
    close(fd);

    rf_log(RF_LOG_INFO, "已清除 %s 上的分区表痕迹", device);
    return RF_OK;
}

int rf_part_reread(const char *device)
{
    int fd = open(device, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return RF_ERR_IO;
    if (ioctl(fd, BLKRRPART, 0) != 0) {
        /* 设备忙时不致命：内核会在挂载/访问时重新读取 */
        rf_log(RF_LOG_DEBUG, "BLKRRPART 失败（通常因设备忙）: %s", strerror(errno));
    }
    close(fd);
    return RF_OK;
}
