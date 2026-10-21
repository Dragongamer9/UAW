#include "alamo/xml.hpp"

#include <cctype>
#include <cstdlib>
#include <cstring>

namespace alamo {

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

std::string trim(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string(s.substr(b, e - b));
}

const std::string* XmlNode::attr(std::string_view name) const {
    for (const auto& [k, v] : attrs)
        if (iequals(k, name)) return &v;
    return nullptr;
}

const XmlNode* XmlNode::child(std::string_view t) const {
    for (const auto& c : children)
        if (iequals(c->tag, t)) return c.get();
    return nullptr;
}

std::vector<const XmlNode*> XmlNode::childrenNamed(std::string_view t) const {
    std::vector<const XmlNode*> out;
    for (const auto& c : children)
        if (iequals(c->tag, t)) out.push_back(c.get());
    return out;
}

namespace {

constexpr int kMaxIncludeDepth = 16;

std::string decodeEntities(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') { out += s[i]; continue; }
        size_t semi = s.find(';', i);
        if (semi == std::string_view::npos || semi - i > 10) { out += '&'; continue; }
        std::string_view ent = s.substr(i + 1, semi - i - 1);
        if (ent == "amp") out += '&';
        else if (ent == "lt") out += '<';
        else if (ent == "gt") out += '>';
        else if (ent == "quot") out += '"';
        else if (ent == "apos") out += '\'';
        else if (!ent.empty() && ent[0] == '#') {
            unsigned long cp = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X')
                ? std::strtoul(std::string(ent.substr(2)).c_str(), nullptr, 16)
                : std::strtoul(std::string(ent.substr(1)).c_str(), nullptr, 10);
            out += cp < 256 ? char(cp) : '?';
        } else { out += '&'; continue; }
        i = semi;
    }
    return out;
}

bool isNameChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == ':' || c == '.';
}

class Parser {
public:
    Parser(XmlDocument& doc, const XmlIncludeResolver& inc) : doc_(doc), inc_(inc) {}

