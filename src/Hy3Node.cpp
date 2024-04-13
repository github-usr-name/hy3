#include <assert.h>
#include <sstream>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/helpers/Box.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>

#include "Hy3Node.hpp"
#include "globals.hpp"

const float MIN_RATIO = 0.0f;

// Hy3GroupData //

Hy3GroupData::Hy3GroupData(Hy3GroupLayout layout): layout(layout) {
	if (layout != Hy3GroupLayout::Tabbed) {
		this->previous_nontab_layout = layout;
	}
}

Hy3GroupData::Hy3GroupData(Hy3GroupData&& from) {
	this->layout = from.layout;
	this->previous_nontab_layout = from.previous_nontab_layout;
	this->children = std::move(from.children);
	this->group_focused = from.group_focused;
	this->expand_focused = from.expand_focused;
	this->focused_child = from.focused_child;
	from.focused_child = nullptr;
	this->tab_bar = from.tab_bar;
	from.tab_bar = nullptr;
}

Hy3GroupData::~Hy3GroupData() {
	if (this->tab_bar != nullptr) this->tab_bar->bar.beginDestroy();
}

void Hy3GroupData::collapseExpansions() {
	if (this->expand_focused == ExpandFocusType::NotExpanded) return;
	this->expand_focused = ExpandFocusType::NotExpanded;

	Hy3Node* node = this->focused_child;

	while (node->data.type == Hy3NodeType::Group
	       && node->data.as_group.expand_focused == ExpandFocusType::Stack)
	{
		node->data.as_group.expand_focused = ExpandFocusType::NotExpanded;
		node = node->data.as_group.focused_child;
	}
}

void Hy3GroupData::setLayout(Hy3GroupLayout layout) {
	this->layout = layout;

	if (layout != Hy3GroupLayout::Tabbed) {
		this->previous_nontab_layout = layout;
	}
}

void Hy3GroupData::setEphemeral(GroupEphemeralityOption ephemeral) {
	switch (ephemeral) {
	case GroupEphemeralityOption::Standard: this->ephemeral = false; break;
	case GroupEphemeralityOption::ForceEphemeral: this->ephemeral = true; break;
	case GroupEphemeralityOption::Ephemeral:
		// no change
		break;
	}
}

// Hy3NodeData //

Hy3NodeData::Hy3NodeData(): Hy3NodeData((CWindow*) nullptr) {}

Hy3NodeData::Hy3NodeData(CWindow* window): type(Hy3NodeType::Window) { this->as_window = window; }

Hy3NodeData::Hy3NodeData(Hy3GroupLayout layout): Hy3NodeData(Hy3GroupData(layout)) {}

Hy3NodeData::Hy3NodeData(Hy3GroupData group): type(Hy3NodeType::Group) {
	new (&this->as_group) Hy3GroupData(std::move(group));
}

Hy3NodeData::Hy3NodeData(Hy3NodeData&& from): type(from.type) {
	switch (from.type) {
	case Hy3NodeType::Window: this->as_window = from.as_window; break;
	case Hy3NodeType::Group: new (&this->as_group) Hy3GroupData(std::move(from.as_group)); break;
	}
}

Hy3NodeData::~Hy3NodeData() {
	switch (this->type) {
	case Hy3NodeType::Window: break;
	case Hy3NodeType::Group:
		this->as_group.~Hy3GroupData();

		// who ever thought calling the dtor after a move was a good idea?
		this->type = Hy3NodeType::Window;
		break;
	}
}

Hy3NodeData& Hy3NodeData::operator=(CWindow* window) {
	*this = Hy3NodeData(window);

	return *this;
}

Hy3NodeData& Hy3NodeData::operator=(Hy3GroupLayout layout) {
	*this = Hy3NodeData(layout);

	return *this;
}

Hy3NodeData& Hy3NodeData::operator=(Hy3NodeData&& from) {
	if (this->type == Hy3NodeType::Group) {
		this->as_group.~Hy3GroupData();
	}

	this->type = from.type;

	switch (this->type) {
	case Hy3NodeType::Window: this->as_window = from.as_window; break;
	case Hy3NodeType::Group: new (&this->as_group) Hy3GroupData(std::move(from.as_group)); break;
	}

	return *this;
}

bool Hy3NodeData::operator==(const Hy3NodeData& rhs) const { return this == &rhs; }

// Hy3Node //

