# SD Theme Creation

SD themes are folders that contain a `theme.json` file plus optional assets such as generated icons and UI fonts. The current theme system is FreeInkUI-oriented: authors should build screens from rows, columns, and named components instead of calculating every element with absolute pixels.

Themes are selected from Settings after they are copied to the SD card or downloaded from the hosted theme manifest.

## Start Here

Use this structure for a hosted theme:

```text
sd-themes/<theme-id>/
  theme.json
  icons/*.bmp        # generated fallbacks for theme icons
  fonts/*.cpfont     # optional theme UI fonts
```

Installed themes are discovered from either SD-card root:

```text
/.themes/<theme-id>/theme.json
/themes/<theme-id>/theme.json
```

Theme ids must be path-safe and should use only letters, numbers, `-`, and `_`. Do not use spaces for hosted themes because ids are used in URLs, settings, and update checks.

Every published theme must include a `version`. Increase it whenever users should see an update available.

```json
{
  "schema": 1,
  "version": 1,
  "id": "my-theme",
  "name": "My Theme",
  "description": "Short text shown in the theme downloader.",
  "inherits": "lyra"
}
```

## Authoring Model

The preferred model is:

1. Put shared spacing in `metrics`.
2. Put reusable styles in top-level `components`.
3. Build the home screen with `screens.home.layout` using rows and columns.
4. Override Settings and reader menu chrome with `screens.settings` and `screens.readerMenu` when the theme needs its own list/header/button style.
5. Declare FreeInk/Lucide icon names in `assets.freeInkIcons`.
6. Generate SD BMP fallbacks and the hosted manifest before publishing.

Unknown JSON fields are ignored, so compatible apps can add namespaced data under `extensions.<app-or-namespace>`.

## Top-Level Fields

- `schema`: currently `1`.
- `version`: integer used by the hosted manifest and downloader update checks.
- `id`: stable id used for folder name, settings, downloads, and updates.
- `name`: display name shown in Settings and the downloader.
- `description`: short downloader text.
- `inherits`: usually `lyra`. `classic` is accepted for manually installed themes that intentionally use the Classic renderer.
- `metrics`: shared spacing, sizing, and renderer values.
- `components`: reusable styles for menus, lists, headers, tabs, button hints, and older home recents.
- `screens`: screen-specific layout and chrome. Modern home layouts live here.
- `assets`: icon maps and optional UI font family.
- `devices`: per-device overrides keyed by `x3` or `x4`.
- `freeInkUI`: metadata describing the FreeInkUI components the theme targets.
- `requires`: optional compatibility metadata for tooling or compatible apps.
- `extensions`: optional namespaced metadata for compatible apps.

## Minimal Row/Column Theme

This is the smallest useful modern theme. It creates a centered home menu without absolute coordinates:

```json
{
  "schema": 1,
  "version": 1,
  "id": "quiet-menu",
  "name": "Quiet Menu",
  "description": "A simple row-based home theme.",
  "inherits": "lyra",
  "metrics": {
    "homeContinueReadingInMenu": true,
    "homeShowContinueReadingHeader": false
  },
  "components": {
    "homeMenu": {
      "font": "medium",
      "style": "regular",
      "centeredText": true,
      "showIcons": false,
      "selectionStyle": "underline"
    },
    "buttonHints": {
      "layout": "shapes",
      "fill": false,
      "outline": false,
      "drawEmpty": false
    }
  },
  "screens": {
    "home": {
      "layout": {
        "type": "rows",
        "padding": {
          "x": 60,
          "top": 120,
          "bottom": 120
        },
        "gap": 8,
        "children": [
          {
            "type": "box",
            "fill": false,
            "outline": false
          },
          {
            "type": "menu-grid",
            "h": 220,
            "columns": 1,
            "gap": 8,
            "font": "medium",
            "fill": false,
            "outline": false
          },
          {
            "type": "box",
            "fill": false,
            "outline": false
          }
        ]
      }
    }
  }
}
```

Rows divide available height among children. A child with `h` gets that fixed height; children without `h` share the remaining space. Columns work the same way for width. This is the main way themes scale across devices.

## Screens

Supported screen keys:

