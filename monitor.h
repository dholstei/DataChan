/**
 * @file monitor.h
 * @brief Monitor dependencies and signal-controlled run flag.
 */
#include "DataChan.h"
#include <csignal>

/** @brief Signal-handler-safe run flag; not a general interthread atomic.
 * @note Defined in this header, which is intended for one translation unit.
 */
volatile std::sig_atomic_t keep_running = 1;

/** @brief Request monitor termination on SIGINT by clearing keep_running.
 * @param signal Delivered signal number.
 * @note Register this handler separately; defining it does not install it.
 */
void signal_handler(int signal) {
    if (signal == SIGINT) keep_running = 0;
}