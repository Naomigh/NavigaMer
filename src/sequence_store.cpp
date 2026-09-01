#include "navigamer/sequence_store.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>

namespace navigamer {
namespace {

std::string clean_sequence(std::string_view raw) {
  std::string result;
  result.reserve(raw.size());
  for (const unsigned char c : raw) {
    if (!std::isspace(c)) {
      result.push_back(static_cast<char>(std::toupper(c)));
    }
  }
  return result;
}

void fnv_mix(std::uint64_t& hash, std::string_view value) {
  constexpr std::uint64_t kPrime = 1099511628211ULL;
  for (const unsigned char c : value) {
    hash ^= c;
    hash *= kPrime;
  }
}

}  // namespace

SequenceStore SequenceStore::from_fasta(const std::filesystem::path& path,
                                        std::uint32_t window_length,
                                        std::uint32_t stride,
                                        std::uint64_t limit) {
  if (window_length == 0 || stride == 0) {
    throw std::invalid_argument("window length and stride must be positive");
  }
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("cannot open reference FASTA: " + path.string());
  }

  SequenceStore store;
  store.window_length_ = window_length;
  store.stride_ = stride;

  std::string line;
  Contig current;
  auto finish_contig = [&]() {
    if (current.name.empty()) return;
    if (current.bases.size() >= window_length) {
      current.window_count =
          1 + (current.bases.size() - window_length) / stride;
      if (limit != 0) {
        const auto remaining = limit - std::min(limit, store.sequence_count_);
        current.window_count = std::min(current.window_count, remaining);
      }
    }
    current.first_sequence_id = store.sequence_count_;
    store.sequence_count_ += current.window_count;
    store.contigs_.push_back(std::move(current));
    current = {};
  };

  while (std::getline(input, line)) {
    if (!line.empty() && line.front() == '>') {
      finish_contig();
      current.name = line.substr(1);
      const auto space = current.name.find_first_of(" \t");
      if (space != std::string::npos) current.name.resize(space);
    } else if (!current.name.empty()) {
      auto cleaned = clean_sequence(line);
      current.bases.append(cleaned);
    }
  }
  finish_contig();
  if (store.contigs_.empty()) {
    throw std::runtime_error("reference FASTA contains no records");
  }
  store.recompute_checksum();
  return store;
}

SequenceStore SequenceStore::from_sequences(std::vector<std::string> sequences) {
  if (sequences.empty() || sequences.front().empty()) {
    throw std::invalid_argument("test sequence collection must be non-empty");
  }
  const auto length = sequences.front().size();
  SequenceStore store;
  store.window_length_ = static_cast<std::uint32_t>(length);
  store.stride_ = static_cast<std::uint32_t>(length);
  store.sequence_count_ = sequences.size();
  store.contigs_.reserve(sequences.size());
  SequenceId id = 0;
  for (auto& sequence : sequences) {
    sequence = clean_sequence(sequence);
    if (sequence.size() != length) {
      throw std::invalid_argument("all explicit sequences must have equal length");
    }
    store.contigs_.push_back(
        Contig{"sequence_" + std::to_string(id), std::move(sequence), id, 1});
    ++id;
  }
  store.recompute_checksum();
  return store;
}

const SequenceStore::Contig& SequenceStore::contig_for(SequenceId id) const {
  if (id >= sequence_count_) throw std::out_of_range("invalid sequence ID");
  auto it = std::upper_bound(
      contigs_.begin(), contigs_.end(), id,
      [](SequenceId value, const Contig& contig) {
        return value < contig.first_sequence_id;
      });
  if (it == contigs_.begin()) throw std::logic_error("corrupt sequence offsets");
  --it;
  if (id >= it->first_sequence_id + it->window_count) {
    throw std::logic_error("sequence ID falls in an empty contig gap");
  }
  return *it;
}

std::string_view SequenceStore::sequence(SequenceId id) const {
  const auto& contig = contig_for(id);
  const auto local = id - contig.first_sequence_id;
  const auto offset = local * stride_;
  return std::string_view(contig.bases).substr(offset, window_length_);
}

std::pair<std::string_view, std::uint64_t> SequenceStore::location(
    SequenceId id) const {
  const auto& contig = contig_for(id);
  return {contig.name, (id - contig.first_sequence_id) * stride_};
}

void SequenceStore::recompute_checksum() {
  checksum_ = 1469598103934665603ULL;
  for (const auto& contig : contigs_) {
    fnv_mix(checksum_, contig.name);
    fnv_mix(checksum_, contig.bases);
  }
  fnv_mix(checksum_, std::to_string(window_length_));
  fnv_mix(checksum_, std::to_string(stride_));
  fnv_mix(checksum_, std::to_string(sequence_count_));
}

std::vector<QueryRecord> read_queries(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("cannot open query file: " + path.string());
  std::vector<QueryRecord> records;
  std::string first;
  while (std::getline(input, first) && first.empty()) {
  }
  if (first.empty()) return records;

  if (first.front() == '>') {
    QueryRecord current{first.substr(1), {}};
    std::string line;
    while (std::getline(input, line)) {
      if (!line.empty() && line.front() == '>') {
        current.sequence = clean_sequence(current.sequence);
        records.push_back(std::move(current));
        current = {line.substr(1), {}};
      } else {
        current.sequence.append(line);
      }
    }
    current.sequence = clean_sequence(current.sequence);
    records.push_back(std::move(current));
  } else if (first.front() == '@') {
    std::string header = std::move(first);
    while (true) {
      std::string sequence, plus, quality;
      if (!std::getline(input, sequence) || !std::getline(input, plus) ||
          !std::getline(input, quality)) {
        throw std::runtime_error("truncated FASTQ record");
      }
      if (plus.empty() || plus.front() != '+') {
        throw std::runtime_error("invalid FASTQ separator");
      }
      records.push_back({header.substr(1), clean_sequence(sequence)});
      if (!std::getline(input, header)) break;
      if (header.empty() || header.front() != '@') {
        throw std::runtime_error("invalid FASTQ header");
      }
    }
  } else {
    throw std::runtime_error("query file must be FASTA or FASTQ");
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
      default: c = 'N'; break;
    }
  }
  return result;
}

}  // namespace navigamer
