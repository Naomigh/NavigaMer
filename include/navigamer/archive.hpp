#pragma once

#include "navigamer/sequence_store.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>

namespace navigamer {

inline constexpr std::uint64_t kArchiveRange = UINT64_MAX;
struct ArchiveMembers {
  std::uint64_t source{kArchiveRange}, owner{0}, page{0}, count{0}, blocks{0};
};
struct ArchiveNode {
  SequenceId center{kNoSequence};
  NodeId parent{kNoNode}, first_child{0};
  std::uint64_t child_count{0}, depth{0};
  ArchiveMembers members;
};
struct ArchiveSource {
  std::string name;
  std::uint64_t nonce{0}, bytes{0};
};
struct ArchiveMetadata {
  std::uint64_t nonce{0}, window{0}, stride{1}, tolerance{5};
  std::uint64_t reference_windows{0}, reference_fingerprint{0}, nodes{0};
  std::uint64_t root_radius{UINT32_MAX};
  bool complete{false};
  std::vector<std::uint32_t> radii;
  std::vector<ArchiveSource> sources;
};

// Metadata and nodes stay on disk. Opening an archive does not expand members.
class ArchiveIndex {
 public:
  static ArchiveIndex open(const std::filesystem::path& directory, bool require_complete = true);
  [[nodiscard]] ArchiveNode node(NodeId id) const;
  [[nodiscard]] const ArchiveMetadata& metadata() const { return metadata_; }
  [[nodiscard]] const std::filesystem::path& directory() const { return directory_; }
  void check_reference(const SequenceStore& reference) const;
  [[nodiscard]] std::string summary() const;
 private:
  std::filesystem::path directory_;
  ArchiveMetadata metadata_;
};

class ArchiveMemberReader {
 public:
  ArchiveMemberReader(const ArchiveIndex& archive, NodeId node);
  ~ArchiveMemberReader();
  // At most max_members global occurrence IDs, always strictly increasing.
  [[nodiscard]] std::vector<SequenceId> next(std::size_t max_members);
  void skip(std::uint64_t members);
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

struct ArchiveBuildConfig {
  std::vector<std::uint32_t> radii;
  // A changed T reclassifies legacy first-layer centers over the full reference.
  std::optional<std::uint32_t> tolerance;
  std::uint32_t threads{1};
  // Temporary heap buffers, excluding the immutable reference and thread stacks.
  std::uint64_t working_memory_mib{1024};
  std::uint64_t checkpoint_members{1000000};
  // Legacy R75/R90: zero-based center ordinal. New archive: node ID.
  std::optional<NodeId> world;
  bool resume{false};
  bool copy_input{false};  // Explicit cross-filesystem fallback to hard links.
  std::function<bool()> stop_requested;
};

class ArchiveBuilder {
 public:
  // Read input reference parameters only; legacy member chunks are not scanned.
  static ArchiveMetadata input_metadata(const std::filesystem::path& input);
  // Legacy input: checkpoint.bin and memberships/chunks. New input: archive.bin.
  // Returns false after a requested stop; repeat with resume=true to continue.
  static bool extend(const SequenceStore& reference, const std::filesystem::path& input,
                     const std::filesystem::path& output, const ArchiveBuildConfig& config);
};

}  // namespace navigamer
