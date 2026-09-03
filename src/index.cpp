#include "navigamer/index.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <type_traits>

namespace navigamer {
namespace {

struct FileHeader {
  std::array<char, 8> magic{};
  std::uint32_t version{4};
  std::uint32_t endian{0x01020304U};
  std::uint32_t window_length{0};
  std::uint32_t stride{0};
  std::uint32_t max_beacons{0};
  std::uint32_t routing_mode{0};
  std::uint32_t containment_tolerance{0};
  std::uint32_t reserved{0};
  std::uint64_t reference_count{0};
  std::uint64_t reference_checksum{0};
  std::uint64_t root{0};
  std::uint64_t layer_count{0};
  std::uint64_t node_count{0};
  std::uint64_t child_count{0};
  std::uint64_t beacon_count{0};
  std::uint64_t distance_count{0};
  std::uint64_t metric_root_count{0};
  std::uint64_t metric_node_count{0};
  std::uint64_t metric_edge_count{0};
  std::uint64_t dense_offset_count{0};
  std::uint64_t dense_distance_count{0};
};

static_assert(std::is_trivially_copyable_v<FileHeader>);
static_assert(std::is_trivially_copyable_v<WorldNode>);
static_assert(std::is_trivially_copyable_v<LayerInfo>);

template <class T>
void write_raw(std::ofstream& output, const T* data, std::size_t count) {
  if (count == 0) return;
  output.write(reinterpret_cast<const char*>(data),
               static_cast<std::streamsize>(sizeof(T) * count));
  if (!output) throw std::runtime_error("failed while writing index");
}

template <class T>
void read_raw(std::ifstream& input, T* data, std::size_t count) {
  if (count == 0) return;
  input.read(reinterpret_cast<char*>(data),
             static_cast<std::streamsize>(sizeof(T) * count));
  if (!input) throw std::runtime_error("truncated NavigaMer index");
}

template <class T>
void checked_resize(std::vector<T>& vector, std::uint64_t count) {
  if (count > vector.max_size()) throw std::length_error("index vector too large");
  vector.resize(static_cast<std::size_t>(count));
}

}  // namespace

void NavigaMerIndex::save(const std::filesystem::path& path) const {
  validate();
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) throw std::runtime_error("cannot create index: " + path.string());
  FileHeader header{};
  std::memcpy(header.magic.data(), "NVMIDX4", 7);
  header.window_length = window_length;
  header.stride = stride;
  header.max_beacons = max_beacons;
  header.routing_mode = static_cast<std::uint32_t>(routing_mode);
  header.containment_tolerance = containment_tolerance;
  header.reference_count = reference_count;
  header.reference_checksum = reference_checksum;
  header.root = root;
  header.layer_count = layers.size();
  header.node_count = nodes.size();
  header.child_count = children.size();
  header.beacon_count = beacons.size();
  header.distance_count = child_beacon_distances.size();
  header.metric_root_count = metric_roots.size();
  header.metric_node_count = metric_nodes.size();
  header.metric_edge_count = metric_edges.size();
  header.dense_offset_count = dense_pair_offsets.size();
  header.dense_distance_count = dense_pair_distances.size();
  write_raw(output, &header, 1);
  write_raw(output, layers.data(), layers.size());
  write_raw(output, nodes.data(), nodes.size());
  write_raw(output, children.data(), children.size());
  write_raw(output, beacons.data(), beacons.size());
  write_raw(output, child_beacon_distances.data(),
            child_beacon_distances.size());
  write_raw(output, metric_roots.data(), metric_roots.size());
  write_raw(output, metric_nodes.data(), metric_nodes.size());
  write_raw(output, metric_edges.data(), metric_edges.size());
  write_raw(output, dense_pair_offsets.data(), dense_pair_offsets.size());
  write_raw(output, dense_pair_distances.data(), dense_pair_distances.size());
}