- `screens.home`: full FreeInkUI-backed home layout.
- `screens.settings`: metrics and component overrides for the main Settings list.
- `screens.readerMenu`: metrics and component overrides for the in-reader menu.

### Home Layout

`screens.home.layout` must be a single root element. Use `rows` and `columns` for structure:

```json
"screens": {
  "home": {
    "layout": {
      "type": "rows",
      "padding": {"x": 36, "top": 8, "bottom": 20},
      "gap": 10,
      "children": [
        {
          "type": "tab-bar",
          "h": 36,
          "icons": ["book-open", "library", "wifi", "sliders-horizontal"],
          "selectedIndex": 0,
          "font": "small",
          "style": "bold",
          "outline": false
        },
        {
          "type": "book-card",
          "h": 150,
          "coverWidth": 86,
          "coverHeight": 122,
          "font": "medium",
          "outline": false
        },
        {
          "type": "cover-grid",
          "h": 220,
          "columns": 3,
          "count": 3,
          "coverWidth": "fill",
          "coverHeight": 190,
          "showTitle": false,
          "outline": false
        },
        {
          "type": "metric-cards",
          "h": 112,
          "count": 3,
          "labels": ["last read", "total read", "completed"],
          "outline": true
        }
      ]
    }
  }
}
```

Supported layout element types:

- `rows`, aliases `vstack`, `column`
- `columns`, aliases `hstack`, `row`
- `box`, aliases `rect`, `panel`
- `divider`, alias `line`
- `label`, alias `text`
- `tab-bar`, alias `tabs`
- `book-card`, alias `current-book`
- `cover-grid`, alias `cover-row`
- `menu-grid`, alias `buttons`
- `metric-cards`, alias `stats`

Common layout fields:

- `w` or `width`: fixed width inside a column layout.
- `h` or `height`: fixed height inside a row layout.
- `padding`: number or object. Objects support `x`, `y`, `top`, `right`, `bottom`, `left`.
- `gap`: spacing between children.
- `radius`, `lineWidth` or `strokeWidth`.
- `fill`: draw a filled background.
- `outline` or `border`: draw a border. Defaults to `false`.
- `font`: `small`, `medium`, or `large`.
- `style`: `regular` or `bold`.
- `children` or `items`: nested elements. Nesting is limited to keep parsing bounded.

Component-specific fields:

- `tab-bar`: `labels`, `icons`, `selectedIndex`.
- `book-card`: `coverWidth`, `coverHeight`, `showTitle`, `showAuthor`, `showProgress`.
- `cover-grid`: `columns`, `count`, `coverWidth`, `coverHeight`, `showTitle`, `showAuthor`, `showProgress`.
- `menu-grid`: `columns`, `gap`.
- `metric-cards`: `count`, `labels`.

Use `coverWidth: "fill"` when covers should fill the available card width. Use explicit `coverHeight` to control the visual rhythm.

### Settings And Reader Menu Screens

Settings and reader menu do not use full arbitrary layouts yet. They support screen-specific metrics and component chrome:

```json
"screens": {
  "settings": {
    "metrics": {
      "topPadding": 5,
      "headerHeight": 84,
      "tabBarHeight": 40,
      "verticalSpacing": 16,
      "contentSidePadding": 20,
      "listRowHeight": 40,
      "listWithSubtitleRowHeight": 60,
      "buttonHintsHeight": 40
    },
    "components": {
      "header": {
        "font": "large",
        "style": "bold",
        "showDivider": true
      },
      "tabBar": {
        "font": "medium",
        "selectionStyle": "fill",
        "selectedTextInverted": true,
        "drawDivider": true
      },
      "list": {
        "font": "medium",
        "showIcons": true,
        "selectionStyle": "fill",
        "selectionFill": true,
        "selectionOutline": false,
        "rowBackgrounds": false
      },
      "buttonHints": {
        "font": "small",
        "layout": "buttons",
        "fill": true,
        "outline": true
      }
    }
  },
  "readerMenu": {
    "metrics": {
      "headerHeight": 84,
      "listRowHeight": 40,
      "buttonHintsHeight": 40
    },
    "components": {
      "header": {
        "font": "large",
        "style": "bold"
      },
      "list": {
        "font": "medium",
        "showIcons": true,
        "selectionStyle": "fill"
      },
      "buttonHints": {
        "layout": "buttons",
        "fill": true,
        "outline": true
      }
    }
  }
}
```

