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
    {"arrow-down", 24, &icon_arrow_down_24},
    {"arrow-down", 32, &icon_arrow_down_32},
    {"arrow-left", 24, &icon_arrow_left_24},
    {"arrow-left", 32, &icon_arrow_left_32},
    {"arrow-right", 24, &icon_arrow_right_24},
    {"arrow-right", 32, &icon_arrow_right_32},
    {"arrow-up", 24, &icon_arrow_up_24},
    {"arrow-up", 32, &icon_arrow_up_32},
    {"book-open", 24, &icon_book_open_24},
    {"book-open", 32, &icon_book_open_32},
    {"bookmark", 24, &icon_bookmark_24},
    {"bookmark", 32, &icon_bookmark_32},
    {"check", 24, &icon_check_24},
    {"check", 32, &icon_check_32},
    {"clock-3", 24, &icon_clock_3_24},
    {"clock-3", 32, &icon_clock_3_32},
    {"corner-down-left", 24, &icon_corner_down_left_24},
    {"corner-down-left", 32, &icon_corner_down_left_32},
    {"download", 24, &icon_download_24},
    {"download", 32, &icon_download_32},
    {"file", 24, &icon_file_24},
    {"file", 32, &icon_file_32},
    {"file-text", 24, &icon_file_text_24},
    {"file-text", 32, &icon_file_text_32},
    {"folder", 24, &icon_folder_24},
    {"folder", 32, &icon_folder_32},
    {"folder-open", 24, &icon_folder_open_24},
    {"folder-open", 32, &icon_folder_open_32},
    {"image", 24, &icon_image_24},
    {"image", 32, &icon_image_32},
    {"library", 24, &icon_library_24},
    {"library", 32, &icon_library_32},
    {"menu", 24, &icon_menu_24},
    {"menu", 32, &icon_menu_32},
    {"router", 24, &icon_router_24},
    {"router", 32, &icon_router_32},
    {"save", 24, &icon_save_24},
    {"save", 32, &icon_save_32},
    {"sliders-horizontal", 24, &icon_sliders_horizontal_24},
    {"sliders-horizontal", 32, &icon_sliders_horizontal_32},
    {"wifi", 24, &icon_wifi_24},
    {"wifi", 32, &icon_wifi_32},
    {"x", 24, &icon_x_24},
    {"x", 32, &icon_x_32},
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
