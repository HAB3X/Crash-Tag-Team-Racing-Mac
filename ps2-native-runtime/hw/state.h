#pragma once

// The hardware's state written out and read back (capture.h's recordings): plain bytes of the registers and memories, for the
// same build to read

#include "common.h"

#include <cstring>
#include <type_traits>
#include <vector>

namespace NativeGraphics
{
class StateWriter
{
public:
    explicit StateWriter(std::vector<u8>& out) : out_(out) {}

    void Bytes(const void* data, size_t size)
    {
        const u8* at = static_cast<const u8*>(data);
        out_.insert(out_.end(), at, at + size);
    }

    template <typename T>
    void Put(const T& value)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        Bytes(&value, sizeof(T));
    }

    template <typename T>
    void PutVector(const std::vector<T>& values)
    {
        Put(static_cast<u64>(values.size()));
        Bytes(values.data(), values.size() * sizeof(T));
    }

private:
    std::vector<u8>& out_;
};

class StateReader
{
public:
    StateReader(const u8* data, size_t size) : at_(data), end_(data + size) {}

    void Bytes(void* data, size_t size)
    {
        if (static_cast<size_t>(end_ - at_) < size)
        {
            failed_ = true;
            return;
        }

        std::memcpy(data, at_, size);
        at_ += size;
    }

    template <typename T>
    void Get(T& value)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        Bytes(&value, sizeof(T));
    }

    template <typename T>
    void GetVector(std::vector<T>& values)
    {
        u64 size = 0;
        Get(size);
        if (failed_ || size * sizeof(T) > static_cast<size_t>(end_ - at_))
        {
            failed_ = true;
            return;
        }

        values.resize(size);
        Bytes(values.data(), size * sizeof(T));
    }

    bool failed() const { return failed_; }

private:
    const u8* at_;
    const u8* end_;
    bool failed_ = false;
};

// One list of a class's fields for both directions: io.Field(member), io.Vector(member), io.Bytes(pointer, size)
struct Saver
{
    StateWriter& writer;
    static constexpr bool Loading = false;
    template <typename T>
    void Field(T& value) { writer.Put(value); }
    template <typename T>
    void Vector(std::vector<T>& values) { writer.PutVector(values); }
    void Bytes(void* data, size_t size) { writer.Bytes(data, size); }
};

struct Loader
{
    StateReader& reader;
    static constexpr bool Loading = true;
    template <typename T>
    void Field(T& value) { reader.Get(value); }
    template <typename T>
    void Vector(std::vector<T>& values) { reader.GetVector(values); }
    void Bytes(void* data, size_t size) { reader.Bytes(data, size); }
};
}
