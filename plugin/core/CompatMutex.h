#pragma once

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <mutex>
#endif

namespace zsoda::core {

// Windows AE host process에서 MSVCP std::mutex ABI 경로를 피하기 위한 호환 락.
class CompatMutex {
 public:
  CompatMutex() = default;
  CompatMutex(const CompatMutex&) = delete;
  CompatMutex& operator=(const CompatMutex&) = delete;

  void lock() noexcept {
#if defined(_WIN32)
    ::AcquireSRWLockExclusive(&lock_);
#else
    mutex_.lock();
#endif
  }

  void unlock() noexcept {
#if defined(_WIN32)
    ::ReleaseSRWLockExclusive(&lock_);
#else
    mutex_.unlock();
#endif
  }

 private:
#if defined(_WIN32)
  SRWLOCK lock_ = SRWLOCK_INIT;
#else
  std::mutex mutex_;
#endif
};

class CompatLockGuard {
 public:
  explicit CompatLockGuard(CompatMutex& mutex) : mutex_(mutex) { mutex_.lock(); }
  CompatLockGuard(const CompatLockGuard&) = delete;
  CompatLockGuard& operator=(const CompatLockGuard&) = delete;
  ~CompatLockGuard() { mutex_.unlock(); }

 private:
  CompatMutex& mutex_;
};

}  // namespace zsoda::core
