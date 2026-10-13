/* config.h for libcdio -- Linux (POSIX/glibc) port
 *
 * 本文件原为 Rufus 仓库中 src/libcdio/config.h 的 MinGW/MSVC 版本。
 * 移植到 Linux 时，我们把原先"缺少什么就补什么"的定义反过来：
 * POSIX 环境下绝大多数函数都存在，因此直接声明为可用，
 * 并删除所有 MSVC/MinGW 专有的宏替换（如 strdup -> _strdup、inline -> __inline）。
 */

#ifndef LIBCDIO_CONFIG_H
#define LIBCDIO_CONFIG_H 1

/* ---- 基础头文件：Linux 全部具备 ---- */
#define HAVE_ERRNO_H 1
#define HAVE_FCNTL_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_LIMITS_H 1
#define HAVE_MEMORY_H 1
#define HAVE_STDARG_H 1
#define HAVE_STDBOOL_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_STRINGS_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_PARAM_H 1
#define HAVE_UNISTD_H 1
#define STDC_HEADERS 1

/* ---- 函数可用性 ---- */
#define HAVE_CHDIR 1
#define HAVE_DRAND48 1
#define HAVE_FTRUNCATE 1
#define HAVE_ICONV 1
#define HAVE_LSTAT 1
#define HAVE_MEMCPY 1
#define HAVE_MEMSET 1
#define HAVE_RAND 1
#define HAVE_READLINK 1
#define HAVE_REALPATH 1
#define HAVE_SETENV 1
#define HAVE_SETEUID 1
#define HAVE_SETEGID 1
#define HAVE_SLEEP 1
#define HAVE_SNPRINTF 1
#define HAVE_VSNPRINTF 1
#define HAVE_STRDUP 1
#define HAVE_STRNDUP 1
#define HAVE_TIMEGM 1
#define HAVE_GMTIME_R 1
#define HAVE_LOCALTIME_R 1
#define HAVE_TZSET 1
#define HAVE_UNSETENV 1
#define HAVE_USLEEP 1

/* ---- 大文件 / 64 位偏移 ---- */
#define HAVE_LSEEK64 1
#define HAVE_FSEEKO 1
#define HAVE_FSEEKO64 1
#define _FILE_OFFSET_BITS 64
/* glibc 没有 fseeko64/ftello64：在 _FILE_OFFSET_BITS=64 下
 * fseeko/ftello 本身就是 64 位偏移版本，因此直接做宏映射。
 * 这与 MinGW 上 "fseeko64 即 fseeko" 的语义等价。 */
#ifndef __MINGW32__
# define fseeko64 fseeko
# define ftello64 ftello
#endif

/* ---- 时区 / 结构体细节 ---- */
#define HAVE_DAYLIGHT 1
#define HAVE_TIMEZONE_VAR 1
#define HAVE_TZNAME 1
#define HAVE_STRUCT_TIMESPEC 1
#define HAVE_TM_GMTOFF 1
#define HAVE_S_ISLNK 1
#define HAVE_S_ISSOCK 1

/* ---- libcdio 功能开关 ---- */
/* 使用 2020 版 API，与 Rufus 内置的调用方式一致 */
#define DO_NOT_WANT_COMPATIBILITY /**/
/* Rock Ridge 扩展（Linux ISO 常见） */
#define HAVE_ROCK 1
/* Joliet 扩展（Windows ISO 常见），依赖 iconv */
#define HAVE_JOLIET 1
/* ISO C99 _Pragma 支持 */
#define HAVE_ISOC99_PRAGMA 1

/* ---- 显式关闭 Windows 专有项 ---- */
/* #undef HAVE_WINDOWS_H */
/* #undef HAVE_WIN32_CDROM */
/* #undef HAVE_SYS_CDIO_H */
/* #undef HAVE__STATI64 */

/* 空数组包装宏（Rufus 的 MinGW 构建使用同一约定） */
#define EMPTY_ARRAY_SIZE

/* glibc 的 iconv() 第二参数为 char **（非 const char **），
 * 原 Windows 构建中通过此宏消解差异。 */
#define ICONV_CONST

#endif /* LIBCDIO_CONFIG_H */
