/**
 * @file monitor.cpp
 * @brief Console light-state monitor with initial reporting and periodic polling.
 */
#include "monitor.h"

/**
 * @brief Connect, report initial states, and run periodic light monitoring.
 * @param argc Argument count; unused.
 * @param argv Argument vector; unused.
 * @return 1 on reported setup/initial-read failure; otherwise returns 0 implicitly.
 * @note The main loop sleeps 500 ms between flag checks. This implementation
 *       does not register signal_handler and disconnects before stopping the worker.
 */
int main(int argc, char* argv[])
{
    CommTCP device = CommTCP("localhost", 8080);
    DataChan<CommTCP> LightChan = DataChan<CommTCP>(250, device);

    /** @brief Print new/changed state objects and identify removed lights.
     * @note The first snapshot reports all lights because last_state is empty.
     */
    LightChan.Update = [](DataChan<CommTCP>& LightChan) {
        for (const auto& [id, state] : LightChan.cur_state) {
            const auto previous = LightChan.last_state.find(id);
            if (previous == LightChan.last_state.end() || previous->second != state)
                std::printf("%s\n", state.dump(2).c_str());
        }

        for (const auto& [id, state] : LightChan.last_state) {
            if (LightChan.cur_state.find(id) == LightChan.cur_state.end()) {
                const std::string name = state.value("name", "");
                std::printf("%s (%s) has been removed\n", name.c_str(), id.c_str());
            }
        }
    };

    if (!LightChan.Connect()) {
        LightChan.error.Publish();
        return 1;
    }

    // Quick functional test and initialization before starting the worker thread.
    LightChan.Read();
    if (LightChan.error.level > NOERR) {LightChan.error.Publish(); return 1;}
    LightChan.Update(LightChan);

    if (!LightChan.Start()) {
        LightChan.error.Publish();
        return 1;
    }

    while (keep_running) std::this_thread::sleep_for(std::chrono::milliseconds(500));

    LightChan.Disconnect();
    if (LightChan.error.level > NOERR)
        LightChan.error.Publish();
    LightChan.Stop();
}