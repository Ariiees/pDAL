#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/err.h>
#include <iostream>
#include <fstream>
#include <vector>
#include <filesystem>
#include <chrono>
#include <cstring>
#include <algorithm>

namespace fs = std::filesystem;

class AESGCMEncryptor {
private:
    std::vector<unsigned char> key;
    static const int TAG_LENGTH = 16;  // 128-bit authentication tag
    static const int IV_LENGTH = 12;   // 96-bit IV (recommended for GCM)

public:
    AESGCMEncryptor() {
        // Generate a random 256-bit key
        key.resize(32);  // 32 bytes = 256 bits
        if (RAND_bytes(key.data(), key.size()) != 1) {
            throw std::runtime_error("Failed to generate random key");
        }
    }

    // Constructor with existing key
    AESGCMEncryptor(const std::vector<unsigned char>& existing_key) : key(existing_key) {
        if (key.size() != 32 && key.size() != 16) {
            throw std::runtime_error("Key must be 128 or 256 bits");
        }
    }

    // Encrypt data with AES-GCM
    bool encrypt(const std::vector<unsigned char>& plaintext,
                 std::vector<unsigned char>& ciphertext,
                 std::vector<unsigned char>& iv,
                 std::vector<unsigned char>& tag) {
        
        // Generate random IV
        iv.resize(IV_LENGTH);
        if (RAND_bytes(iv.data(), IV_LENGTH) != 1) {
            std::cerr << "Failed to generate IV" << std::endl;
            return false;
        }

        EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
        if (!ctx) {
            std::cerr << "Failed to create context" << std::endl;
            return false;
        }

        int len;
        int ciphertext_len;

        // Initialize encryption
        if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) {
            std::cerr << "Failed to initialize encryption" << std::endl;
            EVP_CIPHER_CTX_free(ctx);
            return false;
        }

        // Set IV length
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, IV_LENGTH, nullptr) != 1) {
            std::cerr << "Failed to set IV length" << std::endl;
            EVP_CIPHER_CTX_free(ctx);
            return false;
        }

        // Initialize key and IV
        if (EVP_EncryptInit_ex(ctx, nullptr, nullptr, key.data(), iv.data()) != 1) {
            std::cerr << "Failed to set key and IV" << std::endl;
            EVP_CIPHER_CTX_free(ctx);
            return false;
        }

        // Allocate space for ciphertext
        ciphertext.resize(plaintext.size() + EVP_CIPHER_CTX_block_size(ctx));

        // Encrypt
        if (EVP_EncryptUpdate(ctx, ciphertext.data(), &len, plaintext.data(), plaintext.size()) != 1) {
            std::cerr << "Encryption failed" << std::endl;
            EVP_CIPHER_CTX_free(ctx);
            return false;
        }
        ciphertext_len = len;

        // Finalize encryption
        if (EVP_EncryptFinal_ex(ctx, ciphertext.data() + len, &len) != 1) {
            std::cerr << "Failed to finalize encryption" << std::endl;
            EVP_CIPHER_CTX_free(ctx);
            return false;
        }
        ciphertext_len += len;
        ciphertext.resize(ciphertext_len);

        // Get authentication tag
        tag.resize(TAG_LENGTH);
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, TAG_LENGTH, tag.data()) != 1) {
            std::cerr << "Failed to get tag" << std::endl;
            EVP_CIPHER_CTX_free(ctx);
            return false;
        }

        EVP_CIPHER_CTX_free(ctx);
        return true;
    }

    // Decrypt data with AES-GCM
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

        // Initialize decryption
        if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) {
            std::cerr << "Failed to initialize decryption" << std::endl;
            EVP_CIPHER_CTX_free(ctx);
            return false;
        }

        // Set IV length
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, IV_LENGTH, nullptr) != 1) {
            std::cerr << "Failed to set IV length" << std::endl;
            EVP_CIPHER_CTX_free(ctx);
            return false;
        }

        // Initialize key and IV
        if (EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.data(), iv.data()) != 1) {
            std::cerr << "Failed to set key and IV" << std::endl;
            EVP_CIPHER_CTX_free(ctx);
            return false;
        }

        // Allocate space for plaintext
        plaintext.resize(ciphertext.size());

        // Decrypt
        if (EVP_DecryptUpdate(ctx, plaintext.data(), &len, ciphertext.data(), ciphertext.size()) != 1) {
            std::cerr << "Decryption failed" << std::endl;
            EVP_CIPHER_CTX_free(ctx);
            return false;
        }
        plaintext_len = len;

        // Set expected tag
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, TAG_LENGTH, 
                                const_cast<unsigned char*>(tag.data())) != 1) {
            std::cerr << "Failed to set tag" << std::endl;
            EVP_CIPHER_CTX_free(ctx);
            return false;
        }

        // Finalize decryption (this also verifies the tag)
        int ret = EVP_DecryptFinal_ex(ctx, plaintext.data() + len, &len);
        EVP_CIPHER_CTX_free(ctx);

        if (ret > 0) {
            plaintext_len += len;
            plaintext.resize(plaintext_len);
            return true;
        } else {
            std::cerr << "Authentication failed - data may be corrupted!" << std::endl;
            return false;
        }
    }

    const std::vector<unsigned char>& getKey() const { return key; }
};

