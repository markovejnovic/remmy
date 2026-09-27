#ifndef CUTILS_IO_CONCURRENT_WRITER_HPP
#define CUTILS_IO_CONCURRENT_WRITER_HPP

#include <concepts>
#include <cstddef>
#include <expected>
#include <memory>
#include <string_view>
#include <system_error>
#include <utility>

#include "cutils/io/buffered_writer.hpp"
#include "cutils/io/mutex_writer.hpp"
#include "cutils/io/writer.hpp"

namespace cutils::io {

template <Writer W, class Allocator = std::allocator<char>>
class ConcurrentWriter {
  using Shared = MutexWriter<BufferedWriter<W, Allocator>>;

  class SharedRef {
   public:
    explicit constexpr SharedRef(Shared& shared) noexcept : shared_(&shared) {}

    [[nodiscard]] auto Write(std::string_view sv) noexcept
        -> std::expected<std::size_t, std::errc> {
      return shared_->Write(sv);
    }

    template <PieceRange R>
    [[nodiscard]] auto WriteMany(R&& pieces) noexcept
        -> std::expected<void, std::errc> {
      return shared_->WriteMany(std::forward<R>(pieces));
    }

    [[nodiscard]] auto Flush() noexcept -> std::expected<void, std::errc> {
      return {};
    }

   private:
    Shared* shared_;
  };

 public:
  struct Capacity {
    std::size_t shared;
    std::size_t handle;
  };

  class Handle {
   public:
    Handle(const Handle& other) : Handle(*other.owner_) {}
    Handle(Handle&&) noexcept = default;
    auto operator=(const Handle&) -> Handle& = delete;
    auto operator=(Handle&&) -> Handle& = delete;

    ~Handle() { (void)writer_.Flush(); }

    [[nodiscard]] auto Write(std::string_view sv) noexcept
        -> std::expected<std::size_t, std::errc> {
      return writer_.Write(sv);
    }

    template <PieceRange R>
    [[nodiscard]] auto WriteMany(R&& pieces) noexcept
        -> std::expected<void, std::errc> {
      return writer_.WriteMany(std::forward<R>(pieces));
    }

    [[nodiscard]] auto Flush() noexcept -> std::expected<void, std::errc> {
      return writer_.Flush();
    }

   private:
    friend ConcurrentWriter;

    explicit Handle(ConcurrentWriter& owner)
        : owner_(&owner),
          writer_(std::allocator_arg, owner.alloc_, owner.capacity_.handle,
                  owner.shared_) {}

    ConcurrentWriter* owner_;
    BufferedWriter<SharedRef, Allocator> writer_;
  };

  template <class... Args>
    requires std::default_initializable<Allocator> &&
                 std::constructible_from<W, Args...>
  explicit ConcurrentWriter(Capacity capacity, Args&&... args)
      : shared_(capacity.shared, std::forward<Args>(args)...),
        capacity_(capacity) {}

  template <class... Args>
    requires std::constructible_from<W, Args...>
  ConcurrentWriter(std::allocator_arg_t, const Allocator& alloc,
                   Capacity capacity, Args&&... args)
      : shared_(std::allocator_arg, alloc, capacity.shared,
                std::forward<Args>(args)...),
        capacity_(capacity),
        alloc_(alloc) {}

  ConcurrentWriter(const ConcurrentWriter&) = delete;
  ConcurrentWriter(ConcurrentWriter&&) = delete;
  auto operator=(const ConcurrentWriter&) -> ConcurrentWriter& = delete;
  auto operator=(ConcurrentWriter&&) -> ConcurrentWriter& = delete;

  ~ConcurrentWriter() { (void)shared_.Flush(); }

  [[nodiscard]] auto MakeHandle() -> Handle { return Handle(*this); }

  [[nodiscard]] auto Write(std::string_view sv) noexcept
      -> std::expected<std::size_t, std::errc> {
    return shared_.Write(sv);
  }

  template <PieceRange R>
  [[nodiscard]] auto WriteMany(R&& pieces) noexcept
      -> std::expected<void, std::errc> {
    return shared_.WriteMany(std::forward<R>(pieces));
  }

  [[nodiscard]] auto Flush() noexcept -> std::expected<void, std::errc> {
    return shared_.Flush();
  }

 private:
  Shared shared_;
  Capacity capacity_;
  [[no_unique_address]] Allocator alloc_{};
};

}  // namespace cutils::io

#endif  // CUTILS_IO_CONCURRENT_WRITER_HPP
