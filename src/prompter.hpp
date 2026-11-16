// See LICENSE in the repository root.

/// @brief The one thread that asks: it answers the workers' requests until
///        every task and request is done.
#ifndef REMMY_PROMPTER_HPP
#define REMMY_PROMPTER_HPP

#include <cerrno>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "dir_node.hpp"
#include "policy.hpp"
#include "prompt.hpp"
#include "remover.hpp"
#include "request.hpp"
#include "unlink_worker.hpp"
#include "verified_dirs.hpp"

namespace remmy {

/// @brief Answers requests on the main thread, which alone owns the Console.
///
/// It also does what a yes asks for, so a worker never waits on an answer.
template <class Policy, class Scheduler>
class Prompter {
 public:
  /// @param remover Must reach directories through `verified`.
  /// @note Every argument must outlive this.
  Prompter(Console& console, RequestList& requests, Scheduler& scheduler,
           Policy policy, Remover<Policy> remover,
           VerifiedDirs& verified) noexcept
      : console_(&console),
        requests_(&requests),
        scheduler_(&scheduler),
        policy_(policy),
        remover_(std::move(remover)),
        verified_(&verified) {}

  [[nodiscard]] auto Failures() const noexcept -> std::size_t {
    return remover_.Failures();
  }

  /// @brief Answer requests until Drained.
  void Run() noexcept {
    while (true) {
      for (Request& request : requests_->TakeAll()) {
        if (std::holds_alternative<Drained>(request)) {
          return;
        }
        std::visit([this](auto& asked) { this->Handle(asked); }, request);
      }
    }
  }

 private:
  void Handle(ConfirmRemove& asked) noexcept {
    AskingAbout(*asked.parent, asked.name);
    if (console_->Ask(RemovePrompt{.path = path_}) == Answer::kYes) {
      Unlink(*asked.parent, asked.name);
    }
    remover_.Release(asked.parent, *scheduler_);
  }

  void Handle(ConfirmOverride& asked) noexcept {
    AskingAbout(*asked.parent, asked.name);
    if (console_->Ask(OverridePrompt{.info = asked.info, .path = path_}) ==
        Answer::kYes) {
      Unlink(*asked.parent, asked.name);
    }
    remover_.Release(asked.parent, *scheduler_);
  }

  void Handle(ConfirmExamine& asked) noexcept {
    asked.dir->PathInto(path_);
    const Answer answer = AskToEnter(*asked.dir);
    // Submitted before this request's token goes, so the scheduler cannot
    // finish in between.
    if (answer == Answer::kYes &&
        scheduler_->Submit(asked.dir, FileUnlinkWorker<Policy>::kRunnable)) {
      return;
    }

    // Skipped, so its parent's rmdir will fail with ENOTEMPTY, as rm's does.
    DirNode* parent = asked.dir->parent_;
    delete asked.dir;
    remover_.Release(parent, *scheduler_);
  }

  void Handle(ConfirmRmdir& asked) noexcept {
    asked.dir->PathInto(path_);
    if (AskToRmdir(*asked.dir) == Answer::kYes) {
      remover_.RemoveDir(asked.dir);
    }

    DirNode* parent = asked.dir->parent_;
    delete asked.dir;
    if (parent != nullptr) {
      remover_.Release(parent, *scheduler_);
    }
  }

  void Handle(Drained&) noexcept {}

  /// @brief Set path_ to the entry `name` of `dir`, as fts prints it.
  void AskingAbout(const DirNode& dir, std::string_view name) noexcept {
    dir.PathInto(path_);
    path_ += '/';
    path_ += name;
  }

  /// @brief Remove the entry `name` of `dir`, which path_ names, as a yes
  ///        asks.
  ///
  /// Every failure is reported, an entry gone by now included, as rm does
  /// without -f: a person said yes to this one.
  void Unlink(const DirNode& dir, const std::string& name) noexcept {
    if (remover_.Logged(verified_->UnlinkAt(dir, name, 0), "{}", path_) != 0) {
      remover_.Fail(path_, errno);
    }
  }

  /// @brief The question before entering `dir`, whose path is path_.
  [[nodiscard]] auto AskToEnter(const DirNode& dir) noexcept -> Answer {
    if constexpr (Policy::kAsksEveryEntry) {
      return policy_.AsksToExamine()
                 ? console_->Ask(ExaminePrompt{.path = path_})
                 : console_->Ask(RemovePrompt{.path = path_});
    } else {
      // Legacy mode's override question, asked before entering.
      return console_->Ask(OverridePrompt{.info = *dir.stat_, .path = path_});
    }
  }

  /// @brief The question before removing the emptied `dir`, whose path is
  ///        path_.
  [[nodiscard]] auto AskToRmdir(const DirNode& dir) noexcept -> Answer {
    if constexpr (Policy::kAsksEveryEntry) {
      return console_->Ask(RemovePrompt{.path = path_});
    } else {
      // As rm, from the stat taken when the walk found it.
      return console_->Ask(OverridePrompt{.info = *dir.stat_, .path = path_});
    }
  }

  Console* console_;
  RequestList* requests_;
  Scheduler* scheduler_;
  Policy policy_;
  Remover<Policy> remover_;
  VerifiedDirs* verified_;

  /// @brief The path asked about, rebuilt for each question.
  std::string path_;
};

}  // namespace remmy

#endif
