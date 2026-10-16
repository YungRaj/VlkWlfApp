#pragma once

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <unistd.h>
#include <sys/syscall.h>

#ifdef __ANDROID__
#include <android/log.h>

#define LOGV(tag, fmt, ...) __android_log_print(ANDROID_LOG_VERBOSE, tag, fmt, ##__VA_ARGS__)
#define LOGD(tag, fmt, ...) __android_log_print(ANDROID_LOG_DEBUG, tag, fmt, ##__VA_ARGS__)
#define LOGI(tag, fmt, ...) __android_log_print(ANDROID_LOG_INFO, tag, fmt, ##__VA_ARGS__)
#define LOGW(tag, fmt, ...) __android_log_print(ANDROID_LOG_WARN, tag, fmt, ##__VA_ARGS__)
#define LOGE(tag, fmt, ...) __android_log_print(ANDROID_LOG_ERROR, tag, fmt, ##__VA_ARGS__)

#else

namespace vkwlf {
inline void HostLogPrint(char level, const char* tag, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));

inline void HostLogPrint(char level, const char* tag, const char* fmt, ...) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  struct tm tm_info;
  localtime_r(&ts.tv_sec, &tm_info);
  char time_buf[32];
  strftime(time_buf, sizeof(time_buf), "%m-%d %H:%M:%S", &tm_info);
  long ms = ts.tv_nsec / 1000000;
  pid_t pid = getpid();
  long tid = syscall(SYS_gettid);

  fprintf(stderr, "%s.%03ld %5d %5ld %c %-16s: ", time_buf, ms, pid, tid, level, tag);
  va_list args;
  va_start(args, fmt);
  vfprintf(stderr, fmt, args);
  va_end(args);
  fprintf(stderr, "\n");
  fflush(stderr);
}
}  // namespace vkwlf


#define LOGV(tag, fmt, ...) ::vkwlf::HostLogPrint('V', tag, fmt, ##__VA_ARGS__)
#define LOGD(tag, fmt, ...) ::vkwlf::HostLogPrint('D', tag, fmt, ##__VA_ARGS__)
#define LOGI(tag, fmt, ...) ::vkwlf::HostLogPrint('I', tag, fmt, ##__VA_ARGS__)
#define LOGW(tag, fmt, ...) ::vkwlf::HostLogPrint('W', tag, fmt, ##__VA_ARGS__)
#define LOGE(tag, fmt, ...) ::vkwlf::HostLogPrint('E', tag, fmt, ##__VA_ARGS__)

#endif
