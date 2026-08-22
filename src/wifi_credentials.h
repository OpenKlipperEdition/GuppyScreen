#ifndef __WIFI_CREDENTIALS_H__
#define __WIFI_CREDENTIALS_H__

#include <string>
#include <vector>

struct WifiCredential {
  std::string ssid;
  std::string password;
};

struct WifiCredentialParseResult {
  std::vector<WifiCredential> credentials;
  std::string error;

  bool ok() const { return error.empty() && !credentials.empty(); }
};

// Parse the deliberately simple USB provisioning format:
//
//   SSID=My Network
//
//   PASSWORD=correct horse battery staple
//
// Blank lines may separate SSID and PASSWORD and also separate multiple
// networks. Lines beginning with '#' or ';' are comments. Keys are
// case-insensitive; "psk" is accepted as an alias for
// "password". Values may be surrounded by matching single or double quotes.
WifiCredentialParseResult parse_wifi_credentials(const std::string &contents);

#endif // __WIFI_CREDENTIALS_H__
