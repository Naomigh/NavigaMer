#include "navigamer/sequence_store.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>

namespace navigamer {
namespace {

std::string clean_sequence(std::string_view raw) {
  std::string result;
  for (const unsigned char c : raw) {
    if (!std::isspace(c)) result.push_back(static_cast<char>(std::toupper(c)));
  }
  return result;
}

}  // namespace

QueryStats& QueryStats::operator+=(const QueryStats& other) noexcept {
  for (std::size_t i = 0; i < 3; ++i) {
    route_edit_calls[i] += other.route_edit_calls[i];
    route_candidates[i] += other.route_candidates[i];
    route_pruned[i] += other.route_pruned[i];
  }
  leaf_members += other.leaf_members;
  exact_verifications += other.exact_verifications;
  cache_candidates += other.cache_candidates;
  return *this;
}

SequenceStore SequenceStore::from_fasta(const std::filesystem::path& path,
                                        std::uint32_t window_length,
                                        std::uint32_t stride,
                                        std::uint64_t limit) {
  std::ifstream input(path);
  SequenceStore store;
  store.window_length_ = window_length;
  store.stride_ = stride;
  Contig current;
  auto finish_contig = [&]() {
    if (current.name.empty()) return;
    if (current.bases.size() >= window_length) {
      current.window_count = 1 + (current.bases.size() - window_length) / stride;
      if (limit != 0) {
        current.window_count = std::min(current.window_count,
                                        limit - store.sequence_count_);
      }
    }
    current.first_sequence_id = store.sequence_count_;
    store.sequence_count_ += current.window_count;
    store.contigs_.push_back(std::move(current));
    current = {};
  };
  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty() && line.front() == '>') {
      finish_contig();
      if (limit != 0 && store.sequence_count_ >= limit) break;
      current.name = line.substr(1);
      const auto space = current.name.find_first_of(" \t");
      if (space != std::string::npos) current.name.resize(space);
    } else {
      current.bases.append(clean_sequence(line));
    }
  }
  finish_contig();
  return store;
}

SequenceStore SequenceStore::from_sequences(std::vector<std::string> sequences) {
  SequenceStore store;
  store.window_length_ = sequences[0].size();
  store.stride_ = store.window_length_;
  store.sequence_count_ = sequences.size();
  for (SequenceId id = 0; id < sequences.size(); ++id) {
    store.contigs_.push_back({"sequence_" + std::to_string(id),
                              clean_sequence(sequences[id]), id, 1});
  }
  return store;
}

const SequenceStore::Contig& SequenceStore::contig_for(SequenceId id) const {
  const auto it = std::upper_bound(
      contigs_.begin(), contigs_.end(), id,
      [](SequenceId value, const Contig& contig) { return value < contig.first_sequence_id; });
  return *(it - 1);
}

std::string_view SequenceStore::sequence(SequenceId id) const {
  const auto& contig = contig_for(id);
  return std::string_view(contig.bases).substr(
      (id - contig.first_sequence_id) * stride_, window_length_);
}

std::pair<std::string_view, std::uint64_t> SequenceStore::location(SequenceId id) const {
  const auto& contig = contig_for(id);
  return {contig.name, (id - contig.first_sequence_id) * stride_};
}

std::vector<QueryRecord> read_queries(const std::filesystem::path& path,
                                      std::uint64_t limit) {
  std::ifstream input(path);
  std::vector<QueryRecord> records;
  std::string first;
  while (std::getline(input, first) && first.empty()) {}
  if (first.front() == '>') {
    QueryRecord current{first.substr(1), {}};
    std::string line;
    while (std::getline(input, line)) {
      if (!line.empty() && line.front() == '>') {
        current.sequence = clean_sequence(current.sequence);
        records.push_back(std::move(current));
        if (limit != 0 && records.size() == limit) return records;
        current = {line.substr(1), {}};
      } else {
        current.sequence.append(line);
      }
    }
    current.sequence = clean_sequence(current.sequence);
    records.push_back(std::move(current));
  } else {
    std::string header = std::move(first);
    do {
      std::string sequence, plus, quality;
      std::getline(input, sequence);
      std::getline(input, plus);
      std::getline(input, quality);
      records.push_back({header.substr(1), clean_sequence(sequence)});
      if (limit != 0 && records.size() == limit) return records;
    } while (std::getline(input, header));
  }
  return records;
}

std::string reverse_complement(std::string_view sequence) {
  std::string result(sequence.rbegin(), sequence.rend());
  for (char& c : result) {
    switch (static_cast<char>(std::toupper(static_cast<unsigned char>(c)))) {
      case 'A': c = 'T'; break;
      case 'C': c = 'G'; break;
      case 'G': c = 'C'; break;
      case 'T': c = 'A'; break;
      default: c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); break;
    }
  }
  return result;
}

}  // namespace navigamer
