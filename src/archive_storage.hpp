#pragma once

#include "navigamer/archive.hpp"

#include <fstream>
#include <stdexcept>
#include <zstd.h>

namespace navigamer::archive_detail {

inline constexpr std::uint64_t kDataMagic = 0x31415441444d564eULL;
inline constexpr std::uint64_t kFrameMagic = 0x31454d415246564eULL;
inline constexpr std::uint64_t kPageMagic = 0x31454741504d564eULL;
inline constexpr std::size_t kNodeBytes = 88, kNodeHeaderBytes = 32;
inline constexpr std::size_t kMaxFrameRaw = 2 * 1024 * 1024;

template <typename T> void write(std::ostream& out, const T& value) {
  out.write(reinterpret_cast<const char*>(&value), sizeof(value));
  if (!out) throw std::runtime_error("archive write failed");
}
template <typename T> T read(std::istream& in) {
  T value{};
  in.read(reinterpret_cast<char*>(&value), sizeof(value));
  if (!in) throw std::runtime_error("truncated archive");
  return value;
}
void write_string(std::ostream& out, const std::string& value);
std::string read_string(std::istream& in);
std::uint64_t hash(std::span<const std::uint8_t> bytes);
void sync_file(const std::filesystem::path& path);
void publish(const std::filesystem::path& temporary, const std::filesystem::path& path);
void write_metadata(std::ostream& out, const ArchiveMetadata& metadata);
ArchiveMetadata read_metadata(std::istream& in);
void save_metadata(const std::filesystem::path& directory, const ArchiveMetadata& metadata);
ArchiveMetadata load_metadata(const std::filesystem::path& directory, bool require_complete = true);
void write_node(std::ostream& out, const ArchiveNode& node);
ArchiveNode read_node(std::istream& in);
void write_members(std::ostream& out, const ArchiveMembers& members);
ArchiveMembers read_members(std::istream& in);

class NodeTable {
 public:
  NodeTable(const std::filesystem::path& path, std::uint64_t fingerprint, bool create, bool writable = false);
  ArchiveNode get(NodeId id);
  void put(NodeId id, const ArchiveNode& node);
  void flush();
 private:
  std::fstream file_;
};

struct BlockEntry { std::uint64_t offset, bytes, count, first, last; };
static_assert(sizeof(BlockEntry) == 40);
struct MemberCursor {
  ArchiveMembers members;
  std::uint64_t last{kNoSequence};
  std::vector<BlockEntry> pending;
};

class MemberWriter {
 public:
  MemberWriter(const std::filesystem::path& path, const ArchiveMetadata& metadata,
               std::size_t page_capacity, bool create);
  ~MemberWriter();
  void bitmap(MemberCursor& cursor, SequenceId first, std::uint64_t windows,
              std::span<const std::uint64_t> bits);
  void selected(MemberCursor& cursor, std::span<const SequenceId> ids,
                std::span<const std::uint64_t> bits);
  void finish(MemberCursor& cursor);
  void flush();
  void page_capacity(std::size_t capacity);
  [[nodiscard]] std::uint64_t position() const { return position_; }
 private:
  void frame(MemberCursor& cursor, std::uint64_t encoding, std::uint64_t count,
             SequenceId first, SequenceId last, std::span<const std::uint8_t> raw);
  std::filesystem::path path_;
  std::fstream file_;
  ZSTD_CCtx* compressor_{nullptr};
  std::size_t page_capacity_;
  std::uint64_t position_{64}, last_space_check_{0};
};

// Strict, read-only compatibility with the R75 production archive (V1).
struct LegacyCenters {
  std::uint64_t fingerprint, windows, center_hash;
  std::vector<SequenceId> ids;
  explicit LegacyCenters(const std::filesystem::path& directory, std::uint64_t memory_limit = UINT64_MAX);
};
struct LegacyChunk {
  std::uint64_t first, count;
  std::vector<std::uint64_t> bits;
};
LegacyChunk read_legacy_chunk(const std::filesystem::path& directory,
                             const LegacyCenters& centers, std::uint64_t first,
                             std::uint64_t memory_limit);

}  // namespace navigamer::archive_detail
