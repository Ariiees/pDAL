#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/err.h>
#include <iostream>
#include <fstream>
#include <vector>
#include <filesystem>
#include <chrono>
#include <algorithm>

namespace fs = std::filesystem;

class AESGCMDecryptor {
private:
    std::vector<unsigned char> key;
    static const int TAG_LENGTH = 16;
    static const int IV_LENGTH = 12;

public:
    AESGCMDecryptor(const std::vector<unsigned char>& encryption_key) : key(encryption_key) {
        if (key.size() != 32 && key.size() != 16) {
            throw std::runtime_error("Key must be 128 or 256 bits");
        }
    }

    bool decrypt(const std::vector<unsigned char>& ciphertext,
                 const std::vector<unsigned char>& iv,
                 const std::vector<unsigned char>& tag,
                 std::vector<unsigned char>& plaintext) {
        
        EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
        if (!ctx) {
            std::cerr << "Failed to create context" << std::endl;
            return false;
        }

        int len;
        int plaintext_len;

        if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) {
            std::cerr << "Failed to initialize decryption" << std::endl;
            EVP_CIPHER_CTX_free(ctx);
            return false;
        }

        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, IV_LENGTH, nullptr) != 1) {
            std::cerr << "Failed to set IV length" << std::endl;
            EVP_CIPHER_CTX_free(ctx);
            return false;
        }

        if (EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.data(), iv.data()) != 1) {
            std::cerr << "Failed to set key and IV" << std::endl;
            EVP_CIPHER_CTX_free(ctx);
            return false;
        }

        plaintext.resize(ciphertext.size());

        if (EVP_DecryptUpdate(ctx, plaintext.data(), &len, ciphertext.data(), ciphertext.size()) != 1) {
            std::cerr << "Decryption failed" << std::endl;
            EVP_CIPHER_CTX_free(ctx);
            return false;
        }
        plaintext_len = len;

        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, TAG_LENGTH, 
                                const_cast<unsigned char*>(tag.data())) != 1) {
            std::cerr << "Failed to set tag" << std::endl;
            EVP_CIPHER_CTX_free(ctx);
            return false;
        }

        int ret = EVP_DecryptFinal_ex(ctx, plaintext.data() + len, &len);
        EVP_CIPHER_CTX_free(ctx);

        if (ret > 0) {
            plaintext_len += len;
            plaintext.resize(plaintext_len);
            return true;
        } else {
            std::cerr << "Authentication failed - data may be corrupted or tampered!" << std::endl;
            return false;
        }
    }
};

bool readFile(const std::string& filename, std::vector<unsigned char>& data) {
    std::ifstream file(filename, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::cerr << "Failed to open file: " << filename << std::endl;
        return false;
    }

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    data.resize(size);
    if (!file.read(reinterpret_cast<char*>(data.data()), size)) {
        std::cerr << "Failed to read file: " << filename << std::endl;
        return false;
    }

    return true;
}

bool writeFile(const std::string& filename, const std::vector<unsigned char>& data) {
    std::ofstream file(filename, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Failed to create file: " << filename << std::endl;
        return false;
    }

    file.write(reinterpret_cast<const char*>(data.data()), data.size());
    return file.good();
}

