/**
 * @file DataChan.cpp
 * @brief Data-channel lifecycle, HTTP light-state specialization, and TCP requests.
 */
#include "DataChan.h"

#include <chrono>
#include <cerrno>
#include <cstring>
#include <memory>
#include <utility>

#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

/**
 * @brief Initialize device before the polling interval, in member declaration order.
 */
template<class T>
DataChan<T>::DataChan(int timeout, T device)
    : device(std::move(device)), timeout(timeout)
{
    if (timeout <= 0)
        error = {ERR, "Worker timeout must be positive", "DataChan::timeout"};
}

/**
 * @brief Join the worker before releasing the legacy descriptor.
 */
template<class T>
DataChan<T>::~DataChan()
{
    Stop();
    if (socket_ != -1)
        ::close(socket_);
}

/**
 * @brief Delegate connection setup and propagate the device diagnostic.
 */
template<class T>
bool DataChan<T>::Connect()
{
    const bool ok = device.Connect();
    error = device.error;
    return ok;
}

/**
 * @brief Delegate disconnection; worker lifecycle is managed separately.
 */
template<class T>
void DataChan<T>::Disconnect()
{
    device.Disconnect();
    error = device.error;
}

/**
 * @brief Create the worker while holding the lifecycle mutex.
 */
template<class T>
bool DataChan<T>::Start()
{
    std::lock_guard<std::mutex> lock(mutex_);   // We should add `try/catch` around this to handle exceptions safely.
    if (running_)
        return true;
    if (timeout <= 0) {
        error = {ERR, "Worker timeout must be positive", "DataChan::Start"};
        return false; 
    }

    error = {NOERR, "", ""};
    running_ = true;
    worker_ = std::thread(&DataChan::Worker, this);
    return true;
}

/**
 * @brief Clear running_, interrupt the timer wait, and join the worker.
 */
template<class T>
void DataChan<T>::Stop()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
    }
    wake_.notify_one();
    if (worker_.joinable())
        worker_.join();
}

/**
 * @brief Poll on a steady-clock cadence, skipping ticks missed during reads/updates.
 * @note Releases the mutex around reads and callbacks. A reported error exits
 *       the worker without clearing running_; Stop() is needed to reset/join it.
 */
template<class T>
void DataChan<T>::Worker()
{
    using Clock = std::chrono::steady_clock;
    const auto interval = std::chrono::milliseconds(timeout);
    auto next = Clock::now() + interval;
    std::unique_lock<std::mutex> lock(mutex_);

    while (running_) {
        if (wake_.wait_until(lock, next, [this] { return !running_; }))
            break;

        lock.unlock();
        Read();
        if (error.level > NOERR) { error.Publish(); return; }
        if (Update) Update(*this);
        lock.lock();

        // Keep the timer cadence; skip missed ticks if Update runs too long.
        const auto now = Clock::now();
        next += interval * ((now - next) / interval + 1);
    }
}

/**
 * @brief Generic read delegates to T::Read() and copies T::error.
 */
template<class T>
bool DataChan<T>::Read()
{
    const bool ok = device.Read();
    error = device.error;
    return ok;
}

/**
 * @brief Build a complete light snapshot before replacing current/previous maps.
 * @note Round brightness from the server byte range to integer percent.
 */
template<>
bool DataChan<CommTCP>::Read()
{
    std::string incoming_json;
    nlohmann::json parser, state;
    if (!device.Request("GET", "/lights", incoming_json))
        {error = device.error; return false;}
    // const auto end = incoming_json.find_last_not_of(" \t\r\n");
    // if (end != std::string::npos)  incoming_json.erase(end-2);

    try {
        parser = nlohmann::json::parse(incoming_json, nullptr);
    } catch (const nlohmann::json::parse_error& e) {
        error = {ERR, e.what(),
                "GET /lights: byte " + std::to_string(e.byte) +
                ", error " + std::to_string(e.id)};
        return false;
    }

    std::map<std::string, nlohmann::json> states;
    for (const auto& light : parser) {
        const std::string id = light.value("id", "");
        if (id.empty()) {
            error = {ERR, "Light is missing its id", "GET /lights"};
            return false;
        }

        std::string response;
        if (!device.Request("GET", "/lights/" + id, response))
            {error = device.error; return false;}

        try {
            state = nlohmann::json::parse(response, nullptr);
        } catch (const nlohmann::json::parse_error& e) {
            error = {ERR, e.what(),
                    "GET /lights: byte " + std::to_string(e.byte) +
                    ", error " + std::to_string(e.id)};
            return false;
        }

        const unsigned brightness = state.at("brightness").get<unsigned>();
        state["brightness"] = (brightness * 100u + 127u) / 255u;

        states[id] = std::move(state);
    }
    
    last_state = std::move(cur_state);
    cur_state = std::move(states);
    return true;
}

/**
 * @brief Emit generic DataChan members for the supported CommTCP device type.
 */
template class DataChan<CommTCP>;

#ifdef _WIN32
#error "CommTCP currently supports POSIX sockets only; a Windows Winsock implementation is required."
#else // POSIX sockets implementation
/**
 * @brief Resolve IPv4/IPv6 candidates, configure timeouts, and connect.
 */