NavigaMerIndex NavigaMerIndex::load(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("cannot open index: " + path.string());
  FileHeader header;
  read_raw(input, &header, 1);
  if (std::string_view(header.magic.data(), 7) != "NVMIDX4") {
    throw std::runtime_error("not a NavigaMer index");
  }
  if (header.version != 4 || header.endian != 0x01020304U) {
    throw std::runtime_error("unsupported NavigaMer index format");
  }
  NavigaMerIndex index;
  index.window_length = header.window_length;
  index.stride = header.stride;
  index.max_beacons = header.max_beacons;
  index.containment_tolerance = header.containment_tolerance;
  if (header.routing_mode >
      static_cast<std::uint32_t>(RoutingMode::kCompleteNestedBalls)) {
    throw std::runtime_error("unsupported NavigaMer routing mode");
  }
  index.routing_mode = static_cast<RoutingMode>(header.routing_mode);
  index.reference_count = header.reference_count;
  index.reference_checksum = header.reference_checksum;
  index.root = header.root;
  checked_resize(index.layers, header.layer_count);
  checked_resize(index.nodes, header.node_count);
  checked_resize(index.children, header.child_count);
  checked_resize(index.beacons, header.beacon_count);
  checked_resize(index.child_beacon_distances, header.distance_count);
  checked_resize(index.metric_roots, header.metric_root_count);
  checked_resize(index.metric_nodes, header.metric_node_count);
  checked_resize(index.metric_edges, header.metric_edge_count);
  checked_resize(index.dense_pair_offsets, header.dense_offset_count);
  checked_resize(index.dense_pair_distances, header.dense_distance_count);
  read_raw(input, index.layers.data(), index.layers.size());
  read_raw(input, index.nodes.data(), index.nodes.size());
  read_raw(input, index.children.data(), index.children.size());
  read_raw(input, index.beacons.data(), index.beacons.size());
  read_raw(input, index.child_beacon_distances.data(),
           index.child_beacon_distances.size());
  read_raw(input, index.metric_roots.data(), index.metric_roots.size());
  read_raw(input, index.metric_nodes.data(), index.metric_nodes.size());
  read_raw(input, index.metric_edges.data(), index.metric_edges.size());
  read_raw(input, index.dense_pair_offsets.data(), index.dense_pair_offsets.size());
  read_raw(input, index.dense_pair_distances.data(),
           index.dense_pair_distances.size());
  if (input.peek() != std::ifstream::traits_type::eof()) {
    throw std::runtime_error("index contains unexpected trailing bytes");
  }
  index.validate();
  return index;
}

