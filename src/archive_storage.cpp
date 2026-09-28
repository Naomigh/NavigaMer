#include "archive_storage.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <fcntl.h>
#include <unistd.h>

namespace navigamer::archive_detail {

static_assert(std::endian::native == std::endian::little);
constexpr std::uint64_t kMetadataMagic = 0x314352414d564eULL;
constexpr std::uint64_t kNodesMagic = 0x3153444f4e4d564eULL;

void write_string(std::ostream& out, const std::string& value) {
  write(out, std::uint64_t(value.size()));
  out.write(value.data(), value.size());
  if (!out) throw std::runtime_error("archive string write failed");
}
std::string read_string(std::istream& in) {
  const auto size = read<std::uint64_t>(in);
  if (size > 4096) throw std::runtime_error("invalid archive string size");
  std::string value(size, '\0');
  in.read(value.data(), value.size());
  if (!in) throw std::runtime_error("truncated archive string");
  return value;
}
std::uint64_t hash(std::span<const std::uint8_t> bytes) {
  std::uint64_t result = 14695981039346656037ULL;
  for (const auto byte : bytes) result = (result ^ byte) * 1099511628211ULL;
  return result;
}
void sync_file(const std::filesystem::path& path) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) throw std::runtime_error("cannot sync archive file: " + path.string());
  const int result = ::fsync(fd);
  ::close(fd);
  if (result != 0) throw std::runtime_error("archive fsync failed");
}
void publish(const std::filesystem::path& temporary, const std::filesystem::path& path) {
  sync_file(temporary);
  std::filesystem::rename(temporary, path);
  sync_file(path.parent_path());
}
void write_metadata(std::ostream& out, const ArchiveMetadata& m) {
  for (const auto value : {kMetadataMagic, std::uint64_t{1}, m.nonce, m.window, m.stride,
      m.tolerance, m.reference_windows, m.reference_fingerprint, m.nodes, m.root_radius,
      std::uint64_t(m.complete), std::uint64_t(m.radii.size()), std::uint64_t(m.sources.size())}) write(out, value);
  for (const auto radius : m.radii) write(out, radius);
  for (const auto& source : m.sources) {
    write_string(out, source.name);
    write(out, source.nonce);
    write(out, source.bytes);
  }
}
ArchiveMetadata read_metadata(std::istream& in) {
  if (read<std::uint64_t>(in) != kMetadataMagic || read<std::uint64_t>(in) != 1) {
    throw std::runtime_error("unsupported archive metadata format");
  }
  ArchiveMetadata m;
  m.nonce = read<std::uint64_t>(in);
  m.window = read<std::uint64_t>(in); m.stride = read<std::uint64_t>(in);
  m.tolerance = read<std::uint64_t>(in); m.reference_windows = read<std::uint64_t>(in);
  m.reference_fingerprint = read<std::uint64_t>(in); m.nodes = read<std::uint64_t>(in);
  m.root_radius = read<std::uint64_t>(in);
  const auto complete = read<std::uint64_t>(in);
  const auto layers = read<std::uint64_t>(in), sources = read<std::uint64_t>(in);
  if (m.window == 0 || m.window > INT32_MAX - 64 || m.stride == 0 || m.stride > UINT32_MAX ||
      m.tolerance > UINT32_MAX || m.reference_windows == 0 || m.nodes == 0 ||
      complete > 1 || layers > 255 || sources == 0 || sources > 256) {
    throw std::runtime_error("invalid archive metadata");
  }
  m.complete = complete;
  for (std::uint64_t i = 0; i < layers; ++i) m.radii.push_back(read<std::uint32_t>(in));
  for (std::uint64_t i = 0; i < sources; ++i) {
    ArchiveSource source{read_string(in), read<std::uint64_t>(in), read<std::uint64_t>(in)};
    if (source.name.empty() || source.name == "." || source.name == ".." ||
        source.name.find_first_of("/\\") != std::string::npos || source.bytes < 64) {
      throw std::runtime_error("invalid archive data source");
    }
    m.sources.push_back(std::move(source));
  }
  return m;
}
void save_metadata(const std::filesystem::path& directory, const ArchiveMetadata& metadata) {
  const auto temporary = directory / "archive.tmp";
  std::ostringstream encoded(std::ios::binary); write_metadata(encoded, metadata);
  const auto data = encoded.str();
  { std::ofstream out(temporary, std::ios::binary); out.write(data.data(), data.size());
    write(out, hash({reinterpret_cast<const std::uint8_t*>(data.data()), data.size()})); out.close();
    if (!out) throw std::runtime_error("archive metadata write failed"); }
  publish(temporary, directory / "archive.bin");
}
ArchiveMetadata load_metadata(const std::filesystem::path& directory, bool require_complete) {
  const auto path = directory / "archive.bin";
  const auto size = std::filesystem::file_size(path);
  if (size < 8 || size > 2 * 1024 * 1024) throw std::runtime_error("invalid archive metadata size");
  std::ifstream file(path, std::ios::binary);
  std::string bytes(size, '\0'); file.read(bytes.data(), size);
  std::uint64_t checksum = 0; std::memcpy(&checksum, bytes.data() + size - 8, 8);
  if (!file || hash({reinterpret_cast<const std::uint8_t*>(bytes.data()), size - 8}) != checksum) throw std::runtime_error("archive metadata checksum mismatch");
  std::istringstream in(bytes.substr(0, size - 8), std::ios::binary);
  const auto m = read_metadata(in);
  if (in.peek() != std::char_traits<char>::eof()) throw std::runtime_error("trailing archive metadata");
  if (require_complete && !m.complete) throw std::runtime_error("archive is incomplete; resume construction first");
  if (m.nodes > (UINT64_MAX - kNodeHeaderBytes) / kNodeBytes ||
      std::filesystem::file_size(directory / "nodes.bin") != kNodeHeaderBytes + m.nodes * kNodeBytes) {
    throw std::runtime_error("archive node table size mismatch");
  }
  NodeTable table(directory / "nodes.bin", m.reference_fingerprint, false);
  const auto root = table.get(0);
  if (root.parent != kNoNode || root.depth != 0 || root.members.count == 0) {
    throw std::runtime_error("invalid archive root");
  }
  return m;
}
void write_members(std::ostream& out, const ArchiveMembers& m) {
  for (const auto value : {m.source, m.owner, m.page, m.count, m.blocks}) write(out, value);
}
ArchiveMembers read_members(std::istream& in) {
  return {read<std::uint64_t>(in), read<std::uint64_t>(in), read<std::uint64_t>(in),
          read<std::uint64_t>(in), read<std::uint64_t>(in)};
}
void write_node(std::ostream& out, const ArchiveNode& node) {
  for (const auto value : {node.center, node.parent, node.first_child, node.child_count, node.depth}) write(out, value);
  write_members(out, node.members);
}
ArchiveNode read_node(std::istream& in) {
  ArchiveNode n;
  n.center = read<std::uint64_t>(in); n.parent = read<std::uint64_t>(in);
  n.first_child = read<std::uint64_t>(in); n.child_count = read<std::uint64_t>(in);
  n.depth = read<std::uint64_t>(in); n.members = read_members(in);
  return n;
}
NodeTable::NodeTable(const std::filesystem::path& path, std::uint64_t fingerprint, bool create, bool writable) {
  file_.open(path, std::ios::binary | std::ios::in | ((create || writable) ? std::ios::out : std::ios::openmode{})
      | (create ? std::ios::trunc : std::ios::openmode{}));
  if (!file_) throw std::runtime_error("cannot open archive node table");
  if (create) {
    for (const auto value : {kNodesMagic, std::uint64_t{1}, fingerprint, std::uint64_t{kNodeBytes}}) write(file_, value);
  } else if (read<std::uint64_t>(file_) != kNodesMagic || read<std::uint64_t>(file_) != 1 ||
             read<std::uint64_t>(file_) != fingerprint || read<std::uint64_t>(file_) != kNodeBytes) {
    throw std::runtime_error("invalid archive node table header");
  }
}
ArchiveNode NodeTable::get(NodeId id) {
  file_.clear(); file_.seekg(kNodeHeaderBytes + id * kNodeBytes);
  const auto node = read_node(file_);
  static_assert(sizeof(ArchiveNode) == 80);
  if (read<std::uint64_t>(file_) != hash({reinterpret_cast<const std::uint8_t*>(&node), sizeof(node)})) throw std::runtime_error("archive node checksum mismatch");
  return node;
}
void NodeTable::put(NodeId id, const ArchiveNode& node) {
  file_.clear(); file_.seekp(kNodeHeaderBytes + id * kNodeBytes);
  write_node(file_, node);
  write(file_, hash({reinterpret_cast<const std::uint8_t*>(&node), sizeof(node)}));
}
void NodeTable::flush() { file_.flush(); if (!file_) throw std::runtime_error("node table flush failed"); }