    void parseInto(std::string_view text, std::vector<XmlNode*>& stack, int depth) {
        size_t i = 0;
        std::string pendingText;
        auto flushText = [&]() {
            if (!pendingText.empty()) {
                XmlNode* top = stack.back();
                std::string t = trim(decodeEntities(pendingText));
                if (!t.empty()) {
                    if (!top->text.empty()) top->text += ' ';
                    top->text += t;
                }
                pendingText.clear();
            }
        };

        while (i < text.size()) {
            if (text[i] != '<') { pendingText += text[i++]; continue; }

            if (text.compare(i, 4, "<!--") == 0) {
                size_t end = text.find("-->", i + 4);
                std::string_view body = text.substr(i + 4, (end == std::string_view::npos ? text.size() : end) - i - 4);
                handleComment(body, stack, depth);
                i = end == std::string_view::npos ? text.size() : end + 3;
                continue;
            }
            if (text.compare(i, 9, "<![CDATA[") == 0) {
                size_t end = text.find("]]>", i + 9);
                size_t stop = end == std::string_view::npos ? text.size() : end;
                std::string raw(text.substr(i + 9, stop - i - 9));
                // keep CDATA literal: escape '&' so decodeEntities leaves it alone
                for (char c : raw) pendingText += c == '&' ? std::string("&amp;") : std::string(1, c);
                i = end == std::string_view::npos ? text.size() : end + 3;
                continue;
            }
            if (text.compare(i, 2, "<?") == 0 || text.compare(i, 2, "<!") == 0) {
                size_t end = text.find('>', i);
                i = end == std::string_view::npos ? text.size() : end + 1;
                continue;
            }

            flushText();

            if (i + 1 < text.size() && text[i + 1] == '/') {
                size_t end = text.find('>', i);
                std::string name = trim(text.substr(i + 2, (end == std::string_view::npos ? text.size() : end) - i - 2));
                i = end == std::string_view::npos ? text.size() : end + 1;
                closeTag(name, stack);
                continue;
            }

            // Opening tag.
            size_t j = i + 1;
            while (j < text.size() && isNameChar(text[j])) ++j;
            auto node = std::make_unique<XmlNode>();
            node->tag = std::string(text.substr(i + 1, j - i - 1));
            bool selfClose = false;
            // attributes
            while (j < text.size()) {
                while (j < text.size() && std::isspace(static_cast<unsigned char>(text[j]))) ++j;
                if (j >= text.size()) break;
                if (text[j] == '>') { ++j; break; }
                if (text[j] == '/' && j + 1 < text.size() && text[j + 1] == '>') { selfClose = true; j += 2; break; }
                size_t k = j;
                while (k < text.size() && isNameChar(text[k])) ++k;
                if (k == j) { ++j; continue; } // junk char, skip
                std::string key(text.substr(j, k - j));
                j = k;
                while (j < text.size() && std::isspace(static_cast<unsigned char>(text[j]))) ++j;
                std::string value;
                if (j < text.size() && text[j] == '=') {
                    ++j;
                    while (j < text.size() && std::isspace(static_cast<unsigned char>(text[j]))) ++j;
                    if (j < text.size() && (text[j] == '"' || text[j] == '\'')) {
                        char q = text[j++];
                        size_t e = text.find(q, j);
                        if (e == std::string_view::npos) e = text.size();
                        value = decodeEntities(text.substr(j, e - j));
                        j = e < text.size() ? e + 1 : e;
                    } else {
                        size_t e = j;
                        while (e < text.size() && !std::isspace(static_cast<unsigned char>(text[e])) && text[e] != '>') ++e;
                        value = std::string(text.substr(j, e - j));
                        j = e;
                    }
                }
                node->attrs.emplace_back(std::move(key), std::move(value));
            }
            i = j;

            XmlNode* raw = node.get();
            stack.back()->children.push_back(std::move(node));
            if (!selfClose) stack.push_back(raw);
        }
        flushText();
    }

private:
    void closeTag(const std::string& name, std::vector<XmlNode*>& stack) {
        // Find nearest open element with this name (stack[0] is the synthetic root).
        for (size_t k = stack.size(); k-- > 1;) {
            if (iequals(stack[k]->tag, name)) {
                if (k != stack.size() - 1)
                    doc_.warnings.push_back("closing </" + name + "> implicitly closed <" + stack.back()->tag + ">");
                stack.resize(k);
                return;
            }
        }
        doc_.warnings.push_back("stray closing tag </" + name + ">");
    }

    void handleComment(std::string_view body, std::vector<XmlNode*>& stack, int depth) {
        std::string b = trim(body);
        // Directive is exactly INCLUDE "path"; ordinary comments like "Include file: ..." are ignored.
        if (b.compare(0, 7, "INCLUDE") != 0) return;
        size_t q1 = b.find_first_not_of(" \t\r\n", 7);
        if (q1 == std::string::npos || b[q1] != '"') return;
        size_t q2 = q1 == std::string::npos ? q1 : b.find('"', q1 + 1);
        if (q2 == std::string::npos) {
            doc_.warnings.push_back("malformed INCLUDE directive: " + b);
            return;
        }
        std::string path = b.substr(q1 + 1, q2 - q1 - 1);
        if (!inc_) return;
        if (depth >= kMaxIncludeDepth) {
            doc_.warnings.push_back("include depth limit hit at " + path);
            return;
        }
        auto content = inc_(path);
        if (!content) {
            doc_.warnings.push_back("include not found: " + path);
            return;
        }
        // Includes splice into the current element; tags left open inside them are closed at the end.
        const size_t base = stack.size();
        parseInto(*content, stack, depth + 1);
        if (stack.size() > base) {
            doc_.warnings.push_back("include left elements open: " + path);
            stack.resize(base);
        }
    }

    XmlDocument& doc_;
    const XmlIncludeResolver& inc_;
};

} // namespace

XmlDocument parseXml(std::string_view text, const XmlIncludeResolver& includes) {
    XmlDocument doc;
    doc.root.tag = "#document";
    std::vector<XmlNode*> stack{&doc.root};
    Parser p(doc, includes);
    p.parseInto(text, stack, 0);
    if (stack.size() > 1) doc.warnings.push_back("document ended with <" + stack.back()->tag + "> still open");
    return doc;
}

} // namespace alamo
