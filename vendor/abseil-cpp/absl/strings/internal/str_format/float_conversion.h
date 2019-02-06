#ifndef ABSL_STRINGS_INTERNAL_STR_FORMAT_FLOAT_CONVERSION_H_
#define ABSL_STRINGS_INTERNAL_STR_FORMAT_FLOAT_CONVERSION_H_

#include "absl/strings/internal/str_format/extension.h"

namespace absl {
inline namespace lts_2018_12_18 {
namespace str_format_internal {

bool ConvertFloatImpl(float v, const ConversionSpec &conv,
                      FormatSinkImpl *sink);

bool ConvertFloatImpl(double v, const ConversionSpec &conv,
                      FormatSinkImpl *sink);

bool ConvertFloatImpl(long double v, const ConversionSpec &conv,
                      FormatSinkImpl *sink);

}  // namespace str_format_internal
}  // inline namespace lts_2018_12_18
}  // namespace absl

#endif  // ABSL_STRINGS_INTERNAL_STR_FORMAT_FLOAT_CONVERSION_H_
