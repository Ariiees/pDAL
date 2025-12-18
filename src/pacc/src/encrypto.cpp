#include "pacc/encrypto.h"

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/err.h>

#include <fstream>
#include <iostream>
#include <stdexcept>

namespace avs {
namespace crypto {

AESGCMEncryptor::AESGCMEncryptor() {
  key_.resize(32); // 256 bit
  if (RAND_bytes(key_.data(), static_cast<int>(key_.size())) != 1) {
    throw std::runtime_error("Failed to generate random AES key");
  }
}

AESGCMEncryptor::AESGCMEncryptor(const std::vector<unsigned char>& key_bytes) : key_(key_bytes) {
  if (key_.size() != 16 && key_.size() != 32) {
    throw std::runtime_error("AES key must be 16 or 32 bytes");
  }
}

AESGCMEncryptor AESGCMEncryptor::FromKeyFile(const std::string& key_path) {
  std::vector<unsigned char> key_bytes;
  if (!readFile(key_path, key_bytes)) {
    throw std::runtime_error("Failed to read key file: " + key_path);
  }
  return AESGCMEncryptor(key_bytes);
}

bool AESGCMEncryptor::saveKey(const std::string& key_path) const {
  return writeFile(key_path, key_);
}

bool AESGCMEncryptor::encrypt(const std::vector<unsigned char>& plain,
                              std::vector<unsigned char>& cipher,
                              std::vector<unsigned char>& iv,
                              std::vector<unsigned char>& tag) const {
  iv.resize(IV_LENGTH);
  if (RAND_bytes(iv.data(), IV_LENGTH) != 1) {
    std::cerr << "IV generation failed\n";
    return false;
  }

  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (!ctx) {
    std::cerr << "EVP_CIPHER_CTX_new failed\n";
    return false;
  }

  bool ok = true;
  int len = 0;
  int out_len = 0;

  const EVP_CIPHER* cipher_type = (key_.size() == 32) ? EVP_aes_256_gcm() : EVP_aes_128_gcm();

  if (ok && EVP_EncryptInit_ex(ctx, cipher_type, nullptr, nullptr, nullptr) != 1) ok = false;
  if (ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, IV_LENGTH, nullptr) != 1) ok = false;
  if (ok && EVP_EncryptInit_ex(ctx, nullptr, nullptr, key_.data(), iv.data()) != 1) ok = false;

  if (ok) {
    cipher.resize(plain.size() + EVP_CIPHER_block_size(cipher_type));
    if (EVP_EncryptUpdate(ctx, cipher.data(), &len,
                          plain.data(), static_cast<int>(plain.size())) != 1) {
      ok = false;
    } else {
      out_len = len;
    }
  }

  if (ok) {
    if (EVP_EncryptFinal_ex(ctx, cipher.data() + out_len, &len) != 1) {
      ok = false;
    } else {
      out_len += len;
      cipher.resize(out_len);
    }
  }

  if (ok) {
    tag.resize(TAG_LENGTH);
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, TAG_LENGTH, tag.data()) != 1) {
      ok = false;
    }
  }

  EVP_CIPHER_CTX_free(ctx);
  return ok;
}

bool AESGCMEncryptor::decrypt(const std::vector<unsigned char>& cipher,
                              const std::vector<unsigned char>& iv,
                              const std::vector<unsigned char>& tag,
                              std::vector<unsigned char>& plain) const {
  if (static_cast<int>(iv.size()) != IV_LENGTH || static_cast<int>(tag.size()) != TAG_LENGTH) {
    std::cerr << "Invalid IV or tag size\n";
    return false;
  }

  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (!ctx) {
    std::cerr << "EVP_CIPHER_CTX_new failed\n";
    return false;
  }

  bool ok = true;
  int len = 0;
  int out_len = 0;

  const EVP_CIPHER* cipher_type = (key_.size() == 32) ? EVP_aes_256_gcm() : EVP_aes_128_gcm();

  if (ok && EVP_DecryptInit_ex(ctx, cipher_type, nullptr, nullptr, nullptr) != 1) ok = false;
  if (ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, IV_LENGTH, nullptr) != 1) ok = false;
  if (ok && EVP_DecryptInit_ex(ctx, nullptr, nullptr, key_.data(), iv.data()) != 1) ok = false;

  if (ok) {
    plain.resize(cipher.size());
    if (EVP_DecryptUpdate(ctx, plain.data(), &len,
                          cipher.data(), static_cast<int>(cipher.size())) != 1) {
      ok = false;
    } else {
      out_len = len;
    }
  }

  if (ok) {
    // set expected tag before final
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, TAG_LENGTH,
                             const_cast<unsigned char*>(tag.data())) != 1) {
      ok = false;
    }
  }

  if (ok) {
    const int ret = EVP_DecryptFinal_ex(ctx, plain.data() + out_len, &len);
    if (ret > 0) {
      out_len += len;
      plain.resize(out_len);
    } else {
      std::cerr << "Authentication failed in GCM decrypt\n";
      ok = false;
    }
  }

  EVP_CIPHER_CTX_free(ctx);
  return ok;
}

std::vector<unsigned char> AESGCMEncryptor::packMeta(const std::vector<unsigned char>& iv,
                                                     const std::vector<unsigned char>& tag) {
  std::vector<unsigned char> meta;
  meta.reserve(iv.size() + tag.size());
  meta.insert(meta.end(), iv.begin(), iv.end());
  meta.insert(meta.end(), tag.begin(), tag.end());
  return meta;
}

bool AESGCMEncryptor::unpackMeta(const std::vector<unsigned char>& meta,
                                 std::vector<unsigned char>& iv,
                                 std::vector<unsigned char>& tag) {
  if (static_cast<int>(meta.size()) != IV_LENGTH + TAG_LENGTH) return false;
  iv.assign(meta.begin(), meta.begin() + IV_LENGTH);
  tag.assign(meta.begin() + IV_LENGTH, meta.end());
  return true;
}

bool AESGCMEncryptor::writeFile(const std::string& path, const std::vector<unsigned char>& data) {
  std::ofstream f(path, std::ios::binary);
  if (!f.is_open()) return false;
  f.write(reinterpret_cast<const char*>(data.data()),
          static_cast<std::streamsize>(data.size()));
  return f.good();
}

bool AESGCMEncryptor::readFile(const std::string& path, std::vector<unsigned char>& out) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f.is_open()) return false;
  const std::streamsize sz = f.tellg();
  f.seekg(0, std::ios::beg);
  out.resize(static_cast<size_t>(sz));
  if (!f.read(reinterpret_cast<char*>(out.data()), sz)) return false;
  return true;
}

} // namespace crypto
} // namespace avs
