#include "Error.h"

#include <cstring>

std::function<void(const Error&)> Error::Publisher = [](const Error& error) {
    std::printf("%s: %s\nDATA: %s\n", LvlStr(error.level).c_str(),
                error.msg.c_str(), error.data.c_str());
};

char* CError::Copy(const char* source)
{
    if (!source)
        return nullptr;

    const std::size_t size = std::strlen(source) + 1;
    char* destination = new char[size];
    std::memcpy(destination, source, size);
    return destination;
}

CError::CError(lvl level, const char* msg, const char* data)
    : level(level), msg(Copy(msg)), data(Copy(data))
{
}

CError::CError(const Error& source)
    : CError(source.level, source.msg.c_str(), source.data.c_str())
{
}

CError::~CError()
{
    delete[] msg;
    delete[] data;
}

CError* Error::ToCError() const
{
    return new CError(*this);
}

extern "C" void FreeCError(CError* error)
{
    delete error;
}