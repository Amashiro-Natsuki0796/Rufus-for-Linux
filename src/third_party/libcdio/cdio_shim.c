/*
 * cdio_shim.c -- vendored libcdio 在 Linux 上缺失的两个符号
 *
 * Rufus 在 Windows 上只使用 libcdio 的 ISO9660/UDF 解析部分，
 * 因此上游 src/iso.c 自己把 cdio_open()/cdio_destroy() 打桩成空实现
 * （见 rufus-src/src/iso.c 第 73-74 行）。这里做同样的事：
 * 泛型设备驱动（光盘、SCSI MMC）在 Rufus for Linux 中不需要，
 * 我们只通过 _cdio_stdio 直接读取镜像文件。
 */

#include <cdio/cdio.h>
#include <cdio/disc.h>

CdIo_t *cdio_open(const char *psz_source, driver_id_t driver_id)
{
    (void)psz_source;
    (void)driver_id;
    return NULL;
}

CdIo_t *cdio_open_am(const char *psz_source, driver_id_t driver_id,
                     const char *psz_access_mode)
{
    (void)psz_source;
    (void)driver_id;
    (void)psz_access_mode;
    return NULL;
}

void cdio_destroy(CdIo_t *p_cdio)
{
    (void)p_cdio;
}
