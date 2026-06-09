#ifndef QBDI_ANDROID_SIGNAL_HOST_H
#define QBDI_ANDROID_SIGNAL_HOST_H

#include <cstdint>

#if defined(QBDI_PLATFORM_ANDROID)

#define QBDI_ANDROID_SIGNAL_BROKER_HOST_SECTION "qbdi_execbroker_host"
#define QBDI_ANDROID_SIGNAL_BROKER_HOST                                     \
  __attribute__((section(QBDI_ANDROID_SIGNAL_BROKER_HOST_SECTION)))

extern "C" const char __start_qbdi_execbroker_host[];
extern "C" const char __stop_qbdi_execbroker_host[];

inline uintptr_t getAndroidSignalBrokerHostStart() {
  return reinterpret_cast<uintptr_t>(__start_qbdi_execbroker_host);
}

inline uintptr_t getAndroidSignalBrokerHostEnd() {
  return reinterpret_cast<uintptr_t>(__stop_qbdi_execbroker_host);
}

#else

#define QBDI_ANDROID_SIGNAL_BROKER_HOST

#endif

#endif // QBDI_ANDROID_SIGNAL_HOST_H
