#include <avs/append_logger.h>

#include <sqlite3.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "pdal/catalog/resource_catalog.h"
#include "pdal/storage/avs_storage_backend.h"

namespace {

namespace fs = std::filesystem;

int failures = 0;

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      std::cerr << __FILE__ << ':' << __LINE__ << " CHECK failed: "            \
                << #condition << '\n';                                          \
      ++failures;                                                               \
    }                                                                           \
  } while (false)

void Sql(const fs::path& database, const std::string& statement) {
  sqlite3* handle = nullptr;
  if (sqlite3_open(database.c_str(), &handle) != SQLITE_OK) {
    throw std::runtime_error("cannot create test catalog");
  }
  char* error = nullptr;
  const auto status = sqlite3_exec(handle, statement.c_str(), nullptr, nullptr,
                                   &error);
  const std::string message = error ? error : "";
  sqlite3_free(error);
  sqlite3_close(handle);
  if (status != SQLITE_OK) throw std::runtime_error(message);
}

std::vector<std::uint8_t> RecordBytes(std::uint64_t timestamp,
                                      const std::vector<std::uint8_t>& payload) {
  avs::TripHeader trip{};
  avs::ChunkHeader chunk{};
  chunk.start_ts_ns = static_cast<std::int64_t>(timestamp);
  chunk.end_ts_ns = static_cast<std::int64_t>(timestamp);
  chunk.record_count = 1;
  chunk.chunk_size_bytes = sizeof(avs::RecordHeader) + payload.size();
  avs::RecordHeader record{static_cast<std::int64_t>(timestamp),
                           static_cast<std::uint32_t>(payload.size())};
  std::vector<std::uint8_t> bytes(sizeof(trip) + sizeof(chunk) +
                                  sizeof(record) + payload.size());
  auto* output = bytes.data();
  std::memcpy(output, &trip, sizeof(trip));
  output += sizeof(trip);
  std::memcpy(output, &chunk, sizeof(chunk));
  output += sizeof(chunk);
  std::memcpy(output, &record, sizeof(record));
  output += sizeof(record);
  std::memcpy(output, payload.data(), payload.size());
  return bytes;
}

avs::TripIndexEntry Index(std::uint64_t timestamp,
                          std::size_t payload_size) {
  return {static_cast<std::int64_t>(timestamp),
          static_cast<std::int64_t>(timestamp), sizeof(avs::TripHeader),
          static_cast<std::uint32_t>(sizeof(avs::ChunkHeader) +
                                     sizeof(avs::RecordHeader) + payload_size),
          1};
}

void WriteBinary(const fs::path& path, const void* data, std::size_t size) {
  fs::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary);
  output.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
}

void CreateHot(const fs::path& root, std::uint64_t timestamp,
               const std::vector<std::uint8_t>& payload, int trip_id = 0) {
  fs::create_directories(root);
  if (!fs::exists(root / "global.sqlite3")) {
    Sql(root / "global.sqlite3",
        "CREATE TABLE global(sensor_topic TEXT, topic_folder TEXT, day TEXT, "
        "trip_id INTEGER, number_of_records INTEGER, start_ts_ns TEXT, "
        "end_ts_ns TEXT);");
  }
  Sql(root / "global.sqlite3",
      "INSERT INTO global VALUES('/camera', 'camera_front', '2026-01-02', " +
      std::to_string(trip_id) + ", 1, '" + std::to_string(timestamp) +
      "', '" + std::to_string(timestamp) + "');");
  const auto bytes = RecordBytes(timestamp, payload);
  const auto index = Index(timestamp, payload.size());
  const auto directory = root / "camera_front" / "2026-01-02";
  char name[32];
  std::snprintf(name, sizeof(name), "trip_%02d.log", trip_id);
  WriteBinary(directory / name, bytes.data(), bytes.size());
  std::snprintf(name, sizeof(name), "trip_%02d.idx", trip_id);
  WriteBinary(directory / name, &index, sizeof(index));
}

