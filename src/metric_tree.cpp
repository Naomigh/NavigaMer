#include "navigamer/metric_tree.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace navigamer {
namespace {
constexpr std::uint32_t kNone = std::numeric_limits<std::uint32_t>::max();
}

void MetricTree::insert(SequenceId sequence_id, std::uint32_t payload) {
  if (nodes_.empty()) {
    nodes_.push_back({sequence_id, payload, kNone});
    return;
  }
  std::uint32_t current = 0;
  while (true) {
    const int d = distance_(sequences_.sequence(sequence_id),
                            sequences_.sequence(nodes_[current].sequence_id));
    if (d < 0 || d > static_cast<int>(std::numeric_limits<std::uint16_t>::max())) {
      throw std::runtime_error("edit distance exceeds MetricTree edge width");
    }
    std::uint32_t edge = nodes_[current].first_edge;
    while (edge != kNone && edges_[edge].distance != d) edge = edges_[edge].next;
    if (edge != kNone) {
      current = edges_[edge].child;
      continue;
    }
    if (nodes_.size() >= kNone || edges_.size() >= kNone) {
      throw std::length_error("MetricTree exceeds 32-bit construction capacity");
    }
    const auto child = static_cast<std::uint32_t>(nodes_.size());
    nodes_.push_back({sequence_id, payload, kNone});
    const auto new_edge = static_cast<std::uint32_t>(edges_.size());
    edges_.push_back({child, nodes_[current].first_edge,
                      static_cast<std::uint16_t>(d), 0});
    nodes_[current].first_edge = new_edge;
    return;
  }
}

bool MetricTree::any_within(SequenceId query, std::uint32_t radius,
                            std::uint32_t* payload) const {
  if (nodes_.empty()) return false;
  std::vector<std::uint32_t> stack{0};
  while (!stack.empty()) {
    const auto node_id = stack.back();
    stack.pop_back();
    const int d = distance_(sequences_.sequence(query),
                            sequences_.sequence(nodes_[node_id].sequence_id));
    if (d <= static_cast<int>(radius)) {
      if (payload) *payload = nodes_[node_id].payload;
      return true;
    }
    const int low = std::max(0, d - static_cast<int>(radius));
    const int high = d + static_cast<int>(radius);
    for (auto edge = nodes_[node_id].first_edge; edge != kNone;
         edge = edges_[edge].next) {
      if (edges_[edge].distance >= low && edges_[edge].distance <= high) {
        stack.push_back(edges_[edge].child);
      }
    }
  }
  return false;
}

std::vector<std::uint32_t> MetricTree::range(SequenceId query,
                                             std::uint32_t radius) const {
  std::vector<std::uint32_t> result;
  if (nodes_.empty()) return result;
  std::vector<std::uint32_t> stack{0};
  while (!stack.empty()) {
    const auto node_id = stack.back();
    stack.pop_back();
    const int d = distance_(sequences_.sequence(query),
                            sequences_.sequence(nodes_[node_id].sequence_id));
    if (d <= static_cast<int>(radius)) result.push_back(nodes_[node_id].payload);
    const int low = std::max(0, d - static_cast<int>(radius));
    const int high = d + static_cast<int>(radius);
    for (auto edge = nodes_[node_id].first_edge; edge != kNone;
         edge = edges_[edge].next) {
      if (edges_[edge].distance >= low && edges_[edge].distance <= high) {
        stack.push_back(edges_[edge].child);
      }
    }
  }
  return result;
}

MetricTree::NearestResult MetricTree::nearest(
    SequenceId query, std::uint32_t initial_payload,
    std::uint32_t initial_distance) const {
  if (nodes_.empty()) throw std::logic_error("nearest on empty MetricTree");
  NearestResult best{initial_payload, initial_distance};
  std::vector<std::uint32_t> stack{0};
  while (!stack.empty()) {
    const auto node_id = stack.back();
    stack.pop_back();
    const int d = distance_(sequences_.sequence(query),
                            sequences_.sequence(nodes_[node_id].sequence_id));
    if (d < 0) throw std::logic_error("unbounded Edlib returned no distance");
    if (static_cast<std::uint32_t>(d) < best.distance ||
        (static_cast<std::uint32_t>(d) == best.distance &&
         nodes_[node_id].payload < best.payload)) {
      best = {nodes_[node_id].payload, static_cast<std::uint32_t>(d)};
    }
    const int low = std::max(0, d - static_cast<int>(best.distance));
    const int high = d + static_cast<int>(best.distance);
    for (auto edge = nodes_[node_id].first_edge; edge != kNone;
         edge = edges_[edge].next) {
      if (edges_[edge].distance >= low && edges_[edge].distance <= high) {
        stack.push_back(edges_[edge].child);
      }
    }
  }
  return best;
}

}  // namespace navigamer
