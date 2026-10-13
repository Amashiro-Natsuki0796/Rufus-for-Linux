/*
 * rf_write.c -- 把镜像原始写入块设备（DD 模式）
 *
 * 上游 Rufus 的 WriteDrive() 使用 CreateFile("\\\\.\\PhysicalDriveN") +
 * WriteFile + SetFilePointerEx。Linux 上等价物就是直接对 /dev/sdX 做
 * open/pwrite/fsync，另外用 BLKFLSBUF 让内核丢弃缓冲。
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "rf_write.h"
#include "rf_device.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define WRITE_CHUNK (4u * 1024 * 1024)

int rf_flush_device(const char *device)
{
    int fd = open(device, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return RF_ERR_IO;
    ioctl(fd, BLKFLSBUF, 0);
    close(fd);
    return RF_OK;
}

static int open_target(const char *device, int *out_fd, uint64_t *out_size, int *is_block)
{
    struct stat st;
    int fd;

    fd = open(device, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        if (errno == EACCES)
            return RF_ERR_PERMISSION;
        if (errno == EBUSY)
            return RF_ERR_BUSY;
        rf_log(RF_LOG_ERROR, "无法打开目标 %s: %s", device, strerror(errno));
        return RF_ERR_IO;
    }
    if (fstat(fd, &st) != 0) {
        close(fd);
        return RF_ERR_IO;
    }
    *is_block = S_ISBLK(st.st_mode);
    if (*is_block) {
        unsigned long long sz = 0;
        if (ioctl(fd, BLKGETSIZE64, &sz) == 0)
            *out_size = (uint64_t)sz;
        else
            *out_size = 0;
    } else {
        *out_size = (uint64_t)st.st_size;
    }
    *out_fd = fd;
    return RF_OK;
}

int rf_write_at(const char *device, uint64_t offset, const void *data, size_t len)
{
    int fd, is_block;
    uint64_t size;
    ssize_t n;
    int rc;

    rc = open_target(device, &fd, &size, &is_block);
    if (rc != RF_OK)
        return rc;
    n = pwrite(fd, data, len, (off_t)offset);
    fsync(fd);
    close(fd);
    if (n != (ssize_t)len)
        return RF_ERR_IO;
    return RF_OK;
}

int rf_wipe_region(const char *device, uint64_t offset, uint64_t len,
                   unsigned char pattern, rf_progress_cb progress, void *user)
{
    int fd, is_block;
    uint64_t size, done = 0;
    unsigned char *buf;
    double t0;
    int rc;

    rc = open_target(device, &fd, &size, &is_block);
    if (rc != RF_OK)
        return rc;

    buf = malloc(WRITE_CHUNK);
    if (!buf) {
        close(fd);
        return RF_ERR_NO_MEMORY;
    }
    memset(buf, pattern, WRITE_CHUNK);
    t0 = rf_monotonic();

    while (done < len) {
        size_t want = WRITE_CHUNK;
        ssize_t n;
        if (len - done < (uint64_t)want)
            want = (size_t)(len - done);
        n = pwrite(fd, buf, want, (off_t)(offset + done));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            rf_log(RF_LOG_ERROR, "写入失败 @%llu: %s",
                   (unsigned long long)(offset + done), strerror(errno));
            free(buf);
            close(fd);
            return RF_ERR_IO;
        }
        done += (uint64_t)n;
        if (progress) {
            rf_progress p;
            double el = rf_monotonic() - t0;
            memset(&p, 0, sizeof(p));
            p.done = done;
            p.total = len;
            p.speed_bps = el > 0.001 ? (double)done / el : 0.0;
            p.eta_sec = p.speed_bps > 0 ? (double)(len - done) / p.speed_bps : -1.0;
            p.percent = (int)((double)done * 100.0 / (double)len);
            p.phase = "清除";
            if (progress(&p, user))
                break;
        }
    }
    fsync(fd);
    if (is_block)
        ioctl(fd, BLKFLSBUF, 0);
    free(buf);
    close(fd);
    return RF_OK;
}

int rf_write_image(const char *image, const char *device,
                   const rf_write_opts *opts,
                   rf_progress_cb progress, void *user,
                   rf_write_result *result)
{
    rf_write_opts def = { 0 };
    struct stat st_img;
    int fd_src = -1, fd_dst = -1;
    uint64_t total, done = 0, dev_size = 0;
    unsigned char *buf = NULL;
    double t0;
    int rc = RF_OK;
    int is_block = 0;
    rf_hash_ctx hsrc, hdst;
    unsigned char dig_src[128], dig_dst[128];
    int hashing = 0;

    if (!image || !device)
        return RF_ERR_INVALID_ARG;
    if (!opts)
        opts = &def;

    if (stat(image, &st_img) != 0) {
        rf_log(RF_LOG_ERROR, "找不到镜像 %s", image);
        return RF_ERR_NOT_FOUND;
    }

    rc = open_target(device, &fd_dst, &dev_size, &is_block);
    if (rc != RF_OK)
        return rc;

    fd_src = open(image, O_RDONLY | O_CLOEXEC);
    if (fd_src < 0) {
        close(fd_dst);
        rf_log(RF_LOG_ERROR, "打开镜像失败: %s", strerror(errno));
        return RF_ERR_IO;
    }

    total = (uint64_t)st_img.st_size;
    if (opts->max_bytes && opts->max_bytes < total)
        total = opts->max_bytes;

    if (is_block && dev_size && total > dev_size) {
        char a[32], b[32];
        rf_log(RF_LOG_ERROR, "镜像(%s)大于目标设备(%s)，中止",
               rf_size_human(total, a, sizeof(a)), rf_size_human(dev_size, b, sizeof(b)));
        close(fd_src); close(fd_dst);
        return RF_ERR_NO_SPACE;
    }

    rf_log(RF_LOG_INFO, "开始写入: %s -> %s (%llu 字节)", image, device,
           (unsigned long long)total);

    buf = malloc(WRITE_CHUNK);
    if (!buf) {
        close(fd_src); close(fd_dst);
        return RF_ERR_NO_MEMORY;
    }

    if (opts->verify) {
        hashing = 1;
        rf_hash_init(&hsrc, opts->verify_algo);
        rf_hash_init(&hdst, opts->verify_algo);
    }

    t0 = rf_monotonic();

    while (done < total) {
        size_t want = WRITE_CHUNK;
        ssize_t nr, nw;
        size_t off;

        if (total - done < (uint64_t)want)
            want = (size_t)(total - done);

        nr = read(fd_src, buf, want);
        if (nr < 0) {
            if (errno == EINTR) continue;
            rf_log(RF_LOG_ERROR, "读取镜像失败: %s", strerror(errno));
            rc = RF_ERR_IO;
            break;
        }
        if (nr == 0)
            break;

        if (hashing)
            rf_hash_update(&hsrc, buf, (size_t)nr);

        /* 写循环：处理短写 */
        off = 0;
        while (off < (size_t)nr) {
            nw = pwrite(fd_dst, buf + off, (size_t)nr - off, (off_t)(done + off));
            if (nw < 0) {
                if (errno == EINTR) continue;
                rf_log(RF_LOG_ERROR, "写入目标失败 @%llu: %s",
                       (unsigned long long)(done + off), strerror(errno));
                rc = RF_ERR_IO;
                break;
            }
            off += (size_t)nw;
        }
        if (rc != RF_OK)
            break;

        done += (uint64_t)nr;

        if (progress) {
            rf_progress p;
            double el = rf_monotonic() - t0;
            memset(&p, 0, sizeof(p));
            p.done = done;
            p.total = total;
            p.speed_bps = el > 0.001 ? (double)done / el : 0.0;
            p.eta_sec = p.speed_bps > 0 ? (double)(total - done) / p.speed_bps : -1.0;
            p.percent = (int)((double)done * 100.0 / (double)total);
            p.phase = "写入镜像";
            if (progress(&p, user)) {
                rf_log(RF_LOG_WARN, "用户取消写入");
                rc = RF_ERR_CANCELLED;
                break;
            }
        }
        if (rf_is_cancelled()) {
            rc = RF_ERR_CANCELLED;
            break;
        }
    }

    if (rc == RF_OK) {
        if (fsync(fd_dst) != 0)
            rf_log(RF_LOG_WARN, "fsync 失败: %s", strerror(errno));
        if (is_block)
            ioctl(fd_dst, BLKFLSBUF, 0);
    }

    if (result) {
        result->bytes_written = done;
        result->duration_ms = (uint64_t)((rf_monotonic() - t0) * 1000.0);
        result->avg_bps = result->duration_ms ?
            (double)done * 1000.0 / (double)result->duration_ms : 0.0;
        result->verify_ok = 0;
        result->image_hash[0] = result->device_hash[0] = '\0';
    }

    if (rc == RF_OK && opts->verify && result) {
        rf_hash_final(&hsrc, dig_src);
        rf_hash_hex(dig_src, rf_hash_size(opts->verify_algo), result->image_hash);

        rf_log(RF_LOG_INFO, "写入完成，开始读回校验（%s）...",
               rf_hash_name(opts->verify_algo));

        /* 关闭并只读重新打开，确保读到的是设备真实内容 */
        close(fd_dst);
        fd_dst = open(device, O_RDONLY | O_CLOEXEC);
        if (fd_dst < 0) {
            rc = RF_ERR_IO;
        } else {
            int vrc = rf_hash_fd(opts->verify_algo, fd_dst, total, dig_dst,
                                 "读回校验", progress, user);
            if (vrc == RF_OK) {
                rf_hash_hex(dig_dst, rf_hash_size(opts->verify_algo), result->device_hash);
                if (memcmp(dig_src, dig_dst, rf_hash_size(opts->verify_algo)) == 0) {
                    result->verify_ok = 1;
                    rf_log(RF_LOG_INFO, "校验通过: %s", result->image_hash);
                } else {
                    rf_log(RF_LOG_ERROR, "校验失败！");
                    rf_log(RF_LOG_ERROR, "  镜像: %s", result->image_hash);
                    rf_log(RF_LOG_ERROR, "  设备: %s", result->device_hash);
                    rc = RF_ERR_VERIFY_FAILED;
                }
            } else {
                rc = vrc;
            }
        }
    } else if (rc == RF_OK && opts->verify) {
        rf_hash_final(&hsrc, dig_src);   /* 丢弃 */
    }

    free(buf);
    if (fd_dst >= 0) close(fd_dst);
    if (fd_src >= 0) close(fd_src);
    return rc;
}
