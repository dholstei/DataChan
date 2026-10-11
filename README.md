# DataChan programmer guide

`DataChan<T>` pairs a communication device with a periodic worker and a caller-defined update callback. The included `CommTCP` device performs synchronous HTTP/1.1 requests. Its `DataChan<CommTCP>::Read()` specialization monitors lights through a local service, retaining successful snapshots and reporting changes.

## Source layout

| File | Responsibility |
| --- | --- |
| `DataChan.h` | Device interface, channel template, and HTTP read specialization declaration |
| `DataChan.cpp` | Worker lifecycle, template definitions/instantiation, TCP requests, and light-state reads |
| `monitor.h` | Monitor dependencies, run flag, and SIGINT handler |
| `monitor.cpp` | Device configuration, console update callback, and application lifecycle |
| `Error.h`, `Error.cpp` | Structured diagnostics and replaceable error publisher |

## Build

Use a C++17 compiler, POSIX sockets, pthread support, and the header-only `nlohmann/json.hpp` library. No JSON library needs to be linked.

Build the executable with the existing Makefile:

```bash
make DEBUG=-g monitor
./monitor
```

An equivalent direct build is:

```bash
g++ -g -std=c++17 -pthread monitor.cpp DataChan.cpp Error.cpp -o monitor
```

The static archive must include both `DataChan.o` and `Error.o`; `Error.cpp` supplies the definition of `Error::Publisher`. Building `monitor` does not automatically build the separate shared-library target.

```make
libDataChan.a: DataChan.o Error.o
monitor.o DataChan.o: DataChan.h Error.h
```

The existing recipes can remain in place. Add other shared headers to dependencies as they are introduced. A class-layout change requires every consuming object to be rebuilt; stale objects can access the wrong member offsets. Use `make clean` before rebuilding if dependencies have not yet been corrected.

The communication implementation currently targets Linux/POSIX. A Windows build needs Winsock support.

## Device and channel responsibilities

`CommTCP` holds the socket descriptor, host, port, and network diagnostic. `DataChan<T>` holds a device value, a positive polling interval in milliseconds, channel diagnostics, snapshots, and worker synchronization.

```cpp
CommTCP device("localhost", 8080);
DataChan<CommTCP> LightChan(100, device);
```

The constructor receives a device by value and moves that parameter into the member. The caller's device is copied in this example. Do not copy a connected `CommTCP`: its default copy/move operations copy the integer descriptor rather than transfer exclusive socket ownership. Configure the device before connecting the channel.

The channel itself is noncopyable. In a constructor whose parameter is also named `device`, `device` refers to the parameter and `this->device` refers to the member. After moving the parameter, inspect the member's properties.

## Public interfaces

| Interface | Behavior |
| --- | --- |
| `CommTCP::Connect()` | Resolve host addresses, configure socket timeouts, and connect; return false with `error` on failure |
| `CommTCP::Disconnect()` | Reset the descriptor to `-1` and close it; inspect `error`, because the current implementation returns true even if close fails |
| `CommTCP::Request(method, path, response, body)` | Perform an HTTP request; return true for a complete 2xx response |
| `DataChan<T>::Connect()` | Delegate to the device and copy its diagnostic into the channel |
| `DataChan<T>::Disconnect()` | Delegate to the device and copy its diagnostic; does not stop the worker |
| `DataChan<T>::Start()` | Start polling; reject a nonpositive interval and accept an already-running channel |
| `DataChan<T>::Stop()` | Clear the run flag, wake the timer, and join the worker |
| `DataChan<T>::Read()` | Generic device delegation or a specialized application read |
| `DataChan<T>::Update` | Optional `std::function<void(DataChan&)>` assigned by the caller |
| `DataChan<T>::Write()` | Declared placeholder; not implemented |

## HTTP requests

```cpp
std::string response;
if (!LightChan.device.Request("GET", "/lights", response))
    LightChan.device.error.Publish();
```

PUT uses the same method with a body. The following illustrates the call shape; accepted fields depend on the server:

```cpp
std::string body = R"({"on":true})";
LightChan.device.Request("PUT", "/lights/light-id", response, body);
```

Requests send HTTP/1.1, a Host header, JSON content/accept headers, and a request Content-Length. A disconnected device connects on demand. Requests are not automatically replayed after a failed exchange.

The implementation reads the response headers through `\r\n\r\n`, obtains Content-Length, allocates a string of that size, and receives directly into it. This framing permits connection reuse without waiting for the peer to close. HTTP 204 is handled as an empty body. Transfer-Encoding, including chunked responses, is rejected; this is a small Content-Length-based client, not a general HTTP implementation.

Transport/framing errors close the socket; a complete non-2xx response sets an HTTP error after consuming its body. A server's `Connection: close` header causes the client to release the connection.

