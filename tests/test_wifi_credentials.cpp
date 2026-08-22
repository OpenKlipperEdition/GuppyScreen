#include "minitest.h"
#include "../src/wifi_credentials.h"

TEST(WifiCredentials, parses_one_network_and_comments) {
  auto result = parse_wifi_credentials(
      "# Put this file at /opt/printer_data/gcodes/USB/sda1/guppy-wifi.conf\n"
      "SSID=Network-Name\n"
      "\n"
      "PASSWORD=Network-Password\n");

  ASSERT_TRUE(result.ok());
  ASSERT_EQ(result.credentials.size(), static_cast<size_t>(1));
  ASSERT_EQ(result.credentials[0].ssid, std::string("Network-Name"));
  ASSERT_EQ(result.credentials[0].password, std::string("Network-Password"));
}

TEST(WifiCredentials, parses_multiple_networks_and_psk_alias) {
  auto result = parse_wifi_credentials(
      "ssid=first\npassword=12345678\n\n"
      "SSID=second\nPSK=abcdefgh\n");

  ASSERT_TRUE(result.ok());
  ASSERT_EQ(result.credentials.size(), static_cast<size_t>(2));
  ASSERT_EQ(result.credentials[1].ssid, std::string("second"));
  ASSERT_EQ(result.credentials[1].password, std::string("abcdefgh"));
}

TEST(WifiCredentials, accepts_quotes_bom_and_colon_separator) {
  auto result = parse_wifi_credentials(
      "\xef\xbb\xbfSSID: \"Guest WiFi\"\n"
      "password: '12345678'\n");

  ASSERT_TRUE(result.ok());
  ASSERT_EQ(result.credentials[0].ssid, std::string("Guest WiFi"));
  ASSERT_EQ(result.credentials[0].password, std::string("12345678"));
}

TEST(WifiCredentials, rejects_incomplete_and_invalid_records) {
  ASSERT_FALSE(parse_wifi_credentials("ssid=only-ssid\n").ok());
  ASSERT_FALSE(parse_wifi_credentials("ssid=short\npassword=short\n").ok());
  ASSERT_FALSE(parse_wifi_credentials("ssid=network\npassword=12345678\nextra=x\n").ok());
}

TEST(WifiCredentials, rejects_newlines_inside_quoted_values) {
  auto result = parse_wifi_credentials("ssid=network\npassword=\"1234567\\n8\"\n");
  ASSERT_FALSE(result.ok());
  ASSERT_TRUE(result.error.find("newlines") != std::string::npos);
}

int main() {
  return minitest::run_all("wifi_credentials") > 0 ? 1 : 0;
}
