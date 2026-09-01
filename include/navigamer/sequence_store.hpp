#pragma once

#include "navigamer/types.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace navigamer {

class SequenceStore {
 public:
  struct Contig {
    std::string name;
    std::string bases;
    SequenceId first_sequence_id{0};
    std::uint64_t window_count{0};
  };

  static SequenceStore from_fasta(const std::filesystem::path& path,
                                  std::uint32_t window_length,
                                  std::uint32_t stride = 1,
                                  std::uint64_t limit = 0);

  static SequenceStore from_sequences(std::vector<std::string> sequences);

  [[nodiscard]] std::uint32_t window_length() const noexcept {
    return window_length_;
  }
  [[nodiscard]] std::uint32_t stride() const noexcept { return stride_; }
  [[nodiscard]] std::uint64_t size() const noexcept { return sequence_count_; }
  [[nodiscard]] std::string_view sequence(SequenceId id) const;
  [[nodiscard]] std::pair<std::string_view, std::uint64_t> location(
      SequenceId id) const;
  [[nodiscard]] std::uint64_t checksum() const noexcept { return checksum_; }
  [[nodiscard]] const std::vector<Contig>& contigs() const noexcept {
    return contigs_;
  }

 private:
  const Contig& contig_for(SequenceId id) const;
  void recompute_checksum();

  std::uint32_t window_length_{0};
  std::uint32_t stride_{1};
  std::uint64_t sequence_count_{0};
  std::uint64_t checksum_{0};
  std::vector<Contig> contigs_;
};

std::vector<QueryRecord> read_queries(const std::filesystem::path& path);
std::string reverse_complement(std::string_view sequence);

}  // namespace navigamer
