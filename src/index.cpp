#include "navigamer/index.hpp"

#include <array>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace navigamer {
namespace {

constexpr std::array<char, 8> kIndexMagic{'N', 'V', 'M', 'I', 'D', 'X', '2', '\0'};

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
  std::ofstream out(path, std::ios::binary);
  if (!out) throw std::runtime_error("cannot open index for writing: " + path.string());
  out.write(kIndexMagic.data(), kIndexMagic.size());
  write_value(out, kIndexFormatVersion);
  write_value(out, window_length);
  write_value(out, stride);
  write_value(out, max_tolerance);
  write_value(out, reference_window_count);
  write_value(out, root);
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
  write_strings(out, center_sequences);
  write_vector(out, children);
  write_vector(out, leaf_ids);
  write_strings(out, leaf_sequences);
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
  if (!in || magic != kIndexMagic) {
    throw std::runtime_error("unsupported NavigaMer index format: " + path.string());
  }
  const auto version = read_value<std::uint32_t>(in);
  if (version != kIndexFormatVersion) {
    throw std::runtime_error("unsupported NavigaMer index version " +
                             std::to_string(version));
  }
  NavigaMerIndex index;
  index.window_length = read_value<std::uint32_t>(in);
  index.stride = read_value<std::uint32_t>(in);
  index.max_tolerance = read_value<std::uint32_t>(in);
  index.reference_window_count = read_value<std::uint64_t>(in);
  index.root = read_value<NodeId>(in);
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
  }
  index.center_sequences = read_strings(in);
  index.children = read_vector<NodeId>(in);
  index.leaf_ids = read_vector<SequenceId>(in);
  index.leaf_sequences = read_strings(in);
  index.beacon_ordinals = read_vector<std::uint64_t>(in);
  index.beacon_distances = read_vector<int>(in);
  return index;
}

std::uint64_t NavigaMerIndex::bytes() const noexcept {
  std::uint64_t result = sizeof(*this) + nodes.size() * sizeof(WorldNode) +
      children.size() * sizeof(NodeId) + leaf_ids.size() * sizeof(SequenceId) +
      beacon_ordinals.size() * sizeof(std::uint64_t) + beacon_distances.size() * sizeof(int);
  for (const auto& sequence : center_sequences) result += sizeof(std::string) + sequence.size();
  for (const auto& sequence : leaf_sequences) result += sizeof(std::string) + sequence.size();
  return result;
}

std::string NavigaMerIndex::summary() const {
  std::ostringstream out;
  out << "format_version=" << kIndexFormatVersion
      << " window=" << window_length << " stride=" << stride
      << " T=" << max_tolerance << " windows=" << reference_window_count
      << " leaf_memberships=" << leaf_ids.size() << '\n';
  for (std::size_t layer = 0; layer < layers.size(); ++layer) {
    out << "layer=" << layer << " radius=" << layers[layer].selection_radius
        << " worlds=" << layers[layer].node_count << '\n';
  }
  out << "beacons=" << beacon_ordinals.size()
      << " beacon_distances=" << beacon_distances.size()
      << " index_bytes=" << bytes() << '\n';
  return out.str();
}

}  // namespace navigamer