void check_zstd(std::size_t result) {
  if (ZSTD_isError(result)) throw std::runtime_error(ZSTD_getErrorName(result));
}
MemberWriter::MemberWriter(const std::filesystem::path& path, const ArchiveMetadata& m,
    std::size_t page_capacity, bool create) : path_(path), page_capacity_(page_capacity) {
  if (page_capacity == 0 || page_capacity > 128) throw std::invalid_argument("invalid archive index page capacity");
  file_.open(path, std::ios::binary | std::ios::in | std::ios::out | (create ? std::ios::trunc : std::ios::openmode{}));
  if (!file_) throw std::runtime_error("cannot open archive member store");
  const std::array<std::uint64_t, 8> expected{kDataMagic, 1, m.window, m.stride, m.tolerance,
      m.reference_windows, m.reference_fingerprint, m.sources.back().nonce};
  if (create) { for (const auto value : expected) write(file_, value); }
  else {
    for (const auto value : expected) if (read<std::uint64_t>(file_) != value) throw std::runtime_error("member store header mismatch");
    position_ = std::filesystem::file_size(path);
    file_.seekp(position_);
  }
  compressor_ = ZSTD_createCCtx();
  if (!compressor_) throw std::bad_alloc();
}
MemberWriter::~MemberWriter() { ZSTD_freeCCtx(compressor_); }
void MemberWriter::page_capacity(std::size_t capacity) {
  if (capacity == 0 || capacity > 128) throw std::invalid_argument("invalid member page capacity");
  page_capacity_ = capacity;
}
void MemberWriter::flush() { file_.flush(); if (!file_) throw std::runtime_error("member store flush failed"); }
void MemberWriter::frame(MemberCursor& cursor, std::uint64_t encoding, std::uint64_t count,
    SequenceId first, SequenceId last, std::span<const std::uint8_t> raw) {
  if (count == 0) return;
  if (raw.size() > kMaxFrameRaw || (cursor.last != kNoSequence && first <= cursor.last)) {
    throw std::runtime_error("invalid member frame order or size");
  }
  std::vector<std::uint8_t> compressed(ZSTD_compressBound(raw.size()));
  check_zstd(ZSTD_CCtx_setParameter(compressor_, ZSTD_c_compressionLevel, 1));
  check_zstd(ZSTD_CCtx_setParameter(compressor_, ZSTD_c_checksumFlag, 1));
  const auto size = ZSTD_compress2(compressor_, compressed.data(), compressed.size(), raw.data(), raw.size());
  check_zstd(size);
  if (position_ - last_space_check_ >= 32 * 1024 * 1024 || last_space_check_ == 0) {
    if (std::filesystem::space(path_.parent_path()).available < size + 1024 * 1024) {
      throw std::runtime_error("insufficient disk space for archive output");
    }
    last_space_check_ = position_;
  }
  const auto offset = position_;
  for (const auto value : {kFrameMagic, cursor.members.owner, encoding, count, first, last,
                          std::uint64_t(raw.size()), std::uint64_t(size)}) write(file_, value);
  file_.write(reinterpret_cast<const char*>(compressed.data()), size);
  if (!file_) throw std::runtime_error("member frame write failed");
  position_ += 64 + size;
  cursor.pending.push_back({offset, 64 + size, count, first, last});
  cursor.last = last;
  cursor.members.count += count;
  ++cursor.members.blocks;
  if (cursor.pending.size() >= page_capacity_) finish(cursor);
}
void MemberWriter::finish(MemberCursor& cursor) {
  if (cursor.pending.empty()) return;
  const auto bytes = std::span(reinterpret_cast<const std::uint8_t*>(cursor.pending.data()), cursor.pending.size() * sizeof(BlockEntry));
  const auto offset = position_;
  for (const auto value : {kPageMagic, cursor.members.owner, cursor.members.page,
                          std::uint64_t(cursor.pending.size()), hash(bytes)}) write(file_, value);
  file_.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  if (!file_) throw std::runtime_error("member index write failed");
  position_ += 40 + bytes.size();
  cursor.members.page = offset;
  cursor.pending.clear();
}
void MemberWriter::bitmap(MemberCursor& cursor, SequenceId first, std::uint64_t windows,
                          std::span<const std::uint64_t> bits) {
  if (bits.size() != (windows + 63) / 64) throw std::runtime_error("bitmap dimensions mismatch");
  std::uint64_t count = 0;
  for (const auto word : bits) count += std::popcount(word);
  frame(cursor, 0, count, first, first + windows - 1,
        {reinterpret_cast<const std::uint8_t*>(bits.data()), bits.size() * 8});
}
void MemberWriter::selected(MemberCursor& cursor, std::span<const SequenceId> ids,
                            std::span<const std::uint64_t> bits) {
  std::vector<std::uint8_t> raw;
  std::uint64_t count = 0, first = 0, previous = 0;
  for (std::size_t w = 0; w < bits.size(); ++w) {
    auto mask = bits[w];
    while (mask) {
      const auto ordinal = w * 64 + std::countr_zero(mask);
      if (ordinal >= ids.size()) throw std::runtime_error("nonzero classification padding");
      const auto id = ids[ordinal];
      if (count == 0) first = previous = id;
      auto delta = id - previous;
      while (delta >= 128) { raw.push_back((delta & 127) | 128); delta >>= 7; }
      raw.push_back(delta);
      previous = id; ++count; mask &= mask - 1;
    }
  }
  frame(cursor, 1, count, first, previous, raw);
}