int main(int argc, char* argv[]) {
    if (argc != 4) {
        std::cout << "Usage: " << argv[0] << " <encrypted_folder> <key_file> <output_folder>" << std::endl;
        std::cout << "Example: " << argv[0] << " ./encrypted_frames ./encrypted_frames/encryption_key.bin ./decrypted_frames" << std::endl;
        return 1;
    }

    std::string encrypted_folder = argv[1];
    std::string key_file = argv[2];
    std::string output_folder = argv[3];

    // Create output folder
    try {
        fs::create_directories(output_folder);
    } catch (const std::exception& e) {
        std::cerr << "Failed to create output directory: " << e.what() << std::endl;
        return 1;
    }

    // Load encryption key
    std::vector<unsigned char> key;
    if (!readFile(key_file, key)) {
        std::cerr << "Failed to load encryption key from: " << key_file << std::endl;
        return 1;
    }

    std::cout << "Loaded encryption key from: " << key_file << std::endl << std::endl;

    // Initialize decryptor
    AESGCMDecryptor decryptor(key);

    // Process all encrypted files
    std::vector<fs::path> encrypted_files;
    for (const auto& entry : fs::directory_iterator(encrypted_folder)) {
        if (entry.is_regular_file() && entry.path().extension() == ".enc") {
            encrypted_files.push_back(entry.path());
        }
    }

    if (encrypted_files.empty()) {
        std::cerr << "No encrypted files (.enc) found in: " << encrypted_folder << std::endl;
        return 1;
    }

    std::cout << "Found " << encrypted_files.size() << " encrypted files to decrypt" << std::endl << std::endl;
    std::sort(encrypted_files.begin(), encrypted_files.end());

    int successful = 0;
    int failed = 0;
    double total_time = 0.0;

    for (const auto& enc_path : encrypted_files) {
        auto start_time = std::chrono::high_resolution_clock::now();

        std::cout << "Processing: " << enc_path.filename() << " ... ";

        // Read encrypted data
        std::vector<unsigned char> ciphertext;
        if (!readFile(enc_path.string(), ciphertext)) {
            std::cout << "FAILED (read error)" << std::endl;
            failed++;
            continue;
        }

        // Read metadata (IV and tag)
        std::string metadata_file = encrypted_folder + "/metadata/" + enc_path.stem().string() + ".meta";
        std::vector<unsigned char> metadata;
        if (!readFile(metadata_file, metadata)) {
            std::cout << "FAILED (metadata read error)" << std::endl;
            failed++;
            continue;
        }

        if (metadata.size() != 28) {  // 12 bytes IV + 16 bytes tag
            std::cout << "FAILED (invalid metadata size)" << std::endl;
            failed++;
            continue;
        }

        std::vector<unsigned char> iv(metadata.begin(), metadata.begin() + 12);
        std::vector<unsigned char> tag(metadata.begin() + 12, metadata.end());

        // Decrypt
        std::vector<unsigned char> plaintext;
        if (!decryptor.decrypt(ciphertext, iv, tag, plaintext)) {
            std::cout << "FAILED (decryption error)" << std::endl;
            failed++;
            continue;
        }

        // Read the original filename from manifest
        std::string manifest_file = encrypted_folder + "/metadata/" + enc_path.stem().string() + ".filename";
        std::string original_filename;
        std::ifstream manifest_in(manifest_file);
        if (manifest_in.is_open()) {
            std::getline(manifest_in, original_filename);
            manifest_in.close();
        }
        
        // Use manifest filename if available, otherwise default to .bin
        std::string output_file;
        if (!original_filename.empty()) {
            output_file = output_folder + "/" + original_filename;
        } else {
            // Fallback: try to preserve extension from encrypted filename
            output_file = output_folder + "/" + enc_path.stem().string() + ".bin";
        }
        
        if (!writeFile(output_file, plaintext)) {
            std::cout << "FAILED (write error)" << std::endl;
            failed++;
            continue;
        }

        auto end_time = std::chrono::high_resolution_clock::now();
        double elapsed = std::chrono::duration<double, std::milli>(end_time - start_time).count();
        total_time += elapsed;

        double throughput = (plaintext.size() / 1024.0 / 1024.0) / (elapsed / 1000.0);
        std::cout << "OK (" << elapsed << " ms, " << throughput << " MB/s)" << std::endl;
        
        successful++;
    }

    std::cout << std::endl << "=== Decryption Summary ===" << std::endl;
    std::cout << "Total files: " << encrypted_files.size() << std::endl;
    std::cout << "Successful: " << successful << std::endl;
    std::cout << "Failed: " << failed << std::endl;
    if (successful > 0) {
        std::cout << "Average time per frame: " << (total_time / successful) << " ms" << std::endl;
    }
    std::cout << std::endl << "Decrypted files saved to: " << output_folder << std::endl;

    return (failed == 0) ? 0 : 1;
}