bool Hy3Node::operator==(const Hy3Node& rhs) const { return this->data == rhs.data; }

void Hy3Node::focus() {
	this->markFocused();

	switch (this->data.type) {
	case Hy3NodeType::Window:
		this->data.as_window->setHidden(false);
		g_pCompositor->focusWindow(this->data.as_window);
		break;
	case Hy3NodeType::Group:
		g_pCompositor->focusWindow(nullptr);
		this->raiseToTop();
		break;
	}
}

CWindow* Hy3Node::bringToTop() {
	switch (this->data.type) {
	case Hy3NodeType::Window:
		this->markFocused();
		this->data.as_window->setHidden(false);

		return this->data.as_window;
	case Hy3NodeType::Group:
		if (this->data.as_group.layout == Hy3GroupLayout::Tabbed) {
			if (this->data.as_group.focused_child != nullptr) {
				return this->data.as_group.focused_child->bringToTop();
			}
		} else {
			for (auto* node: this->data.as_group.children) {
				auto* window = node->bringToTop();
				if (window != nullptr) return window;
			}
		}

		return nullptr;
	default: return nullptr;
	}
}

void Hy3Node::focusWindow() {
	auto* window = this->bringToTop();
	if (window != nullptr) g_pCompositor->focusWindow(window);
}

void markGroupFocusedRecursive(Hy3GroupData& group) {
	group.group_focused = true;
	for (auto& child: group.children) {
		if (child->data.type == Hy3NodeType::Group) markGroupFocusedRecursive(child->data.as_group);
	}
}

void Hy3Node::markFocused() {
	Hy3Node* node = this;

	// update focus
	if (this->data.type == Hy3NodeType::Group) {
		markGroupFocusedRecursive(this->data.as_group);
	}

	// undo decos for root focus
	auto* root = node;
	while (root->parent != nullptr) {
		root->parent->data.as_group.focused_child = root;
		root->parent->data.as_group.group_focused = false;
		root = root->parent;
	}

	root->updateDecos();
}

void Hy3Node::raiseToTop() {
	switch (this->data.type) {
	case Hy3NodeType::Window: g_pCompositor->changeWindowZOrder(this->data.as_window, true); break;
	case Hy3NodeType::Group:
		for (auto* child: this->data.as_group.children) {
			child->raiseToTop();
		}
		break;
	}
}

Hy3Node* Hy3Node::getFocusedNode(bool ignore_group_focus, bool stop_at_expanded) {
	switch (this->data.type) {
	case Hy3NodeType::Window: return this;
	case Hy3NodeType::Group:
		if (this->data.as_group.focused_child == nullptr
		    || (!ignore_group_focus && this->data.as_group.group_focused)
		    || (stop_at_expanded && this->data.as_group.expand_focused != ExpandFocusType::NotExpanded))
		{
			return this;
		} else {
			return this->data.as_group.focused_child->getFocusedNode(
			    ignore_group_focus,
			    stop_at_expanded
			);
		}
	default: return nullptr;
	}
}

bool Hy3Node::isIndirectlyFocused() {
	Hy3Node* node = this;

	while (node->parent != nullptr) {
		if (!node->parent->data.as_group.group_focused
		    && node->parent->data.as_group.focused_child != node)
			return false;
		node = node->parent;
	}

	return true;
}

// note: assumes this node is the expanded one without checking
Hy3Node& Hy3Node::getExpandActor() {
	Hy3Node* node = this;

	while (node->parent != nullptr
	       && node->parent->data.as_group.expand_focused != ExpandFocusType::NotExpanded)
		node = node->parent;

	return *node;
}