`Connect()` configures five-second send/receive socket timeouts. These are separate from `DataChan::timeout`, which is the polling interval. They are per blocking operation, not an overall deadline for the complete request. DNS resolution is not bounded by the socket timeouts.

## Light-state snapshots

The HTTP specialization requests `/lights`, extracts each light's ID, then requests `/lights/<id>` for its detailed state. Expected detail fields include `id`, `name`, `room`, `on`, and unsigned byte-range `brightness`.

Brightness is rounded to an integer percentage before storing the state:

```cpp
const unsigned brightness = state.at("brightness").get<unsigned>();
state["brightness"] = (brightness * 100u + 127u) / 255u;
```

Thus 0 becomes 0, 128 becomes 50, and 255 becomes 100. Comparisons and output use the normalized value; raw changes that round to the same percentage do not produce a brightness delta.

Snapshots are keyed by ID:

```cpp
std::map<std::string, nlohmann::json> cur_state, last_state;
```

Reads accumulate states in a temporary map. Only after all requests and parsing succeed are snapshots committed:

```cpp
last_state = std::move(cur_state);
cur_state = std::move(states);
```

Reported failures before commit preserve both snapshots. The first successful read has an empty previous map and therefore reports every light and its complete state. Subsequent comparisons identify additions, changes, and removals without depending on the order returned by `/lights`.

## Update callback and manual testing

