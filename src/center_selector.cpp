#include "navigamer/center_selector.hpp"
#include "navigamer/batch_distance.hpp"
#include "build_parallel.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace navigamer {

struct CenterSelector::Impl {
  struct Hint {
    std::size_t center = std::numeric_limits<std::size_t>::max();
    std::uint32_t upper = 0;
    std::pair<std::string_view, std::uint64_t> location;
  };
  const SequenceStore& reference;
  std::uint32_t radius;
  std::uint64_t memory_limit;
  bool use_batch;
  std::vector<SequenceId> ids;
  PackedCenters packed;
  BatchDistance profiles;
  LayerBuildStats total;
  detail::WorkerPool workers;
  std::vector<Hint> hints;
  std::vector<std::string> scratch;
  std::vector<BatchDistance::Workspace> workspaces;
  std::vector<LayerBuildStats> local;
  std::vector<std::uint8_t> accepted;
  Hint serial_hint;
  std::string serial_scratch;
  BatchDistance::Workspace serial_workspace;
  SequenceId last_member = kNoSequence;

  Impl(const SequenceStore& ref, std::uint32_t r, std::uint32_t threads,
       const std::vector<SequenceId>& existing, std::uint64_t limit)
      : reference(ref), radius(r), memory_limit(limit),
        use_batch(r > EditDistance::kLeapMaxDistance && BatchDistance::simd_available()),
        packed(ref.window_length()), profiles(ref.window_length()),
        workers(std::min(4096u, threads)), hints(std::min(4096u, threads)),
        scratch(hints.size()), workspaces(hints.size()), local(hints.size()), accepted(4096) {
    if (!existing.empty() && (!std::is_sorted(existing.begin(), existing.end()) ||
        std::adjacent_find(existing.begin(), existing.end()) != existing.end())) {
      throw std::invalid_argument("resumed centers must be sorted and unique");
    }
    for (const auto id : existing) append(id);
    if (!ids.empty()) last_member = ids.back();
  }

  void append(SequenceId id) {
    const auto sequence = reference.sequence(id);
    ids.push_back(id);
    packed.append(sequence);
    if (use_batch) profiles.append(sequence);
    serial_hint = {ids.size() - 1, 0, reference.location(id)};
    if (memory_limit && packed.bytes() + profiles.bytes() + ids.capacity() * 8 > memory_limit) {
      throw std::runtime_error("center selection exceeded the working-memory budget");
    }
  }

  bool covered(SequenceId id, std::size_t first, std::size_t last, Hint& hint,
               std::string& buffer, BatchDistance::Workspace& workspace, LayerBuildStats& stats) {
    if (first == last) return false;
    const bool valid = hint.center >= first && hint.center < last;
    const auto location = reference.location(id);
    if (valid && location.first.data() == hint.location.first.data()) {
      const auto shift = location.second >= hint.location.second
          ? location.second - hint.location.second : hint.location.second - location.second;
      if (shift <= (radius - hint.upper) / 2) {
        hint.upper += 2 * shift;
        hint.location = location;
        ++stats.center_bound_accepts;
        return true;
      }
    }
    const auto sequence = reference.sequence(id);
    PreparedQuery query(sequence);
    if (valid) {
      ++stats.center_edit_calls;
      const auto d = query.distance(packed, hint.center, buffer, radius);
      if (d >= 0) {
        hint.upper = d;
        hint.location = location;
        ++stats.center_hint_hits;
        return true;
      }
    }
    for (auto c = first; c < last; ++c) {
      if (use_batch && c % BatchDistance::kLanes == 0 && last - c >= BatchDistance::kLanes) {
        const auto values = profiles.exact_group(sequence, c / BatchDistance::kLanes, workspace);
        stats.center_edit_calls += BatchDistance::kLanes;
        ++stats.center_batch_groups;
        for (std::size_t lane = 0; lane < BatchDistance::kLanes; ++lane) {
          if (values[lane] <= static_cast<int>(radius)) {
            hint = {c + lane, static_cast<std::uint32_t>(values[lane]), location};
            return true;
          }
        }
        c += BatchDistance::kLanes - 1;
        continue;
      }
      if (valid && c == hint.center) continue;
      ++stats.center_edit_calls;
      const auto d = query.distance(packed, c, buffer, radius);
      if (d >= 0) {
        hint = {c, static_cast<std::uint32_t>(d), location};
        return true;
      }
    }
    return false;
  }

  void consume(std::span<const SequenceId> members) {
    if (members.empty()) return;
    if ((last_member != kNoSequence && members.front() <= last_member) ||
        std::adjacent_find(members.begin(), members.end(), std::greater_equal<>()) != members.end()) {
      throw std::invalid_argument("selection members must be strictly increasing");
    }
    last_member = members.back();
    if (ids.empty()) { append(members.front()); members = members.subspan(1); }
    if (radius >= reference.window_length()) return;
    if (hints.size() == 1 || members.size() < 4096) {
      for (const auto id : members) {
        if (!covered(id, 0, ids.size(), serial_hint, serial_scratch, serial_workspace, total)) append(id);
      }
      return;
    }
    for (std::size_t first = 0; first < members.size(); first += 4096) {
      const auto block = members.subspan(first, std::min<std::size_t>(4096, members.size() - first));
      const auto snapshot = ids.size();
      workers.run([&](std::size_t worker, std::size_t count) {
        for (auto i = block.size() * worker / count; i < block.size() * (worker + 1) / count; ++i) {
          accepted[i] = covered(block[i], 0, snapshot, hints[worker], scratch[worker], workspaces[worker], local[worker]);
        }
      });
      for (std::size_t i = 0; i < block.size(); ++i) {
        if (!accepted[i] && !covered(block[i], snapshot, ids.size(), serial_hint, serial_scratch, serial_workspace, total)) append(block[i]);
      }
    }
    for (auto& value : local) {
      total.center_edit_calls += value.center_edit_calls;
      total.center_bound_accepts += value.center_bound_accepts;
      total.center_hint_hits += value.center_hint_hits;
      total.center_batch_groups += value.center_batch_groups;
      value = {};
    }
  }
};

CenterSelector::CenterSelector(const SequenceStore& reference, std::uint32_t radius,
    std::uint32_t threads, const std::vector<SequenceId>& existing, std::uint64_t memory_limit)
    : impl_(std::make_unique<Impl>(reference, radius, threads, existing, memory_limit)) {}
CenterSelector::~CenterSelector() = default;
void CenterSelector::consume(std::span<const SequenceId> members) { impl_->consume(members); }
const std::vector<SequenceId>& CenterSelector::centers() const { return impl_->ids; }
const PackedCenters& CenterSelector::sequences() const { return impl_->packed; }
const LayerBuildStats& CenterSelector::stats() const { return impl_->total; }

}  // namespace navigamer