void Hy3Node::recalcSizePosRecursive(bool no_animation) {
	// clang-format off
	static const auto gaps_in = ConfigValue<Hyprlang::CUSTOMTYPE, CCssGapData>("general:gaps_in");
	static const auto gaps_out = ConfigValue<Hyprlang::CUSTOMTYPE, CCssGapData>("general:gaps_out");
	static const auto group_inset = ConfigValue<Hyprlang::INT>("plugin:hy3:group_inset");
	static const auto tab_bar_height = ConfigValue<Hyprlang::INT>("plugin:hy3:tabs:height");
	static const auto tab_bar_padding = ConfigValue<Hyprlang::INT>("plugin:hy3:tabs:padding");
	// clang-format on

	// clang-format off
	auto gap_topleft_offset = Vector2D(
	    -(gaps_in->left - gaps_out->left),
	    -(gaps_in->top - gaps_out->top)
	);

	auto gap_bottomright_offset = Vector2D(
	    -(gaps_in->right - gaps_out->right),
	    -(gaps_in->bottom - gaps_out->bottom)
	);
	// clang-format on

	if (this->data.type == Hy3NodeType::Window && this->data.as_window->m_bIsFullscreen) {
		auto* monitor = g_pCompositor->getMonitorFromID(this->workspace->m_iMonitorID);

		if (this->workspace->m_efFullscreenMode == FULLSCREEN_FULL) {
			this->data.as_window->m_vRealPosition = monitor->vecPosition;
			this->data.as_window->m_vRealSize = monitor->vecSize;
			return;
		}

		Hy3Node fake_node = {
		    .data = this->data.as_window,
		    .position = monitor->vecPosition + monitor->vecReservedTopLeft,
		    .size = monitor->vecSize - monitor->vecReservedTopLeft - monitor->vecReservedBottomRight,
		    .gap_topleft_offset = gap_topleft_offset,
		    .gap_bottomright_offset = gap_bottomright_offset,
		    .workspace = this->workspace,
		};

		this->layout->applyNodeDataToWindow(&fake_node);
		return;
	}

	if (this->parent != nullptr) {
		gap_topleft_offset = this->gap_topleft_offset;
		gap_bottomright_offset = this->gap_bottomright_offset;
	}

	auto tpos = this->position;
	auto tsize = this->size;

	double tab_height_offset = *tab_bar_height + *tab_bar_padding;

	if (this->data.type == Hy3NodeType::Window) {
		this->data.as_window->setHidden(this->hidden);
		this->layout->applyNodeDataToWindow(this, no_animation);
		return;
	}

	auto* group = &this->data.as_group;

	double constraint;
	switch (group->layout) {
	case Hy3GroupLayout::SplitH:
		constraint = tsize.x - gap_topleft_offset.x - gap_bottomright_offset.x;
		break;
	case Hy3GroupLayout::SplitV:
		constraint = tsize.y - gap_topleft_offset.y - gap_bottomright_offset.y;
		break;
	case Hy3GroupLayout::Tabbed: break;
	}

	auto expand_focused = group->expand_focused != ExpandFocusType::NotExpanded;
	bool directly_contains_expanded =
	    expand_focused
	    && (group->focused_child->data.type == Hy3NodeType::Window
	        || group->focused_child->data.as_group.expand_focused == ExpandFocusType::NotExpanded);

	auto child_count = group->children.size();
	double ratio_mul =
	    group->layout != Hy3GroupLayout::Tabbed ? child_count <= 0 ? 0 : constraint / child_count : 0;

	double offset = 0;

	if (group->layout == Hy3GroupLayout::Tabbed && group->focused_child != nullptr
	    && !group->focused_child->hidden)
	{
		group->focused_child->setHidden(false);

		auto box = CBox {tpos.x, tpos.y, tsize.x, tsize.y};
		g_pHyprRenderer->damageBox(&box);
	}

	if (group->expand_focused == ExpandFocusType::Latch) {
		auto* expanded_node = group->focused_child;

		while (expanded_node != nullptr && expanded_node->data.type == Hy3NodeType::Group
		       && expanded_node->data.as_group.expand_focused != ExpandFocusType::NotExpanded)
		{
			expanded_node = expanded_node->data.as_group.focused_child;
		}

		if (expanded_node == nullptr) {
			hy3_log(
			    ERR,
			    "recalcSizePosRecursive: unable to find expansion target of latch node {:x}",
			    (uintptr_t) this
			);
			errorNotif();
			return;
		}

		expanded_node->position = tpos;
		expanded_node->size = tsize;
		expanded_node->setHidden(this->hidden);

		expanded_node->gap_topleft_offset = gap_topleft_offset;
		expanded_node->gap_bottomright_offset = gap_bottomright_offset;

		expanded_node->recalcSizePosRecursive(no_animation);
	}

	for (auto* child: group->children) {
		if (directly_contains_expanded && child == group->focused_child) {
			switch (group->layout) {
			case Hy3GroupLayout::SplitH: offset += child->size_ratio * ratio_mul; break;
			case Hy3GroupLayout::SplitV: offset += child->size_ratio * ratio_mul; break;
			case Hy3GroupLayout::Tabbed: break;
			}

			continue;
		}

		switch (group->layout) {
		case Hy3GroupLayout::SplitH:
			child->position.x = tpos.x + offset;
			child->size.x = child->size_ratio * ratio_mul;
			offset += child->size.x;
			child->position.y = tpos.y;
			child->size.y = tsize.y;
			child->hidden = this->hidden || expand_focused;

			if (group->children.size() == 1) {
				child->gap_topleft_offset = gap_topleft_offset;
				child->gap_bottomright_offset = gap_bottomright_offset;
				child->size.x = tsize.x;
				if (this->parent != nullptr) child->gap_bottomright_offset.x += *group_inset;
			} else if (child == group->children.front()) {
				child->gap_topleft_offset = gap_topleft_offset;
				child->gap_bottomright_offset = Vector2D(0, gap_bottomright_offset.y);
				child->size.x += gap_topleft_offset.x;
				offset += gap_topleft_offset.x;
			} else if (child == group->children.back()) {
				child->gap_topleft_offset = Vector2D(0, gap_topleft_offset.y);
				child->gap_bottomright_offset = gap_bottomright_offset;
				child->size.x += gap_bottomright_offset.x;
			} else {
				child->gap_topleft_offset = Vector2D(0, gap_topleft_offset.y);
				child->gap_bottomright_offset = Vector2D(0, gap_bottomright_offset.y);
			}

			child->recalcSizePosRecursive(no_animation);
			break;
		case Hy3GroupLayout::SplitV:
			child->position.y = tpos.y + offset;
			child->size.y = child->size_ratio * ratio_mul;
			offset += child->size.y;
			child->position.x = tpos.x;
			child->size.x = tsize.x;
			child->hidden = this->hidden || expand_focused;

			if (group->children.size() == 1) {
				child->gap_topleft_offset = gap_topleft_offset;
				child->gap_bottomright_offset = gap_bottomright_offset;
				child->size.y = tsize.y;
				if (this->parent != nullptr) child->gap_bottomright_offset.y += *group_inset;
			} else if (child == group->children.front()) {
				child->gap_topleft_offset = gap_topleft_offset;
				child->gap_bottomright_offset = Vector2D(gap_bottomright_offset.x, 0);
				child->size.y += gap_topleft_offset.y;
				offset += gap_topleft_offset.y;
			} else if (child == group->children.back()) {
				child->gap_topleft_offset = Vector2D(gap_topleft_offset.x, 0);
				child->gap_bottomright_offset = gap_bottomright_offset;
				child->size.y += gap_bottomright_offset.y;
			} else {
				child->gap_topleft_offset = Vector2D(gap_topleft_offset.x, 0);
				child->gap_bottomright_offset = Vector2D(gap_bottomright_offset.x, 0);
			}

			child->recalcSizePosRecursive(no_animation);
			break;
		case Hy3GroupLayout::Tabbed:
			child->position = tpos;
			child->size = tsize;
			child->hidden = this->hidden || expand_focused || group->focused_child != child;

			child->gap_topleft_offset =
			    Vector2D(gap_topleft_offset.x, gap_topleft_offset.y + tab_height_offset);
			child->gap_bottomright_offset = gap_bottomright_offset;

			child->recalcSizePosRecursive(no_animation);
			break;
		}
	}

	this->updateTabBar(no_animation);
}