LegacyCenters::LegacyCenters(const std::filesystem::path& directory, std::uint64_t memory_limit) {
  std::ifstream in(directory / "checkpoint.bin", std::ios::binary);
  if (read<std::uint64_t>(in) != 0x31504b43353752ULL) throw std::runtime_error("invalid R75 checkpoint magic");
  fingerprint = read<std::uint64_t>(in); windows = read<std::uint64_t>(in);
  const auto next = read<std::uint64_t>(in), count = read<std::uint64_t>(in);
  center_hash = read<std::uint64_t>(in);
  const auto radius = read<std::uint32_t>(in), tolerance = read<std::uint32_t>(in);
  const auto window = read<std::uint32_t>(in), stride = read<std::uint32_t>(in);
  (void)read<double>(in);
  if (windows == 0 || next != windows || count == 0 || count > windows || radius != 75 ||
      tolerance != 5 || window != 150 || stride != 1 || count > 10000000 || count > memory_limit / 8 ||
      std::filesystem::file_size(directory / "checkpoint.bin") != 72 + count * 8) {
    throw std::runtime_error("input must be a complete R75/T5/150/stride1 checkpoint");
  }
  ids.resize(count);
  in.read(reinterpret_cast<char*>(ids.data()), count * 8);
  if (!in || ids.front() != 0 || ids.back() >= windows ||
      std::adjacent_find(ids.begin(), ids.end(), std::greater_equal<>()) != ids.end() ||
      hash({reinterpret_cast<const std::uint8_t*>(ids.data()), ids.size() * 8}) != center_hash) {
    throw std::runtime_error("invalid R75 center IDs or hash");
  }
}
LegacyChunk read_legacy_chunk(const std::filesystem::path& directory, const LegacyCenters& centers,
    std::uint64_t first, std::uint64_t memory_limit) {
  std::ostringstream name; name << std::setw(12) << std::setfill('0') << first << ".r75z";
  const auto path = directory / "memberships" / "chunks" / name.str();
  std::ifstream in(path, std::ios::binary);
  std::array<std::uint64_t, 16> h;
  for (auto& value : h) value = read<std::uint64_t>(in);
  const auto count = std::min<std::uint64_t>(65536, centers.windows - first), words = (count + 63) / 64;
  const auto raw = centers.ids.size() * words * 8;
  if (h[0] != 0x314b4e4843353752ULL || h[1] != 1 || h[2] != centers.fingerprint ||
      h[3] != centers.center_hash || h[4] != centers.windows || h[5] != centers.ids.size() ||
      h[6] != first || h[7] != count || h[8] != 75 || h[9] != 5 || h[10] != 150 ||
      h[11] != 1 || h[13] != raw || h[15] != 0 || h[14] > ZSTD_compressBound(raw) ||
      std::filesystem::file_size(path) != 128 + h[5] * 4 + h[14]) {
    throw std::runtime_error("R75 chunk metadata mismatch: " + path.string());
  }
  if (raw + h[14] + h[5] * 4 > memory_limit) throw std::runtime_error("R75 chunk exceeds working-memory budget");
  std::vector<std::uint32_t> counts(h[5]);
  std::vector<std::uint8_t> compressed(h[14]);
  LegacyChunk result{first, count, std::vector<std::uint64_t>(raw / 8)};
  in.read(reinterpret_cast<char*>(counts.data()), counts.size() * 4);
  in.read(reinterpret_cast<char*>(compressed.data()), compressed.size());
  if (!in) throw std::runtime_error("truncated R75 chunk");
  const auto decoded = ZSTD_decompress(result.bits.data(), raw, compressed.data(), compressed.size());
  check_zstd(decoded);
  if (decoded != raw) throw std::runtime_error("R75 decompressed size mismatch");
  std::vector<std::uint64_t> covered(words, 0);
  std::uint64_t sum = 0;
  const auto center_begin = std::lower_bound(centers.ids.begin(), centers.ids.end(), first);
  const auto center_end = std::lower_bound(center_begin, centers.ids.end(), first + count);
  for (std::size_t c = 0; c < centers.ids.size(); ++c) {
    std::uint64_t members = 0;
    for (std::size_t w = 0; w < words; ++w) {
      const auto value = result.bits[c * words + w];
      if (w + 1 == words && count % 64 && value >> (count % 64)) throw std::runtime_error("R75 bitmap padding is nonzero");
      members += std::popcount(value); covered[w] |= value;
    }
    for (auto center = center_begin; center != center_end; ++center) {
      const auto q = *center - first;
      if (bool((result.bits[c * words + q / 64] >> (q % 64)) & 1) != (std::size_t(center - centers.ids.begin()) == c)) {
        throw std::runtime_error("R75 center exclusivity mismatch");
      }
    }
    if (members != counts[c]) throw std::runtime_error("R75 world count mismatch");
    sum += members;
  }
  if (sum != h[12]) throw std::runtime_error("R75 membership sum mismatch");
  for (std::size_t w = 0; w < words; ++w) {
    const auto expected = w + 1 == words && count % 64 ? (std::uint64_t{1} << (count % 64)) - 1 : UINT64_MAX;
    if (covered[w] != expected) throw std::runtime_error("R75 archive contains an uncovered window");
  }
  return result;
}

}  // namespace navigamer::archive_detail

