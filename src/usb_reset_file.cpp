#include "usb_reset_file.h"

#include "config.h"

#include <experimental/filesystem>
#include <fstream>

namespace fs = std::experimental::filesystem;

std::vector<std::string> get_usb_touch_reset_roots() {
  return {
    "/opt/printer_data/gcodes/USB/sda1",
    "/usb",
    "/tmp/udisk",
    "/media/usb",
    "/mnt/usb"
  };
}

std::vector<std::string> get_usb_touch_reset_names() {
  return {".guppy-reset-touch", ".guppy-reset-touch.txt",
          "guppy-reset-touch", "guppy-reset-touch.txt"};
}

bool find_usb_touch_reset_file(const std::vector<std::string> &roots,
                               const std::vector<std::string> &names,
                               std::string &found_path) {
  for (const auto &root : roots) {
    for (const auto &name : names) {
      const fs::path candidate = fs::path(root) / name;
      if (fs::exists(candidate) && fs::is_regular_file(candidate)) {
        found_path = candidate.string();
        return true;
      }
    }
  }
  return false;
}

bool apply_touch_calibration_reset_from_usb() {
  std::string path;
  if (!find_usb_touch_reset_file(get_usb_touch_reset_roots(), get_usb_touch_reset_names(), path)) {
    return false;
  }

  try {
    fs::remove(path);
  } catch (const fs::filesystem_error &) {
    // Best effort: if the file cannot be removed, keep the reset request
    // effective in config so the calibration wizard still runs on next boot.
  }

  Config *conf = Config::get_instance();
  conf->set<bool>("/touch_calibrated", true);
  conf->set<json>("/touch_calibration_coeff", json());
  conf->save();
  return true;
}
