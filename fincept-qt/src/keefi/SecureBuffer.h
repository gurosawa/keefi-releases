#pragma once

#include <QString>
#include <cstddef>
#include <map>

namespace keefi {

// Move-only, guarded and explicitly locked storage. Allocation/locking failure
// throws std::runtime_error; callers must refuse secret use rather than downgrade.
class SecureBuffer final {
  public:
    SecureBuffer() = default;
    explicit SecureBuffer(std::size_t size);
    SecureBuffer(const void* bytes, std::size_t size);
    ~SecureBuffer();
    SecureBuffer(SecureBuffer&& other) noexcept;
    SecureBuffer& operator=(SecureBuffer&& other) noexcept;
    SecureBuffer(const SecureBuffer&) = delete;
    SecureBuffer& operator=(const SecureBuffer&) = delete;

    static SecureBuffer fromUtf8(const QString& text);
    unsigned char* data() noexcept { return data_; }
    const unsigned char* data() const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }
    void clear() noexcept;

  private:
    unsigned char* data_ = nullptr;
    std::size_t size_ = 0;
};

using CredentialSet = std::map<QString, SecureBuffer>;

} // namespace keefi
