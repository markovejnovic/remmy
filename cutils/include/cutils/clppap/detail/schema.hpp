// See LICENSE in the repository root.

#ifndef CUTILS_CLPPAP_DETAIL_SCHEMA_HPP
#define CUTILS_CLPPAP_DETAIL_SCHEMA_HPP

#include <algorithm>
#include <array>
#include <cutils/clppap/annotations.hpp>
#include <cutils/clppap/positional_iterator.hpp>
#include <expected>
#include <iterator>
#include <meta>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

namespace cpplap::detail {

struct Annotations {
  std::optional<std::string_view> short_name;
  std::optional<std::string_view> long_name;
  std::optional<std::string_view> help;
  bool positional = false;
  bool help_flag = false;

  template <std::size_t N>
  [[nodiscard]] consteval auto Apply(const Short<N>& value)
      -> std::expected<void, SchemaError> {
    return ApplyText(short_name, {value.text, N - 1});
  }

  template <std::size_t N>
  [[nodiscard]] consteval auto Apply(const Long<N>& value)
      -> std::expected<void, SchemaError> {
    return ApplyText(long_name, {value.text, N - 1});
  }

  template <std::size_t N>
  [[nodiscard]] consteval auto Apply(const Help<N>& value)
      -> std::expected<void, SchemaError> {
    return ApplyText(help, {value.text, N - 1});
  }

  [[nodiscard]] consteval auto Apply(PositionalTag)
      -> std::expected<void, SchemaError> {
    if (positional) {
      return std::unexpected(SchemaError::DuplicatePositionalAnnotation);
    }
    positional = true;
    return {};
  }

  [[nodiscard]] consteval auto Apply(HelpFlagTag)
      -> std::expected<void, SchemaError> {
    if (help_flag) {
      return std::unexpected(SchemaError::DuplicateHelpAnnotation);
    }
    help_flag = true;
    return {};
  }

  template <typename T>
  [[nodiscard]] consteval auto Apply(const T&)
      -> std::expected<void, SchemaError> {
    return std::unexpected(SchemaError::UnsupportedAnnotation);
  }

 private:
  [[nodiscard]] consteval auto ApplyText(
      std::optional<std::string_view>& target, std::string_view value)
      -> std::expected<void, SchemaError> {
    if (target) {
      return std::unexpected(SchemaError::DuplicateAnnotation);
    }

    if (value.find('\0') != std::string_view::npos) {
      return std::unexpected(SchemaError::EmbeddedNullAnnotation);
    }

    target = std::define_static_string(value);
    return {};
  }
};

// GCC 16's debug iterators try to format consteval-only info at runtime.
// Copy bounded data during constant evaluation; keep _GLIBCXX_DEBUG enabled.
template <auto Query>
consteval auto ReflectionArray() {
  constexpr std::size_t count = Query().size();
  auto values = Query();

  std::array<std::meta::info, count> result{};
  for (std::size_t i = 0; i < count; ++i) {
    result.data()[i] = values.data()[i];
  }

  return result;
}

template <std::meta::info Entity>
[[nodiscard]] consteval auto ReadAnnotations()
    -> std::expected<Annotations, SchemaError> {
  Annotations out;

  template for (constexpr auto annotation : ReflectionArray<[] consteval {
                  return std::meta::annotations_of(Entity);
                }>()) {
    using Raw = [:std::meta::type_of(annotation):];
    using Value = std::remove_cv_t<Raw>;

    if (auto applied = out.Apply(std::meta::extract<Value>(annotation));
        !applied) {
      return std::unexpected(applied.error());
    }
  }
  return out;
}

template <typename T>
inline constexpr auto kMembers = ReflectionArray<[] consteval {
  return std::meta::nonstatic_data_members_of(
      ^^T, std::meta::access_context::current());
}>();

template <typename T>
inline constexpr std::size_t kFlagCount = [] consteval {
  std::size_t count = 0;
  for (auto member : kMembers<T>) {
    if (std::meta::type_of(member) == (^^bool)) {
      ++count;
    }
  }
  return count;
}();

template <std::size_t N>
struct Schema {
  std::array<Option, N> options{};
  std::string_view description;
  std::string_view positional_name;
  std::string_view positional_help;
  std::size_t help_index = 0;
  bool has_positionals = false;
  bool has_help = false;
};

consteval auto ValidShort(std::string_view name) -> bool {
  return name.size() == 2 && name[0] == '-' && name[1] > ' ' &&
         name[1] < '\x7f' && name[1] != '-' && name[1] != '=';
}

consteval auto ValidLong(std::string_view name) -> bool {
  if (name.size() < 3 || !name.starts_with("--") || name[2] == '-') {
    return false;
  }
  for (char c : name.substr(2)) {
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_')) {
      return false;
    }
  }
  return true;
}

