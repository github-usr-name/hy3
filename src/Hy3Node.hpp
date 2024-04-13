#pragma once

struct Hy3Node;
struct Hy3GroupData;
enum class Hy3GroupLayout;

#include <list>

#include <hyprland/src/desktop/Window.hpp>

#include "Hy3Layout.hpp"
#include "TabGroup.hpp"
#include "conversions.hpp"

enum class Hy3GroupLayout {
	SplitH,
	SplitV,
	Tabbed,
};

enum class Hy3NodeType {
	Window,
	Group,
};

enum class ExpandFocusType {
	NotExpanded,
	Latch,
	Stack,
};

struct Hy3GroupData {
	Hy3GroupLayout layout = Hy3GroupLayout::SplitH;
	Hy3GroupLayout previous_nontab_layout = Hy3GroupLayout::SplitH;
	std::list<Hy3Node*> children;
	bool group_focused = true;
	Hy3Node* focused_child = nullptr;
	ExpandFocusType expand_focused = ExpandFocusType::NotExpanded;
	bool ephemeral = false;
	bool containment = false;
	Hy3TabGroup* tab_bar = nullptr;

	Hy3GroupData(Hy3GroupLayout layout);
	~Hy3GroupData();

	void collapseExpansions();
	void setLayout(Hy3GroupLayout layout);
	void setEphemeral(GroupEphemeralityOption ephemeral);

private:
	Hy3GroupData(Hy3GroupData&&);
	Hy3GroupData(const Hy3GroupData&) = delete;

	friend class Hy3NodeData;
};

class Hy3NodeData {
public:
	Hy3NodeType type;
	union {
		Hy3GroupData as_group;
		CWindow* as_window;
	};

	Hy3NodeData();
	Hy3NodeData(CWindow* window);
	Hy3NodeData(Hy3GroupLayout layout);
	~Hy3NodeData();

	Hy3NodeData& operator=(CWindow*);
	Hy3NodeData& operator=(Hy3GroupLayout);

	bool operator==(const Hy3NodeData&) const;

	// private: - I give up, C++ wins
	Hy3NodeData(Hy3GroupData);
	Hy3NodeData(Hy3NodeData&&);
	Hy3NodeData& operator=(Hy3NodeData&&);
};

struct Hy3Node {
	Hy3Node* parent = nullptr;
	bool reparenting = false;
	Hy3NodeData data;
	Vector2D position;
	Vector2D size;
	Vector2D gap_topleft_offset;
	Vector2D gap_bottomright_offset;
	float size_ratio = 1.0;
	PHLWORKSPACE workspace = nullptr;
	bool hidden = false;
	Hy3Layout* layout = nullptr;

	bool operator==(const Hy3Node&) const;

	void focus();
	void focusWindow();
	CWindow* bringToTop();
	void markFocused();
	void raiseToTop();
	Vector2D middle();
	Hy3Node* getFocusedNode(bool ignore_group_focus = false, bool stop_at_expanded = false);
	Hy3Node* findNeighbor(ShiftDirection);
	Hy3Node* getImmediateSibling(ShiftDirection);
	Hy3Node* getRoot();
	void resize(ShiftDirection, double, bool no_animation = false);
	bool isIndirectlyFocused();
	Hy3Node& getExpandActor();

	void recalcSizePosRecursive(bool no_animation = false);
	void updateTabBar(bool no_animation = false);
	void updateTabBarRecursive();
	void updateDecos();

	std::string getTitle();
	bool isUrgent();
	void setHidden(bool);

	Hy3Node* findNodeForTabGroup(Hy3TabGroup&);
	void appendAllWindows(std::vector<CWindow*>&);
	bool hasChild(Hy3Node* child);
	std::string debugNode();

	// Remove this node from its parent, deleting the parent if it was
	// the only child and recursing if the parent was the only child of it's
	// parent.
	// expand actor should be recalc'd if set
	Hy3Node* removeFromParentRecursive(Hy3Node** expand_actor);

	// Replace this node with a group, returning this node's new address.
	Hy3Node* intoGroup(Hy3GroupLayout, GroupEphemeralityOption);

	// Attempt to swallow a group. returns true if swallowed
	static bool swallowGroups(Hy3Node* into);
	static void swapData(Hy3Node&, Hy3Node&);
};

struct Distance {
	double primary_axis;
	double secondary_axis;

	Distance() = default;

	Distance(ShiftDirection direction, Vector2D from, Vector2D to) {
		auto dist = from - to;
		primary_axis = getAxis(direction) == Axis::Horizontal ? dist.x : dist.y;
		secondary_axis = getAxis(direction) == Axis::Horizontal ? dist.y : dist.x;
	}

	bool operator<(Distance other) {
		return signbit(primary_axis) == signbit(other.primary_axis)
		    && (abs(primary_axis) < abs(other.primary_axis)
		        || (primary_axis == other.primary_axis
		            && abs(secondary_axis) < abs(other.secondary_axis)));
	}

	bool operator>(Distance other) {
		return signbit(primary_axis) == signbit(other.primary_axis)
		    && (abs(primary_axis) > abs(other.primary_axis)
		        || (primary_axis == other.primary_axis
		            && abs(secondary_axis) > abs(other.secondary_axis)));
	}

	bool isSameDirection(Distance other) {
		return signbit(primary_axis) == signbit(other.primary_axis);
	}

	bool isInDirection(ShiftDirection direction) {
		return std::signbit(primary_axis)
		    == (getSearchDirection(direction) == SearchDirection::Forwards);
	}
};