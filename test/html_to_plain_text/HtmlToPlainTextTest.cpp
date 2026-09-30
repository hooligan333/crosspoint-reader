// Host tests for htmlToPlainText(), used to render dictionary definitions that
// cannot be laid out as styled pages.

#include <gtest/gtest.h>

#include <string>

#include "HtmlToPlainText.h"

namespace {

// The fork's newline/NBSP cases predate upstream's headingMarker argument; they
// use no headings, so they run against the same marker the upstream cases pass.
using ::htmlToPlainText;
std::string htmlToPlainText(const std::string& html) { return ::htmlToPlainText(html, '\x01'); }

TEST(HtmlToPlainText, StripsTagsAndKeepsText) {
  EXPECT_EQ(htmlToPlainText("<b>quixotic</b> <i>adj.</i>", '\x01'), "quixotic adj.");
  EXPECT_EQ(htmlToPlainText("<span class=\"x\">in span</span>", '\x01'), "in span");
  EXPECT_EQ(htmlToPlainText("plain", '\x01'), "plain");
  EXPECT_EQ(htmlToPlainText("", '\x01'), "");
}

TEST(HtmlToPlainText, BlockElementsBecomeBreaks) {
  EXPECT_EQ(htmlToPlainText("a<br>b", '\x01'), "a\nb");
  EXPECT_EQ(htmlToPlainText("<div>a</div><div>b</div>", '\x01'), "a\nb");
  EXPECT_EQ(htmlToPlainText("<p>a</p><p>b</p>", '\x01'), "a\n\nb");
  // Consecutive breaks do not stack up.
  EXPECT_EQ(htmlToPlainText("a<br><br><br>b", '\x01'), "a\nb");
}

TEST(HtmlToPlainText, HeadingsBecomeBoldLabelLines) {
  // tagBreak() lists h1-h6 among the heading names, so they must actually
  // reach that comparison -- a name scan that stops at the first digit
  // reads "h1" as "h" and the heading runs into the text after it.
  const char marker = '\x01';
  for (const char* tag : {"h1", "h2", "h3", "h4", "h5", "h6"}) {
    const std::string html = std::string("<") + tag + ">title</" + tag + ">body";
    EXPECT_EQ(htmlToPlainText(html, marker), std::string(1, marker) + "title\nbody") << tag;
  }
  // The marker only means something at the start of a line, so a heading that
  // follows text still starts its own line.
  EXPECT_EQ(htmlToPlainText("lead<h2>title</h2>body", marker), "lead\n" + std::string(1, marker) + "title\nbody");
  EXPECT_EQ(htmlToPlainText("<hr>after", marker), "after");
}

TEST(HtmlToPlainText, DecodesEntities) {
  EXPECT_EQ(htmlToPlainText("Tom &amp; Jerry", '\x01'), "Tom & Jerry");
  // Fork: NBSP is a breakable space on the plain path.
  EXPECT_EQ(htmlToPlainText("a&nbsp;b", '\x01'), "a b");
  EXPECT_EQ(htmlToPlainText("&#65;&#66;", '\x01'), "AB");
  EXPECT_EQ(htmlToPlainText("&#x2014;", '\x01'), "\xE2\x80\x94");
  // Not entities: left as written rather than swallowed.
  EXPECT_EQ(htmlToPlainText("&notanentity;", '\x01'), "&notanentity;");
  EXPECT_EQ(htmlToPlainText("100% & up", '\x01'), "100% & up");
  EXPECT_EQ(htmlToPlainText("<p>Literal &lt;b&gt;word&lt;/b&gt; and &#60;tag&#62;</p>", '\x01'),
            "Literal <b>word</b> and <tag>");
}

TEST(HtmlToPlainText, TrimsSurroundingWhitespace) {
  EXPECT_EQ(htmlToPlainText("<p>only</p>", '\x01'), "only");
  EXPECT_EQ(htmlToPlainText("text   ", '\x01'), "text");
  EXPECT_EQ(htmlToPlainText("a\tb", '\x01'), "a b");
  EXPECT_EQ(htmlToPlainText("<br>a", '\x01'), "a");
}

TEST(HtmlToPlainText, SurvivesMalformedMarkup) {
  EXPECT_EQ(htmlToPlainText("a <b", '\x01'), "a <b");
  EXPECT_EQ(htmlToPlainText("x < y", '\x01'), "x < y");
  EXPECT_EQ(htmlToPlainText("<!-- comment -->kept", '\x01'), "kept");
}

TEST(HtmlToPlainText, RawBlankLinesAreParagraphBreaks) {
  EXPECT_EQ(htmlToPlainText("a\nb"), "a\nb");
  EXPECT_EQ(htmlToPlainText("a\n\nb"), "a\n\nb");
  // Longer runs cap at one blank line.
  EXPECT_EQ(htmlToPlainText("a\n\n\nb"), "a\n\nb");
  EXPECT_EQ(htmlToPlainText("a\n\n\n\n\n\nb"), "a\n\nb");
  // Whitespace-only lines still count as blank.
  EXPECT_EQ(htmlToPlainText("a\n  \t\nb"), "a\n\nb");
  // Leading/trailing blank lines are trimmed.
  EXPECT_EQ(htmlToPlainText("\n\na\n\n"), "a");
  // OED-style quotation list.
  EXPECT_EQ(htmlToPlainText("1. sense\n\n1600 Q1\n\n1700 Q2"), "1. sense\n\n1600 Q1\n\n1700 Q2");
}

TEST(HtmlToPlainText, CrlfCountsOnce) {
  EXPECT_EQ(htmlToPlainText("a\r\nb"), "a\nb");
  EXPECT_EQ(htmlToPlainText("a\r\n\r\nb"), "a\n\nb");
  EXPECT_EQ(htmlToPlainText("a\r\n\r\n\r\nb"), "a\n\nb");
  // A lone '\r' is still a space.
  EXPECT_EQ(htmlToPlainText("a\rb"), "a b");
}

TEST(HtmlToPlainText, NbspIsABreakableSpace) {
  EXPECT_EQ(htmlToPlainText("a\xC2\xA0"
                            "b"),
            "a b");
  // OED sense-letter run: collapses to one space.
  EXPECT_EQ(htmlToPlainText("all.\xE2\x80\x9D\xC2\xA0\xC2\xA0\xCE\xB2"), "all.\xE2\x80\x9D \xCE\xB2");
  EXPECT_EQ(htmlToPlainText("a&nbsp;&nbsp;b"), "a b");
  EXPECT_EQ(htmlToPlainText("a&#160;b&#xA0;c"), "a b c");
  // Never leads a line.
  EXPECT_EQ(htmlToPlainText("a\n\xC2\xA0"
                            "b"),
            "a\nb");
  EXPECT_EQ(htmlToPlainText("\xC2\xA0"
                            "a\xC2\xA0"),
            "a");
  // Other sequences led by 0xC2 are untouched.
  EXPECT_EQ(htmlToPlainText("\xC2\xA9"), "\xC2\xA9");
}

TEST(HtmlToPlainText, TagsAndRawNewlinesMix) {
  // A tag break followed by a lone raw newline stays a single break.
  EXPECT_EQ(htmlToPlainText("a<br>\nb"), "a\nb");
  // A tag break followed by a raw blank line becomes a paragraph break.
  EXPECT_EQ(htmlToPlainText("a<br>\n\nb"), "a\n\nb");
  // Paragraph tags plus raw blank lines never exceed one blank line.
  EXPECT_EQ(htmlToPlainText("<p>a</p>\n\n<p>b</p>"), "a\n\nb");
  EXPECT_EQ(htmlToPlainText("<b>word</b>\n\n<i>quote</i>\n<i>next</i>"), "word\n\nquote\nnext");
}

}  // namespace