namespace navigamer {
using namespace archive_detail;

ArchiveIndex ArchiveIndex::open(const std::filesystem::path& directory, bool require_complete) {
  ArchiveIndex result;
  result.directory_ = std::filesystem::canonical(directory);
  result.metadata_ = load_metadata(result.directory_, require_complete);
  for (const auto& source : result.metadata_.sources) {
    if (std::filesystem::file_size(result.directory_ / source.name) != source.bytes) {
      throw std::runtime_error("archive data source size mismatch");
    }
    std::ifstream in(result.directory_ / source.name, std::ios::binary);
    for (const auto value : {kDataMagic, std::uint64_t{1}, result.metadata_.window, result.metadata_.stride,
        result.metadata_.tolerance, result.metadata_.reference_windows, result.metadata_.reference_fingerprint, source.nonce}) {
      if (read<std::uint64_t>(in) != value) throw std::runtime_error("archive data source binding mismatch");
    }
  }
  return result;
}
ArchiveNode ArchiveIndex::node(NodeId id) const {
  if (id >= metadata_.nodes) throw std::out_of_range("archive node ID out of range");
  NodeTable table(directory_ / "nodes.bin", metadata_.reference_fingerprint, false);
  const auto n = table.get(id);
  if (n.depth > metadata_.radii.size() || n.members.count == 0 ||
      n.first_child > metadata_.nodes || n.child_count > metadata_.nodes - n.first_child ||
      (n.center != kNoSequence && n.center >= metadata_.reference_windows) ||
      (n.members.source != kArchiveRange && n.members.source >= metadata_.sources.size())) {
    throw std::runtime_error("invalid archive node metadata");
  }
  return n;
}
void ArchiveIndex::check_reference(const SequenceStore& reference) const {
  if (reference.size() != metadata_.reference_windows || reference.fingerprint() != metadata_.reference_fingerprint ||
      reference.window_length() != metadata_.window || reference.stride() != metadata_.stride) {
    throw std::invalid_argument("reference does not match archive");
  }
}
std::string ArchiveIndex::summary() const {
  const auto root = node(0);
  std::ostringstream out;
  out << "archive_version=1 complete=true window=" << metadata_.window << " stride=" << metadata_.stride
      << " T=" << metadata_.tolerance << " layers=" << metadata_.radii.size() << " nodes=" << metadata_.nodes
      << " reference_windows=" << metadata_.reference_windows << " scope_members=" << root.members.count << '\n';
  out << "scope=" << (root.center == kNoSequence ? "reference" : "world");
  if (root.center != kNoSequence) out << " root_center_sequence_id=" << root.center;
  out << " member_storage=compressed-blocks\nradii=";
  for (std::size_t i = 0; i < metadata_.radii.size(); ++i) out << (i ? "," : "") << metadata_.radii[i];
  out << '\n';
  return out.str();
}

struct ArchiveMemberReader::Impl {
  ArchiveMetadata metadata;
  ArchiveMembers handle;
  std::ifstream file;
  std::vector<std::uint64_t> pages;
  std::vector<BlockEntry> entries;
  std::size_t page_position{0}, entry_position{0}, raw_position{0}, word_position{0};
  std::vector<std::uint8_t> raw;
  std::array<std::uint64_t, 8> frame{};
  std::uint64_t word{0}, previous{0}, frame_seen{0}, emitted{0}, blocks{0}, total_blocks{0};

