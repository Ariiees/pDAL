cmake_minimum_required(VERSION 3.10)
project(AESGCMFrameEncryption)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find OpenSSL
find_package(OpenSSL REQUIRED)

# Main encryption program
add_executable(encrypt_frames aes_gcm_frame_encryption.cpp)
target_link_libraries(encrypt_frames OpenSSL::Crypto OpenSSL::SSL)

# Decryption utility
add_executable(decrypt_frames aes_gcm_frame_decryption.cpp)
target_link_libraries(decrypt_frames OpenSSL::Crypto OpenSSL::SSL)

# Enable optimizations for release build
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native")