These screen blocks inherit top-level metrics first, then apply the screen override. If a screen component is omitted, the top-level component style is reused.

## Components

Top-level `components` apply across the theme unless a screen override replaces them.

### Fonts

Most components and layout elements accept:

```json
{
  "font": "medium",
  "style": "bold"
}
```

Supported font tokens:

- `small`: remaps to the theme's 8 point UI font when provided.
- `medium`: remaps to the theme's 10 point UI font when provided.
- `large`: remaps to the theme's 12 point UI font when provided.

Legacy aliases `ui10` and `ui12` still parse for older themes, but new themes should use `small`, `medium`, and `large`.

### Theme UI Fonts

Declare a UI font family in `assets.uiFontFamily`:

```json
"assets": {
  "uiFontFamily": "MyThemeUI"
}
```

Bundle matching `.cpfont` files:

```text
sd-themes/my-theme/fonts/MyThemeUI_8.cpfont
sd-themes/my-theme/fonts/MyThemeUI_10.cpfont
sd-themes/my-theme/fonts/MyThemeUI_12.cpfont
```

At runtime:

- `small` uses `<Family>_8.cpfont`
- `medium` uses `<Family>_10.cpfont`
- `large` uses `<Family>_12.cpfont`

Missing sizes fall back to the built-in UI font. Theme UI fonts are loaded only while the SD theme is active and are released before network-heavy operations to preserve heap.

### Home Menu

`components.homeMenu` styles the legacy menu renderer and `menu-grid` layout element.

Fields:

- `font`, `fontId`, `style`, `bold`
- `centeredText`
- `centerVertically`
- `showIcons`
- `panelWidth`
- `drawPanel`
- `panelCornerRadius`
- `selectionStyle`: `fill`, `outline`, `triangle`, `underline`, or `pill`
- `selectionCornerRadius`
- `selectionInset`
- `selectedTextInverted`
- `selectionFillBlack`
- `rowPaddingX`
- `textInsetX`

### Lists

`components.list` styles list rows. Top-level list styles affect browse/settings-style screens; `screens.settings.components.list` and `screens.readerMenu.components.list` override those screens.

Fields:

- `font`, `fontId`, `style`, `bold`
- `subtitleFontId`
- `valueFontId`
- `showIcons`
- `iconSize`
- `textGap`
- `selectionStyle`: `fill`, `outline`, or `underline`
- `selectionCornerRadius`
- `selectionFill`
- `selectionOutline`
- `selectedTextInverted`
- `rowBackgrounds`
- `centerSingleLineRows`
- `subtitleRowAutoHeight`
- `centerValueVertically`
- `rowSidePadding`
- `rowGap`
- `textInsetX`
- `selectionInsetX`
- `selectionInsetY`
- `titleOffsetY`
- `subtitleOffsetY`
- `subtitleTopPadding`
- `subtitleBottomPadding`
- `subtitleInterLineGap`
- `valueOffsetY`
- `subtitleValueOffsetY`
- `iconOffsetY`

### Header

Fields:

- `font`, `fontId`, `style`, `bold`
- `centeredTitle`
- `showDivider`
- `titleOffsetY`
- `batteryOffsetY`

### Tab Bar

Fields:

- `font`, `fontId`, `style`, `bold`
- `equalWidth`
- `selectionStyle`: `fill` or `underline`
- `selectedCornerRadius`
- `selectedTextInverted`
- `drawDivider`
- `horizontalInset`

For folder-style tabs, use `selectionStyle: "fill"`, `selectedTextInverted: true`, compact `horizontalInset`, and icon-only tab labels in the home layout.

### Button Hints

Fields:

- `font`, `fontId`, `style`, `bold`
- `layout`: `buttons`, `groups`, `shapes`, or `icons`
- `buttonWidth`
- `smallButtonHeight`
- `cornerRadius`
- `fill`
- `outline`
- `drawEmpty`
- `sidePadding`
- `groupGap`
- `bottomMargin`
- `innerPadding`
- `shapeSize`
- `textOffsetY`

