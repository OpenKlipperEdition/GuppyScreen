#include "wifi_credentials.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace {

std::string trim(const std::string &value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return "";
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return (char)std::tolower(c); });
  return value;
}

std::string unquote(std::string value) {
  value = trim(value);
  if (value.size() < 2) return value;

  const char quote = value.front();
  if ((quote != '"' && quote != '\'') || value.back() != quote) return value;

  std::string out;
  out.reserve(value.size() - 2);
  for (size_t i = 1; i + 1 < value.size(); ++i) {
    if (value[i] == '\\' && i + 2 < value.size()) {
      const char escaped = value[++i];
      if (escaped == 'n') out.push_back('\n');
      else if (escaped == 'r') out.push_back('\r');
      else if (escaped == 't') out.push_back('\t');
      else out.push_back(escaped);
    } else {
      out.push_back(value[i]);
    }
  }
  return out;
}

bool has_control_char(const std::string &value) {
  return std::any_of(value.begin(), value.end(), [](unsigned char c) {
    return c == 0 || c == '\r' || c == '\n';
  });
}

bool valid_credential(const WifiCredential &credential, std::string &error) {
  if (credential.ssid.empty()) {
    error = "SSID is empty";
    return false;
  }
  if (credential.ssid.size() > 32) {
    error = "SSID is longer than 32 bytes";
    return false;
  }
  if (has_control_char(credential.ssid) || has_control_char(credential.password)) {
    error = "SSID and password must not contain newlines";
    return false;
  }
  // WPA passphrases are 8-63 characters; a 64-character hexadecimal PSK is
  // also valid and is accepted by wpa_supplicant.
  if (credential.password.size() < 8 || credential.password.size() > 64) {
    error = "password must be between 8 and 64 bytes";
    return false;
  }
  return true;
}

}  // namespace

WifiCredentialParseResult parse_wifi_credentials(const std::string &contents) {
  WifiCredentialParseResult result;
  std::istringstream input(contents);
  std::string line;
  WifiCredential current;
  bool have_ssid = false;
  bool have_password = false;
  size_t line_number = 0;

  auto finish = [&]() -> bool {
    if (!have_ssid && !have_password) return true;
    if (!have_ssid || !have_password) {
      result.error = "each network must contain both ssid and password";
      return false;
    }
    std::string validation_error;
    if (!valid_credential(current, validation_error)) {
      result.error = validation_error;
      return false;
    }
    result.credentials.push_back(current);
    current = WifiCredential{};
    have_ssid = false;
    have_password = false;
    return true;
  };

  while (std::getline(input, line)) {
    ++line_number;
    if (line_number == 1 && line.size() >= 3
        && (unsigned char)line[0] == 0xef
        && (unsigned char)line[1] == 0xbb
        && (unsigned char)line[2] == 0xbf) {
      line.erase(0, 3);  // tolerate a UTF-8 BOM from Windows editors
    }

    line = trim(line);
    if (line.empty()) {
      // Allow the human-friendly SSID / blank line / PASSWORD layout. Once a
      // complete record exists, a blank line starts the next record.
      if (have_ssid && have_password && !finish()) return result;
      continue;
    }
    if (line.front() == '#' || line.front() == ';') continue;

    const auto separator = line.find_first_of("=:");
    if (separator == std::string::npos) {
      result.error = "line " + std::to_string(line_number) + " is not key=value";
      return result;
    }

    const std::string key = lower(trim(line.substr(0, separator)));
    const std::string value = unquote(line.substr(separator + 1));
    if (key == "ssid") {
      // Starting another SSID without a blank separator is still unambiguous
      // and makes hand-written files less surprising.
      if (have_ssid && !finish()) return result;
      current.ssid = value;
      have_ssid = true;
    } else if (key == "password" || key == "psk") {
      current.password = value;
      have_password = true;
    } else {
      result.error = "line " + std::to_string(line_number)
                   + " has unsupported key '" + key + "'";
      return result;
    }
  }

  if (!finish()) return result;
  if (result.credentials.empty()) result.error = "no WiFi credentials found";
  return result;
}