struct FindTopWindowInNodeResult {
	CWindow* window = nullptr;
	size_t index = 0;
};

void findTopWindowInNode(Hy3Node& node, FindTopWindowInNodeResult& result) {
	switch (node.data.type) {
	case Hy3NodeType::Window: {
		auto* window = node.data.as_window;
		auto& windows = g_pCompositor->m_vWindows;

		for (; result.index < windows.size(); result.index++) {
			if (&*windows[result.index] == window) {
				result.window = window;
				break;
			}
		}

	} break;
	case Hy3NodeType::Group: {
		auto& group = node.data.as_group;

		if (group.layout == Hy3GroupLayout::Tabbed) {
			if (group.focused_child != nullptr) findTopWindowInNode(*group.focused_child, result);
		} else {
			for (auto* child: group.children) {
				findTopWindowInNode(*child, result);
			}
		}
	} break;
	}
}

void Hy3Node::updateTabBar(bool no_animation) {
	if (this->data.type == Hy3NodeType::Group) {
		auto& group = this->data.as_group;

		if (group.layout == Hy3GroupLayout::Tabbed) {
			if (group.tab_bar == nullptr) group.tab_bar = &this->layout->tab_groups.emplace_back(*this);
			group.tab_bar->updateWithGroup(*this, no_animation);

			FindTopWindowInNodeResult result;
			findTopWindowInNode(*this, result);
			group.tab_bar->target_window = result.window;
			if (result.window != nullptr) group.tab_bar->workspace = result.window->m_pWorkspace;
		} else if (group.tab_bar != nullptr) {
			group.tab_bar->bar.beginDestroy();
			group.tab_bar = nullptr;
		}
	}
}

