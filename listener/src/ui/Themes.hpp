// The themes this client knows: the ones it ships, then any in <data dir>/themes/*.json. A file
// that does not read is skipped with a warning, never fatal.
#pragma once

#include "ui/ThemeSpec.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace ke::ui {

class ThemeLibrary {
  public:
    ThemeLibrary();
    void loadDirectory(const std::filesystem::path& dir);
    const std::vector<ThemeSpec>& all() const { return themes_; }
    // By name, else the default.
    const ThemeSpec& named(const std::string& name) const;

  private:
    std::vector<ThemeSpec> themes_;
};

// The one library this process has.
ThemeLibrary& themeLibrary();

} // namespace ke::ui
