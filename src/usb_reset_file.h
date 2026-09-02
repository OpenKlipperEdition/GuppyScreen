#ifndef __USB_RESET_FILE_H__
#define __USB_RESET_FILE_H__

#include <string>
#include <vector>

std::vector<std::string> get_usb_touch_reset_roots();
std::vector<std::string> get_usb_touch_reset_names();
bool find_usb_touch_reset_file(const std::vector<std::string> &roots,
                               const std::vector<std::string> &names,
                               std::string &found_path);
bool apply_touch_calibration_reset_from_usb();

#endif // __USB_RESET_FILE_H__
