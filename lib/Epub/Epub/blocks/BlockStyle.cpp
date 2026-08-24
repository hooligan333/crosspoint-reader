#include "BlockStyle.h"

#ifdef CROSSPOINT_CSS_CLASS_RULES

void BlockStyle::inheritBorderRules(BlockStyle& result, const BlockStyle& parent, const BlockStyle& child) {
  // Carry the ancestors' rules down, then append the child's own border at the x where
  // its border box starts: the parent block's content edge plus the child's own left
  // margin. Computed before the width is folded into the inset, so the rule sits outside
  // the child's padding, the way CSS lays a border box out.
  const uint8_t ownWidth = child.borders.ownWidth;
  result.borders = parent.borders;

  if (ownWidth > 0) {
    if (result.borders.count < MAX_BORDER_RULES) {
      result.borders.x[result.borders.count++] = static_cast<int16_t>(parent.leftInset() + child.marginLeft);
      result.borders.width = ownWidth;
    }
    // The border occupies horizontal space whether or not a rule was recorded, so levels
    // past the cap keep lining up with the text they enclose.
    result.paddingLeft = static_cast<int16_t>(result.paddingLeft + ownWidth);
  }
  result.borders.ownWidth = 0;
}

#endif  // CROSSPOINT_CSS_CLASS_RULES