void Hy3Node::updateTabBarRecursive() {
	auto* node = this;

	do {
		node->updateTabBar();
		node = node->parent;
	} while (node != nullptr);
}

void Hy3Node::updateDecos() {
	switch (this->data.type) {
	case Hy3NodeType::Window:
		if (this->data.as_window->m_bIsMapped)
			g_pCompositor->updateWindowAnimatedDecorationValues(this->data.as_window);
		break;
	case Hy3NodeType::Group:
		for (auto* child: this->data.as_group.children) {
			child->updateDecos();
		}

		this->updateTabBar();
	}
}

std::string Hy3Node::getTitle() {
	switch (this->data.type) {
	case Hy3NodeType::Window: return this->data.as_window->m_szTitle;
	case Hy3NodeType::Group:
		std::string title;

		switch (this->data.as_group.layout) {
		case Hy3GroupLayout::SplitH: title = "[H] "; break;
		case Hy3GroupLayout::SplitV: title = "[V] "; break;
		case Hy3GroupLayout::Tabbed: title = "[T] "; break;
		}

		if (this->data.as_group.focused_child == nullptr) {
			title += "Group";
		} else {
			title += this->data.as_group.focused_child->getTitle();
		}

		return title;
	}

	return "";
}

bool Hy3Node::isUrgent() {
	switch (this->data.type) {
	case Hy3NodeType::Window: return this->data.as_window->m_bIsUrgent;
	case Hy3NodeType::Group:
		for (auto* child: this->data.as_group.children) {
			if (child->isUrgent()) return true;
		}

		return false;
	default: return false;
	}
}

void Hy3Node::setHidden(bool hidden) {
	this->hidden = hidden;

	if (this->data.type == Hy3NodeType::Group) {
		for (auto* child: this->data.as_group.children) {
			child->setHidden(hidden);
		}
	}
}

Hy3Node* Hy3Node::findNodeForTabGroup(Hy3TabGroup& tab_group) {
	if (this->data.type == Hy3NodeType::Group) {
		if (this->hidden) return nullptr;

		auto& group = this->data.as_group;

		if (group.layout == Hy3GroupLayout::Tabbed && group.tab_bar == &tab_group) {
			return this;
		}

		for (auto& node: group.children) {
			auto* r = node->findNodeForTabGroup(tab_group);
			if (r != nullptr) return r;
		}
	} else return nullptr;

	return nullptr;
}

void Hy3Node::appendAllWindows(std::vector<CWindow*>& list) {
	switch (this->data.type) {
	case Hy3NodeType::Window: list.push_back(this->data.as_window); break;
	case Hy3NodeType::Group:
		for (auto* child: this->data.as_group.children) {
			child->appendAllWindows(list);
		}
		break;
	}
}

std::string Hy3Node::debugNode() {
	std::stringstream buf;
	std::string addr = "0x" + std::to_string((size_t) this);
	switch (this->data.type) {
	case Hy3NodeType::Window:
		buf << "window(";
		buf << std::hex << this;
		buf << ") [hypr ";
		buf << this->data.as_window;
		buf << "] size ratio: ";
		buf << this->size_ratio;
		break;
	case Hy3NodeType::Group:
		buf << "group(";
		buf << std::hex << this;
		buf << ") [";

		switch (this->data.as_group.layout) {
		case Hy3GroupLayout::SplitH: buf << "splith"; break;
		case Hy3GroupLayout::SplitV: buf << "splitv"; break;
		case Hy3GroupLayout::Tabbed: buf << "tabs"; break;
		}

		buf << "] size ratio: ";
		buf << this->size_ratio;

		if (this->data.as_group.expand_focused != ExpandFocusType::NotExpanded) {
			buf << ", has-expanded";
		}

		if (this->data.as_group.ephemeral) {
			buf << ", ephemeral";
		}

		if (this->data.as_group.containment) {
			buf << ", containment";
		}

		for (auto* child: this->data.as_group.children) {
			buf << "\n|-";
			if (child == nullptr) {
				buf << "nullptr";
			} else {
				// this is terrible
				for (char c: child->debugNode()) {
					buf << c;
					if (c == '\n') buf << "  ";
				}
			}
		}

		break;
	}

	return buf.str();
}