For GUI, hardware, database, and publishing integration, see [Configure application interfaces in the caller](#configure-application-interfaces-in-the-caller). Defining callbacks in `main()` keeps those concrete dependencies out of the channel headers.

Assign the callback before starting the worker:

```cpp
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
```

JSON equality compares values recursively. Object-key ordering and source whitespace do not affect the comparison. The callback prints complete new/changed objects; removal notices use the previous name and map ID. Initial states can establish automation conditions, with later transitions driving follow-on actions.

Before enabling polling, invoke reads and updates manually while the worker is stopped:

```cpp
if (LightChan.Read()) {
    if (LightChan.Update) LightChan.Update(LightChan);
} else {
    LightChan.error.Publish();
}
```

## Polling and shutdown

The worker waits one interval before its first read. It uses a steady-clock timer and skips ticks missed while reads or callbacks execute. The lifecycle mutex is released during both operations. When a read leaves an error above NOERR, the worker publishes it and exits; otherwise it invokes Update if assigned.

Call Start/Stop from the owner thread. Do not call `Stop()` or destroy the channel from Update: joining the current worker is invalid. `Stop()` wakes a timer wait, but an active network operation or callback must finish before the join completes.

One worker may use the device without another request mutex if all communication is confined to it. Do not call Request, Read, or Disconnect concurrently from main. Independent devices may connect to the same host/port; serialization concerns a particular socket, not the server address.

For graceful shutdown, stop before disconnecting:

```cpp
LightChan.Stop();
LightChan.Disconnect();
LightChan.error.Publish();
```

In the supplied source, Disconnect does not stop the worker, and the channel destructor stops it but only closes the legacy `socket_` member, not `device.socket`. Explicit device disconnection is therefore required. The supplied monitor disconnects before calling `Stop()`; reverse that order when adopting the lifecycle above.

The monitor header defines a SIGINT handler and a `volatile std::sig_atomic_t` flag, but the supplied main does not register the handler. Register it before starting the worker:

```cpp
std::signal(SIGINT, signal_handler);
```

Main can then sleep between flag checks, avoiding a CPU-consuming busy loop:

```cpp
while (keep_running)
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
```

This notices the flag after sleeping; SIGINT does not cancel `sleep_for()`. The handler should only set the flag. The header's flag and handler definitions currently assume a single including translation unit; use extern declarations with definitions in monitor.cpp if other files include it.

## Diagnostics

`Error` carries `level`, `msg`, and `data`. Network failures come from resolver/socket diagnostics; request context is retained in data. Channel wrappers copy device diagnostics to their own error.

```cpp
LightChan.error.Publish();
```

Publish is an instance method using a shared, replaceable method, `Error::Publisher`; its default handler prints through printf. Configure a replacement before starting workers. A GUI publisher must dispatch copied data to the GUI thread rather than retain a borrowed reference.

JSON parsing uses exceptions locally to retain the parser diagnostic:

```cpp
try {
    parser = nlohmann::json::parse(incoming_json);
} catch (const nlohmann::json::parse_error& e) {
    error = {ERR, e.what(), "JSON byte " + std::to_string(e.byte)};
    return false;
}
```

Passing false as the allow-exceptions argument instead returns a discarded value and suppresses the parse exception. The current specialization catches parse errors, but does not catch missing-field/type errors from value(), at(), or get(). It assumes the documented response schema. Mutex/thread exceptions are also not translated to Error.

The worker's error exit currently leaves `running_` set until `Stop()` resets it. Call `Stop()` to join/reset before restarting after such a failure. Successful specialized reads do not explicitly clear an earlier channel error, so clear or revise that behavior when implementing recovery. These are current implementation details, not automatic recovery features.

## Adding another device type

The generic `Read` delegates to `device.Read()` and copies `device.error`. A new device must provide that interface along with `Connect` and `Disconnect`. Application-specific reads can specialize a single member, as the HTTP light monitor does.

Declare a specialization after the class in the header:

```cpp
template<>
bool DataChan<CommTCP>::Read();
```

Define generic methods and specialized members in the implementation, then explicitly instantiate the supported class once:

```cpp
template class DataChan<CommTCP>;
```

With definitions confined to `DataChan.cpp`, add an explicit instantiation for each additional supported device type. Alternatively, put template definitions in an included .tpp file to make them available for arbitrary types. The template schedules work; each device or specialization supplies the protocol and application behavior.

## Dynamic polymorphism and DLL-only drivers

The existing `DataChan<T>` design selects a concrete device type at compile time. Runtime-selected devices, including DLL-only closed-source drivers, can use an explicit specialization of the same template:

```cpp
template<>
class DataChan<void*> {
public:
    void* device = nullptr;
    const int timeout;

    // Runtime dispatch table, ownership policy, and worker implementation.
};
```

This is a design option, not an implemented driver interface. `DataChan<CommTCP>` and `DataChan<void*>` are distinct types and can coexist. A separate non-template `class DataChan` cannot coexist with the template under the same name in the same namespace. Function overloads such as `XPath()` and `XPath<T>()` follow different rules.

A void pointer only stores an opaque device handle. Runtime polymorphic behavior requires an interface or function table that dispatches operations to the loaded driver. For example, a C-compatible table can contain:

```cpp
struct DeviceAPI {
    void* device;
    bool (*Connect)(void*);
    bool (*Disconnect)(void*);
    bool (*Read)(void*);
    void (*Destroy)(void*);
    // Add error retrieval and other operations required by the driver contract.
};
```

An adapter can populate this table from a vendor's exported API; the vendor DLL does not need to implement the DataChan C++ interface. Closed-source status alone does not require this approach, but an opaque-handle interface is useful when source and concrete device types are unavailable to the consumer.

Define who owns the handle and which function releases it. Use the vendor-provided destroy operation rather than deleting an opaque pointer. Stop and join the worker before releasing the device, and keep the library loaded until all its calls and destruction have completed. Match the vendor's calling convention, architecture, and ABI when resolving DLL functions. The example table is illustrative; it is not a complete portable plugin ABI.

Declare the full specialization before any use that would implicitly instantiate `DataChan<void*>`. A full class specialization supplies its own members and method definitions: it does not automatically reuse the generic template's worker implementation. Shared lifecycle code can be factored into a common helper if both forms are implemented later. Methods of the fully specialized class are ordinary methods and do not need a `template<class T>` prefix.

Dynamic loading does not itself make the POSIX CommTCP implementation Windows-compatible. A Windows runtime driver still needs an appropriate Windows implementation or vendor API, while the existing Linux build can continue using its concrete device types.

## Configure application interfaces in the caller

The calling `main()` can assemble the GUI, hardware-device, database, and publishing interfaces, then connect them through callbacks. Keep their concrete includes in the calling implementation file rather than introducing them into `DataChan.h` or device-independent headers.

For example, `Update` can capture a publisher supplied by the application:

```cpp
LightsChan.Update = [&publisher](DataChan<CommTCP>& channel) {
    // Compare snapshots and pass changes to the application's publisher.
    publisher.PublishChanges(channel.cur_state, channel.last_state);
};
```

`PublishChanges` is an illustrative application interface, not a method provided by this library. The channel only needs the callback signature; it does not need the publisher's class definition. The same convention can connect database storage, hardware actions, or GUI notifications without coupling the worker to those implementations.

This arrangement reduces transitive `#include` dependencies, naming/macro conflicts, and opportunities for circular dependencies. Changes to an application's concrete interface can remain in its own implementation and the caller. Types appearing directly in a public signature or stored by value still require the appropriate declarations or definitions; callback configuration does not eliminate those language requirements.

Objects captured by reference must outlive all callback invocations. Stop and join the worker before destroying those objects. Update runs on the worker thread, so a GUI callback must dispatch to the GUI thread, and database/publishing operations must follow their own thread-access requirements. Configure the shared `Error::Publisher` in the caller as well; if it captures an object, reset the callback before that object is destroyed.