bool CommTCP::Connect()
{
    error = {NOERR, "", ""};
    if (socket != -1)
        return true;

    const std::string endpoint = host + ":" + std::to_string(port);
    if (host.empty() || port < 1 || port > 65535) {
        error = {ERR, "Host must be nonempty and port must be in 1..65535", endpoint};
        return false;
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* result = nullptr;
    const std::string service = std::to_string(port);
    const int status = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &result);
    if (status != 0) {
        const int saved_errno = errno;
        error = {ERR, status == EAI_SYSTEM ? std::strerror(saved_errno) : ::gai_strerror(status),
                 "getaddrinfo: " + endpoint + " (code " + std::to_string(status) + ")"};
        return false;
    }
    std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> addresses(result, &::freeaddrinfo);

    for (const addrinfo* address = addresses.get(); address; address = address->ai_next) {
        const int fd = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd == -1) {
            const int saved_errno = errno;
            error = {ERR, std::strerror(saved_errno),
                     "socket: " + endpoint + " (errno " + std::to_string(saved_errno) + ")"};
            continue;
        }

        // Set socket timeouts here.
        const timeval io_timeout{5, 0};  // Five-second send/receive timeout.
        if (::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &io_timeout, sizeof(io_timeout)) == -1 ||
            ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &io_timeout, sizeof(io_timeout)) == -1) {
            const int saved_errno = errno;
            ::close(fd);
            error = {ERR, std::strerror(saved_errno),
                    "setsockopt: " + endpoint + " (errno " + std::to_string(saved_errno) + ")"};
            continue;
        }
        
        if (::connect(fd, address->ai_addr, address->ai_addrlen) == 0) {
            socket = fd;
            error = {NOERR, "", ""};
            return true;
        }
        const int saved_errno = errno;
        ::close(fd);
        error = {ERR, std::strerror(saved_errno),
                 "connect: " + endpoint + " (errno " + std::to_string(saved_errno) + ")"};
    }
    if (error.level == NOERR)
        error = {ERR, "No TCP addresses found", endpoint};
    return false;
}

/**
 * @brief Reset the descriptor before close; report close errors through error.
 */
bool CommTCP::Disconnect()
{
    error = {NOERR, "", ""};
    const int fd = std::exchange(socket, -1);
    if (fd != -1 && ::close(fd) == -1) {
        const int saved_errno = errno;
        error = {ERR, std::strerror(saved_errno),
                 "close: " + host + ":" + std::to_string(port) +
                 " (errno " + std::to_string(saved_errno) + ")"};
    }
    return true;
}

/**
 * @brief Send the complete request and receive exactly the framed response body.
 * @note EINTR retries are local to each I/O operation. Successful requests keep
 *       the socket unless the server specifies Connection: close.
 */
bool CommTCP::Request(const std::string& method, const std::string& path,
                      std::string& response, const std::string& body)
{
    error = {NOERR, "", ""};
    if (socket < 0 && !Connect())
        return false;
    auto fail = [&](const std::string& msg) {
        ::close(std::exchange(socket, -1));
        error = {ERR, msg, method + " " + path};
        return false;
    };

    const std::string authority = (host.find(':') != std::string::npos ? "[" + host + "]" : host) +
                                  ":" + std::to_string(port);
    const std::string request = method + " " + path + " HTTP/1.1\r\nHost: " + authority +
        "\r\nAccept: application/json\r\nContent-Type: application/json\r\nContent-Length: " +
        std::to_string(body.size()) + "\r\n\r\n" + body;
    for (std::size_t sent = 0; sent < request.size();) {
        const auto count = ::send(socket, request.data() + sent, request.size() - sent, MSG_NOSIGNAL);
        if (count == -1 && errno == EINTR) continue;
        if (count <= 0) return fail(std::string("send: ") + std::strerror(count == 0 ? EPIPE : errno));
        sent += static_cast<std::size_t>(count);
    }

    // Read headers without consuming body bytes; no second body buffer needed.
    std::string head;
    while (head.size() < 4 || head.compare(head.size() - 4, 4, "\r\n\r\n") != 0) {
        char ch;
        const auto count = ::recv(socket, &ch, 1, 0);
        if (count == -1 && errno == EINTR) continue;
        if (count <= 0) return fail(count == 0 ? "Connection closed during headers" :
                                   std::string("recv: ") + std::strerror(errno));
        head += ch;
    }
    std::istringstream headers(head);
    std::string version, line;
    int status = 0;
    if (!(headers >> version >> status) || version != "HTTP/1.1")
        return fail("Invalid HTTP/1.1 status line");
    std::getline(headers, line);
    std::size_t length = 0;
    bool has_length = false, close_after = false;
    while (std::getline(headers, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        for (char& ch : line) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        const auto name = line.substr(0, colon);
        if (name == "content-length") {
            std::istringstream value(line.substr(colon + 1));
            if (!(value >> length) || !(value >> std::ws).eof())
                return fail("Invalid Content-Length");
            has_length = true;
        }
        if (name == "transfer-encoding") return fail("Expected Content-Length framing");
        if (name == "connection" && line.find("close", colon) != std::string::npos) close_after = true;
    }
    if (status == 204) length = 0;
    else if (!has_length) return fail("Missing Content-Length");

    std::string incoming(length, '\0');
    for (std::size_t received = 0; received < length;) {
        const auto count = ::recv(socket, incoming.data() + received, length - received, 0);
        if (count == -1 && errno == EINTR) continue;
        if (count <= 0) return fail(count == 0 ? "Connection closed during body" :
                                   std::string("recv: ") + std::strerror(errno));
        received += static_cast<std::size_t>(count);
    }
    if (close_after && ::close(std::exchange(socket, -1)) == -1)
        return fail(std::string("close: ") + std::strerror(errno));
    if (status < 200 || status >= 300) {
        error = {ERR, "HTTP status " + std::to_string(status), method + " " + path};
        return false;
    }
    response = std::move(incoming);
    return true;
}

#endif
