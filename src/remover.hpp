// See LICENSE in the repository root.

/// @brief What the workers and the prompter share: removing an emptied
///        directory chain and reporting the outcome as BSD rm does.
#ifndef REMMY_REMOVER_HPP
#define REMMY_REMOVER_HPP

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "cli.hpp"
#include "cutils/io/print.hpp"
#include "cutils/io/warn.hpp"
#include "dir_node.hpp"
#include "gated_output.hpp"
#include "request.hpp"

namespace remmy {

/// @brief Removes and reports for one thread, under `Policy`.
///
/// Each thread owns its Remover: the path buffer and the failure count are
/// per thread.
template <class Policy>
class Remover {
 public:
  using Output = GatedOutput<typename Policy::Gate>;

  /// @param requests Where questions go; null exactly when Policy never asks.
  /// @note `cli` must outlive this.
  Remover(const Cli& cli, Policy policy, Output output,
          RequestList* requests) noexcept
      : cli_(&cli),
        policy_(policy),
        output_(std::move(output)),
        requests_(requests) {}

  [[nodiscard]] auto Failures() const noexcept -> std::size_t {
    return failures_;
  }

  /// @brief The command line this removes for.
  [[nodiscard]] auto Command() const noexcept -> const Cli& { return *cli_; }

  [[nodiscard]] auto Out() noexcept -> Output& { return output_; }

  [[nodiscard]] auto Requests() const noexcept -> RequestList& {
    return *requests_;
  }

  /// @brief A buffer for building paths, reused across calls.
  [[nodiscard]] auto Scratch() noexcept -> std::string& { return scratch_; }

  /// @brief warn(3) about `path` and count it as a failure.
  void Fail(std::string_view path, int error) noexcept {
    ++failures_;
    auto err = output_.Err();
    std::ignore = cutils::io::Warn(err, cli_->CommandName(), error, "{}", path);
  }

  /// @brief Fail for the entry `name` of the directory at `dir`.
  void Fail(std::string_view dir, std::string_view name, int error) noexcept {
    ++failures_;
    auto err = output_.Err();
    std::ignore =
        cutils::io::Warn(err, cli_->CommandName(), error, "{}/{}", dir, name);
  }

  /// @brief Under -v, print the removed path when `status` is 0; returns
  ///        `status`.
  template <class... Args>
  auto Logged(int status,
              cutils::io::FormatString<std::type_identity_t<Args>...> fmt,
              const Args&... args) noexcept -> int {
    if (status == 0 && cli_->Options().verbose) {
      auto out = output_.Out();
      std::ignore = cutils::io::PrintLn(out, fmt, args...);
    }
    return status;
  }

  /// @brief rmdir the emptied `node`, reporting a failure other than ENOENT.
  void RemoveDir(const DirNode* node) noexcept {
    if (RemoveEmptyLogged(node) != 0 && errno != ENOENT) {
      Fail(scratch_, errno);
    }
  }

  /// @brief RemoveEmpty, under -v printing the path once removed. Leaves the
  ///        path in Scratch.
  auto RemoveEmptyLogged(const DirNode* node) noexcept -> int {
    return Logged(node->RemoveEmpty(scratch_), "{}", scratch_);
  }

  /// @brief Drop one reference on `node`, removing each directory up the
  ///        chain whose last reference it was.
  ///
  /// A directory the policy asks about before its rmdir is handed to the
  /// prompter instead, which carries on up the chain once it is answered.
  ///
  /// @param holder Hands out WorkTokens: a TaskContext or the scheduler.
  void Release(DirNode* node, auto& holder) noexcept {
    const auto chain = node->ParentsMut();

    for (auto it = chain.begin(); it != chain.end();) {
      DirNode* current = *it;
      ++it;

      if (current->remaining_children_dirs_.fetch_sub(
              1, std::memory_order_release) != 1) {
        return;
      }
      std::atomic_thread_fence(std::memory_order_acquire);

      if (policy_.AsksBeforeRmdir()) {
        requests_->Push(ConfirmRmdir{.dir = current, .token = holder.Hold()});
        return;
      }

      RemoveDir(current);
      delete current;
    }
  }

 private:
  const Cli* cli_;
  [[no_unique_address]] Policy policy_;
  Output output_;
  RequestList* requests_;
  std::string scratch_;
  std::size_t failures_ = 0;
};

}  // namespace remmy

#endif
