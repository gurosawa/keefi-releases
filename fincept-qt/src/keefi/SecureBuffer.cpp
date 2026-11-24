#include "keefi/SecureBuffer.h"
#include <QByteArray>
#include <sodium.h>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace keefi {

SecureBuffer::SecureBuffer(std::size_t size) : size_(size) {
    if (sodium_init() < 0)
        throw std::runtime_error("SECURE_MEMORY_INIT_FAILED");
    if (!size)
        return;
    data_ = static_cast<unsigned char*>(sodium_malloc(size));
    if (!data_)
        throw std::runtime_error("SECURE_MEMORY_ALLOCATION_FAILED");
    // sodium_malloc alone does not report an mlock failure.
    if (sodium_mlock(data_, size) != 0) {
        sodium_free(data_);
        data_ = nullptr;
        size_ = 0;
        throw std::runtime_error("SECURE_MEMORY_LOCK_FAILED");
    }
    sodium_memzero(data_, size_);
}

SecureBuffer::SecureBuffer(const void* bytes, std::size_t size) : SecureBuffer(size) {
    if (size)
        std::memcpy(data_, bytes, size);
}

SecureBuffer::~SecureBuffer() { clear(); }

SecureBuffer::SecureBuffer(SecureBuffer&& other) noexcept
    : data_(std::exchange(other.data_, nullptr)), size_(std::exchange(other.size_, 0)) {}

SecureBuffer& SecureBuffer::operator=(SecureBuffer&& other) noexcept {
    if (this != &other) {
        clear();
        data_ = std::exchange(other.data_, nullptr);
        size_ = std::exchange(other.size_, 0);
    }
    return *this;
}

void SecureBuffer::clear() noexcept {
    if (data_) {
        sodium_memzero(data_, size_);
        sodium_free(data_);
    }
    data_ = nullptr;
    size_ = 0;
}

SecureBuffer SecureBuffer::fromUtf8(const QString& text) {
    QByteArray bytes = text.toUtf8();
    try {
        SecureBuffer result(bytes.constData(), static_cast<std::size_t>(bytes.size()));
        sodium_memzero(bytes.data(), static_cast<std::size_t>(bytes.size()));
        return result;
    } catch (...) {
        sodium_memzero(bytes.data(), static_cast<std::size_t>(bytes.size()));
        throw;
    }
}

} // namespace keefi
