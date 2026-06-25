#include "FreeInkThemeIconRegistry.h"

#include <cstring>

#include "freeink_theme_icons.generated.h"

namespace {
struct IconEntry {
  const char* name;
  int size;
  const freeink::Icon* icon;
};

const IconEntry ICONS[] = {
    {"book-open", 24, &icon_book_open_24},
    {"book-open", 32, &icon_book_open_32},
    {"bookmark", 24, &icon_bookmark_24},
    {"bookmark", 32, &icon_bookmark_32},
    {"clock-3", 24, &icon_clock_3_24},
    {"clock-3", 32, &icon_clock_3_32},
    {"download", 24, &icon_download_24},
    {"download", 32, &icon_download_32},
    {"file", 24, &icon_file_24},
    {"file", 32, &icon_file_32},
    {"file-text", 24, &icon_file_text_24},
    {"file-text", 32, &icon_file_text_32},
    {"folder", 24, &icon_folder_24},
    {"folder", 32, &icon_folder_32},
    {"image", 24, &icon_image_24},
    {"image", 32, &icon_image_32},
    {"library", 24, &icon_library_24},
    {"library", 32, &icon_library_32},
    {"router", 24, &icon_router_24},
    {"router", 32, &icon_router_32},
    {"sliders-horizontal", 24, &icon_sliders_horizontal_24},
    {"sliders-horizontal", 32, &icon_sliders_horizontal_32},
    {"wifi", 24, &icon_wifi_24},
    {"wifi", 32, &icon_wifi_32},
};
}  // namespace

const freeink::Icon* findFreeInkThemeIcon(const char* lucideName, int size) {
  if (lucideName == nullptr || size <= 0) return nullptr;
  int bestSize = 0;
  const freeink::Icon* best = nullptr;
  for (const auto& entry : ICONS) {
    if (std::strcmp(entry.name, lucideName) != 0) continue;
    if (entry.size == size) return entry.icon;
    if (best == nullptr || (entry.size >= size && (bestSize < size || entry.size < bestSize)) ||
        (bestSize < size && entry.size > bestSize)) {
      best = entry.icon;
      bestSize = entry.size;
    }
  }
  return best;
}
