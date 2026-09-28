#include "navigamer/builder.hpp"
#include "navigamer/query.hpp"

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <numeric>
#include <tuple>

namespace {

int distance(std::string_view a, std::string_view b) {
  std::vector<int> row(b.size() + 1);
  std::iota(row.begin(), row.end(), 0);
  for (std::size_t i = 1; i <= a.size(); ++i) {
    int diagonal = row[0];
    row[0] = i;
    for (std::size_t j = 1; j <= b.size(); ++j) {
      const int previous = row[j];
      row[j] = std::min({previous + 1, row[j - 1] + 1,
                         diagonal + (a[i - 1] != b[j - 1])});
      diagonal = previous;
    }
  }
  return row.back();
}

std::string contents(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void check_cover(const navigamer::NavigaMerIndex& index,
                 const std::vector<std::uint32_t>& radii) {
  for (const auto& parent : index.nodes) {
    if (parent.child_count == 0) continue;
    const auto radius = radii[parent.layer == navigamer::kRootLayer ? 0 : parent.layer + 1];
    assert(parent.beacon_count > 0 && parent.beacon_count <= parent.child_count);
    assert(index.beacon_ordinals[parent.first_beacon] == 0);
    for (std::size_t c = 0; c < parent.child_count; ++c) {
      const auto center = index.center_sequences.decode(index.children[parent.first_child + c]);
      bool covered = false;
      for (std::size_t b = 0; b < parent.beacon_count; ++b) {
        const auto ordinal = index.beacon_ordinals[parent.first_beacon + b];
        assert(ordinal < parent.child_count);
        const auto anchor = index.center_sequences.decode(index.children[parent.first_child + ordinal]);
        const auto d = distance(center, anchor);
        covered |= static_cast<std::uint32_t>(d) <= radius;
        assert(index.beacon_distances[parent.first_distance + c * parent.beacon_count + b] == d);
        for (std::size_t previous = 0; previous < b; ++previous) {
          const auto other = index.beacon_ordinals[parent.first_beacon + previous];
          assert(other < ordinal);
          assert(static_cast<std::uint32_t>(distance(anchor, index.center_sequences.decode(
              index.children[parent.first_child + other]))) > radius);
        }
      }
      assert(covered);
    }
  }
}

}  // namespace

void run_checks(const std::filesystem::path& directory, std::uint32_t tolerance) {
  std::filesystem::create_directories(directory);
  std::vector<std::string> sequences{""};
  for (int i = 0; i < 4; ++i) {
    std::vector<std::string> next;
    for (const auto& prefix : sequences) for (const char base : std::string("ACGT")) {
      next.push_back(prefix + base);
    }
    sequences = std::move(next);
  }
  sequences.insert(sequences.end(), {"NNNN", "ANNN", "AAAA"});
  const auto reference = navigamer::SequenceStore::from_sequences(sequences);
  navigamer::BuildConfig config;
  config.center_radii = {3 + 2 * tolerance, 2 + 2 * tolerance, 1 + 2 * tolerance};
  config.max_tolerance = tolerance;
  const auto baseline = navigamer::IndexBuilder::build(reference, config);
  for (const auto radii : {std::vector<std::uint32_t>{0, 0, 0}, {4, 3, 2},
                          {std::numeric_limits<std::uint32_t>::max(), 4, 4}}) {
    config.beacon_cover_radii = radii;
    config.threads = 1;
    navigamer::BuildStats stats;
    const auto index = navigamer::IndexBuilder::build(reference, config, &stats);
    check_cover(index, radii);
    assert(index.children == baseline.children);
    assert(index.leaf_ids == baseline.leaf_ids);
    assert(index.legacy_leaf_sequences.empty() && baseline.legacy_leaf_sequences.empty());
    assert(index.nodes.size() == baseline.nodes.size());
    for (std::size_t i = 0; i < index.nodes.size(); ++i) {
      assert(index.nodes[i].center_sequence_id == baseline.nodes[i].center_sequence_id);
      assert(index.nodes[i].member_count == baseline.nodes[i].member_count);
    }
    std::uint64_t beacons = 0;
    for (const auto& layer : stats.layers) beacons += layer.selected_beacons;
    assert(beacons == index.beacon_ordinals.size());
    index.save(directory / "single.nvm");
    config.threads = 4;
    const auto parallel = navigamer::IndexBuilder::build(reference, config);
    parallel.save(directory / "parallel.nvm");
    assert(contents(directory / "single.nvm") == contents(directory / "parallel.nvm"));
    const auto restored = navigamer::NavigaMerIndex::load(directory / "single.nvm");
    check_cover(restored, radii);
    navigamer::QueryEngine engine(restored, reference);
    auto queries = sequences;
    queries.insert(queries.end(), {"", "ACG", "ACGTA", "RYNN"});
    for (const auto& q : queries) {
      for (const auto mode : {navigamer::RouteMode::kScan, navigamer::RouteMode::kMultilateration}) {
        const auto hits = engine.query(q, {tolerance, mode, true, true});
        std::vector<std::tuple<navigamer::SequenceId, bool, unsigned>> expected, actual;
        for (std::size_t id = 0; id < sequences.size(); ++id) {
          const auto forward = distance(q, sequences[id]);
          const auto reverse = distance(navigamer::reverse_complement(q), sequences[id]);
          if (forward <= static_cast<int>(tolerance)) expected.emplace_back(id, false, forward);
          if (reverse <= static_cast<int>(tolerance)) expected.emplace_back(id, true, reverse);
        }
        for (const auto& hit : hits) actual.emplace_back(hit.occurrence_id, hit.reverse, hit.distance);
        assert(actual == expected);
      }
    }
  }
}

int main(int argc, char** argv) {
  (void)argc;
  run_checks(std::filesystem::path(argv[1]) / "exact", 0);
  run_checks(std::filesystem::path(argv[1]) / "one-edit", 1);
}