template <typename T>
[[nodiscard]] consteval auto BuildSchema()
    -> std::expected<Schema<kFlagCount<T>>, SchemaError> {
  Schema<kFlagCount<T>> out;

  constexpr auto type = ReadAnnotations<^^T>();
  if (!type) {
    return std::unexpected(type.error());
  }

  if (type->short_name || type->long_name || type->positional ||
      type->help_flag) {
    return std::unexpected(SchemaError::InvalidTypeAnnotation);
  }

  out.description = type->help.value_or("");

  std::size_t index = 0;
  template for (constexpr auto member : kMembers<T>) {
    constexpr auto field = ReadAnnotations<member>();
    if (!field) {
      return std::unexpected(field.error());
    }

    if constexpr (std::meta::type_of(member) == (^^bool)) {
      if (field->positional || (!field->short_name && !field->long_name)) {
        return std::unexpected(SchemaError::InvalidFlagAnnotations);
      }

      if ((field->short_name && !ValidShort(*field->short_name)) ||
          (field->long_name && !ValidLong(*field->long_name))) {
        return std::unexpected(SchemaError::InvalidOptionSpelling);
      }

      for (std::size_t i = 0; i < index; ++i) {
        if ((field->short_name &&
             *field->short_name == out.options[i].short_name) ||
            (field->long_name &&
             *field->long_name == out.options[i].long_name)) {
          return std::unexpected(SchemaError::DuplicateOptionSpelling);
        }
      }

      if (field->help_flag) {
        if (out.has_help) {
          return std::unexpected(SchemaError::DuplicateHelpAnnotation);
        }
        out.has_help = true;
        out.help_index = index;
      }

      out.options[index++] = {field->short_name.value_or(""),
                              field->long_name.value_or(""),
                              field->help.value_or("")};
    } else if constexpr (std::meta::type_of(member) == (^^PositionalIterator)) {
      if (field->help_flag) {
        return std::unexpected(SchemaError::InvalidHelpAnnotations);
      }
      if (!field->positional || field->short_name || field->long_name ||
          out.has_positionals) {
        return std::unexpected(SchemaError::InvalidPositionalAnnotations);
      }
      out.has_positionals = true;
      out.positional_name =
          std::define_static_string(std::meta::identifier_of(member));
      out.positional_help = field->help.value_or("");
    } else {
      return std::unexpected(SchemaError::UnsupportedFieldType);
    }
  }
  return out;
}

template <typename T>
inline constexpr auto kSchema = [] consteval {
  static_assert(!std::is_union_v<T> && std::is_aggregate_v<T> &&
                    std::is_trivially_copyable_v<T>,
                std::meta::display_string_of(
                    std::meta::reflect_constant(SchemaError::InvalidCliType)));
  static_assert(
      std::meta::bases_of(^^T, std::meta::access_context::current()).empty(),
      std::meta::display_string_of(
          std::meta::reflect_constant(SchemaError::InheritedFields)));
  constexpr auto schema = BuildSchema<T>();
  if constexpr (!schema) {
    static_assert(schema.has_value(),
                  std::meta::display_string_of(
                      std::meta::reflect_constant(schema.error())));
  }
  return *schema;
}();

template <typename T>
inline constexpr std::string_view kHelpText = [] consteval {
  constexpr auto& schema = kSchema<T>;
  std::string text;
  // GCC 16 UBSan rejects string::append's null check on reflected strings in
  // constant evaluation. Copy characters instead, keeping runtime checks
  // enabled.
  if (!schema.description.empty()) {
    std::ranges::copy(schema.description, std::back_inserter(text));
    text += '\n';
  }

  if (!schema.options.empty()) {
    if (!text.empty()) {
      text += '\n';
    }
    text += "Options:\n";
    for (const auto& option : schema.options) {
      text += "  ";
      std::ranges::copy(option.short_name, std::back_inserter(text));
      if (!option.short_name.empty() && !option.long_name.empty()) {
        text += ", ";
      }
      std::ranges::copy(option.long_name, std::back_inserter(text));
      text += '\n';
      if (!option.help.empty()) {
        text += "    ";
        std::ranges::copy(option.help, std::back_inserter(text));
        text += '\n';
      }
    }
  }

  if (schema.has_positionals) {
    if (!text.empty()) {
      text += '\n';
    }

    text += "Arguments:\n  ";
    std::ranges::copy(schema.positional_name, std::back_inserter(text));
    text += '\n';
    if (!schema.positional_help.empty()) {
      text += "    ";
      std::ranges::copy(schema.positional_help, std::back_inserter(text));
      text += '\n';
    }
  }

  return std::string_view{std::define_static_string(text)};
}();

}  // namespace cpplap::detail

#endif  // CUTILS_CLPPAP_DETAIL_SCHEMA_HPP