Use `layout: "icons"` with named `hint*` icons for custom button hint art. Use `layout: "shapes"` for simple built-in arrow/circle/square hints.

### Home Recents

`components.homeRecents` is the older cover-strip renderer. New dashboard-style themes should prefer `screens.home.layout` with `book-card` and `cover-grid`, but `homeRecents` remains useful for simple cover-focused themes.

Supported types:

- `default`
- `none`
- `cover-strip`

Important fields:

- `maxBooks`
- `wrap`
- `drawPanel`
- `panelCornerRadius`
- `panelInsetX`
- `selectionLineWidth`
- `inactiveSelectionLineWidth`
- `selectionCornerRadius`
- `slots`

Each slot supports `book`, `bookIndex`, `x`, `y`, `height`, `widthPercent`, `xOffset`, `yOffset`, `selected`, and `title`.

## Metrics

Metrics are optional. Omitted values use Lyra defaults.

Common metrics:

- `topPadding`
- `headerHeight`
- `verticalSpacing`
- `contentSidePadding`
- `listRowHeight`
- `listWithSubtitleRowHeight`
- `menuRowHeight`
- `menuSpacing`
- `tabSpacing`
- `tabBarHeight`
- `homeTopPadding`
- `homeCoverHeight`
- `homeCoverTileHeight`
- `homeRecentBooksCount`
- `homeContinueReadingInMenu`
- `homeShowContinueReadingHeader`
- `homeMenuTopOffset`
- `buttonHintsHeight`
- `sideButtonHintsWidth`

Reader/status metrics:

- `progressBarHeight`
- `progressBarMarginTop`
- `statusBarHorizontalMargin`
- `statusBarVerticalMargin`
- `batteryWidth`
- `batteryHeight`
- `batteryBarHeight`

Other supported groups:

- Scroll bars: `scrollBarWidth`, `scrollBarRightOffset`
- Keyboard: `keyboardKeyWidth`, `keyboardKeyHeight`, `keyboardKeySpacing`, `keyboardBottomKeyHeight`, `keyboardBottomKeySpacing`, `keyboardBottomAligned`, `keyboardCenteredText`, `keyboardVerticalOffset`, `keyboardTextFieldWidthPercent`, `keyboardWidthPercent`, `keyboardKeyCornerRadius`, `keyboardFillUnselected`, `keyboardOutlineAllUnselected`, `keyboardDrawSpecialOutlineWhenUnselected`, `keyboardSecondaryLabelRightPadding`, `keyboardSecondaryLabelTopPadding`, `keyboardMinArrowHeadSize`
- Popups: `popupTopOffsetRatio`, `popupMarginX`, `popupMarginY`, `popupFrameThickness`, `popupCornerRadius`, `popupTextBold`, `popupTextInverted`, `popupTextBaselineOffsetY`, `popupProgressBarHeight`, `popupProgressDrawOutline`, `popupProgressClampPercent`, `popupProgressFillInverted`, `popupProgressOutlineInverted`
- Text fields: `textFieldHorizontalPadding`, `textFieldNormalThickness`, `textFieldCursorThickness`, `textFieldLineEndOffset`

## Icons

New themes should declare FreeInk/Lucide icons first:

```json
"assets": {
  "freeInkIcons": {
    "book": "book-open",
    "folder": "folder",
    "settings": "sliders-horizontal",
    "hintLeft": "arrow-left",
    "hintRight": "arrow-right",
    "hintMenu": "menu"
  }
}
```

The firmware cannot rasterize Lucide SVGs at runtime. There are two generated outputs:

- A firmware registry, compiled from every hosted theme's `assets.freeInkIcons`, used when the icon exists in the current firmware.
- SD-card BMP fallbacks in each theme's `icons/` folder, used when an icon is not compiled into firmware or when a theme is manually installed.

Generate both when publishing hosted themes:

```bash
python3 scripts/generate-theme-icons.py \
  --themes sd-themes \
  --freeink-sdk freeink-sdk \
  --firmware-out src/components/icons/freeink_theme_icons.generated.h \
  --registry-out src/components/icons/FreeInkThemeIconRegistry.cpp

python3 scripts/generate-theme-icons.py \
  --themes sd-themes \
  --from-freeink-sdk \
  --freeink-sdk freeink-sdk
```

