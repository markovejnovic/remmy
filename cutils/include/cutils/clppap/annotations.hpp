// See LICENSE in the repository root.

#ifndef CUTILS_CLPPAP_ANNOTATIONS_HPP
#define CUTILS_CLPPAP_ANNOTATIONS_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace cpplap {

enum class SchemaError : std::uint8_t {
  InvalidCliType,
  InheritedFields,
  DuplicateAnnotation,
  DuplicatePositionalAnnotation,
  EmbeddedNullAnnotation,
  UnsupportedAnnotation,
  InvalidTypeAnnotation,
  InvalidFlagAnnotations,
  InvalidOptionSpelling,
  DuplicateOptionSpelling,
  InvalidPositionalAnnotations,
  DuplicateHelpAnnotation,
  InvalidHelpAnnotations,
  UnsupportedFieldType
};

template <std::size_t N>
struct Text {
  char text[N]{};
  consteval explicit Text(const char (&value)[N]) {
    std::ranges::copy(value, text);
  }
};

template <std::size_t N>
struct Short : Text<N> {
  consteval explicit Short(const char (&value)[N]) : Text<N>(value) {}
};

template <std::size_t N>
struct Long : Text<N> {
  consteval explicit Long(const char (&value)[N]) : Text<N>(value) {}
};

template <std::size_t N>
struct Help : Text<N> {
  consteval explicit Help(const char (&value)[N]) : Text<N>(value) {}
};

struct PositionalTag {};

struct HelpFlagTag {};

}  // namespace cpplap

#endif  // CUTILS_CLPPAP_ANNOTATIONS_HPP
