// See LICENSE in the repository root.

/// @brief A worker's stdout and stderr, held back while a prompt is open.
#ifndef REMMY_GATED_OUTPUT_HPP
#define REMMY_GATED_OUTPUT_HPP

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include "cutils/io/stderr_writer.hpp"
#include "cutils/io/stdout_writer.hpp"
#include "cutils/io/string_writer.hpp"
#include "cutils/io/writer.hpp"

namespace remmy {

/// @brief Writes to stdout and stderr through `Gate`; whatever the gate turns
///        away is kept, in order, and written once it lets this in again.
///
/// The worker never waits: a closed gate costs it a copy of the line. With
/// cutils::sync::NoGate every write goes straight through.
template <class Gate>
class GatedOutput {
  enum class Which : std::uint8_t { kOut, kErr };

 public:
  /// @brief A cutils::io::Writer for one of the two streams.
  class Stream {
   public:
    [[nodiscard]] auto Write(std::string_view sv) noexcept
        -> std::expected<std::size_t, std::errc> {
      return WriteMany(std::span{&sv, 1}).transform([&] { return sv.size(); });
    }

    template <cutils::io::PieceRange R>
    [[nodiscard]] auto WriteMany(R&& pieces) noexcept
        -> std::expected<void, std::errc> {
      return owner_->Emit(which_, std::forward<R>(pieces));
    }

    [[nodiscard]] static auto Flush() noexcept
        -> std::expected<void, std::errc> {
      return {};
    }

   private:
    friend GatedOutput;

    Stream(GatedOutput& owner, Which which) noexcept
        : owner_(&owner), which_(which) {}

    GatedOutput* owner_;
    Which which_;
  };

  GatedOutput(Gate& gate, cutils::io::StdoutWriter& out) noexcept
      : gate_(&gate), out_(&out) {}

  [[nodiscard]] auto Out() noexcept -> Stream { return {*this, Which::kOut}; }

  [[nodiscard]] auto Err() noexcept -> Stream { return {*this, Which::kErr}; }

  /// @brief Write what was held back, if the gate lets this in now.
  void Release() noexcept {
    if (held_.empty() || !gate_->TryEnter()) {
      return;
    }
    WriteHeld();
    gate_->Leave();
  }

 private:
  struct Held {
    Which which;
    std::string text;
  };

  template <cutils::io::PieceRange R>
  [[nodiscard]] auto Emit(Which which, R&& pieces) noexcept
      -> std::expected<void, std::errc> {
    if (!gate_->TryEnter()) {
      std::string text;
      cutils::io::StringWriter into(text);
      std::ignore = into.WriteMany(pieces);
      held_.push_back(Held{.which = which, .text = std::move(text)});
      return {};
    }

    WriteHeld();
    auto written =
        which == Which::kOut
            ? out_->WriteMany(std::forward<R>(pieces))
            : cutils::io::stderr_writer.WriteMany(std::forward<R>(pieces));
    gate_->Leave();
    return written;
  }

  void WriteHeld() noexcept {
    for (const Held& held : held_) {
      const std::string_view text = held.text;
      std::ignore = held.which == Which::kOut
                        ? out_->Write(text)
                        : cutils::io::stderr_writer.Write(text);
    }
    held_.clear();
  }

  Gate* gate_;
  cutils::io::StdoutWriter* out_;
  std::vector<Held> held_;
};

}  // namespace remmy

#endif
