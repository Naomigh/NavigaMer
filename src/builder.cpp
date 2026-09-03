#include "navigamer/builder.hpp"

#include "navigamer/metric_tree.hpp"

#include <algorithm>
#include <chrono>
#include <deque>
#include <future>
#include <limits>
#include <memory>
#include <numeric>
#include <random>
#include <stdexcept>
#include <thread>
#include <unordered_set>
#include <utility>

namespace navigamer {
namespace {

using Clock = std::chrono::steady_clock;

std::uint32_t effective_threads(std::uint32_t requested) {
  if (requested != 0) return requested;
  return std::max(1U, std::thread::hardware_concurrency());
}

template <class Fn>
void run_ranges(std::uint64_t count, std::uint32_t threads, Fn&& fn) {
  if (count == 0) return;
  threads = std::min<std::uint32_t>(threads, count);
  std::vector<std::future<void>> futures;
  futures.reserve(threads);
  for (std::uint32_t thread = 0; thread < threads; ++thread) {
    const auto begin = count * thread / threads;
    const auto end = count * (thread + 1) / threads;
    futures.push_back(std::async(std::launch::async, [&, thread, begin, end] {
      fn(thread, begin, end);
    }));
  }
  for (auto& future : futures) future.get();
}

struct CenterSelection {
  std::vector<SequenceId> centers;
  std::vector<std::uint32_t> covering_hint;
  std::unique_ptr<MetricTree> directory;
};

struct Partition {
  std::vector<SequenceId> centers;
  std::vector<std::vector<SequenceId>> members;
  std::vector<std::uint16_t> cover_radii;
};

struct TemporaryWorld {
  SequenceId center{kNoSequence};
  std::uint64_t member_count{0};
  std::uint32_t parent_local{0};
  std::uint16_t cover_radius{0};
  std::vector<SequenceId> members;
  std::vector<std::uint32_t> child_worlds;
};

CenterSelection select_centers(const SequenceStore& sequences,
                               const EditDistance& distance,
                               const std::vector<SequenceId>& items,
                               std::uint32_t radius,
                               std::uint32_t hot_cache_size,
                               bool delayed_centers,
                               bool exact_global_reuse = true) {
  CenterSelection result;
  result.covering_hint.resize(items.size());
  result.directory = std::make_unique<MetricTree>(sequences, distance);
  std::deque<std::uint32_t> hot;

  for (std::size_t ordinal = 0; ordinal < items.size(); ++ordinal) {
    const auto sequence_id = items[ordinal];
    auto prepared = distance.prepare(sequences.sequence(sequence_id));
    bool covered = false;
    std::uint32_t covering = 0;
    for (const auto local_id : hot) {
      const int d = prepared(sequences.sequence(result.centers[local_id]));
      if (d <= static_cast<int>(radius)) {
        covered = true;
        covering = local_id;
        break;
      }
    }
    if (!covered && exact_global_reuse && !result.directory->empty() &&
        result.directory->any_within(sequence_id, radius, &covering)) {
      covered = true;
    }

    if (!covered) {
      SequenceId center = sequence_id;
      if (delayed_centers && ordinal + 1 < items.size()) {
        const auto lookahead = std::max<std::uint32_t>(1, radius / 2);
        const auto candidate_ordinal =
            std::min<std::size_t>(items.size() - 1, ordinal + lookahead);
        const auto candidate = items[candidate_ordinal];
        const bool covers_trigger =
            prepared(sequences.sequence(candidate)) <= static_cast<int>(radius);
        std::uint32_t ignored = 0;
        const bool separated = !exact_global_reuse || result.directory->empty() ||
            !result.directory->any_within(candidate, radius, &ignored);
        if (covers_trigger && separated) center = candidate;
      }
      if (result.centers.size() >=
          static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        throw std::length_error("one parent has more than 2^32 child worlds");
      }
      covering = static_cast<std::uint32_t>(result.centers.size());
      result.centers.push_back(center);
      if (exact_global_reuse) result.directory->insert(center, covering);
    }
    result.covering_hint[ordinal] = covering;
    if (hot_cache_size != 0) {
      const auto found = std::find(hot.begin(), hot.end(), covering);
      if (found != hot.end()) hot.erase(found);
      hot.push_front(covering);
      while (hot.size() > hot_cache_size) hot.pop_back();
    }
  }
  if (result.centers.empty()) throw std::logic_error("center selection is empty");
  return result;
}

Partition assign_nearest_owners(const SequenceStore& sequences,
                                const EditDistance& distance,
                                const std::vector<SequenceId>& items,
                                CenterSelection selection,
                                std::uint32_t radius,
                                std::uint32_t threads) {
  std::vector<std::uint32_t> owners(items.size());
  std::vector<std::uint16_t> owner_distances(items.size());
  run_ranges(items.size(), threads,
             [&](std::uint32_t, std::uint64_t begin, std::uint64_t end) {
    for (std::uint64_t ordinal = begin; ordinal < end; ++ordinal) {
      const auto sequence_id = items[ordinal];
      const auto hint = selection.covering_hint[ordinal];
      const int hint_distance = distance(
          sequences.sequence(sequence_id),
          sequences.sequence(selection.centers[hint]));
      if (hint_distance < 0 || hint_distance > static_cast<int>(radius)) {
        throw std::logic_error("online covering hint violates world radius");
      }
      const auto nearest = selection.directory->nearest(
          sequence_id, hint, static_cast<std::uint32_t>(hint_distance));
      if (nearest.distance > radius || nearest.distance >= kMissingDistance) {
        throw std::logic_error("nearest-owner compaction lost world coverage");
      }
      owners[ordinal] = nearest.payload;
      owner_distances[ordinal] = static_cast<std::uint16_t>(nearest.distance);
    }
  });

  Partition partition;
  partition.centers = std::move(selection.centers);
  partition.members.resize(partition.centers.size());
  partition.cover_radii.assign(partition.centers.size(), 0);
  std::vector<std::size_t> counts(partition.centers.size(), 0);
  for (const auto owner : owners) ++counts[owner];
  for (std::size_t owner = 0; owner < counts.size(); ++owner) {
    partition.members[owner].reserve(counts[owner]);
  }
  for (std::size_t ordinal = 0; ordinal < items.size(); ++ordinal) {
    const auto owner = owners[ordinal];
    partition.members[owner].push_back(items[ordinal]);
    partition.cover_radii[owner] =
        std::max(partition.cover_radii[owner], owner_distances[ordinal]);
  }
  for (std::size_t owner = 0; owner < partition.centers.size(); ++owner) {
    if (partition.members[owner].empty()) {
      throw std::logic_error("nearest-owner compaction produced an empty world");
    }
  }
  return partition;
}

Partition assign_online_owners(const SequenceStore& sequences,
                               const EditDistance& distance,
                               const std::vector<SequenceId>& items,
                               CenterSelection selection,
                               std::uint32_t radius,
                               std::uint32_t threads) {
  std::vector<std::uint16_t> owner_distances(items.size());
  run_ranges(items.size(), threads,
             [&](std::uint32_t, std::uint64_t begin, std::uint64_t end) {
    for (std::uint64_t ordinal = begin; ordinal < end; ++ordinal) {
      const auto owner = selection.covering_hint[ordinal];
      const int d = distance(sequences.sequence(items[ordinal]),
                             sequences.sequence(selection.centers[owner]),
                             static_cast<int>(radius));
      if (d < 0 || d >= kMissingDistance) {
        throw std::logic_error("online owner violates leaf-world radius");
      }
      owner_distances[ordinal] = static_cast<std::uint16_t>(d);
    }
  });

  Partition partition;
  partition.centers = std::move(selection.centers);
  partition.members.resize(partition.centers.size());
  partition.cover_radii.assign(partition.centers.size(), 0);
  std::vector<std::size_t> counts(partition.centers.size(), 0);
  for (const auto owner : selection.covering_hint) ++counts[owner];
  for (std::size_t owner = 0; owner < counts.size(); ++owner) {
    if (counts[owner] == 0) {
      throw std::logic_error("online construction produced an empty leaf world");
    }
    partition.members[owner].reserve(counts[owner]);
  }
  for (std::size_t ordinal = 0; ordinal < items.size(); ++ordinal) {
    const auto owner = selection.covering_hint[ordinal];
    partition.members[owner].push_back(items[ordinal]);
    partition.cover_radii[owner] =
        std::max(partition.cover_radii[owner], owner_distances[ordinal]);
  }
  return partition;
}

std::vector<TemporaryWorld> pack_frozen_worlds(
    const SequenceStore& sequences, const EditDistance& distance,
    std::vector<TemporaryWorld>& children, std::uint32_t parent_radius,
    std::uint32_t hot_cache_size, bool exact_global_reuse) {
  std::vector<TemporaryWorld> parents;
  MetricTree directory(sequences, distance);
  std::deque<std::uint32_t> hot;

  for (std::uint32_t child_local = 0; child_local < children.size();
       ++child_local) {
    auto& child = children[child_local];
    auto prepared = distance.prepare(sequences.sequence(child.center));
    if (child.cover_radius > parent_radius) {
      throw std::logic_error("child occupied ball exceeds parent radius");
    }
    const auto center_slack = parent_radius - child.cover_radius;
    bool contained = false;
    std::uint32_t parent_local = 0;
    int center_distance = -1;
    for (const auto candidate : hot) {
      const int d = prepared(sequences.sequence(parents[candidate].center));
      if (d <= static_cast<int>(center_slack)) {
        contained = true;
        parent_local = candidate;
        center_distance = d;
        break;
      }
    }
    if (!contained && exact_global_reuse && !directory.empty() &&
        directory.any_within(child.center, center_slack, &parent_local)) {
      center_distance = distance(
          sequences.sequence(child.center),
          sequences.sequence(parents[parent_local].center),
          static_cast<int>(center_slack));
      if (center_distance < 0) {
        throw std::logic_error("metric directory returned invalid containment");
      }
      contained = true;
    }
    if (!contained) {
      if (parents.size() >=
          static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        throw std::length_error("one packed layer exceeds 2^32 worlds");
      }
      parent_local = static_cast<std::uint32_t>(parents.size());
      TemporaryWorld parent;
      parent.center = child.center;
      parent.cover_radius = child.cover_radius;
      parents.push_back(std::move(parent));
      if (exact_global_reuse) directory.insert(child.center, parent_local);
      center_distance = 0;
    }

    auto& parent = parents[parent_local];
    const auto occupied_radius = static_cast<std::uint32_t>(center_distance) +
                                 child.cover_radius;
    if (occupied_radius > parent_radius || occupied_radius >= kMissingDistance) {
      throw std::logic_error("packed child ball is not contained by parent");
    }
    parent.cover_radius = std::max(
        parent.cover_radius, static_cast<std::uint16_t>(occupied_radius));
    parent.member_count += child.member_count;
    parent.child_worlds.push_back(child_local);
    child.parent_local = parent_local;

    if (hot_cache_size != 0) {
      const auto found = std::find(hot.begin(), hot.end(), parent_local);
      if (found != hot.end()) hot.erase(found);
      hot.push_front(parent_local);
      while (hot.size() > hot_cache_size) hot.pop_back();
    }
  }
  if (parents.empty()) throw std::logic_error("packed layer is empty");
  return parents;
}

// Top-down construction first creates candidate centers at every radius. This
// repair pass then gives every occupied child ball exactly one containing
// parent. Candidate parents that never receive a child disappear, while a
// child with no containing candidate promotes its own center into the parent
// layer. The active-parent directory and hot deque prefer already-used worlds,
// which keeps both the number of worlds and the number of physical edges low.
std::vector<TemporaryWorld> repair_parent_layer(
    const SequenceStore& sequences, const EditDistance& distance,
    std::vector<SequenceId> candidate_centers,
    std::vector<TemporaryWorld>& children, std::uint32_t parent_radius,
    std::uint32_t hot_cache_size, bool exact_global_reuse) {
  if (candidate_centers.empty() || children.empty()) {
    throw std::logic_error("top-down repair received an empty layer");
  }

  const auto initial_candidate_count = candidate_centers.size();
  std::unique_ptr<MetricTree> candidates;
  std::unique_ptr<MetricTree> active;
  if (exact_global_reuse) {
    candidates = std::make_unique<MetricTree>(sequences, distance);
    active = std::make_unique<MetricTree>(sequences, distance);
    for (std::uint32_t candidate = 0; candidate < candidate_centers.size();
         ++candidate) {
      candidates->insert(candidate_centers[candidate], candidate);
    }
  }
  std::vector<std::uint32_t> candidate_to_parent(
      candidate_centers.size(), std::numeric_limits<std::uint32_t>::max());
  std::deque<std::uint32_t> hot_candidates;
  std::vector<TemporaryWorld> parents;

  auto activate = [&](std::uint32_t candidate) {
    if (candidate >= candidate_centers.size()) {
      throw std::logic_error("top-down repair candidate is out of range");
    }
    auto& mapped = candidate_to_parent[candidate];
    if (mapped != std::numeric_limits<std::uint32_t>::max()) return mapped;
    if (parents.size() >=
        static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
      throw std::length_error("one repaired layer exceeds 2^32 worlds");
    }
    mapped = static_cast<std::uint32_t>(parents.size());
    TemporaryWorld parent;
    parent.center = candidate_centers[candidate];
    parents.push_back(std::move(parent));
    if (exact_global_reuse) {
      active->insert(candidate_centers[candidate], candidate);
    }
    return mapped;
  };

  auto touch_hot = [&](std::uint32_t candidate) {
    if (hot_cache_size == 0) return;
    const auto found = std::find(
        hot_candidates.begin(), hot_candidates.end(), candidate);
    if (found != hot_candidates.end()) hot_candidates.erase(found);
    hot_candidates.push_front(candidate);
    while (hot_candidates.size() > hot_cache_size) hot_candidates.pop_back();
  };

  for (std::uint32_t child_local = 0; child_local < children.size();
       ++child_local) {
    auto& child = children[child_local];
    auto prepared = distance.prepare(sequences.sequence(child.center));
    if (child.cover_radius > parent_radius) {
      throw std::logic_error("child occupied ball exceeds repaired parent radius");
    }
    const auto slack = parent_radius - child.cover_radius;
    std::uint32_t candidate = std::numeric_limits<std::uint32_t>::max();
    int center_distance = -1;

    for (const auto hot : hot_candidates) {
      const int d = prepared(sequences.sequence(candidate_centers[hot]));
      if (d <= static_cast<int>(slack)) {
        candidate = hot;
        center_distance = d;
        break;
      }
    }

    if (candidate == std::numeric_limits<std::uint32_t>::max() &&
        exact_global_reuse && !active->empty() &&
        active->any_within(child.center, slack, &candidate)) {
      center_distance = distance(
          sequences.sequence(child.center),
          sequences.sequence(candidate_centers[candidate]),
          static_cast<int>(slack));
      if (center_distance < 0) {
        throw std::logic_error("active directory returned invalid containment");
      }
    }

    if (candidate == std::numeric_limits<std::uint32_t>::max() &&
        exact_global_reuse &&
        candidates->any_within(child.center, slack, &candidate)) {
      center_distance = distance(
          sequences.sequence(child.center),
          sequences.sequence(candidate_centers[candidate]),
          static_cast<int>(slack));
      if (center_distance < 0) {
        throw std::logic_error("candidate directory returned invalid containment");
      }
    }

    // The scalable local mode checks the candidate centers nearest in scan
    // order. A miss only promotes the child center, so it can create a
    // redundant parent but can never lose containment or a reference member.
    if (candidate == std::numeric_limits<std::uint32_t>::max() &&
        !exact_global_reuse) {
      const auto initial_end = candidate_centers.begin() +
          static_cast<std::ptrdiff_t>(initial_candidate_count);
      const auto position = std::lower_bound(
          candidate_centers.begin(), initial_end, child.center);
      const auto ordinal = static_cast<std::size_t>(
          position - candidate_centers.begin());
      const auto neighborhood = std::max<std::size_t>(1, hot_cache_size);
      const auto begin = ordinal > neighborhood ? ordinal - neighborhood : 0;
      const auto end = std::min(initial_candidate_count,
                                ordinal + neighborhood + 1);
      for (std::size_t local = begin; local < end; ++local) {
        const int d = prepared(sequences.sequence(candidate_centers[local]));
        if (d <= static_cast<int>(slack)) {
          candidate = static_cast<std::uint32_t>(local);
          center_distance = d;
          break;
        }
      }
    }

    if (candidate == std::numeric_limits<std::uint32_t>::max()) {
      if (candidate_centers.size() >=
          static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        throw std::length_error("top-down promoted layer exceeds 2^32 worlds");
      }
      candidate = static_cast<std::uint32_t>(candidate_centers.size());
      candidate_centers.push_back(child.center);
      candidate_to_parent.push_back(
          std::numeric_limits<std::uint32_t>::max());
      if (exact_global_reuse) candidates->insert(child.center, candidate);
      center_distance = 0;
    }

    const auto parent_local = activate(candidate);
    auto& parent = parents[parent_local];
    const auto occupied_radius = static_cast<std::uint32_t>(center_distance) +
                                 child.cover_radius;
    if (occupied_radius > parent_radius || occupied_radius >= kMissingDistance) {
      throw std::logic_error("repaired child ball is not contained by parent");
    }
    parent.cover_radius = std::max(
        parent.cover_radius, static_cast<std::uint16_t>(occupied_radius));
    parent.member_count += child.member_count;
    parent.child_worlds.push_back(child_local);
    child.parent_local = parent_local;
    touch_hot(candidate);
  }

  if (parents.empty()) throw std::logic_error("top-down repair made no parents");
  return parents;
}

constexpr std::uint32_t kMaximumConstructionQgram = 9;
constexpr std::uint32_t kConstructionAlphabet = 5;

std::uint32_t construction_symbol(unsigned char symbol) noexcept {
  switch (symbol) {
    case 'A': return 0;
    case 'C': return 1;
    case 'G': return 2;
    case 'T': return 3;
    default: return 4;
  }
}

std::uint32_t construction_qgram_code(std::string_view sequence,
                                      std::size_t offset,
                                      std::uint32_t qgram) noexcept {
  std::uint32_t code = 0;
  for (std::uint32_t i = 0; i < qgram; ++i) {
    code = code * kConstructionAlphabet +
        construction_symbol(static_cast<unsigned char>(sequence[offset + i]));
  }
  return code;
}

// Materialize every reference point in every terminal radius ball. Candidate
// generation uses the exact q-gram count lemma only during construction; all
// candidates are verified by the full edit metric and the query algorithm
// never uses seeds. For equal-length strings of length L, ED(A,B)<=R implies
// at least (L-q+1)-qR shared q-grams (with multiplicity). Occurrences in the
// underlying contigs contribute whole intervals of sliding windows, so an
// event sweep avoids expanding the highly redundant postings eagerly.
void expand_complete_leaf_memberships(
    const SequenceStore& sequences, const EditDistance& distance,
    std::vector<TemporaryWorld>& leaves, std::uint32_t radius,
    std::uint32_t threads) {
  if (leaves.empty()) throw std::logic_error("cannot expand empty leaf layer");
  const auto qgram = std::min<std::uint32_t>(
      kMaximumConstructionQgram,
      sequences.window_length() / (radius + 1));
  if (qgram == 0) {
    throw std::invalid_argument(
        "terminal radius must be smaller than the indexed window");
  }
  const auto qgram_count = sequences.window_length() - qgram + 1;
  const auto minimum_shared = static_cast<int>(qgram_count) -
      static_cast<int>(qgram * radius);
  if (minimum_shared <= 0) {
    throw std::invalid_argument(
        "terminal radius is too large for exact construction q-gram filter");
  }
  std::uint32_t universe = 1;
  for (std::uint32_t i = 0; i < qgram; ++i) {
    universe *= kConstructionAlphabet;
  }
  struct Occurrence {
    std::uint32_t contig{0};
    std::uint32_t position{0};
  };
  std::vector<std::vector<Occurrence>> postings(universe);
  for (std::uint32_t contig_id = 0; contig_id < sequences.contigs().size();
       ++contig_id) {
    const auto& contig = sequences.contigs()[contig_id];
    if (contig.bases.size() > std::numeric_limits<std::uint32_t>::max()) {
      throw std::length_error("one contig exceeds construction posting width");
    }
    for (std::uint32_t position = 0;
         position + qgram <= contig.bases.size(); ++position) {
      postings[construction_qgram_code(contig.bases, position, qgram)].push_back(
          {contig_id, position});
    }
  }

  std::vector<std::vector<SequenceId>> expanded(leaves.size());
  run_ranges(leaves.size(), threads,
             [&](std::uint32_t, std::uint64_t begin, std::uint64_t end) {
    for (std::uint64_t leaf = begin; leaf < end; ++leaf) {
      struct Event {
        SequenceId sequence{0};
        std::int32_t delta{0};
      };
      const auto query = sequences.sequence(leaves[leaf].center);
      std::vector<Event> events;
      events.reserve(static_cast<std::size_t>(qgram_count) * 64);
      for (std::uint32_t offset = 0; offset < qgram_count; ++offset) {
        const auto code = construction_qgram_code(query, offset, qgram);
        for (const auto occurrence : postings[code]) {
          const auto& contig = sequences.contigs()[occurrence.contig];
          if (contig.window_count == 0) continue;
          const auto reach = sequences.window_length() - qgram;
          const auto low_base = occurrence.position > reach
              ? occurrence.position - reach : 0;
          const auto high_base = std::min<std::uint64_t>(
              occurrence.position,
              contig.bases.size() - sequences.window_length());
          const auto low_ordinal =
              (low_base + sequences.stride() - 1) / sequences.stride();
          const auto high_ordinal = high_base / sequences.stride();
          if (low_ordinal > high_ordinal ||
              low_ordinal >= contig.window_count) {
            continue;
          }
          const auto bounded_high = std::min<std::uint64_t>(
              high_ordinal, contig.window_count - 1);
          events.push_back(
              {contig.first_sequence_id + low_ordinal, 1});
          events.push_back(
              {contig.first_sequence_id + bounded_high + 1, -1});
        }
      }
      std::sort(events.begin(), events.end(), [](const auto& lhs,
                                                 const auto& rhs) {
        if (lhs.sequence != rhs.sequence) return lhs.sequence < rhs.sequence;
        return lhs.delta < rhs.delta;
      });
      auto prepared = distance.prepare(query);
      auto& output = expanded[leaf];
      std::int32_t shared = 0;
      SequenceId previous = events.empty() ? 0 : events.front().sequence;
      std::size_t event = 0;
      while (event < events.size()) {
        const auto position = events[event].sequence;
        if (shared >= minimum_shared) {
          for (SequenceId candidate = previous; candidate < position;
               ++candidate) {
            if (prepared(sequences.sequence(candidate)) <=
                static_cast<int>(radius)) {
              output.push_back(candidate);
            }
          }
        }
        std::int32_t delta = 0;
        while (event < events.size() && events[event].sequence == position) {
          delta += events[event].delta;
          ++event;
        }
        shared += delta;
        previous = position;
      }
      std::sort(output.begin(), output.end());
      output.erase(std::unique(output.begin(), output.end()), output.end());
      if (output.empty()) {
        throw std::logic_error("complete terminal world lost its center");
      }
    }
  });

  for (std::size_t leaf = 0; leaf < leaves.size(); ++leaf) {
    leaves[leaf].members = std::move(expanded[leaf]);
    leaves[leaf].member_count = leaves[leaf].members.size();
    leaves[leaf].cover_radius = static_cast<std::uint16_t>(radius);
  }
}

// The streaming repair pass establishes one globally valid containing parent.
// Add the other coordinate-local containing parents used by nearby-query cache
// hand-offs. Missing a distant redundant edge cannot affect correctness: the
// repaired skeletal edge remains, and strict early termination is enabled only
// for complete terminal balls. Sorting parents by SequenceId makes the join a
// compact, prefetch-friendly neighborhood scan instead of a degenerate global
// BK range search.
void rebind_local_containing_parents(
    const SequenceStore& sequences, const EditDistance& distance,
    std::vector<TemporaryWorld>& parents,
    const std::vector<TemporaryWorld>& children, std::uint32_t parent_radius,
    std::uint32_t hot_cache_size, std::uint32_t threads) {
  if (parents.empty() || children.empty()) {
    throw std::logic_error("cannot rebind an empty hierarchy layer");
  }
  std::vector<std::uint32_t> parent_order(parents.size());
  std::iota(parent_order.begin(), parent_order.end(), 0);
  std::sort(parent_order.begin(), parent_order.end(), [&](auto lhs, auto rhs) {
    if (parents[lhs].center != parents[rhs].center) {
      return parents[lhs].center < parents[rhs].center;
    }
    return lhs < rhs;
  });

  std::vector<std::vector<std::uint32_t>> parents_for_child(children.size());
  run_ranges(children.size(), threads,
             [&](std::uint32_t, std::uint64_t begin, std::uint64_t end) {
    for (std::uint64_t child = begin; child < end; ++child) {
      if (children[child].cover_radius > parent_radius) {
        throw std::logic_error("child ball exceeds parent layer radius");
      }
      const auto slack = parent_radius - children[child].cover_radius;
      std::vector<std::uint32_t> matches{children[child].parent_local};
      const auto position = std::lower_bound(
          parent_order.begin(), parent_order.end(), children[child].center,
          [&](std::uint32_t parent, SequenceId center) {
            return parents[parent].center < center;
          });
      const auto ordinal = static_cast<std::size_t>(
          position - parent_order.begin());
      const auto neighborhood = std::max<std::size_t>(1, hot_cache_size);
      const auto local_begin = ordinal > neighborhood
          ? ordinal - neighborhood : 0;
      const auto local_end = std::min(parent_order.size(),
                                      ordinal + neighborhood + 1);
      auto prepared = distance.prepare(
          sequences.sequence(children[child].center));
      for (std::size_t local = local_begin; local < local_end; ++local) {
#if defined(__GNUC__) || defined(__clang__)
        if (local + 8 < local_end) {
          __builtin_prefetch(&parents[parent_order[local + 8]], 0, 1);
        }
#endif
        const auto parent = parent_order[local];
        if (prepared(sequences.sequence(parents[parent].center)) <=
            static_cast<int>(slack)) {
          matches.push_back(parent);
        }
      }
      std::sort(matches.begin(), matches.end());
      matches.erase(std::unique(matches.begin(), matches.end()), matches.end());
      if (matches.empty()) {
        throw std::logic_error("rebind lost the repaired containing parent");
      }
      parents_for_child[child] = std::move(matches);
    }
  });

  for (auto& parent : parents) {
    parent.child_worlds.clear();
    parent.member_count = 0;
    parent.cover_radius = static_cast<std::uint16_t>(parent_radius);
  }
  for (std::uint32_t child = 0; child < children.size(); ++child) {
    for (const auto parent : parents_for_child[child]) {
      parents[parent].child_worlds.push_back(child);
      parents[parent].member_count += children[child].member_count;
    }
  }
  for (auto& parent : parents) {
    std::sort(parent.child_worlds.begin(), parent.child_worlds.end());
    if (parent.child_worlds.empty()) {
      throw std::logic_error("rebind produced an empty active parent");
    }
  }
}

struct LocalMetricNode {
  std::uint64_t child_slot{0};
  std::vector<std::pair<std::uint16_t, std::uint32_t>> edges;
  std::uint32_t subtree_cover_radius{0};
};

void build_persistent_metric_tree(NavigaMerIndex& index, NodeId parent_id,
                                  const SequenceStore& sequences,
                                  const EditDistance& distance) {
  const auto& parent = index.nodes[parent_id];
  if (index.is_terminal(parent) || parent.child_count == 0 ||
      (parent_id == index.root && parent.child_count > 8192)) {
    return;
  }

  std::vector<std::uint64_t> slots(parent.child_count);
  std::iota(slots.begin(), slots.end(), parent.first_child);
  // Deterministic shuffling avoids reference-coordinate insertion order. It
  // is not a sequence filter and cannot change the returned set.
  std::mt19937_64 random(0x4e61766967614d65ULL ^ parent_id);
  std::shuffle(slots.begin(), slots.end(), random);

  std::vector<LocalMetricNode> local;
  const auto root_child = index.children[slots.front()];
  local.push_back(
      {slots.front(), {}, index.nodes[root_child].cover_radius});
  for (std::size_t ordinal = 1; ordinal < slots.size(); ++ordinal) {
    const auto slot = slots[ordinal];
    const auto child = index.children[slot];
    const auto center = index.nodes[child].center_sequence_id;
    auto prepared = distance.prepare(sequences.sequence(center));
    std::uint32_t current = 0;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> ancestors;
    while (true) {
      const auto current_child = index.children[local[current].child_slot];
      const auto current_center = index.nodes[current_child].center_sequence_id;
      const int d = prepared(sequences.sequence(current_center));
      if (d < 0 || d >= kMissingDistance) {
        throw std::length_error("metric-directory distance exceeds uint16");
      }
      ancestors.push_back(
          {current, static_cast<std::uint32_t>(d)});
      auto edge = std::find_if(local[current].edges.begin(),
          local[current].edges.end(), [&](const auto& value) {
            return value.first == static_cast<std::uint16_t>(d);
          });
      if (edge != local[current].edges.end()) {
        current = edge->second;
        continue;
      }
      if (local.size() >= std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("persistent metric directory exceeds 32-bit IDs");
      }
      const auto child_node = static_cast<std::uint32_t>(local.size());
      local.push_back({slot, {}, index.nodes[child].cover_radius});
      for (const auto& [ancestor, ancestor_distance] : ancestors) {
        local[ancestor].subtree_cover_radius = std::max(
            local[ancestor].subtree_cover_radius,
            ancestor_distance + index.nodes[child].cover_radius);
      }
      local[current].edges.push_back(
          {static_cast<std::uint16_t>(d), child_node});
      break;
    }
  }

  if (index.metric_nodes.size() + local.size() >=
      std::numeric_limits<std::uint32_t>::max()) {
    throw std::length_error("global metric directory exceeds 32-bit IDs");
  }
  const auto base = static_cast<std::uint32_t>(index.metric_nodes.size());
  index.metric_roots[parent_id] = base;
  index.metric_nodes.resize(index.metric_nodes.size() + local.size());
  for (std::uint32_t local_id = 0; local_id < local.size(); ++local_id) {
    auto& output = index.metric_nodes[base + local_id];
    output.child_slot = local[local_id].child_slot;
    output.first_edge = index.metric_edges.size();
    std::sort(local[local_id].edges.begin(), local[local_id].edges.end());
    for (const auto& [edge_distance, child_node] : local[local_id].edges) {
      index.metric_edges.push_back(
          {static_cast<std::uint32_t>(base + child_node), edge_distance, 0});
    }
    output.edge_count = static_cast<std::uint32_t>(local[local_id].edges.size());
    output.subtree_cover_radius = local[local_id].subtree_cover_radius;
  }
}

void build_dense_pair_matrix(NavigaMerIndex& index, NodeId parent_id,
                             const SequenceStore& sequences,
                             const EditDistance& distance,
                             std::uint32_t threads) {
  constexpr std::uint32_t kDensePairLimit = 4096;
  const auto& parent = index.nodes[parent_id];
  if (index.is_terminal(parent) || parent.child_count == 0 ||
      parent.child_count > kDensePairLimit) {
    return;
  }
  const auto count = static_cast<std::uint64_t>(parent.child_count);
  const auto offset = index.dense_pair_distances.size();
  index.dense_pair_offsets[parent_id] = offset;
  index.dense_pair_distances.resize(offset + count * count);
  auto calculate_rows = [&](std::uint64_t begin, std::uint64_t end) {
    for (std::uint64_t row = begin; row < end; ++row) {
      const auto row_child = index.children[parent.first_child + row];
      const auto row_center = index.nodes[row_child].center_sequence_id;
      auto prepared = distance.prepare(sequences.sequence(row_center));
      for (std::uint64_t column = 0; column < count; ++column) {
        const auto column_child = index.children[parent.first_child + column];
        const auto column_center = index.nodes[column_child].center_sequence_id;
        const int d = prepared(sequences.sequence(column_center));
        if (d < 0 || d >= std::numeric_limits<std::uint8_t>::max()) {
          throw std::length_error("dense center distance exceeds uint8 capacity");
        }
        index.dense_pair_distances[offset + row * count + column] =
            static_cast<std::uint8_t>(d);
      }
    }
  };
  if (count >= 256 && threads > 1) {
    run_ranges(count, threads,
               [&](std::uint32_t, std::uint64_t begin, std::uint64_t end) {
      calculate_rows(begin, end);
    });
  } else {
    calculate_rows(0, count);
  }
}

}  // namespace

NavigaMerIndex IndexBuilder::build(const BuildConfig& input_config,
                                   BuildStats* stats) const {
  BuildConfig config = input_config;
  config.threads = effective_threads(config.threads);
  if (sequences_.size() == 0) throw std::invalid_argument("reference has no windows");
  if (config.radii.empty() || config.max_beacons == 0) {
    throw std::invalid_argument("at least one radius and beacon are required");
  }
  if (config.radii.size() > kSyntheticLayer) {
    throw std::invalid_argument("too many hierarchy layers for disk layout");
  }
  for (std::size_t layer = 0; layer < config.radii.size(); ++layer) {
    if (config.radii[layer] == 0 || config.radii[layer] >= kMissingDistance) {
      throw std::invalid_argument("radii must fit positive uint16 ranges");
    }
    if (layer != 0 && config.radii[layer - 1] <= config.radii[layer]) {
      throw std::invalid_argument("radii must be strictly decreasing");
    }
  }
  if (config.top_fill_radius != 0 &&
      (config.top_fill_radius > config.radii.front() ||
       (config.radii.size() > 1 &&
        config.top_fill_radius < config.radii[1]))) {
    throw std::invalid_argument(
        "top fill radius must be between the top and second-layer radii");
  }
  if (config.max_beacons > std::numeric_limits<std::uint8_t>::max()) {
    throw std::invalid_argument("max_beacons above 255 is unsupported");
  }
  if (config.mode == BuildMode::kTopDownNested &&
      config.radii.back() <= 2 * config.containment_tolerance) {
    throw std::invalid_argument(
        "terminal radius must exceed twice containment tolerance");
  }

  distance_.reset_calls();
  double center_seconds = 0.0;
  double owner_seconds = 0.0;
  double membership_seconds = 0.0;
  double packing_seconds = 0.0;
  std::vector<std::vector<TemporaryWorld>> temporary(config.radii.size());
  std::vector<SequenceId> all_sequences(sequences_.size());
  std::iota(all_sequences.begin(), all_sequences.end(), SequenceId{0});

  if (config.mode == BuildMode::kTopDownNested) {
    const auto leaf = config.radii.size() - 1;
    std::vector<std::vector<SequenceId>> candidate_centers(
        config.radii.size());
    CenterSelection leaf_selection;
    const auto center_start = Clock::now();
    std::vector<std::future<CenterSelection>> selections;
    selections.reserve(config.radii.size());
    for (std::size_t layer = 0; layer < config.radii.size(); ++layer) {
      selections.push_back(std::async(std::launch::async, [&, layer] {
        const auto selection_radius = layer == leaf
            ? config.radii[layer] - 2 * config.containment_tolerance
            : config.radii[layer];
        return select_centers(
            sequences_, distance_, all_sequences, selection_radius,
            config.hot_cache_size, false, config.exact_global_reuse);
      }));
    }
    for (std::size_t layer = 0; layer < config.radii.size(); ++layer) {
      auto selection = selections[layer].get();
      if (layer == leaf) {
        leaf_selection = std::move(selection);
      } else {
        candidate_centers[layer] = std::move(selection.centers);
      }
    }
    center_seconds = std::chrono::duration<double>(
        Clock::now() - center_start).count();

    const auto owner_start = Clock::now();
    auto partition = assign_online_owners(
        sequences_, distance_, all_sequences, std::move(leaf_selection),
        config.radii[leaf] - 2 * config.containment_tolerance,
        config.threads);
    owner_seconds = std::chrono::duration<double>(
        Clock::now() - owner_start).count();
    temporary[leaf].reserve(partition.centers.size());
    for (std::size_t child = 0; child < partition.centers.size(); ++child) {
      TemporaryWorld world;
      world.center = partition.centers[child];
      world.member_count = partition.members[child].size();
      world.cover_radius = partition.cover_radii[child];
      world.members = std::move(partition.members[child]);
      temporary[leaf].push_back(std::move(world));
    }

    const auto membership_start = Clock::now();
    expand_complete_leaf_memberships(
        sequences_, distance_, temporary[leaf], config.radii[leaf],
        config.threads);
    membership_seconds = std::chrono::duration<double>(
        Clock::now() - membership_start).count();

    const auto repair_start = Clock::now();
    for (std::size_t child_layer = leaf; child_layer > 0; --child_layer) {
      const auto parent_layer = child_layer - 1;
      const auto fill_radius = parent_layer == 0 &&
              config.top_fill_radius != 0
          ? config.top_fill_radius
          : config.radii[parent_layer];
      temporary[child_layer - 1] = repair_parent_layer(
          sequences_, distance_, std::move(candidate_centers[child_layer - 1]),
          temporary[child_layer], fill_radius,
          config.hot_cache_size, config.exact_global_reuse);
      rebind_local_containing_parents(
          sequences_, distance_, temporary[child_layer - 1],
          temporary[child_layer], fill_radius, config.hot_cache_size,
          config.threads);
    }
    packing_seconds = std::chrono::duration<double>(
        Clock::now() - repair_start).count();
  } else if (config.mode == BuildMode::kNestedBalls) {
    const auto leaf = config.radii.size() - 1;
    const auto center_start = Clock::now();
    auto selection = select_centers(
        sequences_, distance_, all_sequences, config.radii[leaf],
        config.hot_cache_size, false, config.exact_global_reuse);
    center_seconds += std::chrono::duration<double>(
        Clock::now() - center_start).count();

    const auto owner_start = Clock::now();
    auto partition = assign_online_owners(
        sequences_, distance_, all_sequences, std::move(selection),
        config.radii[leaf], config.threads);
    owner_seconds += std::chrono::duration<double>(
        Clock::now() - owner_start).count();
    temporary[leaf].reserve(partition.centers.size());
    for (std::size_t child = 0; child < partition.centers.size(); ++child) {
      TemporaryWorld world;
      world.center = partition.centers[child];
      world.member_count = partition.members[child].size();
      world.cover_radius = partition.cover_radii[child];
      world.members = std::move(partition.members[child]);
      temporary[leaf].push_back(std::move(world));
    }

    const auto packing_start = Clock::now();
    for (std::size_t child_layer = leaf; child_layer > 0; --child_layer) {
      temporary[child_layer - 1] = pack_frozen_worlds(
          sequences_, distance_, temporary[child_layer],
          config.radii[child_layer - 1], config.hot_cache_size,
          config.exact_global_reuse);
    }
    packing_seconds = std::chrono::duration<double>(
        Clock::now() - packing_start).count();
  } else {
    for (std::size_t layer = 0; layer < config.radii.size(); ++layer) {
      const std::size_t parent_count =
          layer == 0 ? 1 : temporary[layer - 1].size();
      std::vector<CenterSelection> selections(parent_count);
      auto items_for_parent =
          [&](std::size_t parent) -> const std::vector<SequenceId>& {
        return layer == 0 ? all_sequences : temporary[layer - 1][parent].members;
      };

      const auto center_start = Clock::now();
      run_ranges(parent_count, layer == 0 ? 1 : config.threads,
                 [&](std::uint32_t, std::uint64_t begin, std::uint64_t end) {
        for (std::uint64_t parent = begin; parent < end; ++parent) {
          selections[parent] = select_centers(
              sequences_, distance_, items_for_parent(parent),
              config.radii[layer], config.hot_cache_size,
              config.delayed_centers);
        }
      });
      center_seconds += std::chrono::duration<double>(
          Clock::now() - center_start).count();

      const auto owner_start = Clock::now();
      std::vector<Partition> partitions(parent_count);
      if (parent_count == 1) {
        partitions[0] = assign_nearest_owners(
            sequences_, distance_, items_for_parent(0),
            std::move(selections[0]), config.radii[layer], config.threads);
      } else {
        run_ranges(parent_count, config.threads,
                   [&](std::uint32_t, std::uint64_t begin,
                       std::uint64_t end) {
          for (std::uint64_t parent = begin; parent < end; ++parent) {
            partitions[parent] = assign_nearest_owners(
                sequences_, distance_, items_for_parent(parent),
                std::move(selections[parent]), config.radii[layer], 1);
          }
        });
      }
      owner_seconds += std::chrono::duration<double>(
          Clock::now() - owner_start).count();

      std::size_t world_count = 0;
      for (const auto& part : partitions) world_count += part.centers.size();
      temporary[layer].reserve(world_count);
      for (std::uint32_t parent = 0; parent < partitions.size(); ++parent) {
        auto& part = partitions[parent];
        for (std::size_t child = 0; child < part.centers.size(); ++child) {
          TemporaryWorld world;
          world.center = part.centers[child];
          world.member_count = part.members[child].size();
          world.parent_local = parent;
          world.cover_radius = part.cover_radii[child];
          world.members = std::move(part.members[child]);
          temporary[layer].push_back(std::move(world));
        }
      }
      if (layer > 0) {
        for (auto& parent : temporary[layer - 1]) {
          std::vector<SequenceId>().swap(parent.members);
        }
      }
    }
  }

  if (config.mode == BuildMode::kNearestOwner) {
    for (std::size_t layer = 1; layer < temporary.size(); ++layer) {
      for (std::uint32_t child = 0; child < temporary[layer].size(); ++child) {
        const auto parent = temporary[layer][child].parent_local;
        if (parent >= temporary[layer - 1].size()) {
          throw std::logic_error("orphan nearest-owner world");
        }
        temporary[layer - 1][parent].child_worlds.push_back(child);
      }
    }
  }

  const auto topology_start = Clock::now();
  NavigaMerIndex index;
  index.window_length = sequences_.window_length();
  index.stride = sequences_.stride();
  index.max_beacons = config.max_beacons;
  index.containment_tolerance = config.containment_tolerance;
  index.routing_mode = config.mode == BuildMode::kTopDownNested
      ? RoutingMode::kCompleteNestedBalls
      : config.mode == BuildMode::kNestedBalls
      ? RoutingMode::kNestedBalls
      : RoutingMode::kNearestOwner;
  index.reference_count = sequences_.size();
  index.reference_checksum = sequences_.checksum();
  index.root = 0;
  WorldNode root;
  root.bwt_interval_length = sequences_.size();
  index.nodes.push_back(root);
  for (std::size_t layer = 0; layer < temporary.size(); ++layer) {
    LayerInfo info;
    info.first_node = index.nodes.size();
    info.node_count = temporary[layer].size();
    info.radius = config.radii[layer];
    index.layers.push_back(info);
    for (const auto& source : temporary[layer]) {
      WorldNode node;
      node.center_sequence_id = source.center;
      node.bwt_interval_length = source.member_count;
      node.cover_radius = source.cover_radius;
      node.layer = static_cast<std::uint8_t>(layer);
      index.nodes.push_back(node);
    }
  }

  auto append_child = [&](WorldNode& parent, std::uint64_t child) {
    if (parent.child_count == std::numeric_limits<std::uint32_t>::max()) {
      throw std::length_error("one world has more than 2^32 children");
    }
    index.children.push_back(child);
    ++parent.child_count;
  };
  index.nodes[index.root].first_child = index.children.size();
  for (std::uint64_t local = 0; local < temporary.front().size(); ++local) {
    append_child(index.nodes[index.root], index.layers.front().first_node + local);
  }
  index.nodes[index.root].bwt_interval_length = 0;
  for (const auto& world : temporary.front()) {
    index.nodes[index.root].bwt_interval_length += world.member_count;
  }
  for (std::size_t layer = 0; layer < temporary.size(); ++layer) {
    for (std::uint32_t local = 0; local < temporary[layer].size(); ++local) {
      auto& node = index.nodes[index.layers[layer].first_node + local];
      node.first_child = index.children.size();
      if (layer + 1 == temporary.size()) {
        for (const auto sequence_id : temporary[layer][local].members) {
          append_child(node, sequence_id);
        }
      } else {
        for (const auto child_local : temporary[layer][local].child_worlds) {
          if (child_local >= temporary[layer + 1].size()) {
            throw std::logic_error("invalid packed parent-child edge");
          }
          append_child(node,
                       index.layers[layer + 1].first_node + child_local);
        }
      }
    }
  }

  index.metric_roots.assign(index.nodes.size(), kNoNode);
  index.dense_pair_offsets.assign(index.nodes.size(), kNoNode);
  for (NodeId parent = 0; parent < index.nodes.size(); ++parent) {
    build_persistent_metric_tree(index, parent, sequences_, distance_);
    build_dense_pair_matrix(index, parent, sequences_, distance_, config.threads);
  }
  const auto topology_end = Clock::now();

  const auto mbb_start = Clock::now();
  for (auto& node : index.nodes) {
    node.first_beacon = index.beacons.size();
    if (node.child_count == 0) continue;
    const auto wanted = std::min(config.max_beacons, node.child_count);
    std::unordered_set<SequenceId> seen;
    auto child_sequence_at = [&](std::uint64_t ordinal) {
      const auto child = index.children[node.first_child + ordinal];
      return index.is_terminal(node)
          ? static_cast<SequenceId>(child)
          : index.nodes[child].center_sequence_id;
    };
    // A world's own center is the cheapest and usually the most proximal
    // local beacon: routing has normally calculated d(Q, center) already.
    // Put it first so the per-query exact-distance cache turns this constraint
    // into an O(1) lookup before evaluating more distant beacons.
    if (node.center_sequence_id != kNoSequence) {
      for (std::uint32_t ordinal = 0; ordinal < node.child_count; ++ordinal) {
        if (child_sequence_at(ordinal) == node.center_sequence_id) {
          seen.insert(node.center_sequence_id);
          index.beacons.push_back(node.center_sequence_id);
          break;
        }
      }
    }
    for (std::uint32_t sample = 0;
         index.beacons.size() - node.first_beacon < wanted && sample < wanted;
         ++sample) {
      const std::uint64_t ordinal = wanted == 1
          ? 0
          : static_cast<std::uint64_t>(sample) * (node.child_count - 1) /
                (wanted - 1);
      const auto beacon = child_sequence_at(ordinal);
      if (seen.insert(beacon).second) index.beacons.push_back(beacon);
    }
    // Duplicate centers can make a stratified slot collide. Fill any remaining
    // beacon slots in child order; this is construction-only and exact.
    for (std::uint32_t ordinal = 0;
         index.beacons.size() - node.first_beacon < wanted &&
             ordinal < node.child_count;
         ++ordinal) {
      const auto beacon = child_sequence_at(ordinal);
      if (seen.insert(beacon).second) index.beacons.push_back(beacon);
    }
    node.beacon_count = static_cast<std::uint8_t>(
        index.beacons.size() - node.first_beacon);
  }

  index.child_beacon_distances.assign(
      index.children.size() * index.max_beacons, kMissingDistance);
  run_ranges(index.nodes.size(), config.threads,
             [&](std::uint32_t, std::uint64_t begin, std::uint64_t end) {
    for (NodeId node_id = begin; node_id < end; ++node_id) {
      const auto& node = index.nodes[node_id];
      for (std::uint32_t child_ordinal = 0; child_ordinal < node.child_count;
           ++child_ordinal) {
        const auto child_slot = node.first_child + child_ordinal;
        const auto child = index.children[child_slot];
        const SequenceId child_sequence = index.is_terminal(node)
            ? child
            : index.nodes[child].center_sequence_id;
        auto prepared = distance_.prepare(sequences_.sequence(child_sequence));
        for (std::uint32_t beacon_ordinal = 0;
             beacon_ordinal < node.beacon_count; ++beacon_ordinal) {
          const auto beacon = index.beacons[node.first_beacon + beacon_ordinal];
          const int d = prepared(sequences_.sequence(beacon));
          if (d < 0 || d >= kMissingDistance) {
            throw std::length_error("beacon distance exceeds uint16 capacity");
          }
          index.child_beacon_distances[
              child_slot * index.max_beacons + beacon_ordinal] =
              static_cast<std::uint16_t>(d);
        }
      }
    }
  });
  const auto mbb_end = Clock::now();

  index.validate(&sequences_);
  if (stats) {
    stats->worlds_per_layer.clear();
    for (const auto& layer : index.layers) {
      stats->worlds_per_layer.push_back(layer.node_count);
    }
    stats->world_edges = 0;
    for (const auto& node : index.nodes) {
      if (!index.is_terminal(node)) stats->world_edges += node.child_count;
    }
    stats->terminal_memberships = 0;
    for (const auto& leaf : temporary.back()) {
      stats->terminal_memberships += leaf.members.size();
    }
    stats->unique_terminal_memberships = sequences_.size();
    stats->edit_distance_calls = distance_.calls();
    stats->center_seconds = center_seconds;
    stats->owner_seconds = owner_seconds;
    stats->membership_seconds = membership_seconds;
    stats->packing_seconds = packing_seconds;
    stats->topology_seconds = std::chrono::duration<double>(
        topology_end - topology_start).count();
    stats->mbb_seconds = std::chrono::duration<double>(
        mbb_end - mbb_start).count();
  }
  return index;
}

}  // namespace navigamer