// Read file into vector
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

// Write data to file
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
    if (argc != 3) {
        std::cout << "Usage: " << argv[0] << " <input_folder> <output_folder>" << std::endl;
        std::cout << "Example: " << argv[0] << " ./kitti_frames ./encrypted_frames" << std::endl;
        return 1;
    }

    std::string input_folder = argv[1];
    std::string output_folder = argv[2];

    // Create output folder if it doesn't exist
    try {
        fs::create_directories(output_folder);
        fs::create_directories(output_folder + "/metadata");
    } catch (const std::exception& e) {
        std::cerr << "Failed to create output directories: " << e.what() << std::endl;
        return 1;
    }

    // Initialize encryptor
    AESGCMEncryptor encryptor;
    
    // Save the key for later decryption
    std::string key_file = output_folder + "/encryption_key.bin";
    if (!writeFile(key_file, encryptor.getKey())) {
        std::cerr << "Failed to save encryption key" << std::endl;
        return 1;
    }
    std::cout << "Encryption key saved to: " << key_file << std::endl;
    std::cout << "WARNING: Keep this key secure! It's needed for decryption." << std::endl << std::endl;

    // Process all image files in the input folder
    std::vector<fs::path> image_files;
    for (const auto& entry : fs::directory_iterator(input_folder)) {
        if (entry.is_regular_file()) {
            std::string ext = entry.path().extension().string();
            // Common image extensions
            if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || 
                ext == ".bmp" || ext == ".tiff" || ext == ".ppm") {
                image_files.push_back(entry.path());
            }
        }
    }

    if (image_files.empty()) {
        std::cerr << "No image files found in: " << input_folder << std::endl;
        return 1;
    }

    std::cout << "Found " << image_files.size() << " image files to encrypt" << std::endl << std::endl;

    // Sort files for consistent processing
    std::sort(image_files.begin(), image_files.end());

    int successful = 0;
    int failed = 0;
    double total_time = 0.0;

    for (const auto& image_path : image_files) {
        auto start_time = std::chrono::high_resolution_clock::now();

        std::cout << "Processing: " << image_path.filename() << " ... ";

        // Read image data
        std::vector<unsigned char> plaintext;
        if (!readFile(image_path.string(), plaintext)) {
            std::cout << "FAILED (read error)" << std::endl;
            failed++;
            continue;
        }

        // Encrypt
        std::vector<unsigned char> ciphertext, iv, tag;
        if (!encryptor.encrypt(plaintext, ciphertext, iv, tag)) {
            std::cout << "FAILED (encryption error)" << std::endl;
            failed++;
            continue;
        }

        // Save encrypted data
        std::string output_file = output_folder + "/" + image_path.stem().string() + ".enc";
        if (!writeFile(output_file, ciphertext)) {
            std::cout << "FAILED (write error)" << std::endl;
            failed++;
            continue;
        }

        // Save IV and tag (needed for decryption)
        std::string metadata_file = output_folder + "/metadata/" + image_path.stem().string() + ".meta";
        std::vector<unsigned char> metadata;
        metadata.insert(metadata.end(), iv.begin(), iv.end());
        metadata.insert(metadata.end(), tag.begin(), tag.end());
        if (!writeFile(metadata_file, metadata)) {
            std::cout << "FAILED (metadata write error)" << std::endl;
            failed++;
            continue;
        }

        // Save original filename (for preserving extension during decryption)
        std::string filename_file = output_folder + "/metadata/" + image_path.stem().string() + ".filename";
        std::ofstream filename_out(filename_file);
        if (filename_out.is_open()) {
            filename_out << image_path.filename().string();
            filename_out.close();
        }

        auto end_time = std::chrono::high_resolution_clock::now();
        double elapsed = std::chrono::duration<double, std::milli>(end_time - start_time).count();
        total_time += elapsed;

        double throughput = (plaintext.size() / 1024.0 / 1024.0) / (elapsed / 1000.0);
        std::cout << "OK (" << elapsed << " ms, " << throughput << " MB/s)" << std::endl;
        
        successful++;
    }

    std::cout << std::endl << "=== Encryption Summary ===" << std::endl;
    std::cout << "Total files: " << image_files.size() << std::endl;
    std::cout << "Successful: " << successful << std::endl;
    std::cout << "Failed: " << failed << std::endl;
    std::cout << "Average time per frame: " << (total_time / successful) << " ms" << std::endl;
    std::cout << std::endl << "Encrypted files saved to: " << output_folder << std::endl;

    return (failed == 0) ? 0 : 1;
}