  Impl(const ArchiveIndex& archive, NodeId node) : metadata(archive.metadata()), handle(archive.node(node).members) {
    if (handle.source == kArchiveRange) {
      if (handle.count != metadata.reference_windows || handle.page || handle.blocks) throw std::runtime_error("invalid range membership");
      return;
    }
    file.open(archive.directory() / metadata.sources.at(handle.source).name, std::ios::binary);
    auto page = handle.page;
    while (page) {
      if (page < 64 || page >= metadata.sources[handle.source].bytes || pages.size() > handle.blocks) throw std::runtime_error("invalid member index chain");
      file.seekg(page);
      if (read<std::uint64_t>(file) != kPageMagic || read<std::uint64_t>(file) != handle.owner) throw std::runtime_error("member index owner mismatch");
      const auto prior = read<std::uint64_t>(file), count = read<std::uint64_t>(file);
      if (prior >= page || count == 0 || count > 128 || page + 40 + count * 40 > metadata.sources[handle.source].bytes) throw std::runtime_error("invalid member index page");
      total_blocks += count; pages.push_back(page); page = prior;
    }
    if (total_blocks != handle.blocks || total_blocks == 0) throw std::runtime_error("member block count mismatch");
    std::reverse(pages.begin(), pages.end());
  }
  bool load_entries() {
    if (entry_position == entries.size()) {
      if (page_position == pages.size()) return false;
      file.clear(); file.seekg(pages[page_position++]);
      (void)read<std::uint64_t>(file); (void)read<std::uint64_t>(file); (void)read<std::uint64_t>(file);
      const auto count = read<std::uint64_t>(file), checksum = read<std::uint64_t>(file);
      entries.resize(count); entry_position = 0;
      file.read(reinterpret_cast<char*>(entries.data()), entries.size() * sizeof(BlockEntry));
      if (!file || hash({reinterpret_cast<const std::uint8_t*>(entries.data()), entries.size() * sizeof(BlockEntry)}) != checksum) {
        throw std::runtime_error("member index checksum mismatch");
      }
    }
    return true;
  }
  bool load_frame() {
    if (!load_entries()) return false;
    const auto& entry = entries[entry_position++];
    if (entry.offset < 64 || entry.bytes < 64 || entry.offset > metadata.sources[handle.source].bytes ||
        entry.bytes > metadata.sources[handle.source].bytes - entry.offset) throw std::runtime_error("member frame outside source");
    file.clear(); file.seekg(entry.offset);
    for (auto& value : frame) value = read<std::uint64_t>(file);
    if (frame[0] != kFrameMagic || frame[1] != handle.owner || frame[2] > 1 || frame[3] != entry.count ||
        frame[3] == 0 || frame[4] != entry.first || frame[5] != entry.last || frame[4] > frame[5] ||
        frame[5] >= metadata.reference_windows || frame[6] > kMaxFrameRaw || frame[7] != entry.bytes - 64 ||
        frame[7] > ZSTD_compressBound(frame[6])) throw std::runtime_error("invalid member frame metadata");
    std::vector<std::uint8_t> compressed(frame[7]); raw.resize(frame[6]);
    file.read(reinterpret_cast<char*>(compressed.data()), compressed.size());
    if (!file) throw std::runtime_error("truncated member frame");
    const auto size = ZSTD_decompress(raw.data(), raw.size(), compressed.data(), compressed.size());
    check_zstd(size);
    if (size != raw.size()) throw std::runtime_error("member decompressed size mismatch");
    if (frame[2] == 0) {
      const auto span = frame[5] - frame[4] + 1;
      if (raw.size() != ((span + 63) / 64) * 8) throw std::runtime_error("member bitmap size mismatch");
      std::uint64_t count = 0, last_word = 0;
      for (std::size_t i = 0; i < raw.size(); i += 8) { std::memcpy(&last_word, raw.data() + i, 8); count += std::popcount(last_word); }
      if (count != frame[3] || (span % 64 && last_word >> (span % 64))) throw std::runtime_error("member bitmap count/padding mismatch");
    } else if (frame[3] > 65536 || raw.size() < frame[3] || raw.size() > frame[3] * 10) {
      throw std::runtime_error("invalid delta member dimensions");
    }
    raw_position = word_position = 0; word = frame_seen = 0; ++blocks;
    return true;
  }
  std::vector<SequenceId> next(std::size_t maximum) {
    if (maximum == 0 || maximum > 65536) throw std::invalid_argument("member read size must be between 1 and 65536");
    std::vector<SequenceId> result; result.reserve(maximum);
    if (handle.source == kArchiveRange) {
      while (result.size() < maximum && emitted < handle.count) result.push_back(emitted++);
      return result;
    }
    while (result.size() < maximum && emitted < handle.count) {
      if (frame_seen == frame[3]) {
        if (!raw.empty() && frame[2] == 1 && raw_position != raw.size()) throw std::runtime_error("trailing delta member data");
        if (!load_frame()) throw std::runtime_error("truncated archive member stream");
      }
      SequenceId id = 0;
      if (frame[2] == 0) {
        while (word == 0) {
          if (word_position >= raw.size() / 8) throw std::runtime_error("bitmap member stream exhausted early");
          std::memcpy(&word, raw.data() + word_position++ * 8, 8);
        }
        id = frame[4] + (word_position - 1) * 64 + std::countr_zero(word);
        word &= word - 1;
      } else {
        std::uint64_t delta = 0; unsigned shift = 0;
        for (;;) {
          if (raw_position == raw.size() || shift > 63) throw std::runtime_error("invalid member varint");
          const auto byte = raw[raw_position++];
          if (shift == 63 && byte > 1) throw std::runtime_error("member varint overflow");
          delta |= std::uint64_t(byte & 127) << shift;
          if (!(byte & 128)) break;
          shift += 7;
        }
        if ((frame_seen == 0 && delta != 0) || (frame_seen != 0 && delta == 0) ||
            delta > UINT64_MAX - previous) throw std::runtime_error("invalid member delta");
        id = frame_seen == 0 ? frame[4] : previous + delta;
        if (id > frame[5]) throw std::runtime_error("delta member outside frame");
      }
      if (emitted && id <= previous) throw std::runtime_error("archive members are not strictly increasing");
      previous = id; ++frame_seen; ++emitted; result.push_back(id);
      if (frame[2] == 1 && frame_seen == frame[3] && (id != frame[5] || raw_position != raw.size())) {
        throw std::runtime_error("member delta tail mismatch");
      }
    }
    if (emitted == handle.count && (blocks != handle.blocks || frame_seen != frame[3])) throw std::runtime_error("archive member total mismatch");
    return result;
  }
  void skip(std::uint64_t count) {
    if (count > handle.count - emitted) throw std::runtime_error("archive checkpoint exceeds member stream");
    if (handle.source == kArchiveRange) { emitted += count; return; }
    while (count) {
      if (frame_seen == frame[3]) {
        if (!load_entries()) throw std::runtime_error("truncated member index while skipping");
        const auto& entry = entries[entry_position];
        if (count >= entry.count) {
          if (entry.count == 0 || entry.last >= metadata.reference_windows ||
              (emitted && entry.first <= previous)) throw std::runtime_error("invalid skipped member index entry");
          count -= entry.count; emitted += entry.count; previous = entry.last;
          ++entry_position; ++blocks; raw.clear(); frame.fill(0); frame_seen = 0;
          continue;
        }
      }
      const auto block = next(std::min<std::uint64_t>(count, 65536));
      if (block.empty()) throw std::runtime_error("member stream ended while skipping");
      count -= block.size();
    }
  }
};
ArchiveMemberReader::ArchiveMemberReader(const ArchiveIndex& archive, NodeId node) : impl_(std::make_unique<Impl>(archive, node)) {}
ArchiveMemberReader::~ArchiveMemberReader() = default;
std::vector<SequenceId> ArchiveMemberReader::next(std::size_t maximum) { return impl_->next(maximum); }
void ArchiveMemberReader::skip(std::uint64_t members) {
  impl_->skip(members);
}

}  // namespace navigamer
