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
[[nodiscard]] inline std::vector<double> lay_out(const std::vector<double>& weights,
                                                 const std::vector<double>& minimums,
                                                 double length)
{
    const std::size_t count = weights.size();
    std::vector<double> sizes(count, 0.0);
    if (count == 0) {
        return sizes;
    }
    const double room =
        std::max(0.0, length - kPaneHandle * static_cast<double>(count - 1));
    double total_weight = std::accumulate(weights.begin(), weights.end(), 0.0);
    if (total_weight <= 0.0) {
        total_weight = static_cast<double>(count);
    }

    // Shares first, then every child under its minimum is pinned at it and
    // the rest share what is left, until nobody new is pinned.
    std::vector<bool> pinned(count, false);
    for (std::size_t round = 0; round <= count; ++round) {
        double free_room = room;
        double free_weight = 0.0;
        for (std::size_t i = 0; i < count; ++i) {
            if (pinned[i]) {
                free_room -= minimums[i];
            } else {
                free_weight += weights[i] > 0.0 ? weights[i] : total_weight / static_cast<double>(count);
            }
        }
        bool pinned_more = false;
        for (std::size_t i = 0; i < count; ++i) {
            if (pinned[i]) {
                sizes[i] = minimums[i];
                continue;
            }
            const double weight =
                weights[i] > 0.0 ? weights[i] : total_weight / static_cast<double>(count);
            sizes[i] = free_weight > 0.0 ? std::max(0.0, free_room) * weight / free_weight : 0.0;
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
    return sizes;
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
[[nodiscard]] inline std::vector<double> drag_border(std::vector<double> sizes,
                                                     const std::vector<double>& minimums,
                                                     std::size_t index, double delta)
{
    if (index + 1 >= sizes.size()) {
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
// The two trees, their panes, and how they start.
// ---------------------------------------------------------------------------

inline constexpr std::string_view kPaneSpan = "span";
inline constexpr std::string_view kPaneReceivers = "receivers";
inline constexpr std::string_view kPaneRack = "rack";
inline constexpr std::string_view kPaneDetail = "detail";
inline constexpr std::string_view kPaneSections = "sections";
inline constexpr std::string_view kPaneSpectrum = "spectrum";
inline constexpr std::string_view kPaneWaterfall = "waterfall";

// Today's look: the span over the receivers, the dock at the share it always
// started at.
[[nodiscard]] inline PaneNode default_main_tree()
{
    return pane_split(false, {pane_leaf(std::string(kPaneSpan)), pane_leaf(std::string(kPaneReceivers))},
                      {0.58, 0.42});
}

// The rack, the receiver, and RDS, decoding and audio in a column of their
// own, which is the docked strip's wide arrangement.
[[nodiscard]] inline PaneNode default_receiver_tree()
{
    return pane_split(true,
                      {pane_leaf(std::string(kPaneRack)), pane_leaf(std::string(kPaneDetail)),
                       pane_leaf(std::string(kPaneSections))},
                      {0.24, 0.46, 0.30});
}

// Inside the span, the spectrum over the waterfall at the third it always
// had. Resized, never rearranged: the ruler between them belongs to both.
[[nodiscard]] inline PaneNode default_span_tree()
{
    return pane_split(false,
                      {pane_leaf(std::string(kPaneSpectrum)), pane_leaf(std::string(kPaneWaterfall))},
                      {0.34, 0.66});
}

[[nodiscard]] inline std::vector<std::string> span_tree_panes()
{
    return {std::string(kPaneSpectrum), std::string(kPaneWaterfall)};
}

[[nodiscard]] inline std::vector<std::string> main_tree_panes()
{
    return {std::string(kPaneSpan), std::string(kPaneReceivers)};
}

[[nodiscard]] inline std::vector<std::string> receiver_tree_panes()
{
    return {std::string(kPaneRack), std::string(kPaneDetail), std::string(kPaneSections)};
}

// The floors each pane keeps whatever its content says, in logical pixels.
// The QML raises them to what the content needs (PaneLayout.setMinimum), and
// never lowers them.
[[nodiscard]] inline PaneMinimums default_pane_minimums()
{
    return {
        {std::string(kPaneSpan), {320.0, 200.0}},
        {std::string(kPaneRack), {240.0, 150.0}},
        {std::string(kPaneDetail), {320.0, 240.0}},
        {std::string(kPaneSections), {260.0, 120.0}},
        {std::string(kPaneSpectrum), {0.0, 80.0}},
        {std::string(kPaneWaterfall), {0.0, 80.0}},
    };
}

}  // namespace revenant::ui
