/*
 * Copyright (c) 2024, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

#ifndef SHARE_GC_Z_ZINTRUSIVERBTREE_HPP
#define SHARE_GC_Z_ZINTRUSIVERBTREE_HPP

#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/macros.hpp"
#include <cstddef>
#include <cstdint>

template<typename T, typename Key, typename Compare>
class ZIntrusiveRBTree {
public:
  class Node;

  enum Color { RED = 0b0, BLACK = 0b1 };

  class ColoredNodePtr {
    static constexpr uintptr_t COLOR_MASK = 0b1;
    static constexpr uintptr_t NODE_MASK = ~COLOR_MASK;
    uintptr_t _value;
  public:
    ColoredNodePtr(Node* node = nullptr, Color color = RED)
     : _value(reinterpret_cast<uintptr_t>(node) | color) {}

    constexpr bool is_black() const { return color() == BLACK; }
    constexpr bool is_red() const { return color() == RED; }
    constexpr Color color() const { return static_cast<Color>(_value & COLOR_MASK); }

    Node* node() const { return reinterpret_cast<Node*>(_value & NODE_MASK); }
    Node* red_node() const { precond(is_red()); return reinterpret_cast<Node*>(_value); }
    Node* black_node() const { precond(is_black()); return reinterpret_cast<Node*>(_value ^ BLACK); }
  };

  enum Direction { LEFT, RIGHT };
  static constexpr Direction other(const Direction& dir) { return dir == LEFT ? RIGHT : LEFT; }
  static constexpr const char* str(const Direction& dir) { return dir == LEFT ? "LEFT" : "RIGHT"; }

  class Node {
    friend class ZIntrusiveRBTree<T, Key, Compare>;
  private:
    ColoredNodePtr _colored_parent;
    Node* _left;
    Node* _right;
    template<Direction DIRECTION>
    const Node* find_next_node() const {
      constexpr Direction OTHER_DIRECTION = other(DIRECTION);
      const Node* node = this;
      // Down the tree
      if (node->has_child<DIRECTION>()) {
        node = node->child<DIRECTION>();
        while (node->has_child<OTHER_DIRECTION>()) {
          node = node->child<OTHER_DIRECTION>();
        }
        return node;
      }

      // Up the tree
      const Node* parent = node->parent();
      for (;parent != nullptr && node == parent->child<DIRECTION>(); parent = node->parent()) {
        node = parent;
      }
      return parent;
    }

    template<Direction DIRECTION>
    const Node* child() const {
      switch (DIRECTION) {
        case LEFT: return _left;
        case RIGHT: return _right;
      }
    }
    template<Direction DIRECTION>
    Node* child() { return const_cast<Node*>(const_cast<const Node*>(this)->template child<DIRECTION>()); }

    template<Direction DIRECTION>
    Node* const* child_addr() const {
      switch (DIRECTION) {
        case LEFT: return &_left;
        case RIGHT: return &_right;
      }
    }

    template<Direction DIRECTION>
    bool has_child() const {
      switch (DIRECTION) {
        case LEFT: return _left != nullptr;
        case RIGHT: return _right != nullptr;
      }
    }

    template<Direction DIRECTION>
    void update_child(Node* new_child) {
      switch (DIRECTION) {
        case LEFT: _left = new_child; break;
        case RIGHT: _right = new_child; break;
      }
    }

  public:
    Node() {}

    void link_node(Node* parent, Node** insert_location) {
      // Newly linked node is always red
      _colored_parent = ColoredNodePtr(parent, RED);
      _left = nullptr;
      _right = nullptr;

      // Link into location
      *insert_location = this;
    }

    void update_parent_and_color(ColoredNodePtr colored_parent) { _colored_parent = colored_parent; }
    void update_parent_and_color(Node* parent, Color color) { _colored_parent = ColoredNodePtr(parent, color); }

    void update_parent(Node* parent) { _colored_parent = ColoredNodePtr(parent, color()); }
    void update_color(Color color) { _colored_parent = ColoredNodePtr(parent(), color); }

    void update_left_child(Node* new_child) { update_child<LEFT>(new_child); }
    void update_right_child(Node* new_child) { update_child<RIGHT>(new_child); }

    const Node* parent() const { return _colored_parent.node(); }
    Node* parent() { return const_cast<Node*>(const_cast<const Node*>(this)->parent()); }
    const Node* red_parent() const { return _colored_parent.red_node(); }
    Node* red_parent() { return const_cast<Node*>(const_cast<const Node*>(this)->red_parent()); }
    const Node* black_parent() const { return _colored_parent.black_node(); }
    Node* black_parent() { return const_cast<Node*>(const_cast<const Node*>(this)->black_parent()); }

    bool has_parent() const { return _colored_parent.node() != nullptr; }

    Color color() const { return _colored_parent.color(); }
    bool is_black() const { return _colored_parent.is_black(); }
    bool is_red() const { return _colored_parent.is_red(); }
    static bool is_black(Node* node) { return node == nullptr || node->is_black(); }

    Node* const* left_child_addr() const { return child_addr<LEFT>(); }
    Node* const* right_child_addr() const { return child_addr<RIGHT>(); }

    const Node* left_child() const { return child<LEFT>(); }
    Node* left_child() { return const_cast<Node*>(const_cast<const Node*>(this)->left_child()); }
    const Node* right_child() const { return child<RIGHT>(); }
    Node* right_child() { return const_cast<Node*>(const_cast<const Node*>(this)->right_child()); }

    bool has_left_child() const { return has_child<LEFT>(); }
    bool has_right_child() const { return has_child<RIGHT>(); }

    const Node* prev() const { return find_next_node<LEFT>(); }
    Node* prev() { return const_cast<Node*>(const_cast<const Node*>(this)->prev()); }
    const Node* next() const { return find_next_node<RIGHT>(); }
    Node* next() { return const_cast<Node*>(const_cast<const Node*>(this)->next()); }
  };

  struct Root {
    Node* _node = nullptr;
  };

private:
  Root _root;
  Node* _left_most = nullptr;
  DEBUG_ONLY(uintptr_t _sequence_number;)

  NONCOPYABLE(ZIntrusiveRBTree);

  static Node* cast_to_inner(T* element) { return &element->_node; }
  static  T* cast_to_outer(Node* node) { return (T*)((uintptr_t)node - offset_of(T, _node)); }

  void verify_tree() {
#ifdef ASSERT
    // Properties:
    //  (a) Node's are either BLACK or RED
    //  (b) All nullptr children are counted as BLACK
    //  (c) Compare::operator(T*, T*) <=> 0 is transitive
    // Invariants:
    //  (1) Root node is BLACK
    //  (2) All RED nodes only have BLACK children
    //  (3) Every simple path from the root to a leaf
    //      contains the same amount of BLACK nodes
    //  (4) A node's children must have that node as
    //      its parent
    //  (5) Each node N in the sub-tree formed from a
    //      node A's child must:
    //        if left child:  Compare::operator(A, N) < 0
    //        if right child: Compare::operator(A, N) > 0
    //
    // Note: 1-4 may not hold during a call to insert
    //       and remove.

    // Helpers
    const auto is_leaf = [](Node* node) {
      return node == nullptr;
    };
    const auto is_black = [&](Node* node) {
      return is_leaf(node) || node->is_black();
    };
    const auto is_red = [&](Node* node) {
      return !is_black(node);
    };

    // Verify (1)
    Node* const root_node = _root._node;
    assert(is_black(root_node), "Invariant (1)");

    // Verify (2)
    const auto verify_2 = [&](Node* node) {
      assert(!is_red(node) || is_black(node->left_child()), "Invariant (2)");
      assert(!is_red(node) || is_black(node->right_child()), "Invariant (2)");
    };

    // Verify (3)
    size_t first_simple_path_black_nodes_traversed = 0;
    const auto verify_3 = [&](Node* node, size_t black_nodes_traversed) {
      if (!is_leaf(node)) { return; }
      if (first_simple_path_black_nodes_traversed == 0) {
        first_simple_path_black_nodes_traversed = black_nodes_traversed;
      }
      assert(first_simple_path_black_nodes_traversed == black_nodes_traversed, "Invariant (3)");
    };

    // Verify (4)
    const auto verify_4 = [&](Node* node) {
      if (is_leaf(node)) { return; }
      assert(!node->has_left_child() || node->left_child()->parent() == node, "Invariant (4)");
      assert(!node->has_right_child() || node->right_child()->parent() == node, "Invariant (4)");
    };
    assert(root_node == nullptr || root_node->parent() == nullptr, "Invariant (4)");

    // Verify (5)
    const auto verify_5 = [&](Node* node) {
      // Because of the transitive property of Compare (c) we simply check
      // this that (5) hold for each parent child pair.
      if (is_leaf(node)) { return; }
      Compare compare_fn;
      const auto o = [](Node* n) { return cast_to_outer(n); };
      assert(!node->has_left_child() || compare_fn(o(node->left_child()), o(node)) < 0, "Invariant (5)");
      assert(!node->has_right_child() || compare_fn(o(node->right_child()), o(node)) > 0, "Invariant (5)");
    };

    // Walk every simple path by recursively defending the tree from the root
    const auto recursive_walk = [&](auto&& recurse, Node* node, size_t black_nodes_traversed) {
      if (is_black(node)) { black_nodes_traversed++; }
      verify_2(node);
      verify_3(node, black_nodes_traversed);
      verify_4(node);
      verify_5(node);
      if (is_leaf(node)) { return; }
      recurse(recurse, node->left_child(), black_nodes_traversed);
      recurse(recurse, node->right_child(), black_nodes_traversed);
    };
    recursive_walk(recursive_walk, root_node, 0);
#endif
  }

  template<bool swap_left_right>
  bool verify_node(Node* parent, Node* left_child, Node* right_child) {
    if (swap_left_right) {
      ::swap(left_child, right_child);
    }
    assert(parent->left_child() == left_child, swap_left_right ? "Bad child Swapped" : "Bad child");
    assert(parent->right_child() == right_child, swap_left_right ? "Bad child Swapped" : "Bad child");
    if (left_child != nullptr) {
      assert(left_child->parent() == parent, swap_left_right ? "Bad parent Swapped" : "Bad parent");
    }
    if (right_child != nullptr) {
      assert(right_child->parent() == parent, swap_left_right ? "Bad parent Swapped" : "Bad parent");
    }
    return true;
  }

  template<bool swap_left_right>
  bool verify_node(Node* parent) {
    if (parent == nullptr) {
      return true;
    }
    if (swap_left_right) {
      return verify_node<swap_left_right>(parent, parent->right_child());
    }
    return verify_node<swap_left_right>(parent, parent->left_child());
  }

  template<bool swap_left_right>
  bool verify_node(Node* parent, Node* left_child) {
    if (swap_left_right) {
      return verify_node<swap_left_right>(parent, left_child, parent->left_child());
    }
    return verify_node<swap_left_right>(parent, left_child, parent->right_child());
  }
  struct any_t {};
  template<bool swap_left_right>
  bool verify_node(Node* parent, any_t, Node* right_child) {
    if (swap_left_right) {
      return verify_node<swap_left_right>(parent, parent->right_child(), right_child);
    }
    return verify_node<swap_left_right>(parent, parent->left_child(), right_child);
  }

  Node* const* root_node_addr() const { return &_root._node; }

  void update_child_or_root(Node* old_node, Node* new_node, Node* parent) {
    if (parent == nullptr) {
      // Update root
      _root._node = new_node;
      return;
    }
    if (old_node == parent->left_child()) {
      parent->update_left_child(new_node);
      return;
    }
    assert(old_node == parent->right_child(), "must be");
    parent->update_right_child(new_node);
  }

  template<Direction PARENT_SIBLING_DIRECTION>
  void rebalance_insert_with_sibling(Node* node, Node* parent, Node* grand_parent) {
    DEBUG_ONLY(const bool swap_left_right = PARENT_SIBLING_DIRECTION == LEFT;)
    constexpr Direction OTHER_DIRECTION = other(PARENT_SIBLING_DIRECTION);
    Node* sibling = parent->template child<PARENT_SIBLING_DIRECTION>();
    DEBUG_ONLY(bool rotated_parent = false;)
    if (node == sibling) {
      DEBUG_ONLY(rotated_parent = true;)
      // Rotate up node through parent
      Node* child = node->template child<OTHER_DIRECTION>();

      //// PRE
      //
      //      G          G
      //     /            \
      //    p      or      p
      //     \            /
      //      n          n
      //     /            \
      //   (c)            (c)
      //
      ////
      precond(grand_parent->is_black());
      precond(parent->is_red());
      precond(node->is_red());
      precond(verify_node<swap_left_right>(grand_parent, parent));
      precond(verify_node<swap_left_right>(parent, any_t{}, node));
      precond(verify_node<swap_left_right>(node, child));
      precond(verify_node<swap_left_right>(child));

      // Fix children
      parent->template update_child<PARENT_SIBLING_DIRECTION>(child);
      node->template update_child<OTHER_DIRECTION>(parent);

      // Fix parents and colors
      if (child != nullptr) {
        child->update_parent_and_color(parent, BLACK);
      }
      parent->update_parent_and_color(node, RED);

      //// POST
      //
      //        G          G
      //       /            \
      //      n      or      n
      //     /                \
      //    p                  p
      //     \                /
      //     (C)            (C)
      //
      ////
      postcond(grand_parent->is_black());
      postcond(parent->is_red());
      postcond(node->is_red());
      postcond(Node::is_black(child));
      // The grand_parent is updated in the next rotation
      // postcond(verify_node<swap_left_right>(grand_parent, node));
      postcond(verify_node<swap_left_right>(node, parent));
      postcond(verify_node<swap_left_right>(parent, any_t{}, child));
      postcond(verify_node<swap_left_right>(child));

      parent = node;
      sibling = parent->template child<PARENT_SIBLING_DIRECTION>();
      DEBUG_ONLY(node = parent->template child<OTHER_DIRECTION>();)
    }

    //// PRE
    //
    //        G        G
    //       /          \
    //      p     or     p
    //     / \          / \
    //    n  (s)      (s)  n
    //
    ////
    precond(grand_parent->is_black());
    precond(parent->is_red());
    precond(node->is_red());
    precond(rotated_parent || verify_node<swap_left_right>(grand_parent, parent));
    precond(verify_node<swap_left_right>(parent, node, sibling));
    precond(verify_node<swap_left_right>(node));
    precond(verify_node<swap_left_right>(sibling));

    // Rotate up parent through grand-parent

    // Fix children
    grand_parent->template update_child<OTHER_DIRECTION>(sibling);
    parent->template update_child<PARENT_SIBLING_DIRECTION>(grand_parent);

    // Fix parents and colors
    if (sibling != nullptr) {
      sibling->update_parent_and_color(grand_parent, BLACK);
    }
    rotate_and_update_child_or_root(grand_parent, parent, RED);

    //// POST
    //
    //      P          P
    //     / \        / \
    //    n   g  or  g   n
    //       /        \
    //     (S)        (S)
    //
    ////
    postcond(parent->is_black());
    postcond(grand_parent->is_red());
    postcond(node->is_red());
    postcond(Node::is_black(sibling));
    postcond(verify_node<swap_left_right>(parent, node, grand_parent));
    postcond(verify_node<swap_left_right>(node));
    postcond(verify_node<swap_left_right>(grand_parent, sibling));
    postcond(verify_node<swap_left_right>(sibling));
  }

  template<Direction PARENT_SIBLING_DIRECTION>
  bool rebalance_insert_with_parent_sibling(Node** node_addr, Node** parent_addr, Node* grand_parent) {
    DEBUG_ONLY(const bool swap_left_right = PARENT_SIBLING_DIRECTION == LEFT;)
    constexpr Direction OTHER_DIRECTION = other(PARENT_SIBLING_DIRECTION);
    Node* const parent_sibling = grand_parent->template child<PARENT_SIBLING_DIRECTION>();
    Node*& node = *node_addr;
    Node*& parent = *parent_addr;
    if (parent_sibling != nullptr && parent_sibling->is_red()) {
      //// PRE
      //
      //       G          G
      //      / \        / \
      //     p   u  or  u   p
      //    / \            / \
      //   n | n          n | n
      //
      ////
      precond(grand_parent->is_black());
      precond(parent_sibling->is_red());
      precond(parent->is_red());
      precond(node->is_red());
      precond(verify_node<swap_left_right>(grand_parent, parent, parent_sibling));
      precond(parent->left_child() == node || parent->right_child() == node);
      precond(verify_node<swap_left_right>(parent));
      precond(verify_node<swap_left_right>(parent_sibling));
      precond(verify_node<swap_left_right>(node));

      // Flip colors of parent, parent sibling and grand parent
      parent_sibling->update_parent_and_color(grand_parent, BLACK);
      parent->update_parent_and_color(grand_parent, BLACK);
      Node* grand_grand_parent = grand_parent->black_parent();
      grand_parent->update_parent_and_color(grand_grand_parent, RED);
      //// POST
      //
      //       g          g
      //      / \        / \
      //     P   U  or  U   P
      //    / \            / \
      //   n | n          n | n
      //
      ////
      postcond(grand_parent->is_red());
      postcond(parent_sibling->is_black());
      postcond(parent->is_black());
      postcond(node->is_red());
      postcond(verify_node<swap_left_right>(grand_parent, parent, parent_sibling));
      postcond(parent->left_child() == node || parent->right_child() == node);
      postcond(verify_node<swap_left_right>(parent));
      postcond(verify_node<swap_left_right>(parent_sibling));
      postcond(verify_node<swap_left_right>(node));

      // Recurse up the tree
      node = grand_parent;
      parent = grand_grand_parent;
      return false; // Not finished
    }

    rebalance_insert_with_sibling<PARENT_SIBLING_DIRECTION>(node, parent, grand_parent);
    return true; // Finished
  }

  void rebalance_insert(Node* new_node) {
    Node* node = new_node;
    Node* parent = node->red_parent();
    for (;;) {
      precond(node->is_red());
      if (parent == nullptr) {
        // Recursive (or root) case
        node->update_parent_and_color(parent, BLACK);
        break;
      }
      if (parent->is_black()) {
        // Tree is balanced
        break;
      }
      Node* grand_parent = parent->red_parent();
      if (parent == grand_parent->left_child() ? rebalance_insert_with_parent_sibling<RIGHT>(&node, &parent, grand_parent)
                                               : rebalance_insert_with_parent_sibling<LEFT>(&node, &parent, grand_parent)) {
        break;
      }
    }
    verify_tree();
  }

  void rotate_and_update_child_or_root(Node* old_node, Node* new_node, Color color) {
    Node* const parent = old_node->parent();
    new_node->update_parent_and_color(old_node->_colored_parent);
    old_node->update_parent_and_color(new_node, color);
    update_child_or_root(old_node, new_node, parent);
  }

  template<Direction SIBLING_DIRECTION>
  bool rebalance_remove_with_sibling(Node** node_addr, Node** parent_addr) {
    DEBUG_ONLY(const bool swap_left_right = SIBLING_DIRECTION == LEFT;)
    constexpr Direction OTHER_DIRECTION = other(SIBLING_DIRECTION);
    Node*& node = *node_addr;
    Node*& parent = *parent_addr;
    Node* sibling = parent->template child<SIBLING_DIRECTION>();
    if (sibling->is_red()) {
      Node* sibling_child = sibling->template child<OTHER_DIRECTION>();
      //// PRE
      //
      //     P          P
      //    / \        / \
      //   N   s  or  s   N
      //      /        \
      //     SC        SC
      //
      ////
      precond(parent->is_black());
      precond(Node::is_black(node));
      precond(sibling->is_red());
      precond(Node::is_black(sibling_child));
      precond(verify_node<swap_left_right>(parent, node, sibling));
      precond(verify_node<swap_left_right>(node));
      precond(verify_node<swap_left_right>(sibling, sibling_child));
      precond(verify_node<swap_left_right>(sibling_child));

      // Rotate sibling up through parent

      // Fix children
      parent->template update_child<SIBLING_DIRECTION>(sibling_child);
      sibling->template update_child<OTHER_DIRECTION>(parent);

      // Fix parents and colors
      sibling_child->update_parent_and_color(parent, BLACK);
      rotate_and_update_child_or_root(parent, sibling, RED);

      //// POST
      //
      //       S         S
      //      /           \
      //     p             p
      //    / \           / \
      //   N   SC        SC  N
      //
      ////
      postcond(sibling->is_black());
      postcond(parent->is_red());
      postcond(Node::is_black(node));
      postcond(Node::is_black(sibling_child));
      postcond(verify_node<swap_left_right>(sibling, parent));
      postcond(verify_node<swap_left_right>(parent, node, sibling_child));
      postcond(verify_node<swap_left_right>(node));
      postcond(verify_node<swap_left_right>(sibling_child));

      // node has a new sibling
      sibling = sibling_child;
    }

    Node* sibling_child = sibling->template child<SIBLING_DIRECTION>();
    DEBUG_ONLY(bool rotated_parent = false;)
    if (Node::is_black(sibling_child)) {
      DEBUG_ONLY(rotated_parent = true;)
      Node* sibling_other_child = sibling->template child<OTHER_DIRECTION>();
      if (Node::is_black(sibling_other_child)) {
        //// PRE
        //
        //    (p)        (p)
        //    / \        / \
        //   N   S  or  S   N
        //
        ////
        precond(Node::is_black(node));
        precond(sibling->is_black());
        precond(verify_node<swap_left_right>(parent, node, sibling));

        // Flip sibling color to RED
        sibling->update_parent_and_color(parent, RED);

        //// POST
        //
        //    (p)        (p)
        //    / \        / \
        //   N   s  or  s   N
        //
        ////
        postcond(Node::is_black(node));
        postcond(sibling->is_red());
        postcond(verify_node<swap_left_right>(parent, node, sibling));

        if (parent->is_black()) {
          // We did not introduce a RED-RED edge, if parent is
          // the root we are done, else recurse up the tree
          if (parent->parent() != nullptr) {
            node = parent;
            parent = node->parent();
            return false;
          }
          return true;
        }
        // Change RED-RED edge to BLACK-RED edge
        parent->update_color(BLACK);
        return true;
      }

      Node* sibling_grand_child = sibling_other_child->template child<SIBLING_DIRECTION>();
      //// PRE
      //
      //    (p)          (p)
      //    / \          / \
      //   N   S        S   N
      //      /     or   \
      //    soc          soc
      //      \          /
      //     (sgc)     (sgc)
      //
      ////
      precond(Node::is_black(node));
      precond(sibling->is_black());
      precond(sibling_other_child->is_red());
      precond(verify_node<swap_left_right>(parent, node, sibling));
      precond(verify_node<swap_left_right>(node));
      precond(verify_node<swap_left_right>(sibling, sibling_other_child, sibling_child));
      precond(verify_node<swap_left_right>(sibling_other_child, any_t{}, sibling_grand_child));
      precond(verify_node<swap_left_right>(sibling_grand_child));

      // Rotate sibling other child through the sibling

      // Fix children
      sibling->template update_child<OTHER_DIRECTION>(sibling_grand_child);
      sibling_other_child->template update_child<SIBLING_DIRECTION>(sibling);
      parent->template update_child<SIBLING_DIRECTION>(sibling_other_child);

      // Fix parents and colors
      if (sibling_grand_child != nullptr) {
        sibling_grand_child->update_parent_and_color(sibling, BLACK);
      }
      // Defer updating the sibling and sibling other child parents until
      // after we rotate below. This will also fix the any potential RED-RED
      // edge between parent and sibling_other_child

      //// POST
      //
      //    (p)            (p)
      //    / \            / \
      //   N  soc   or   soc  N
      //      / \        / \
      //    SGC  S      S  SGC
      //
      ////
      postcond(Node::is_black(node));
      postcond(sibling->is_black());
      postcond(sibling_other_child->is_red());
      postcond(Node::is_black(sibling_grand_child));
      // Deferred
      // postcond(verify_node<swap_left_right>(parent, node, sibling_other_child));
      postcond(verify_node<swap_left_right>(node));
      // postcond(verify_node<swap_left_right>(sibling_other_child, sibling_grand_child, sibling));
      postcond(verify_node<swap_left_right>(sibling_grand_child));
      postcond(verify_node<swap_left_right>(sibling));

      // node has a new sibling
      sibling_child = sibling;
      sibling = sibling_other_child;
    }

    Node* sibling_other_child = sibling->template child<OTHER_DIRECTION>();
    //// PRE
    //
    //    (p)              (p)
    //    / \              / \
    //   N   S     or     S   N
    //      / \          / \
    //   (soc)(sc)    (sc)(soc)
    //
    ////
    DEBUG_ONLY(Color parent_color = parent->color());
    precond(Node::is_black(node));
    precond(rotated_parent || sibling->is_black());
    DEBUG_ONLY(bool sibling_other_child_is_black = Node::is_black(sibling_other_child));
    precond(rotated_parent || verify_node<swap_left_right>(parent, node, sibling));
    precond(verify_node<swap_left_right>(node));
    precond(rotated_parent || verify_node<swap_left_right>(sibling, sibling_other_child, sibling_child));
    postcond(verify_node<swap_left_right>(sibling_other_child));
    postcond(verify_node<swap_left_right>(sibling_child));

    // Rotate sibling through parent and fix colors

    // Fix children
    parent->template update_child<SIBLING_DIRECTION>(sibling_other_child);
    sibling->template update_child<OTHER_DIRECTION>(parent);

    // Fix parents and colors
    sibling_child->update_parent_and_color(sibling, BLACK);
    if (sibling_other_child != nullptr) {
      sibling_other_child->update_parent(parent);
    }
    rotate_and_update_child_or_root(parent, sibling, BLACK);

    //// POST
    //
    //      (s)           (s)
    //      / \           / \
    //     P   SC  or    SC  P
    //    / \               / \
    //   N (soc)         (soc) N
    //
    ////
    postcond(sibling->color() == parent_color);
    postcond(parent->is_black());
    postcond(sibling_child->is_black());
    postcond(Node::is_black(node));
    postcond(sibling_other_child_is_black == Node::is_black(sibling_other_child));
    postcond(verify_node<swap_left_right>(sibling, parent, sibling_child));
    postcond(verify_node<swap_left_right>(parent, node, sibling_other_child));
    postcond(verify_node<swap_left_right>(sibling_child));
    postcond(verify_node<swap_left_right>(node));
    postcond(verify_node<swap_left_right>(sibling_other_child));
    return true;
  }

  void rebalance_remove(Node* rebalance_from) {
    Node* node = nullptr;
    Node* parent = rebalance_from;

    for (;;) {
      precond(Node::is_black(node));
      precond(parent != nullptr);
      if (node == parent->left_child() ? rebalance_remove_with_sibling<RIGHT>(&node, &parent)
                                       : rebalance_remove_with_sibling<LEFT>(&node, &parent)) {
        break;
      }
    }
    verify_tree();
  }

public:
  ZIntrusiveRBTree() = default;

  T* first() const {
    return _left_most == nullptr ? nullptr : cast_to_outer(_left_most);
  }

  class FindCursor {
    friend class ZIntrusiveRBTree<T, Key, Compare>;
  private:
    Node** _insert_location;
    Node* _parent;
    bool _left_most;
    DEBUG_ONLY(const uintptr_t _sequence_number;)

    FindCursor(Node** insert_location, Node* parent, bool left_most DEBUG_ONLY(COMMA uintptr_t sequence_number))
    : _insert_location(insert_location),
      _parent(parent),
      _left_most(left_most)
      DEBUG_ONLY(COMMA _sequence_number(sequence_number)) {}

    FindCursor() : _insert_location(nullptr), _parent(nullptr), _left_most() DEBUG_ONLY(COMMA _sequence_number()) {}

#ifdef ASSERT
    bool is_valid(uintptr_t sequence_number) const { return is_valid() && _sequence_number == sequence_number; }
#endif

  public:
    bool is_valid() const { return insert_location() != nullptr; }
    bool found() const { return element() != nullptr; }
    T* element() const { return *_insert_location == nullptr ? nullptr : cast_to_outer(*_insert_location); }
    Node* node() const { precond(is_valid()); return *_insert_location == nullptr ? nullptr : *_insert_location; }
    bool is_left_most() const { precond(is_valid()); return _left_most; }
    Node* parent() const { precond(is_valid()); return _parent; }
    Node** insert_location() const { return _insert_location; }
  };

  FindCursor get_cursor(const Node* node) const {
    if (node == nullptr) {
      // Return a invalid cursor
      return FindCursor();
    }
    const bool is_left_most = node == _left_most;
    if (node->has_parent()) {
      const Node* const parent = node->parent();
      if (parent->left_child() == node) {
        return FindCursor(const_cast<Node**>(parent->left_child_addr()), nullptr, is_left_most DEBUG_ONLY(COMMA _sequence_number));
      }
      assert(parent->right_child() == node, "must be");
        return FindCursor(const_cast<Node**>(parent->right_child_addr()), nullptr, is_left_most DEBUG_ONLY(COMMA _sequence_number));
    }
    // No parent, root node
    return FindCursor(const_cast<Node**>(&_root._node), nullptr, is_left_most DEBUG_ONLY(COMMA _sequence_number));
  }

  FindCursor next(const FindCursor& cursor) const {
    if (cursor.found()) {
      return get_cursor(cursor.node()->next());
    }
    Node* const parent = cursor.parent();
    if (parent == nullptr) {
      assert(&_root._node == cursor.insert_location(), "must be");
      // tree is empty
      return FindCursor();
    }
    if (parent->left_child_addr() == cursor.insert_location()) {
      // Cursor at left leaf, parent is next
      return get_cursor(parent);
    }
    assert(parent->right_child_addr() == cursor.insert_location(), "must be");
    // Cursor at right leaf, parent->next is next
    return get_cursor(parent->next());
  }

  FindCursor find(const Key& key) const {
    Compare compare_fn;
    Node* const* insert_location = root_node_addr();
    Node* parent = nullptr;
    bool left_most = true;
    while (*insert_location != nullptr) {
      T* other = cast_to_outer(*insert_location);
      int result = compare_fn(key, other);
      if (result == 0) {
        assert(*insert_location != _left_most || left_most, "must be");
        return FindCursor(const_cast<Node**>(insert_location), parent, *insert_location == _left_most DEBUG_ONLY(COMMA _sequence_number));
      }
      parent = *insert_location;
      if (result < 0) {
        insert_location = parent->left_child_addr();
      } else {
        insert_location = parent->right_child_addr();
        // We took one step to the right, cannot be left_most.
        left_most = false;
      }
    }
    return FindCursor(const_cast<Node**>(insert_location), parent, left_most DEBUG_ONLY(COMMA _sequence_number));
  }

  void remove(const FindCursor& find_cursor) {
    precond(find_cursor.is_valid(_sequence_number));
    precond(find_cursor.found());
    DEBUG_ONLY(_sequence_number++;)

    Node* node = find_cursor.node();
    Node* parent = node->parent();
    if (find_cursor.is_left_most()) {
      assert(_left_most == node, "must be");
      _left_most = _left_most->next();
    }

    Node* rebalance_from = nullptr;

    if (!node->has_left_child() && !node->has_right_child()) {
      // No children

      // Remove node
      update_child_or_root(node, nullptr, parent);
      if (node->is_black()) {
        // We unbalanced the tree
        rebalance_from = parent;
      }
    } else if (!node->has_left_child() || !node->has_right_child()) {
      assert(node->has_right_child() || node->has_left_child(), "must be");
      // Only one child
      Node* child = node->has_left_child() ? node->left_child() : node->right_child();

      // Let child take nodes places
      update_child_or_root(node, child, parent);

      // And update parent and color
      child->update_parent_and_color(node->_colored_parent);
    } else {
      assert(node->has_left_child() && node->has_right_child(), "must be");
      // Find next node and let it take the nodes place
      // This asymmetry always swap next instead of prev,
      // I wonder how this behaves w.r.t. our strategies
      // for talking from the

      // This will never walk up the tree, hope the compiler sees this.
      Node* next_node = node->next();

      Node* next_node_parent = next_node->parent();
      Node* next_node_child = next_node->right_child();
      if (next_node_parent != node) {
        // Not the direct descendant, adopt node's child
        Node* node_child = node->right_child();
        next_node->update_right_child(node_child);
        node_child->update_parent(next_node);

        // And let parent adopt their grand child
        next_node_parent->update_left_child(next_node_child);
      } else {
        next_node_parent = next_node;
      }
      // Adopt node's other child
      Node* node_child = node->left_child();
      next_node->update_left_child(node_child);
      node_child->update_parent(next_node);

      update_child_or_root(node, next_node, parent);

      // Update parent(s) and colors
      if (next_node_child != nullptr) {
        next_node_child->update_parent_and_color(next_node_parent, BLACK);
      } else if (next_node->is_black()) {
        rebalance_from = next_node_parent;
      }
      next_node->update_parent_and_color(node->_colored_parent);
    }

    if (rebalance_from == nullptr) {
      // Removal did not unbalance the tree
      verify_tree();
      return;
    }

    rebalance_remove(rebalance_from);
  }

  void insert(T* element, const FindCursor& find_cursor) {
    precond(find_cursor.is_valid(_sequence_number));
    precond(!find_cursor.found());
    Node* new_node = cast_to_inner(element);
    DEBUG_ONLY(_sequence_number++;)

    new_node->link_node(find_cursor.parent(), find_cursor.insert_location());
    if (find_cursor.is_left_most()) {
      _left_most = new_node;
    }
    rebalance_insert(new_node);
  }

  void replace(T* element, const FindCursor& find_cursor) {
    precond(find_cursor.is_valid(_sequence_number));
    precond(find_cursor.found());
    Node* new_node = cast_to_inner(element);
    DEBUG_ONLY(_sequence_number++;)

    if (new_node != find_cursor.node()) {
      // Node has changed

      // Copy the node to new location
      *new_node = *find_cursor.node();

      // Update insert location
      *find_cursor.insert_location() = new_node;

      // Update children's parent
      if (new_node->has_left_child()) {
        new_node->left_child()->update_parent(new_node);
      }
      if (new_node->has_right_child()) {
        new_node->right_child()->update_parent(new_node);
      }

      if (find_cursor.is_left_most()) {
        _left_most = new_node;
      }
    }
    verify_tree();
  }
};

#endif // SHARE_GC_Z_ZINTRUSIVERBTREE_HPP
