#pragma once

#include <cstdio>
#include <functional>
#include <string>

enum lvl : int {NOERR, INFO, WARN, ERR};

struct CError;

struct Error {
    lvl level;
    std::string msg;
    std::string data;

    /// Replace once to route errors to a GUI or another output destination.
    static std::function<void(const Error&)> Publisher;
    void Publish() const {
        if (level > NOERR && Publisher)
            Publisher(*this);
    }

    /// Return an independently owned C representation of this error.
    CError* ToCError() const;
};

using ErrorPtr = Error*;

/// Plain-layout error exposed across the shared-library boundary.
struct CError {
    lvl level;
    char* msg;
    char* data;

    CError(lvl level, const char* msg, const char* data);
    CError(const Error& source);
    ~CError();

    CError(const CError&) = delete;
    CError& operator=(const CError&) = delete;

private:
    /// Allocate an independent NUL-terminated copy for foreign callers.
    static char* Copy(const char* source);
};

typedef CError* CErrorPtr;

inline std::string LvlStr(lvl level)
{
    switch (level) {
        case NOERR: return "NOERR";
        case INFO:  return "INFO";
        case WARN:  return "WARN";
        case ERR:   return "ERROR";
        default:    return "UNKNOWN";
    }
}

inline bool IsErr(const Error* error)
{
    return error && error->level > NOERR;
}

inline std::string MsgErr(const Error* error)
{
    return error ? error->msg : "";
}

#define MSG_ERR(error) do { \
    const Error* error_ptr_ = (error); \
    if (error_ptr_) error_ptr_->Publish(); \
} while (0)

// Foreign callers cannot invoke the C++ destructor directly.  Only the
// exported function needs C linkage; CError itself does not have linkage.
extern "C" void FreeCError(CError* error);