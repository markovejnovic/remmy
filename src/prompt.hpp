// See LICENSE in the repository root.

/// @brief rm's questions to the user, and how their answers are read.
///
/// Each question is a type that renders itself and names the grammar its
/// answer is read with; only a Console can ask one.
#ifndef REMMY_PROMPT_HPP
#define REMMY_PROMPT_HPP

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

namespace remmy {

/// @brief The reply to one prompt.
enum class Answer : std::uint8_t { kNo, kYes };

/// @brief How a prompt's reply line is read.
enum class Grammar : std::uint8_t {
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

/// @brief The terminal: the one place questions are asked and answers read.
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
    if constexpr (P::kGrammar == Grammar::kStrictYesNo) {
      while (true) {
        Show(prompt);
        if (const std::optional<Answer> answer = ReadStrictYesNo()) {
          return *answer;
        }
      }
    }
  }

 private:
  template <Prompt P>
  static void Show(const P& prompt) noexcept {
    PromptWriter line(cutils::io::stderr_writer);
    prompt.Render(line);
    std::ignore = line.Flush();
  }

  /// @brief One kStrictYesNo reply, or nullopt to ask again.
  [[nodiscard]] auto ReadStrictYesNo() noexcept -> std::optional<Answer>;

  cutils::io::StdinReader& in_ = cutils::io::stdin_reader;
};

}  // namespace remmy

#endif
