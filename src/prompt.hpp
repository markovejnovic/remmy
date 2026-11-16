// See LICENSE in the repository root.

/// @brief rm's questions to the user, and how their answers are read.
///
/// Each question is a type that renders itself and names the grammar its
/// answer is read with; only a Console can ask one.
#ifndef REMMY_PROMPT_HPP
#define REMMY_PROMPT_HPP

#include <locale.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>

#include "cutils/io/buffered_writer.hpp"
#include "cutils/io/stderr_writer.hpp"
#include "cutils/io/stdin_reader.hpp"
#include "cutils/io/writer_ref.hpp"
#include "cutils/sync/output_gate.hpp"

namespace remmy {

/// @brief The reply to one prompt.
enum class Answer : std::uint8_t { kNo, kYes };

/// @brief How a prompt's reply line is read.
enum class Grammar : std::uint8_t {
  /// rm's yes_or_no(): rpmatch(3) in the user's locale on the line's first
  /// byte alone; end of input, or anything rpmatch does not call yes, is no.
  kFirstByteRpMatch,
  /// rm's check2(): asks again until the line starts with y, Y, n or N; end
  /// of input is no.
  kStrictYesNo,
};

/// @brief Where a prompt renders itself: stderr, in one write.
using PromptWriter = cutils::io::InlineBufferedWriter<
    cutils::io::WriterRef<decltype(cutils::io::stderr_writer)>, 256>;

/// @brief A question rm asks: renders its text and names its answer grammar.
template <class P>
concept Prompt = requires(const P& prompt, PromptWriter& out) {
  { prompt.Render(out) } noexcept;
  { P::kGrammar } -> std::convertible_to<Grammar>;
};

/// @brief -i's question before removing one entry: `remove d/x? `.
struct RemovePrompt {
  static constexpr Grammar kGrammar = Grammar::kFirstByteRpMatch;

  std::string_view path;

  void Render(PromptWriter& out) const noexcept;
};

/// @brief -ir's question before entering a directory, such as
///        `examine files in directory d? `.
struct ExaminePrompt {
  static constexpr Grammar kGrammar = Grammar::kFirstByteRpMatch;

  std::string_view path;

  void Render(PromptWriter& out) const noexcept;
};

/// @brief What the override prompt prints about an entry, from its lstat.
struct OverrideInfo {
  mode_t mode;
  uid_t uid;
  gid_t gid;

  /// @brief chflags(2) flags; 0 on Linux.
  std::uint32_t flags;

  [[nodiscard]] static auto From(const struct stat& st) noexcept
      -> OverrideInfo;
};

/// @brief The question before removing what this process cannot write, or
///        what file flags protect, such as
///        `override r--r--r-- marko/staff uchg for a? `.
struct OverridePrompt {
  static constexpr Grammar kGrammar = Grammar::kFirstByteRpMatch;

  OverrideInfo info;
  std::string_view path;

  void Render(PromptWriter& out) const noexcept;
};

/// @brief -I's single question before anything is removed, such as
///        `recursively remove d and 2 files? `.
struct BulkPrompt {
  static constexpr Grammar kGrammar = Grammar::kStrictYesNo;

  std::size_t dirs;
  std::size_t files;

  /// @brief The directory's name, used when there is exactly one.
  std::string_view dir_name;

  /// @brief -r or -R: directories are asked about by name or count.
  bool recursive;

  void Render(PromptWriter& out) const noexcept;
};

/// @brief The -I question rm asks for `operands`, if it asks one.
///
/// rm asks when, among the operands that exist (lstat(2)), there is a
/// directory under -r, or more than three in all.
[[nodiscard]] auto BulkPromptFor(std::span<char* const> operands,
                                 bool recursive) noexcept
    -> std::optional<BulkPrompt>;

/// @brief The user's locale, for reading answers, apart from the process's.
///
/// rm calls setlocale(LC_ALL, ""); remmy does not, since that is not thread
/// safe and would localise glibc's strerror for every worker. Only the
/// rpmatch(3) call runs in this locale.
class AnswerLocale {
 public:
  AnswerLocale() noexcept;
  AnswerLocale(const AnswerLocale&) = delete;
  AnswerLocale(AnswerLocale&&) = delete;
  auto operator=(const AnswerLocale&) -> AnswerLocale& = delete;
  auto operator=(AnswerLocale&&) -> AnswerLocale& = delete;
  ~AnswerLocale();

  /// @brief Whether rpmatch(3) calls the one-byte answer `first` yes.
  [[nodiscard]] auto IsYes(char first) const noexcept -> bool;

 private:
  /// @brief The user's locale, or null when it cannot be loaded, which (as
  ///        for rm's failed setlocale) leaves answers read in C.
  locale_t locale_;
};

/// @brief The terminal: the one place questions are asked and answers read.
///
/// Owns the OutputGate workers write through, and closes it around each
/// question so nothing they print lands between a prompt and its answer.
class Console {
 public:
  Console() noexcept = default;
  Console(const Console&) = delete;
  Console(Console&&) = delete;
  auto operator=(const Console&) -> Console& = delete;
  auto operator=(Console&&) -> Console& = delete;
  ~Console() = default;

  /// @brief Ask `prompt` on stderr and read the answer from stdin.
  template <Prompt P>
  [[nodiscard]] auto Ask(const P& prompt) noexcept -> Answer {
    gate_.Close();
    const Answer answer = AskWhileClosed(prompt);
    gate_.Open();
    return answer;
  }

  /// @brief The gate workers' output goes through while this asks.
  [[nodiscard]] auto Gate() noexcept -> cutils::sync::OutputGate& {
    return gate_;
  }

 private:
  template <Prompt P>
  [[nodiscard]] auto AskWhileClosed(const P& prompt) noexcept -> Answer {
    if constexpr (P::kGrammar == Grammar::kFirstByteRpMatch) {
      Show(prompt);
      return ReadFirstByteRpMatch();
    } else {
      static_assert(P::kGrammar == Grammar::kStrictYesNo);
      while (true) {
        Show(prompt);
        if (const std::optional<Answer> answer = ReadStrictYesNo()) {
          return *answer;
        }
      }
    }
  }

  template <Prompt P>
  static void Show(const P& prompt) noexcept {
    PromptWriter line(cutils::io::stderr_writer);
    prompt.Render(line);
    std::ignore = line.Flush();
  }

  /// @brief One kFirstByteRpMatch reply.
  [[nodiscard]] auto ReadFirstByteRpMatch() noexcept -> Answer;

  /// @brief One kStrictYesNo reply, or nullopt to ask again.
  [[nodiscard]] auto ReadStrictYesNo() noexcept -> std::optional<Answer>;

  cutils::io::StdinReader& in_ = cutils::io::stdin_reader;
  cutils::sync::OutputGate gate_;

  /// @brief Loaded on the first answer read through rpmatch, so runs that
  ///        never ask one never pay for it.
  std::optional<AnswerLocale> locale_;
};

}  // namespace remmy

#endif
