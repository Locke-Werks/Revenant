// The tiled pane layout: a tree of splits whose leaves are panes, which the
// operator resizes by dragging the borders between them and rearranges by
// dragging a pane onto an edge of another.
//
// THE OWNER'S CALL, 2026-11-09: the desktop layout becomes a workspace in the
// manner of Blender or Photoshop, resizable and rearrangeable, under one hard
// rule: nothing is ever cut off or covered. A pane only shrinks, down to a
// minimum it can still be used at. So this is tiling and nothing else: no
// floating pane over another, no tab stack hiding one behind another, and the
// window itself refuses to get smaller than the sum of the minimums, which
// min_size() computes and the window binds to.
//
// TWO TREES, NOT ONE. The main window's tree holds the span and the receivers;
// the receivers' own tree holds the rack, the focused receiver and its
// sections. The receivers pop out into a window of their own as a unit, so
// they are a unit here too, and that tree is drawn in whichever window has
// them.
//
// WEIGHTS, NOT PIXELS. A split keeps each child's share as a weight, so a
// window resized keeps the proportions the operator chose. lay_out() turns
// weights into pixels and is the one place the minimums are enforced: a
// child whose share would put it under its minimum gets its minimum, and the
// excess is taken back from the children that have room.
//
// This header holds no Qt; ui/tests links it.

#pragma once

#include <algorithm>
#include <cstddef>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace revenant::ui {

// The width of the border between two panes, which is what the operator
// grabs. Counted in every minimum so the window's floor includes them.
inline constexpr double kPaneHandle = 5.0;

struct PaneSize {
    double width = 0.0;
    double height = 0.0;
};

struct PaneNode {
    // A leaf names a pane and has no children.
    std::string pane;

    // A split lays its children side by side when horizontal, stacked top to
    // bottom when not.
    bool horizontal = false;
    std::vector<PaneNode> children;
    std::vector<double> weights;

    // In a view with panes hidden (build_view), where this node is in the
    // stored tree and, for a split, which of the stored node's children each
    // of its own children is. Empty in a stored tree.
    std::vector<std::size_t> source;
    std::vector<std::size_t> kept;

    [[nodiscard]] bool leaf() const { return children.empty(); }
};

using PaneMinimums = std::map<std::string, PaneSize, std::less<>>;

[[nodiscard]] inline PaneNode pane_leaf(std::string pane)
{
    PaneNode node;
    node.pane = std::move(pane);
    return node;
}

[[nodiscard]] inline PaneNode pane_split(bool horizontal, std::vector<PaneNode> children,
                                         std::vector<double> weights)
{
    PaneNode node;
    node.horizontal = horizontal;
    node.children = std::move(children);
    node.weights = std::move(weights);
    return node;
}

// The smallest a node can be: along a split's axis the children's minimums
// and the handles between them add up, across it the largest child decides.
[[nodiscard]] inline PaneSize min_size(const PaneNode& node, const PaneMinimums& minimums)
{
    if (node.leaf()) {
        const auto found = minimums.find(node.pane);
        return found == minimums.end() ? PaneSize{} : found->second;
    }
    PaneSize total;
    for (const PaneNode& child : node.children) {
        const PaneSize size = min_size(child, minimums);
        if (node.horizontal) {
            total.width += size.width;
            total.height = std::max(total.height, size.height);
        } else {
            total.height += size.height;
            total.width = std::max(total.width, size.width);
        }
    }
    const double handles = kPaneHandle * static_cast<double>(node.children.size() - 1);
    (node.horizontal ? total.width : total.height) += handles;
    return total;
}