void NavigaMerIndex::validate(const SequenceStore* reference) const {
  if (window_length == 0 || stride == 0 || max_beacons == 0) {
    throw std::runtime_error("invalid zero-valued index configuration");
  }
  if (routing_mode != RoutingMode::kNearestOwner &&
      routing_mode != RoutingMode::kNestedBalls &&
      routing_mode != RoutingMode::kCompleteNestedBalls) {
    throw std::runtime_error("invalid routing mode");
  }
  if (layers.empty() || nodes.empty() || root >= nodes.size()) {
    throw std::runtime_error("index has no valid hierarchy root");
  }
  if (routing_mode == RoutingMode::kCompleteNestedBalls &&
      layers.back().radius <= 2 * containment_tolerance) {
    throw std::runtime_error("complete-world guard band is invalid");
  }
  if (nodes[root].layer != kSyntheticLayer) {
    throw std::runtime_error("root node is not synthetic");
  }
  if (child_beacon_distances.size() != children.size() * max_beacons) {
    throw std::runtime_error("child/beacon distance matrix has wrong size");
  }
  if (metric_roots.size() != nodes.size()) {
    throw std::runtime_error("metric root array has wrong size");
  }
  if (dense_pair_offsets.size() != nodes.size()) {
    throw std::runtime_error("dense-pair offset array has wrong size");
  }
  for (std::size_t layer = 0; layer < layers.size(); ++layer) {
    const auto& info = layers[layer];
    if (info.radius == 0 || info.first_node + info.node_count > nodes.size()) {
      throw std::runtime_error("invalid layer range");
    }
    for (std::uint64_t id = info.first_node;
         id < info.first_node + info.node_count; ++id) {
      if (nodes[id].layer != layer) throw std::runtime_error("node layer mismatch");
      if (nodes[id].cover_radius > info.radius) {
        throw std::runtime_error("world cover radius exceeds nominal radius");
      }
      if (routing_mode == RoutingMode::kCompleteNestedBalls &&
          layer + 1 == layers.size() &&
          nodes[id].cover_radius != info.radius) {
        throw std::runtime_error("terminal world is not a full-radius ball");
      }
    }
  }
  for (NodeId node_id = 0; node_id < nodes.size(); ++node_id) {
    const auto& node = nodes[node_id];
    const auto metric_root = metric_roots[node_id];
    if (is_terminal(node) || node.child_count == 0) {
      if (metric_root != kNoNode) {
        throw std::runtime_error("terminal/empty world has a metric root");
      }
    } else if (node_id == root && node.child_count > 8192 &&
               metric_root == kNoNode) {
      // Large roots use a cache-local flat multilateration scan.
    } else if (metric_root >= metric_nodes.size()) {
      throw std::runtime_error("internal world has no valid metric root");
    }
  }
  for (const auto& metric_node : metric_nodes) {
    if (metric_node.child_slot >= children.size() ||
        metric_node.first_edge + metric_node.edge_count > metric_edges.size()) {
      throw std::runtime_error("metric node range is invalid");
    }
    if (routing_mode == RoutingMode::kNestedBalls ||
        routing_mode == RoutingMode::kCompleteNestedBalls) {
      const auto child = children[metric_node.child_slot];
      if (child >= nodes.size() ||
          metric_node.subtree_cover_radius < nodes[child].cover_radius) {
        throw std::runtime_error("nested metric subtree radius is invalid");
      }
    }
  }
  for (const auto& edge : metric_edges) {
    if (edge.child_node >= metric_nodes.size()) {
      throw std::runtime_error("metric edge child is invalid");
    }
  }
  for (NodeId node_id = 0; node_id < nodes.size(); ++node_id) {
    const auto offset = dense_pair_offsets[node_id];
    if (offset == kNoNode) continue;
    const auto count = static_cast<std::uint64_t>(nodes[node_id].child_count);
    if (offset + count * count > dense_pair_distances.size()) {
      throw std::runtime_error("dense center-distance matrix is invalid");
    }
  }
  for (NodeId id = 0; id < nodes.size(); ++id) {
    const auto& node = nodes[id];
    if (node.first_child + node.child_count > children.size() ||
        node.first_beacon + node.beacon_count > beacons.size() ||
        node.beacon_count > max_beacons) {
      throw std::runtime_error("node flat-array range is invalid");
    }
    for (std::uint32_t j = 0; j < node.beacon_count; ++j) {
      if (beacons[node.first_beacon + j] >= reference_count) {
        throw std::runtime_error("beacon sequence ID is invalid");
      }
    }
    for (std::uint32_t j = 0; j < node.child_count; ++j) {
      const auto child = children[node.first_child + j];
      if (is_terminal(node)) {
        if (child >= reference_count) {
          throw std::runtime_error("terminal child sequence ID is invalid");
        }
      } else {
        if (child >= nodes.size()) throw std::runtime_error("child node ID invalid");
        const auto expected_layer = node.layer == kSyntheticLayer ? 0 : node.layer + 1;
        if (nodes[child].layer != expected_layer) {
          throw std::runtime_error("edge skips or reverses a hierarchy layer");
        }
      }
    }
  }
  if (reference) {
    if (reference->window_length() != window_length ||
        reference->stride() != stride || reference->size() != reference_count ||
        reference->checksum() != reference_checksum) {
      throw std::runtime_error("reference does not match this index");
    }
  }
}

