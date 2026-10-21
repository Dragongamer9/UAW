// Tolerant XML reader for Alamo game data.
//
// Petroglyph's XML is not always well-formed, so this parser is forgiving:
//  - include fragments may have several root elements
//  - mismatched closing tags close back to the nearest matching open tag
//  - tag and attribute lookups are case-insensitive (files mix Name= / name=)
//  - <!--INCLUDE "path" --> comments are an include directive; the callback
//    returns the fragment text, which is parsed in place
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace alamo {

struct XmlNode {
    std::string tag;                                         // as written
    std::vector<std::pair<std::string, std::string>> attrs;  // as written
    std::string text;                                        // trimmed, entities decoded
    std::vector<std::unique_ptr<XmlNode>> children;

    const std::string* attr(std::string_view name) const;    // case-insensitive
    const XmlNode* child(std::string_view tag) const;        // first match, case-insensitive
    std::vector<const XmlNode*> childrenNamed(std::string_view tag) const;
};

struct XmlDocument {
    // A synthetic root whose children are the top-level elements.
    XmlNode root;
    std::vector<std::string> warnings;
};

// Returns the included file's text, or nullopt if it can't be found.
using XmlIncludeResolver = std::function<std::optional<std::string>(std::string_view path)>;

XmlDocument parseXml(std::string_view text, const XmlIncludeResolver& includes = {});

bool iequals(std::string_view a, std::string_view b);
std::string trim(std::string_view s);

} // namespace alamo