Hy3Node* Hy3Node::removeFromParentRecursive(Hy3Node** expand_actor) {
	Hy3Node* parent = this;

	hy3_log(TRACE, "removing parent nodes of {:x} recursively", (uintptr_t) parent);

	if (this->parent != nullptr) {
		auto& actor = this->getExpandActor();
		if (actor.data.type == Hy3NodeType::Group) {
			actor.data.as_group.collapseExpansions();
			if (expand_actor != nullptr) *expand_actor = &actor;
		}
	}

	while (parent != nullptr) {
		if (parent->parent == nullptr) {
			if (parent != this) parent->layout->nodes.remove(*parent);
			return nullptr;
		}

		auto* child = parent;
		parent = parent->parent;
		auto& group = parent->data.as_group;

		if (group.children.size() > 2) {
			auto iter = std::find(group.children.begin(), group.children.end(), child);

			group.group_focused = false;
			if (iter == group.children.begin()) {
				group.focused_child = *std::next(iter);
			} else {
				group.focused_child = *std::prev(iter);
			}
		}

		if (!group.children.remove(child)) {
			hy3_log(
			    ERR,
			    "unable to remove child node {:x} from parent node {:x}, child's parent pointer is "
			    "likely dangling",
			    (uintptr_t) child,
			    (uintptr_t) parent
			);

			errorNotif();
			return nullptr;
		}

		group.group_focused = false;
		if (group.children.size() == 1) {
			group.focused_child = group.children.front();
		}

		auto child_size_ratio = child->size_ratio;
		if (child != this) {
			parent->layout->nodes.remove(*child);
		} else {
			child->parent = nullptr;
		}

		if (!group.children.empty()) {
			auto child_count = group.children.size();
			if (std::find(group.children.begin(), group.children.end(), this) != group.children.end()) {
				child_count -= 1;
			}

			auto splitmod = -((1.0 - child_size_ratio) / child_count);

			for (auto* child: group.children) {
				child->size_ratio += splitmod;
			}

			break;
		}
	}

	this->parent = nullptr;
	return parent;
}

Hy3Node* Hy3Node::intoGroup(Hy3GroupLayout layout, GroupEphemeralityOption ephemeral) {
	this->layout->nodes.push_back({
	    .parent = this,
	    .data = layout,
	    .workspace = this->workspace,
	    .layout = this->layout,
	});

	auto* node = &this->layout->nodes.back();
	swapData(*this, *node);

	this->data = layout;
	this->data.as_group.children.push_back(node);
	this->data.as_group.group_focused = false;
	this->data.as_group.focused_child = node;
	this->data.as_group.ephemeral = ephemeral == GroupEphemeralityOption::Ephemeral
	                             || ephemeral == GroupEphemeralityOption::ForceEphemeral;
	this->recalcSizePosRecursive();
	this->updateTabBarRecursive();

	return node;
}

bool Hy3Node::swallowGroups(Hy3Node* into) {
	if (into == nullptr || into->data.type != Hy3NodeType::Group
	    || into->data.as_group.children.size() != 1)
		return false;

	auto* child = into->data.as_group.children.front();

	// a lot of segfaulting happens once the assumption that the root node is a
	// group is wrong.
	if (into->parent == nullptr && child->data.type != Hy3NodeType::Group) return false;

	hy3_log(TRACE, "swallowing node {:x} into node {:x}", (uintptr_t) child, (uintptr_t) into);

	Hy3Node::swapData(*into, *child);
	into->layout->nodes.remove(*child);

	return true;
}

Hy3Node* getOuterChild(Hy3GroupData& group, ShiftDirection direction) {
	switch (direction) {
	case ShiftDirection::Left:
	case ShiftDirection::Up: return group.children.front(); break;
	case ShiftDirection::Right:
	case ShiftDirection::Down: return group.children.back(); break;
	default: return nullptr;
	}
}

