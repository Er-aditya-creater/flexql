#pragma once
/*
 * include/concurrency/mutex_guard.h
 * ----------------------------------
 * RAII wrapper for pthread_mutex_t.
 * Locks on construction, unlocks on destruction.
 */
#include <pthread.h>

class MutexGuard {
    pthread_mutex_t& m_;
public:
    explicit MutexGuard(pthread_mutex_t& m) : m_(m) { pthread_mutex_lock(&m_); }
    ~MutexGuard()                                    { pthread_mutex_unlock(&m_); }
    MutexGuard(const MutexGuard&)            = delete;
    MutexGuard& operator=(const MutexGuard&) = delete;
};
