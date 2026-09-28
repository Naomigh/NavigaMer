#include "navigamer/index.hpp"

#include <array>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace navigamer {
namespace {

constexpr std::array<char, 8> kIndexMagic{'N', 'V', 'M', 'I', 'D', 'X', '5', '\0'};
constexpr std::array<char, 8> kV4IndexMagic{'N', 'V', 'M', 'I', 'D', 'X', '4', '\0'};
constexpr std::array<char, 8> kLegacyIndexMagic{'N', 'V', 'M', 'I', 'D', 'X', '3', '\0'};

template <typename T>
void write_value(std::ostream& out, const T& value) {
  out.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T>
T read_value(std::istream& in) {
  T value{};
  in.read(reinterpret_cast<char*>(&value), sizeof(T));
  if (!in) throw std::runtime_error("truncated NavigaMer index");
  return value;
}

template <typename T>
void write_vector(std::ostream& out, const std::vector<T>& values) {
  write_value(out, static_cast<std::uint64_t>(values.size()));
  out.write(reinterpret_cast<const char*>(values.data()), values.size() * sizeof(T));
}

template <typename T>
std::vector<T> read_vector(std::istream& in) {
  std::vector<T> values(read_value<std::uint64_t>(in));
  in.read(reinterpret_cast<char*>(values.data()), values.size() * sizeof(T));
  if (!in) throw std::runtime_error("truncated NavigaMer index");
  return values;
}

void write_strings(std::ostream& out, const std::vector<std::string>& strings) {
  write_value(out, static_cast<std::uint64_t>(strings.size()));
  for (const auto& string : strings) {
    write_value(out, static_cast<std::uint64_t>(string.size()));
    out.write(string.data(), string.size());
  }
}

std::vector<std::string> read_strings(std::istream& in) {
  std::vector<std::string> strings(read_value<std::uint64_t>(in));
  for (auto& string : strings) {
    string.resize(read_value<std::uint64_t>(in));
    in.read(string.data(), string.size());
    if (!in) throw std::runtime_error("truncated NavigaMer index");
  }
  return strings;
}

}  // namespace

void NavigaMerIndex::save(const std::filesystem::path& path) const {
  if (layers.empty() || layers.size() > kRootLayer) {
    throw std::invalid_argument("layer count must be between 1 and 255");
  }
  std::ofstream out(path, std::ios::binary);
  if (!out) throw std::runtime_error("cannot open index for writing: " + path.string());
  // Legacy payloads cannot become reference-backed without the source reference.
  const auto& magic = uses_shared_reference ? kIndexMagic : kV4IndexMagic;
  out.write(magic.data(), magic.size());
  write_value(out, uses_shared_reference ? kIndexFormatVersion : std::uint32_t{4});
  write_value(out, window_length);
  write_value(out, stride);
  write_value(out, max_tolerance);
  write_value(out, reference_window_count);
  if (uses_shared_reference) write_value(out, reference_fingerprint);
  write_value(out, root);
  write_value(out, static_cast<std::uint32_t>(layers.size()));
  for (const auto& layer : layers) {
    write_value(out, layer.first_node);
    write_value(out, layer.node_count);
    write_value(out, layer.selection_radius);
  }
  write_value(out, static_cast<std::uint64_t>(nodes.size()));
  for (const auto& node : nodes) {
    write_value(out, node.center_sequence_id);
    write_value(out, node.member_count);
    write_value(out, node.first_child);
    write_value(out, node.first_member);
    write_value(out, node.first_beacon);
    write_value(out, node.first_distance);
    write_value(out, node.child_count);
    write_value(out, node.beacon_count);
    write_value(out, node.layer);
  }
  write_vector(out, center_sequences.bases_);
  write_vector(out, center_sequences.exception_offsets_);
  write_vector(out, center_sequences.exception_positions_);
  write_vector(out, center_sequences.exception_symbols_);
  write_vector(out, children);
  write_vector(out, leaf_ids);
  if (!uses_shared_reference) write_strings(out, legacy_leaf_sequences);
  write_vector(out, beacon_ordinals);
  write_vector(out, beacon_distances);
  out.flush();
  if (!out) throw std::runtime_error("failed while writing index: " + path.string());
}

NavigaMerIndex NavigaMerIndex::load(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot open index for reading: " + path.string());
  std::array<char, kIndexMagic.size()> magic{};
  in.read(magic.data(), magic.size());
  if (!in || (magic != kIndexMagic && magic != kV4IndexMagic && magic != kLegacyIndexMagic)) {
    throw std::runtime_error("unsupported NavigaMer index format: " + path.string());
  }
  const auto version = read_value<std::uint32_t>(in);
  const bool legacy = magic == kLegacyIndexMagic;
  if (version != (legacy ? 3 : magic == kV4IndexMagic ? 4 : kIndexFormatVersion)) {
    throw std::runtime_error("unsupported NavigaMer index version " +
                             std::to_string(version));
  }
  NavigaMerIndex index;
  index.uses_shared_reference = version == kIndexFormatVersion;
  index.window_length = read_value<std::uint32_t>(in);
  index.stride = read_value<std::uint32_t>(in);
  index.max_tolerance = read_value<std::uint32_t>(in);
  index.reference_window_count = read_value<std::uint64_t>(in);
  if (index.uses_shared_reference) index.reference_fingerprint = read_value<std::uint64_t>(in);
  index.root = read_value<NodeId>(in);
  const auto layer_count = legacy ? 3 : read_value<std::uint32_t>(in);
  if (layer_count == 0 || layer_count > kRootLayer) {
    throw std::runtime_error("invalid index layer count");
  }
  index.layers.resize(layer_count);
  for (auto& layer : index.layers) {
    layer.first_node = read_value<std::uint64_t>(in);
    layer.node_count = read_value<std::uint64_t>(in);
    layer.selection_radius = read_value<std::uint32_t>(in);
  }
  index.nodes.resize(read_value<std::uint64_t>(in));
  for (auto& node : index.nodes) {
    node.center_sequence_id = read_value<SequenceId>(in);
    node.member_count = read_value<std::uint64_t>(in);
    node.first_child = read_value<std::uint64_t>(in);
    node.first_member = read_value<std::uint64_t>(in);
    node.first_beacon = read_value<std::uint64_t>(in);
    node.first_distance = read_value<std::uint64_t>(in);
    node.child_count = read_value<std::uint64_t>(in);
    node.beacon_count = read_value<std::uint64_t>(in);
    node.layer = read_value<std::uint8_t>(in);
    if (legacy && node.layer == 3) node.layer = kRootLayer;
  }
  index.center_sequences = PackedCenters(index.window_length);
  index.center_sequences.bases_ = read_vector<std::uint8_t>(in);
  index.center_sequences.exception_offsets_ = read_vector<std::uint64_t>(in);
  index.center_sequences.exception_positions_ = read_vector<std::uint32_t>(in);
  index.center_sequences.exception_symbols_ = read_vector<char>(in);
  index.center_sequences.prepare_bitplanes();
  index.children = read_vector<NodeId>(in);
  index.leaf_ids = read_vector<SequenceId>(in);
  if (index.uses_shared_reference) {
    if (std::any_of(index.leaf_ids.begin(), index.leaf_ids.end(), [&](SequenceId id) {
          return id >= index.reference_window_count;
        })) throw std::runtime_error("invalid reference member ID in index");
  } else {
    index.legacy_leaf_sequences = read_strings(in);
  }
  index.beacon_ordinals = read_vector<std::uint64_t>(in);
  index.beacon_distances = read_vector<int>(in);
  return index;
}

std::uint64_t NavigaMerIndex::bytes() const noexcept {
  std::uint64_t result = sizeof(*this) + layers.size() * sizeof(LayerInfo) +
      nodes.size() * sizeof(WorldNode) +
      children.size() * sizeof(NodeId) + leaf_ids.size() * sizeof(SequenceId) +
      beacon_ordinals.size() * sizeof(std::uint64_t) + beacon_distances.size() * sizeof(int);
  result += center_sequences.bytes();
  for (const auto& sequence : legacy_leaf_sequences) result += sizeof(std::string) + sequence.size();
  return result;
}

std::string NavigaMerIndex::summary() const {
  std::ostringstream out;
  out << "format_version=" << (uses_shared_reference ? kIndexFormatVersion : 4)
      << " window=" << window_length << " stride=" << stride
      << " T=" << max_tolerance << " layers=" << layers.size()
      << " windows=" << reference_window_count
      << " leaf_memberships=" << leaf_ids.size() << '\n';
  out << "member_storage=" << (uses_shared_reference ? "reference-ids" : "legacy-owned-sequences")
      << " reference_fingerprint=" << reference_fingerprint << '\n';
  for (std::size_t layer = 0; layer < layers.size(); ++layer) {
    out << "layer=" << layer << " radius=" << layers[layer].selection_radius
        << " worlds=" << layers[layer].node_count << '\n';
  }
  out << "beacons=" << beacon_ordinals.size()
      << " beacon_distances=" << beacon_distances.size()
      << " center_bytes=" << center_sequences.bytes()
      << " index_bytes=" << bytes() << '\n';
  return out.str();
}

}  // namespace navigamer