// Each child's length along the split's axis, handles excluded. length is
// the split's whole length, handles included. Every child gets at least its
// minimum; when the length is short of the minimums altogether, which the
// window's own minimum is there to prevent, they get their minimums anyway
// and the split overflows rather than one of them being cut.
//
// A fixed child, a collapsed pane, gets exactly its minimum and no share.
// When every child is fixed the last one takes what is left over, so the
// split never leaves a hole.
[[nodiscard]] inline std::vector<double> lay_out(const std::vector<double>& weights,
                                                 const std::vector<double>& minimums,
                                                 double length,
                                                 const std::vector<bool>& fixed = {})
{
    const std::size_t count = weights.size();
    std::vector<double> sizes(count, 0.0);
    if (count == 0) {
        return sizes;
    }
    const auto is_fixed = [&fixed](std::size_t i) { return i < fixed.size() && fixed[i]; };
    const double room =
        std::max(0.0, length - kPaneHandle * static_cast<double>(count - 1));
    double total_weight = std::accumulate(weights.begin(), weights.end(), 0.0);
    if (total_weight <= 0.0) {
        total_weight = static_cast<double>(count);
    }
    const auto weight_of = [&](std::size_t i) {
        return weights[i] > 0.0 ? weights[i] : total_weight / static_cast<double>(count);
    };

    // Shares first, then every child under its minimum is pinned at it and
    // the rest share what is left, until nobody new is pinned.
    std::vector<bool> pinned(count, false);
    for (std::size_t i = 0; i < count; ++i) {
        pinned[i] = is_fixed(i);
    }
    for (std::size_t round = 0; round <= count; ++round) {
        double free_room = room;
        double free_weight = 0.0;
        for (std::size_t i = 0; i < count; ++i) {
            if (pinned[i]) {
                free_room -= minimums[i];
            } else {
                free_weight += weight_of(i);
            }
        }
        bool pinned_more = false;
        for (std::size_t i = 0; i < count; ++i) {
            if (pinned[i]) {
                sizes[i] = minimums[i];
                continue;
            }
            sizes[i] = free_weight > 0.0 ? std::max(0.0, free_room) * weight_of(i) / free_weight : 0.0;
            if (sizes[i] < minimums[i]) {
                pinned[i] = true;
                pinned_more = true;
            }
        }
        if (!pinned_more) {
            break;
        }
    }
    for (std::size_t i = 0; i < count; ++i) {
        sizes[i] = std::max(sizes[i], minimums[i]);
    }
    bool all_fixed = true;
    for (std::size_t i = 0; i < count; ++i) {
        all_fixed = all_fixed && is_fixed(i);
    }
    if (all_fixed) {
        const double used = std::accumulate(sizes.begin(), sizes.end(), 0.0);
        sizes.back() += std::max(0.0, room - used);
    }
    return sizes;
}