Hy3Node* Hy3Node::getImmediateSibling(ShiftDirection direction) {
	const auto& group = this->parent->data.as_group;

	auto iter = std::find(group.children.begin(), group.children.end(), this);

	std::__cxx11::list<Hy3Node*>::const_iterator list_sibling;

	switch (direction) {
	case ShiftDirection::Left:
	case ShiftDirection::Up: list_sibling = std::prev(iter); break;
	case ShiftDirection::Right:
	case ShiftDirection::Down: list_sibling = std::next(iter); break;
	default: list_sibling = iter;
	}

	if (list_sibling == group.children.end()) {
		hy3_log(WARN, "getImmediateSibling: sibling not found");
		list_sibling = iter;
	}

	return *list_sibling;
}

Hy3Node* Hy3Node::findNeighbor(ShiftDirection direction) {
	auto current_node = this;
	Hy3Node* sibling = nullptr;

	while (sibling == nullptr && current_node->parent != nullptr) {
		auto& parent_group = current_node->parent->data.as_group;

		if (parent_group.layout != Hy3GroupLayout::Tabbed
		    && getAxis(parent_group.layout) == getAxis(direction))
		{
			// If the current node is the outermost child of its parent group then proceed
			// then we need to look at the parent - otherwise, the sibling is simply the immediate
			// sibling in the child collection
			if (getOuterChild(parent_group, direction) != current_node) {
				sibling = current_node->getImmediateSibling(direction);
			}
		}

		current_node = current_node->parent;
	}

	return sibling;
}

int directionToIteratorIncrement(ShiftDirection direction) {
	switch (direction) {
	case ShiftDirection::Left:
	case ShiftDirection::Up: return -1;
	case ShiftDirection::Right:
	case ShiftDirection::Down: return 1;
	default: hy3_log(WARN, "Unknown ShiftDirection enum value: {}", (int) direction); return 1;
	}
}

Vector2D Hy3Node::middle() { return this->position + this->size / 2.f; }

void Hy3Node::resize(ShiftDirection direction, double delta, bool no_animation) {
	auto& parent_node = this->parent;
	auto& containing_group = parent_node->data.as_group;

	if (containing_group.layout != Hy3GroupLayout::Tabbed
	    && getAxis(direction) == getAxis(containing_group.layout))
	{
		double parent_size =
		    getAxis(direction) == Axis::Horizontal ? parent_node->size.x : parent_node->size.y;
		auto ratio_mod = delta * (float) containing_group.children.size() / parent_size;

		const auto end_of_children = containing_group.children.end();
		auto iter = std::find(containing_group.children.begin(), end_of_children, this);

		if (iter != end_of_children) {
			const auto outermost_node_in_group = getOuterChild(containing_group, direction);
			if (this != outermost_node_in_group) {
				auto inc = directionToIteratorIncrement(direction);
				iter = std::next(iter, inc);
				ratio_mod *= inc;
			}

			if (iter != end_of_children) {
				auto* neighbor = *iter;
				auto requested_size_ratio = this->size_ratio + ratio_mod;
				auto requested_neighbor_size_ratio = neighbor->size_ratio - ratio_mod;

				if (requested_size_ratio >= MIN_RATIO && requested_neighbor_size_ratio >= MIN_RATIO) {
					this->size_ratio = requested_size_ratio;
					neighbor->size_ratio = requested_neighbor_size_ratio;

					parent_node->recalcSizePosRecursive(no_animation);
				} else {
					hy3_log(
					    WARN,
					    "Requested size ratio {} or {} out of bounds, ignoring",
					    requested_size_ratio,
					    requested_neighbor_size_ratio
					);
				}
			}
		}
	}
}

void Hy3Node::swapData(Hy3Node& a, Hy3Node& b) {
	Hy3NodeData aData = std::move(a.data);
	a.data = std::move(b.data);
	b.data = std::move(aData);

	if (a.data.type == Hy3NodeType::Group) {
		for (auto child: a.data.as_group.children) {
			child->parent = &a;
		}
	}

	if (b.data.type == Hy3NodeType::Group) {
		for (auto child: b.data.as_group.children) {
			child->parent = &b;
		}
	}
}

bool Hy3Node::hasChild(Hy3Node* node) {
	if (this->data.type == Hy3NodeType::Window) return false;

	auto n = node;
	while (n != nullptr && n->parent != this) n = n->parent;

	return n != nullptr;
}

Hy3Node* Hy3Node::getRoot() {
	Hy3Node* maybeRoot = this;
	while (maybeRoot->parent) maybeRoot = maybeRoot->parent;

	return maybeRoot;
}