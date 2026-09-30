#pragma once

#include <string>

// Convert an HTML fragment to readable plain text. This intentionally ignores
// styling; block elements become line breaks and HTML entities are decoded.
// A raw blank line becomes a paragraph break ("\n\n", never more), a lone
// raw newline a line break, and NBSP (raw or entity) a breakable space.
std::string htmlToPlainText(const std::string& html);