// Whether a node is all collapsed panes, which keeps its minimum along a
// split rather than taking a share.
[[nodiscard]] inline bool all_collapsed(const PaneNode& node,
                                        const std::set<std::string, std::less<>>& collapsed)
{
    if (node.leaf()) {
        return collapsed.contains(node.pane);
    }
    for (const PaneNode& child : node.children) {
        if (!all_collapsed(child, collapsed)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline std::vector<bool> fixed_children(
    const PaneNode& split, const std::set<std::string, std::less<>>& collapsed)
{
    std::vector<bool> out;
    out.reserve(split.children.size());
    for (const PaneNode& child : split.children) {
        out.push_back(all_collapsed(child, collapsed));
    }
    return out;
}

// The children's minimums along a split's axis.
[[nodiscard]] inline std::vector<double> axis_minimums(const PaneNode& split,
                                                       const PaneMinimums& minimums)
{
    std::vector<double> out;
    out.reserve(split.children.size());
    for (const PaneNode& child : split.children) {
        const PaneSize size = min_size(child, minimums);
        out.push_back(split.horizontal ? size.width : size.height);
    }
    return out;
}

// Moves the border after child index by delta pixels, the two children on
// either side of it trading length, neither going under its minimum. Returns
// the new sizes; the caller keeps them as weights.
//
// A fixed child does not trade: the border moves the nearest unfixed child on
// each side instead, so dragging next to a collapsed pane still works.
[[nodiscard]] inline std::vector<double> drag_border(std::vector<double> sizes,
                                                     const std::vector<double>& minimums,
                                                     std::size_t index, double delta,
                                                     const std::vector<bool>& fixed = {})
{
    if (index + 1 >= sizes.size()) {
        return sizes;
    }
    const auto is_fixed = [&fixed](std::size_t i) { return i < fixed.size() && fixed[i]; };
    std::size_t before = index;
    while (is_fixed(before) && before > 0) {
        --before;
    }
    std::size_t after = index + 1;
    while (is_fixed(after) && after + 1 < sizes.size()) {
        ++after;
    }
    if (is_fixed(before) || is_fixed(after)) {
        return sizes;
    }
    if (before != index || after != index + 1) {
        const double most_growth = std::max(0.0, sizes[after] - minimums[after]);
        const double most_shrink = std::max(0.0, sizes[before] - minimums[before]);
        const double moved = std::clamp(delta, -most_shrink, most_growth);
        sizes[before] += moved;
        sizes[after] -= moved;
        return sizes;
    }
    const double most_growth = std::max(0.0, sizes[index + 1] - minimums[index + 1]);
    const double most_shrink = std::max(0.0, sizes[index] - minimums[index]);
    const double moved = std::clamp(delta, -most_shrink, most_growth);
    sizes[index] += moved;
    sizes[index + 1] -= moved;
    return sizes;
}

// The node at a path of child indices, or null when the path leads nowhere.
[[nodiscard]] inline PaneNode* node_at(PaneNode& root, const std::vector<std::size_t>& path)
{
    PaneNode* node = &root;
    for (const std::size_t index : path) {
        if (index >= node->children.size()) {
            return nullptr;
        }
        node = &node->children[index];
    }
    return node;
}

[[nodiscard]] inline const PaneNode* node_at(const PaneNode& root,
                                             const std::vector<std::size_t>& path)
{
    return node_at(const_cast<PaneNode&>(root), path);
}

inline void collect_panes(const PaneNode& node, std::vector<std::string>& out)
{
    if (node.leaf()) {
        out.push_back(node.pane);
        return;
    }
    for (const PaneNode& child : node.children) {
        collect_panes(child, out);
    }
}

// A split left with one child is replaced by that child, and a child split
// running the same way as its parent is folded into it, so the tree never
// holds a split that draws nothing of its own.
inline void normalise(PaneNode& node)
{
    if (node.leaf()) {
        return;
    }
    for (PaneNode& child : node.children) {
        normalise(child);
    }
    std::vector<PaneNode> children;
    std::vector<double> weights;
    for (std::size_t i = 0; i < node.children.size(); ++i) {
        PaneNode& child = node.children[i];
        const double weight = i < node.weights.size() ? node.weights[i] : 1.0;
        if (!child.leaf() && child.horizontal == node.horizontal) {
            const double inner = std::accumulate(child.weights.begin(), child.weights.end(), 0.0);
            for (std::size_t j = 0; j < child.children.size(); ++j) {
                children.push_back(std::move(child.children[j]));
                weights.push_back(inner > 0.0 ? weight * child.weights[j] / inner : weight);
            }
        } else {
            children.push_back(std::move(child));
            weights.push_back(weight);
        }
    }
    node.children = std::move(children);
    node.weights = std::move(weights);
    if (node.children.size() == 1) {
        PaneNode only = std::move(node.children.front());
        node = std::move(only);
    }
}

namespace pane_detail {

// Takes the leaf naming pane out of the tree, giving its share to its
// neighbours. False when it is not there or is the whole tree.
inline bool remove_leaf(PaneNode& node, std::string_view pane)
{
    for (std::size_t i = 0; i < node.children.size(); ++i) {
        PaneNode& child = node.children[i];
        if (child.leaf() && child.pane == pane) {
            node.children.erase(node.children.begin() + static_cast<std::ptrdiff_t>(i));
            if (i < node.weights.size()) {
                node.weights.erase(node.weights.begin() + static_cast<std::ptrdiff_t>(i));
            }
            return true;
        }
        if (!child.leaf() && remove_leaf(child, pane)) {
            return true;
        }
    }
    return false;
}

inline PaneNode* find_leaf(PaneNode& node, std::string_view pane)
{
    if (node.leaf()) {
        return node.pane == pane ? &node : nullptr;
    }
    for (PaneNode& child : node.children) {
        if (PaneNode* found = find_leaf(child, pane)) {
            return found;
        }
    }
    return nullptr;
}

}  // namespace pane_detail

enum class PaneEdge { Left, Right, Top, Bottom };

[[nodiscard]] inline std::optional<PaneEdge> pane_edge(std::string_view name)
{
    if (name == "left") {
        return PaneEdge::Left;
    }
    if (name == "right") {
        return PaneEdge::Right;
    }
    if (name == "top") {
        return PaneEdge::Top;
    }
    if (name == "bottom") {
        return PaneEdge::Bottom;
    }
    return std::nullopt;
}

// Puts pane against the given edge of target, the two splitting what target
// had between them. Returns false and leaves the tree alone when either is
// missing or they are the same pane.
inline bool move_pane(PaneNode& root, std::string_view pane, std::string_view target,
                      PaneEdge edge)
{
    if (pane == target || root.leaf()) {
        return false;
    }
    if (pane_detail::find_leaf(root, pane) == nullptr ||
        pane_detail::find_leaf(root, target) == nullptr) {
        return false;
    }
    PaneNode moved = pane_leaf(std::string(pane));
    if (!pane_detail::remove_leaf(root, pane)) {
        return false;
    }
    normalise(root);
    PaneNode* spot = pane_detail::find_leaf(root, target);
    if (spot == nullptr) {
        return false;
    }
    const bool horizontal = edge == PaneEdge::Left || edge == PaneEdge::Right;
    const bool first = edge == PaneEdge::Left || edge == PaneEdge::Top;
    PaneNode kept = std::move(*spot);
    std::vector<PaneNode> pair;
    if (first) {
        pair.push_back(std::move(moved));
        pair.push_back(std::move(kept));
    } else {
        pair.push_back(std::move(kept));
        pair.push_back(std::move(moved));
    }
    *spot = pane_split(horizontal, std::move(pair), {1.0, 1.0});
    normalise(root);
    return true;
}

// Whether the tree holds exactly the expected panes, each once. A layout read
// back from the settings that does not is from another version, and the
// default is used instead.
[[nodiscard]] inline bool holds_exactly(const PaneNode& root,
                                        const std::vector<std::string>& expected)
{
    std::vector<std::string> panes;
    collect_panes(root, panes);
    std::multiset<std::string> have(panes.begin(), panes.end());
    std::multiset<std::string> want(expected.begin(), expected.end());
    return have == want;
}

// ---------------------------------------------------------------------------
// The settings form: a split is "h(" or "v(", then weight and child pairs
// separated by spaces, then ")". A leaf is its pane's name. So
// "v(0.58 span 0.42 receivers)" is the span over the receivers.
// ---------------------------------------------------------------------------

inline void write_pane_tree(const PaneNode& node, std::string& out)
{
    if (node.leaf()) {
        out += node.pane;
        return;
    }
    out += node.horizontal ? "h(" : "v(";
    for (std::size_t i = 0; i < node.children.size(); ++i) {
        if (i > 0) {
            out += ' ';
        }
        const double weight = i < node.weights.size() ? node.weights[i] : 1.0;
        out += std::to_string(weight);
        out += ' ';
        write_pane_tree(node.children[i], out);
    }
    out += ')';
}

[[nodiscard]] inline std::string write_pane_tree(const PaneNode& node)
{
    std::string out;
    write_pane_tree(node, out);
    return out;
}

namespace pane_detail {

inline void skip_space(std::string_view text, std::size_t& at)
{
    while (at < text.size() && text[at] == ' ') {
        ++at;
    }
}

inline std::optional<PaneNode> read_node(std::string_view text, std::size_t& at, int depth)
{
    if (depth > 16) {
        return std::nullopt;
    }
    skip_space(text, at);
    if (at + 1 < text.size() && (text[at] == 'h' || text[at] == 'v') && text[at + 1] == '(') {
        PaneNode node;
        node.horizontal = text[at] == 'h';
        at += 2;
        for (;;) {
            skip_space(text, at);
            if (at < text.size() && text[at] == ')') {
                ++at;
                break;
            }
            const std::size_t start = at;
            while (at < text.size() && text[at] != ' ' && text[at] != ')') {
                ++at;
            }
            double weight = 0.0;
            try {
                weight = std::stod(std::string(text.substr(start, at - start)));
            } catch (...) {
                return std::nullopt;
            }
            if (!(weight > 0.0) || weight > 1e9) {
                return std::nullopt;
            }
            std::optional<PaneNode> child = read_node(text, at, depth + 1);
            if (!child) {
                return std::nullopt;
            }
            node.children.push_back(std::move(*child));
            node.weights.push_back(weight);
        }
        if (node.children.size() < 2) {
            return std::nullopt;
        }
        return node;
    }
    const std::size_t start = at;
    while (at < text.size()) {
        const char c = text[at];
        const bool name_char = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                               (c >= '0' && c <= '9') || c == '.' || c == '_';
        if (!name_char) {
            break;
        }
        ++at;
    }
    if (at == start) {
        return std::nullopt;
    }
    return pane_leaf(std::string(text.substr(start, at - start)));
}

}  // namespace pane_detail

[[nodiscard]] inline std::optional<PaneNode> read_pane_tree(std::string_view text)
{
    std::size_t at = 0;
    std::optional<PaneNode> node = pane_detail::read_node(text, at, 0);
    pane_detail::skip_space(text, at);
    if (!node || at != text.size()) {
        return std::nullopt;
    }
    return node;
}

// ---------------------------------------------------------------------------
// The tree as drawn, with some panes left out: a receiver pane while the
// receivers are popped out or hidden, a section with nothing in it. A split
// left with one child is replaced by it, one left with none disappears, and
// every node remembers where it came from so a drag on the view lands on the
// stored tree.
// ---------------------------------------------------------------------------

namespace pane_detail {

inline std::optional<PaneNode> view_of(const PaneNode& node, std::vector<std::size_t>& path,
                                       const std::set<std::string, std::less<>>& hidden)
{
    if (node.leaf()) {
        if (hidden.contains(node.pane)) {
            return std::nullopt;
        }
        PaneNode out = pane_leaf(node.pane);
        out.source = path;
        return out;
    }
    PaneNode out;
    out.horizontal = node.horizontal;
    out.source = path;
    for (std::size_t i = 0; i < node.children.size(); ++i) {
        path.push_back(i);
        std::optional<PaneNode> child = view_of(node.children[i], path, hidden);
        path.pop_back();
        if (child) {
            out.children.push_back(std::move(*child));
            out.weights.push_back(i < node.weights.size() ? node.weights[i] : 1.0);
            out.kept.push_back(i);
        }
    }
    if (out.children.empty()) {
        return std::nullopt;
    }
    if (out.children.size() == 1) {
        return std::move(out.children.front());
    }
    return out;
}

}  // namespace pane_detail

// Null when every pane is hidden.
[[nodiscard]] inline std::optional<PaneNode> build_view(
    const PaneNode& stored, const std::set<std::string, std::less<>>& hidden)
{
    std::vector<std::size_t> path;
    return pane_detail::view_of(stored, path, hidden);
}

// Writes sizes a view split was dragged to back into the stored split it came
// from, as weights. The hidden children keep their weights, and the visible
// ones share the total they had between them, so a pane coming back finds its
// share where it left it.
inline void store_view_sizes(PaneNode& stored_split, const PaneNode& view_split,
                             const std::vector<double>& sizes)
{
    double had = 0.0;
    for (const std::size_t slot : view_split.kept) {
        if (slot < stored_split.weights.size()) {
            had += stored_split.weights[slot];
        }
    }
    const double total = std::accumulate(sizes.begin(), sizes.end(), 0.0);
    if (total <= 0.0 || had <= 0.0) {
        return;
    }
    for (std::size_t i = 0; i < view_split.kept.size() && i < sizes.size(); ++i) {
        const std::size_t slot = view_split.kept[i];
        if (slot < stored_split.weights.size()) {
            stored_split.weights[slot] = had * sizes[i] / total;
        }
    }
}

// ---------------------------------------------------------------------------
// The panes, and how the two trees start.
// ---------------------------------------------------------------------------

inline constexpr std::string_view kPaneSpectrum = "spectrum";
inline constexpr std::string_view kPaneWaterfall = "waterfall";
inline constexpr std::string_view kPaneRack = "rack";
inline constexpr std::string_view kPaneDetail = "detail";
inline constexpr std::string_view kPaneRds = "rds";
inline constexpr std::string_view kPaneDecode = "decode";
inline constexpr std::string_view kPaneImage = "image";
inline constexpr std::string_view kPaneCalls = "calls";
inline constexpr std::string_view kPaneTrunk = "trunk";
inline constexpr std::string_view kPaneAudio = "audio";

// The title strip over every pane, and the bar a collapsed pane becomes when
// it is beside others rather than above or below them.
inline constexpr double kPaneStrip = 20.0;
inline constexpr double kPaneCollapsedBar = 22.0;

// How far a pane's own scale goes either way.
inline constexpr double kPaneScaleLow = 0.5;
inline constexpr double kPaneScaleHigh = 2.0;

[[nodiscard]] inline std::vector<std::string> receiver_tree_panes()
{
    return {std::string(kPaneRack),  std::string(kPaneDetail), std::string(kPaneRds),
            std::string(kPaneDecode), std::string(kPaneImage), std::string(kPaneCalls),
            std::string(kPaneTrunk), std::string(kPaneAudio)};
}

[[nodiscard]] inline std::vector<std::string> main_tree_panes()
{
    std::vector<std::string> out = {std::string(kPaneSpectrum), std::string(kPaneWaterfall)};
    for (std::string& pane : receiver_tree_panes()) {
        out.push_back(std::move(pane));
    }
    return out;
}

// The rack, the receiver, and RDS, decoding, images, calls, trunking and
// audio in a column of their own, which is the docked strip's old wide
// arrangement.
[[nodiscard]] inline PaneNode default_receiver_tree()
{
    PaneNode sections = pane_split(
        false,
        {pane_leaf(std::string(kPaneRds)), pane_leaf(std::string(kPaneDecode)),
         pane_leaf(std::string(kPaneImage)), pane_leaf(std::string(kPaneCalls)),
         pane_leaf(std::string(kPaneTrunk)), pane_leaf(std::string(kPaneAudio))},
        {0.10, 0.35, 0.15, 0.20, 0.15, 0.05});
    return pane_split(true,
                      {pane_leaf(std::string(kPaneRack)), pane_leaf(std::string(kPaneDetail)),
                       std::move(sections)},
                      {0.24, 0.46, 0.30});
}

// Today's look: the spectrum over the waterfall at a third of the span, over
// the receivers at the share the dock always started at.
[[nodiscard]] inline PaneNode default_main_tree()
{
    return pane_split(false,
                      {pane_leaf(std::string(kPaneSpectrum)), pane_leaf(std::string(kPaneWaterfall)),
                       default_receiver_tree()},
                      {0.20, 0.38, 0.42});
}

// The floors each pane keeps whatever its content says, in logical pixels at
// a scale of one, title strip excluded. The QML raises them to what the
// content needs (PaneLayoutStore.setMinimum) and never lowers them.
[[nodiscard]] inline PaneMinimums default_pane_minimums()
{
    PaneMinimums out = {
        {std::string(kPaneSpectrum), {240.0, 80.0}},
        {std::string(kPaneWaterfall), {240.0, 80.0}},
        {std::string(kPaneRack), {120.0, 40.0}},
        {std::string(kPaneDetail), {280.0, 120.0}},
    };
    for (const std::string_view pane : {kPaneRds, kPaneDecode, kPaneImage, kPaneCalls,
                                        kPaneTrunk, kPaneAudio}) {
        out.emplace(std::string(pane), PaneSize{200.0, 30.0});
    }
    return out;
}

}  // namespace revenant::ui
