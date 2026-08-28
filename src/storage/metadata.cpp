// SPDX-License-Identifier: MIT
#include <charconv>
#include <cmath>
#include <sstream>

#include <vectordb/storage/metadata.hpp>

namespace vectordb {

MetadataType type_of(const MetadataValue& value) noexcept {
    return std::visit(
        [](const auto& held) -> MetadataType {
            using T = std::decay_t<decltype(held)>;
            if constexpr (std::is_same_v<T, std::int64_t>) {
                return MetadataType::kInteger;
            } else if constexpr (std::is_same_v<T, double>) {
                return MetadataType::kReal;
            } else if constexpr (std::is_same_v<T, std::string>) {
                return MetadataType::kText;
            } else {
                return MetadataType::kNull;
            }
        },
        value);
}

std::string to_display_string(const MetadataValue& value) {
    return std::visit(
        [](const auto& held) -> std::string {
            using T = std::decay_t<decltype(held)>;
            if constexpr (std::is_same_v<T, std::int64_t>) {
                return std::to_string(held);
            } else if constexpr (std::is_same_v<T, double>) {
                // ostringstream rather than std::to_string, which always emits
                // six decimal places and turns 0.5 into "0.500000".
                std::ostringstream out;
                out << held;
                return out.str();
            } else if constexpr (std::is_same_v<T, std::string>) {
                return held;
            } else {
                return "null";
            }
        },
        value);
}

std::string to_json_string(const MetadataValue& value) {
    return std::visit(
        [](const auto& held) -> std::string {
            using T = std::decay_t<decltype(held)>;
            if constexpr (std::is_same_v<T, std::int64_t>) {
                return std::to_string(held);
            } else if constexpr (std::is_same_v<T, double>) {
                // JSON has no NaN or Infinity. Emitting them produces output
                // that no conforming parser will read back, so they become
                // null — and metadata is validated on the way in anyway.
                if (!std::isfinite(held)) {
                    return "null";
                }
                std::ostringstream out;
                out << held;
                return out.str();
            } else if constexpr (std::is_same_v<T, std::string>) {
                std::string quoted;
                quoted.reserve(held.size() + 2);
                quoted.push_back('"');
                for (const char character : held) {
                    switch (character) {
                        case '"':
                            quoted += "\\\"";
                            break;
                        case '\\':
                            quoted += "\\\\";
                            break;
                        case '\n':
                            quoted += "\\n";
                            break;
                        case '\r':
                            quoted += "\\r";
                            break;
                        case '\t':
                            quoted += "\\t";
                            break;
                        default:
                            if (static_cast<unsigned char>(character) < 0x20) {
                                // Control characters must be escaped as \u00XX
                                // or the output is not valid JSON.
                                char buffer[7];
                                std::snprintf(buffer,
                                              sizeof(buffer),
                                              "\\u%04x",
                                              static_cast<unsigned>(
                                                  static_cast<unsigned char>(character)));
                                quoted += buffer;
                            } else {
                                quoted.push_back(character);
                            }
                    }
                }
                quoted.push_back('"');
                return quoted;
            } else {
                return "null";
            }
        },
        value);
}

}  // namespace vectordb