The second command updates `assets.icons` and writes files such as:

```text
sd-themes/my-theme/icons/book.bmp
sd-themes/my-theme/icons/hintLeft.bmp
```

Supported semantic icon keys for built-in UI locations:

- `folder`, `folder24`
- `text`, `text24`
- `image`, `image24`
- `book`, `book24`
- `file`, `file24`
- `recent`
- `settings`, `settings2`
- `transfer`
- `library`
- `wifi`
- `hotspot`
- `bookmark`

Named icons are also supported for theme-specific layout and hint use. Keys such as `hintLeft`, `hintRight`, `hintMenu`, `hintBack`, `hintOpen`, and `hintSelect` are treated as named icons and can be referenced by FreeInkUI-backed theme surfaces.

## Device Overrides

Device-specific overrides live under `devices.x3` or `devices.x4`. They can override `inherits`, `metrics`, `components`, `screens`, `assets`, `freeInkUI`, and `constraints`.

```json
"devices": {
  "x3": {
    "constraints": {
      "screenWidth": 480,
      "screenHeight": 800,
      "frontButtons": 4,
      "sideButtons": "up-down"
    },
    "screens": {
      "home": {
        "layout": {
          "type": "rows",
          "padding": {"x": 36, "top": 8},
          "children": []
        }
      }
    }
  },
  "x4": {
    "constraints": {
      "screenWidth": 480,
      "screenHeight": 800,
      "frontButtons": 0,
      "sideButtons": "up-down"
    }
  }
}
```

Constraints document intent for tools and compatible apps. They do not reject a theme at runtime.

## FreeInkUI Metadata

Themes can declare which FreeInkUI components they target:

```json
"freeInkUI": {
  "components": [
    "header",
    "status-bar",
    "tab-bar",
    "book-card",
    "cover-grid",
    "metric-card",
    "list",
    "button-hints"
  ]
}
```

This metadata is useful for builders, compatible apps, and future tooling. Runtime drawing is still driven by `screens`, `components`, `metrics`, and `assets`.

## Extensions

Put app-specific data under `extensions.<namespace>`:

```json
"extensions": {
  "myapp": {
    "schema": 1,
    "dashboard": {
      "enabled": true
    }
  }
}
```

Rules:

- Keep shared visual behavior in `metrics`, `components`, `screens`, `assets`, and `freeInkUI`.
- Keep app-only behavior under `extensions`.
- Add an extension-local `schema` if that extension may evolve.
- Keep extension payloads compact; discovery still parses the theme JSON.

## Publishing And Updates

After changing a hosted theme:

1. Bump `version` in `sd-themes/<theme-id>/theme.json`.
2. Regenerate icons if `assets.freeInkIcons` changed.
3. Regenerate `sd-themes/themes.json`.
4. Commit the theme folder and regenerated manifest together.

Regenerate the manifest:

```bash
python3 scripts/generate-theme-manifest.py \
  --root sd-themes \
  --base-url https://raw.githubusercontent.com/crosspoint-reader/crosspoint-reader/feat-sd-theme-system/sd-themes \
  --output sd-themes/themes.json
```

The manifest contains file URLs, sizes, CRC32 values, theme ids, names, descriptions, versions, and total sizes. It does not embed full theme JSON content; every file is downloaded by URL to keep device heap usage bounded.

## Validation

Before publishing:

```bash
for f in sd-themes/themes.json sd-themes/*/theme.json; do
  python3 -m json.tool "$f" >/dev/null
done

python3 scripts/generate-theme-manifest.py \
  --root sd-themes \
  --base-url https://raw.githubusercontent.com/crosspoint-reader/crosspoint-reader/feat-sd-theme-system/sd-themes \
  --output sd-themes/themes.json

git diff --check
```

Run the firmware build when changing parser or renderer code.

On device, verify:

- Theme appears in Settings after install or download.
- Update available appears after `version` is bumped and `themes.json` is regenerated.
- Home layout scales and has no unintended borders.
- Covers fill the intended width and height.
- Button hints match the device's physical button layout.
- Settings and reader menu match the theme style.
- Icons render from the firmware registry or SD fallback BMPs.
- Theme UI fonts load when present and fall back cleanly when a size is missing.