std::uint32_t NavigaMerIndex::child_radius(const WorldNode& parent) const noexcept {
  if (parent.layer == kSyntheticLayer) return layers.front().radius;
  if (static_cast<std::size_t>(parent.layer + 1) >= layers.size()) return 0;
  return layers[parent.layer + 1].radius;
}

std::uint64_t NavigaMerIndex::bytes() const noexcept {
  return sizeof(*this) + layers.size() * sizeof(LayerInfo) +
         nodes.size() * sizeof(WorldNode) + children.size() * sizeof(std::uint64_t) +
         beacons.size() * sizeof(SequenceId) +
         child_beacon_distances.size() * sizeof(std::uint16_t) +
         metric_roots.size() * sizeof(NodeId) +
         metric_nodes.size() * sizeof(MetricIndexNode) +
         metric_edges.size() * sizeof(MetricIndexEdge) +
         dense_pair_offsets.size() * sizeof(NodeId) +
         dense_pair_distances.size() * sizeof(std::uint8_t);
}

std::string NavigaMerIndex::summary() const {
  std::ostringstream out;
  out << "window_length\t" << window_length << "\nstride\t" << stride
      << "\nrouting_mode\t"
      << (routing_mode == RoutingMode::kCompleteNestedBalls
              ? "complete_nested_balls"
              : routing_mode == RoutingMode::kNestedBalls
              ? "nested_balls"
              : "nearest_owner")
      << "\ncontainment_tolerance\t" << containment_tolerance
      << "\nreference_sequences\t" << reference_count << "\nlayers\t"
      << layers.size() << "\nroot_children\t" << nodes[root].child_count
      << "\nroot_beacons\t"
      << static_cast<std::uint32_t>(nodes[root].beacon_count) << '\n';
  for (std::size_t i = 0; i < layers.size(); ++i) {
    std::uint64_t child_sum = 0;
    std::uint64_t cover_sum = 0;
    std::uint64_t beacon_sum = 0;
    std::uint32_t maximum_children = 0;
    std::uint32_t maximum_cover = 0;
    std::uint32_t maximum_beacons = 0;
    for (std::uint64_t local = 0; local < layers[i].node_count; ++local) {
      const auto& node = nodes[layers[i].first_node + local];
      child_sum += node.child_count;
      cover_sum += node.cover_radius;
      beacon_sum += node.beacon_count;
      maximum_children = std::max(maximum_children, node.child_count);
      maximum_cover = std::max(
          maximum_cover, static_cast<std::uint32_t>(node.cover_radius));
      maximum_beacons = std::max(
          maximum_beacons, static_cast<std::uint32_t>(node.beacon_count));
    }
    const auto count = static_cast<double>(layers[i].node_count);
    out << "layer_" << i << "_radius\t" << layers[i].radius << '\n'
        << "layer_" << i << "_worlds\t" << layers[i].node_count << '\n'
        << "layer_" << i << "_average_children\t"
        << (count == 0.0 ? 0.0 : static_cast<double>(child_sum) / count) << '\n'
        << "layer_" << i << "_maximum_children\t" << maximum_children << '\n'
        << "layer_" << i << "_average_beacons\t"
        << (count == 0.0 ? 0.0 : static_cast<double>(beacon_sum) / count) << '\n'
        << "layer_" << i << "_maximum_beacons\t" << maximum_beacons << '\n'
        << "layer_" << i << "_average_cover_radius\t"
        << (count == 0.0 ? 0.0 : static_cast<double>(cover_sum) / count) << '\n'
        << "layer_" << i << "_maximum_cover_radius\t" << maximum_cover << '\n';
  }
  out << "nodes\t" << nodes.size() << "\nchild_entries\t" << children.size()
      << "\nbeacons\t" << beacons.size()
      << "\nmetric_nodes\t" << metric_nodes.size()
      << "\nmetric_edges\t" << metric_edges.size()
      << "\ndense_pair_distances\t" << dense_pair_distances.size()
      << "\nbytes\t" << bytes() << '\n';
  return out.str();
}

}  // namespace navigamer
