/**
 * @file DataChan.h
 * @brief TCP/HTTP device interface and scheduled data-channel template.
 */
#pragma once

#include "Error.h"
#include <nlohmann/json.hpp>

#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <sstream>
#include <map>

/**
 * @brief TCP connection providing HTTP/1.1 JSON requests.
 * @note Operations are synchronous and must be serialized by the caller.
 * @note Copying this struct copies its descriptor; do not copy a connected device.
 *       Call Disconnect() explicitly to release the connection.
 */
struct CommTCP {
    int socket = -1; ///< TCP descriptor; -1 indicates no connection.
    std::string host; ///< Hostname or numeric IP address.
    int port = 8080; ///< TCP service port.
    Error error{NOERR, "", ""}; ///< Most recent device operation diagnostic.

    /// @brief Construct a disconnected device with an empty host.
    CommTCP() = default;
    /** @brief Configure a disconnected device.
     * @param host Hostname or numeric IP address.
     * @param port TCP port, defaulting to 8080.
     */
    CommTCP(std::string host, int port = 8080)
        : host(host), port(port) {}

    /** @brief Resolve the host and connect using the first successful address.
     * @return True when connected; false with error populated on failure.
     * @note Configures five-second socket send/receive timeouts before connect.
     *       DNS resolution is not bounded by these socket timeouts.
     */
    bool Connect();
    /** @brief Release the descriptor and reset socket to -1.
     * @return True, including when close fails; inspect error for close failures.
     */
    bool Disconnect();
    /** @brief Perform a synchronous HTTP/1.1 request on a reusable connection.
     * @param method HTTP method, such as GET or PUT.
     * @param path Request target, such as /lights.
     * @param[out] response Response body; unchanged on failure.
     * @param body Optional JSON request body.
     * @return True for a complete 2xx response; false with error populated.
     * @note Requires Content-Length except for 204; rejects Transfer-Encoding.
     *       Reconnects when disconnected; transport/framing failures close the socket.
     */
    bool Request(const std::string& method, const std::string& path,
                 std::string& response, const std::string& body = "");
};

/**
 * @brief Schedule reads and publish changes through a caller-supplied callback.
 * @tparam T Device exposing Connect(), Disconnect(), error, and generic Read().
 * @note CommTCP uses a specialized Read() that fetches HTTP light states.
 *       Template definitions reside in the implementation with explicit instantiation.
 * @note Configure callbacks before Start(); access snapshots on the worker or
 *       while stopped. The lifecycle mutex does not serialize device operations.
 */
template<class T>
class DataChan {
public:
    T device; ///< Device held by value.
    const int timeout;             // Worker interval in milliseconds.

    Error error{NOERR, "", ""}; ///< Channel validation, device, or parsing diagnostic.
    /// Current and previous successful snapshots, keyed by light ID.
    std::map<std::string, nlohmann::json> cur_state, last_state;

    /** @brief Store the supplied device and validate the worker interval.
     * @param timeout Positive polling interval in milliseconds.
     * @param device Device value moved from the constructor parameter.
     */
    DataChan(int timeout, T device);
    /** @brief Stop/join the worker and close the legacy socket_ if present.
     * @note Does not disconnect device; stop and disconnect it explicitly.
     */
    ~DataChan();

    DataChan(const DataChan&) = delete;
    DataChan& operator=(const DataChan&) = delete;

    /** @brief Start the periodic worker; an already-running channel is accepted.
     * @return False for a nonpositive interval; true otherwise.
     * @note Call from the owner thread. Thread/mutex exceptions are not translated.
     */
    bool Start();
    /** @brief Request worker termination, wake its timer, and join it.
     * @note Call from the owner thread, never from Update. An active read or
     *       callback must finish before joining completes.
     */
    void Stop();

    /// @brief Connect the device and copy its diagnostic into error.
    bool Connect();
    /** @brief Disconnect the device and copy its diagnostic into error.
     * @note Does not stop the worker; call Stop() first when it is active.
     */
    void Disconnect();
    /** @brief Delegate to the device, or fetch states through the CommTCP specialization.
     * @return True on success; false on a reported read failure.
     * @note Invoke manually while stopped, or let the worker invoke it.
     */
    bool Read();
    /// @brief Reserved write interface; no implementation is supplied yet.
    bool Write();
    /** @brief Caller-supplied notification invoked after a read without an error.
     * @param channel This channel, including current and previous snapshots.
     * @note Runs on the worker thread during polling; assign before Start().
     */
    std::function<void(DataChan&)> Update;

private:
    int socket_ = -1; ///< Legacy channel descriptor; independent of device.socket.
    std::thread worker_;
    std::mutex mutex_; ///< Protects running_ and the timer wait, not network requests.
    std::condition_variable wake_;
    bool running_ = false;

    /// @brief Wait for each tick, read, and notify; publish an error and exit on failure.
    void Worker();
};

/**
 * @brief Fetch /lights and each detailed state, normalizing brightness to 0..100.
 * @return True after committing both snapshots; false on reported request/parse errors.
 * @note JSON is expected to contain light IDs and unsigned brightness in 0..255.
 *       Parse errors are translated; JSON access/type exceptions are not caught.
 *       Reported failures before commit leave both snapshots unchanged.
 */
template<>
bool DataChan<CommTCP>::Read();