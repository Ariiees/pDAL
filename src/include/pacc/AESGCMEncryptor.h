#pragma once

#include <vector>
#include <string>

namespace avs {
namespace crypto {

class AESGCMEncryptor {
public:
  static constexpr int TAG_LENGTH = 16; // 128 bit authentication tag
  static constexpr int IV_LENGTH  = 12; // 96 bit IV as recommended for GCM

  // Create a new encryptor with a freshly generated 256 bit key
  AESGCMEncryptor();

  // Create from an existing key. Key must be 16 or 32 bytes
  explicit AESGCMEncryptor(const std::vector<unsigned char>& key_bytes);

  // Factory that loads a raw key from file
  static AESGCMEncryptor FromKeyFile(const std::string& key_path);

  // Access the key
  const std::vector<unsigned char>& key() const { return key_; }

  // Save the raw key to a file
  bool saveKey(const std::string& key_path) const;

  // AES GCM encrypt
  // Inputs: plain
  // Outputs: cipher, iv, tag
  // Returns true on success
  bool encrypt(const std::vector<unsigned char>& plain,
               std::vector<unsigned char>& cipher,
               std::vector<unsigned char>& iv,
               std::vector<unsigned char>& tag) const;

  // AES GCM decrypt
  // Returns true on success and authentication
  bool decrypt(const std::vector<unsigned char>& cipher,
               const std::vector<unsigned char>& iv,
               const std::vector<unsigned char>& tag,
               std::vector<unsigned char>& plain) const;

  // Helpers to serialize iv and tag to a single blob and parse it back
  static std::vector<unsigned char> packMeta(const std::vector<unsigned char>& iv,
                                             const std::vector<unsigned char>& tag);
  static bool unpackMeta(const std::vector<unsigned char>& meta,
                         std::vector<unsigned char>& iv,
                         std::vector<unsigned char>& tag);

  // Small file utilities used by many pipelines
  static bool writeFile(const std::string& path, const std::vector<unsigned char>& data);
  static bool readFile(const std::string& path, std::vector<unsigned char>& out);

private:
  std::vector<unsigned char> key_;
};

} // namespace crypto
} // namespace avs