void CreateCold(const fs::path& root, std::uint64_t timestamp,
                const std::vector<std::uint8_t>& payload) {
  fs::create_directories(root);
  Sql(root / "global.sqlite3",
      "CREATE TABLE global(sensor_topic TEXT, day TEXT, trip_id INTEGER, "
      "start_ts_ns TEXT, end_ts_ns TEXT);"
      "INSERT INTO global VALUES('/camera', '2026-01-01', 0, '" +
      std::to_string(timestamp) + "', '" + std::to_string(timestamp) + "');");
  const auto bytes = RecordBytes(timestamp, payload);
  const auto index = Index(timestamp, payload.size());
  constexpr std::uint64_t index_offset = 128;
  constexpr std::uint64_t log_offset = 1024;
  std::vector<std::uint8_t> archive(log_offset + bytes.size());
  std::memcpy(archive.data() + index_offset, &index, sizeof(index));
  std::memcpy(archive.data() + log_offset, bytes.data(), bytes.size());
  const auto directory = root / "camera_front" / "2026" / "01";
  WriteBinary(directory / "2026-01-01.tar", archive.data(), archive.size());
  fs::create_directories(directory);
  std::ofstream tar_index(directory / "2026-01-01.tar.idx");
  tar_index << "2026-01-01/trip_00.idx\t" << index_offset << '\t'
            << sizeof(index) << '\n';
  tar_index << "2026-01-01/trip_00.log\t" << log_offset << '\t'
            << bytes.size() << '\n';
}

pdal::ResourceCatalog Catalog() {
  pdal::CatalogResource camera;
  camera.resource_id = "camera.front";
  camera.resource_type = "sensor-data";
  camera.semantic_name = "Front camera";
  camera.modality = "image";
  camera.representations = {{"jpeg", "image/jpeg", true}};
  camera.historical_binding = {
      "avs", "/camera", {{"topic_folder", "camera_front"}}};
  camera.operations = {pdal::Operation::kHistory};
  return pdal::ResourceCatalog::FromResources({std::move(camera)});
}

void TestUnifiedHotAndCold() {
  const auto root = fs::temp_directory_path() /
                    ("pdal-avs-test-" + pdal::GenerateRequestId());
  const auto hot = root / "hot";
  const auto cold = root / "cold";
  try {
    CreateHot(hot, 100, {9, 0});
    CreateHot(hot, 200, {2, 0, 0}, 1);
    CreateCold(cold, 100, {1, 0});
    const auto catalog = Catalog();
    pdal::AvsStorageBackend backend(hot, cold);
    pdal::ExecutionTask task;
    task.resource_id = "camera.front";
    task.backend_id = "avs";
    task.time = {1, 300};
    task.representation.format = "jpeg";
    auto cursor = backend.OpenHistory({task, 0, 1}, catalog);
    const auto first = cursor->Next();
    CHECK(first.has_value());
    CHECK(first->resource_id == "camera.front");
    CHECK(first->timestamp_ns == 100);
    CHECK(cursor->has_more());
    std::vector<std::uint8_t> payload;
    const auto bytes = backend.Read(*first, [&](const std::uint8_t* data,
                                                std::size_t size) {
      payload.insert(payload.end(), data, data + size);
      return true;
    });
    CHECK(bytes == 2);
    CHECK(payload == (std::vector<std::uint8_t>{9, 0}));

    cursor = backend.OpenHistory({task, 1, 1}, catalog);
    const auto second = cursor->Next();
    CHECK(second.has_value());
    CHECK(second->resource_id == "camera.front");
    CHECK(second->timestamp_ns == 200);
    payload.clear();
    CHECK(backend.Read(*second, [&](const std::uint8_t* data, std::size_t size) {
            payload.insert(payload.end(), data, data + size);
            return true;
          }) == 3);
    CHECK(payload == (std::vector<std::uint8_t>{2, 0, 0}));
  } catch (...) {
    fs::remove_all(root);
    throw;
  }
  fs::remove_all(root);
}

}  // namespace

int main() {
  try {
    TestUnifiedHotAndCold();
  } catch (const std::exception& error) {
    std::cerr << "FAIL unified AVS history: " << error.what() << '\n';
    return 1;
  }
  if (failures != 0) return 1;
  std::cout << "PASS unified AVS hot/cold history\n";
  return 0;
}
