#include <functional>
#include <numeric>
#include <regex>
#include <set>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <ranges>

#include "BitFlag.hpp"
#include "Hy3Layout.hpp"
#include "SelectionHook.hpp"
#include "conversions.hpp"
#include "globals.hpp"

std::unique_ptr<HOOK_CALLBACK_FN> renderHookPtr =
    std::make_unique<HOOK_CALLBACK_FN>(Hy3Layout::renderHook);
std::unique_ptr<HOOK_CALLBACK_FN> windowTitleHookPtr =
    std::make_unique<HOOK_CALLBACK_FN>(Hy3Layout::windowGroupUpdateRecursiveHook);
std::unique_ptr<HOOK_CALLBACK_FN> urgentHookPtr =
    std::make_unique<HOOK_CALLBACK_FN>(Hy3Layout::windowGroupUrgentHook);
std::unique_ptr<HOOK_CALLBACK_FN> tickHookPtr =
    std::make_unique<HOOK_CALLBACK_FN>(Hy3Layout::tickHook);

bool performContainment(Hy3Node& node, bool contained, CWindow* window) {
	if (node.data.type == Hy3NodeType::Group) {
		auto& group = node.data.as_group;
		contained |= group.containment;

		auto iter = node.data.as_group.children.begin();
		while (iter != node.data.as_group.children.end()) {
			switch ((*iter)->data.type) {
			case Hy3NodeType::Group: return performContainment(**iter, contained, window);
			case Hy3NodeType::Window:
				if (contained) {
					auto wpid = (*iter)->data.as_window->getPID();
					auto ppid = getPPIDof(window->getPID());
					while (ppid > 10) { // `> 10` yoinked from HL swallow
						if (ppid == wpid) {
							node.layout->nodes.push_back({
							    .parent = &node,
							    .data = window,
							    .workspace = node.workspace,
							    .layout = node.layout,
							});

							auto& child_node = node.layout->nodes.back();

							group.children.insert(std::next(iter), &child_node);
							child_node.markFocused();
							node.recalcSizePosRecursive();

							return true;
						}

						ppid = getPPIDof(ppid);
					}
				}
			}

			iter = std::next(iter);
		}
	}

	return false;
}

void Hy3Layout::onWindowCreated(CWindow* window, eDirection direction) {
	for (auto& node: this->nodes) {
		if (node.parent == nullptr && performContainment(node, false, window)) {
			return;
		}
	}

	IHyprLayout::onWindowCreated(window, direction);
}

void Hy3Layout::onWindowCreatedTiling(CWindow* window, eDirection) {
	hy3_log(
	    LOG,
	    "onWindowCreatedTiling called with window {:x} (floating: {}, monitor: {}, workspace: {})",
	    (uintptr_t) window,
	    window->m_bIsFloating,
	    window->m_iMonitorID,
	    window->m_pWorkspace->m_iID
	);

	if (window->m_bIsFloating) return;

	auto* existing = this->getNodeFromWindow(window);
	if (existing != nullptr) {
		hy3_log(
		    ERR,
		    "onWindowCreatedTiling called with a window ({:x}) that is already tiled (node: {:x})",
		    (uintptr_t) window,
		    (uintptr_t) existing
		);
		return;
	}

	this->nodes.push_back({
	    .parent = nullptr,
	    .data = window,
	    .workspace = window->m_pWorkspace,
	    .layout = this,
	});

	this->insertNode(this->nodes.back());
}

void Hy3Layout::insertNode(Hy3Node& node) {
	if (node.parent != nullptr) {
		hy3_log(
		    ERR,
		    "insertNode called for node {:x} which already has a parent ({:x})",
		    (uintptr_t) &node,
		    (uintptr_t) node.parent
		);
		return;
	}

	if (!valid(node.workspace)) {
		hy3_log(
		    ERR,
		    "insertNode called for node {:x} with invalid workspace id {}",
		    (uintptr_t) &node,
		    node.workspace->m_iID
		);
		return;
	}

	node.reparenting = true;

	auto* monitor = g_pCompositor->getMonitorFromID(node.workspace->m_iMonitorID);

	Hy3Node* opening_into;
	Hy3Node* opening_after = nullptr;

	auto* root = this->getWorkspaceRootGroup(node.workspace);

	if (root != nullptr) {
		opening_after = root->getFocusedNode();

		// opening_after->parent cannot be nullptr
		if (opening_after == root) {
			opening_after =
			    opening_after->intoGroup(Hy3GroupLayout::SplitH, GroupEphemeralityOption::Standard);
		}
	}

	if (opening_after == nullptr) {
		if (g_pCompositor->m_pLastWindow != nullptr
		    && g_pCompositor->m_pLastWindow->m_pWorkspace == node.workspace
		    && !g_pCompositor->m_pLastWindow->m_bIsFloating
		    && (node.data.type == Hy3NodeType::Window
		        || g_pCompositor->m_pLastWindow != node.data.as_window)
		    && g_pCompositor->m_pLastWindow->m_bIsMapped)
		{
			opening_after = this->getNodeFromWindow(g_pCompositor->m_pLastWindow);
		} else {
			auto* mouse_window = g_pCompositor->vectorToWindowUnified(
			    g_pInputManager->getMouseCoordsInternal(),
			    RESERVED_EXTENTS | INPUT_EXTENTS
			);

			if (mouse_window != nullptr && mouse_window->m_pWorkspace == node.workspace) {
				opening_after = this->getNodeFromWindow(mouse_window);
			}
		}
	}

	if (opening_after != nullptr
	    && ((node.data.type == Hy3NodeType::Group
	         && (opening_after == &node || node.hasChild(opening_after)))
	        || opening_after->reparenting))
	{
		opening_after = nullptr;
	}

	if (opening_after != nullptr) {
		opening_into = opening_after->parent;
	} else {
		if ((opening_into = this->getWorkspaceRootGroup(node.workspace)) == nullptr) {
			static const auto tab_first_window =
			    ConfigValue<Hyprlang::INT>("plugin:hy3:tab_first_window");

			auto width =
			    monitor->vecSize.x - monitor->vecReservedBottomRight.x - monitor->vecReservedTopLeft.x;
			auto height =
			    monitor->vecSize.y - monitor->vecReservedBottomRight.y - monitor->vecReservedTopLeft.y;

			this->nodes.push_back({
			    .data = height > width ? Hy3GroupLayout::SplitV : Hy3GroupLayout::SplitH,
			    .position = monitor->vecPosition + monitor->vecReservedTopLeft,
			    .size = monitor->vecSize - monitor->vecReservedTopLeft - monitor->vecReservedBottomRight,
			    .workspace = node.workspace,
			    .layout = this,
			});

			if (*tab_first_window) {
				auto& parent = this->nodes.back();

				this->nodes.push_back({
				    .parent = &parent,
				    .data = Hy3GroupLayout::Tabbed,
				    .position = parent.position,
				    .size = parent.size,
				    .workspace = node.workspace,
				    .layout = this,
				});

				parent.data.as_group.children.push_back(&this->nodes.back());
			}

			opening_into = &this->nodes.back();
		}
	}

	if (opening_into->data.type != Hy3NodeType::Group) {
		hy3_log(ERR, "opening_into node ({:x}) was not a group node", (uintptr_t) opening_into);
		errorNotif();
		return;
	}

	if (opening_into->workspace != node.workspace) {
		hy3_log(
		    WARN,
		    "opening_into node ({:x}) is on workspace {} which does not match the new window "
		    "(workspace {})",
		    (uintptr_t) opening_into,
		    opening_into->workspace->m_iID,
		    node.workspace->m_iID
		);
	}

	{
		// clang-format off
		static const auto at_enable = ConfigValue<Hyprlang::INT>("plugin:hy3:autotile:enable");
		static const auto at_ephemeral = ConfigValue<Hyprlang::INT>("plugin:hy3:autotile:ephemeral_groups");
		static const auto at_trigger_width = ConfigValue<Hyprlang::INT>("plugin:hy3:autotile:trigger_width");
		static const auto at_trigger_height = ConfigValue<Hyprlang::INT>("plugin:hy3:autotile:trigger_height");
		// clang-format on

		this->updateAutotileWorkspaces();

		auto& target_group = opening_into->data.as_group;
		if (*at_enable && opening_after != nullptr && target_group.children.size() > 1
		    && target_group.layout != Hy3GroupLayout::Tabbed
		    && this->shouldAutotileWorkspace(opening_into->workspace))
		{
			auto is_horizontal = target_group.layout == Hy3GroupLayout::SplitH;
			auto trigger = is_horizontal ? *at_trigger_width : *at_trigger_height;
			auto target_size = is_horizontal ? opening_into->size.x : opening_into->size.y;
			auto size_after_addition = target_size / (target_group.children.size() + 1);

			if (trigger >= 0 && (trigger == 0 || size_after_addition < trigger)) {
				auto opening_after1 = opening_after->intoGroup(
				    is_horizontal ? Hy3GroupLayout::SplitV : Hy3GroupLayout::SplitH,
				    *at_ephemeral ? GroupEphemeralityOption::Ephemeral : GroupEphemeralityOption::Standard
				);
				opening_into = opening_after;
				opening_after = opening_after1;
			}
		}
	}

	node.parent = opening_into;
	node.reparenting = false;

	if (opening_after == nullptr) {
		opening_into->data.as_group.children.push_back(&node);
	} else {
		auto& children = opening_into->data.as_group.children;
		auto iter = std::find(children.begin(), children.end(), opening_after);
		auto iter2 = std::next(iter);
		children.insert(iter2, &node);
	}

	hy3_log(
	    LOG,
	    "tiled node {:x} inserted after node {:x} in node {:x}",
	    (uintptr_t) &node,
	    (uintptr_t) opening_after,
	    (uintptr_t) opening_into
	);

	node.markFocused();
	opening_into->recalcSizePosRecursive();
}

void Hy3Layout::onWindowRemovedTiling(CWindow* window) {
	this->m_focusIntercepts.erase(window);
	static const auto node_collapse_policy =
	    ConfigValue<Hyprlang::INT>("plugin:hy3:node_collapse_policy");

	auto* node = this->getNodeFromWindow(window);

	if (node == nullptr) return;

	hy3_log(
	    LOG,
	    "removing window ({:x} as node {:x}) from node {:x}",
	    (uintptr_t) window,
	    (uintptr_t) node,
	    (uintptr_t) node->parent
	);

	window->m_sSpecialRenderData.rounding = true;
	window->m_sSpecialRenderData.border = true;
	window->m_sSpecialRenderData.decorate = true;

	if (window->m_bIsFullscreen) {
		g_pCompositor->setWindowFullscreen(window, false, FULLSCREEN_FULL);
	}

	Hy3Node* expand_actor = nullptr;
	auto* parent = node->removeFromParentRecursive(&expand_actor);
	this->nodes.remove(*node);
	if (expand_actor != nullptr) expand_actor->recalcSizePosRecursive();

	auto& group = parent->data.as_group;

	if (parent != nullptr) {
		parent->recalcSizePosRecursive();

		// returns if a given node is a group that can be collapsed given the current config
		auto node_is_collapsible = [](Hy3Node* node) {
			if (node->data.type != Hy3NodeType::Group) return false;
			if (*node_collapse_policy == 0) return true;
			else if (*node_collapse_policy == 1) return false;
			return node->parent->data.as_group.layout != Hy3GroupLayout::Tabbed;
		};

		if (group.children.size() == 1
		    && (group.ephemeral || node_is_collapsible(group.children.front())))
		{
			auto* target_parent = parent;
			while (target_parent != nullptr && Hy3Node::swallowGroups(target_parent)) {
				target_parent = target_parent->parent;
			}

			if (target_parent != parent && target_parent != nullptr)
				target_parent->recalcSizePosRecursive();
		}
	}
}

void Hy3Layout::onWindowRemovedFloating(CWindow* window) { this->m_focusIntercepts.erase(window); }

void Hy3Layout::onWindowFocusChange(CWindow* window) {
	auto* node = this->getNodeFromWindow(window);
	if (node == nullptr) return;

	hy3_log(
	    LOG,
	    "changing window focus to window {:x} as node {:x}",
	    (uintptr_t) window,
	    (uintptr_t) node
	);

	node->markFocused();
	while (node->parent != nullptr) node = node->parent;
	node->recalcSizePosRecursive();
}

bool Hy3Layout::isWindowTiled(CWindow* window) {
	return this->getNodeFromWindow(window) != nullptr;
}

void Hy3Layout::recalculateMonitor(const int& monitor_id) {
	hy3_log(LOG, "recalculating monitor {}", monitor_id);
	const auto monitor = g_pCompositor->getMonitorFromID(monitor_id);
	if (monitor == nullptr) return;

	g_pHyprRenderer->damageMonitor(monitor);

	// todo: refactor this

	auto* top_node = this->getWorkspaceRootGroup(monitor->activeWorkspace);
	if (top_node != nullptr) {
		top_node->position = monitor->vecPosition + monitor->vecReservedTopLeft;
		top_node->size =
		    monitor->vecSize - monitor->vecReservedTopLeft - monitor->vecReservedBottomRight;

		top_node->recalcSizePosRecursive();
	}

	top_node = this->getWorkspaceRootGroup(monitor->activeSpecialWorkspace);

	if (top_node != nullptr) {
		top_node->position = monitor->vecPosition + monitor->vecReservedTopLeft;
		top_node->size =
		    monitor->vecSize - monitor->vecReservedTopLeft - monitor->vecReservedBottomRight;

		top_node->recalcSizePosRecursive();
	}
}

void Hy3Layout::recalculateWindow(CWindow* window) {
	auto* node = this->getNodeFromWindow(window);
	if (node == nullptr) return;
	node->recalcSizePosRecursive();
}

ShiftDirection reverse(ShiftDirection direction) {
	switch (direction) {
	case ShiftDirection::Left: return ShiftDirection::Right;
	case ShiftDirection::Right: return ShiftDirection::Left;
	case ShiftDirection::Up: return ShiftDirection::Down;
	case ShiftDirection::Down: return ShiftDirection::Up;
	default: return direction;
	}
}

void executeResizeOperation(
    const Vector2D& delta,
    eRectCorner corner,
    Hy3Node* node,
    CMonitor* monitor
) {
	if (node == nullptr) return;
	if (monitor == nullptr) return;

	const bool display_left =
	    STICKS(node->position.x, monitor->vecPosition.x + monitor->vecReservedTopLeft.x);
	const bool display_right = STICKS(
	    node->position.x + node->size.x,
	    monitor->vecPosition.x + monitor->vecSize.x - monitor->vecReservedBottomRight.x
	);
	const bool display_top =
	    STICKS(node->position.y, monitor->vecPosition.y + monitor->vecReservedTopLeft.y);
	const bool display_bottom = STICKS(
	    node->position.y + node->size.y,
	    monitor->vecPosition.y + monitor->vecSize.y - monitor->vecReservedBottomRight.y
	);

	Vector2D resize_delta = delta;
	bool node_is_root = (node->data.type == Hy3NodeType::Group && node->parent == nullptr)
	                 || (node->data.type == Hy3NodeType::Window
	                     && (node->parent == nullptr || node->parent->parent == nullptr));

	if (node_is_root) {
		if (display_left && display_right) resize_delta.x = 0;
		if (display_top && display_bottom) resize_delta.y = 0;
	}

	// Don't execute the logic unless there's something to do
	if (resize_delta.x != 0 || resize_delta.y != 0) {
		ShiftDirection target_edge_x;
		ShiftDirection target_edge_y;

		// Determine the direction in which we're going to look for the neighbor node
		// that will be resized
		if (corner == CORNER_NONE) { // It's probably a keyboard event.
			target_edge_x = display_right ? ShiftDirection::Left : ShiftDirection::Right;
			target_edge_y = display_bottom ? ShiftDirection::Up : ShiftDirection::Down;

			// If the anchor is not at the top/left then reverse the delta
			if (target_edge_x == ShiftDirection::Left) resize_delta.x = -resize_delta.x;
			if (target_edge_y == ShiftDirection::Up) resize_delta.y = -resize_delta.y;
		} else { // It's probably a mouse event
			// Resize against the edges corresponding to the selected corner
			target_edge_x = corner == CORNER_TOPLEFT || corner == CORNER_BOTTOMLEFT
			                  ? ShiftDirection::Left
			                  : ShiftDirection::Right;
			target_edge_y = corner == CORNER_TOPLEFT || corner == CORNER_TOPRIGHT ? ShiftDirection::Up
			                                                                      : ShiftDirection::Down;
		}

		// Find the neighboring node in each axis, which will be either above or at the
		// same level as the initiating node in the layout hierarchy.  These are the nodes
		// which must get resized (rather than the initiator) because they are the
		// highest point in the hierarchy
		auto horizontal_neighbor = node->findNeighbor(target_edge_x);
		auto vertical_neighbor = node->findNeighbor(target_edge_y);

		static const auto animate = ConfigValue<Hyprlang::INT>("misc:animate_manual_resizes");

		// Note that the resize direction is reversed, because from the neighbor's perspective
		// the edge to be moved is the opposite way round.  However, the delta is still the same.
		if (horizontal_neighbor) {
			horizontal_neighbor->resize(reverse(target_edge_x), resize_delta.x, *animate == 0);
		}

		if (vertical_neighbor) {
			vertical_neighbor->resize(reverse(target_edge_y), resize_delta.y, *animate == 0);
		}
	}
}

void Hy3Layout::resizeNode(const Vector2D& delta, eRectCorner corner, Hy3Node* node) {
	// Is the intended target really a node or a floating window?
	auto window = g_pCompositor->m_pLastWindow;
	if (window && window->m_bIsFloating) {
		this->resizeActiveWindow(delta, corner, window);
	} else if (node && node->workspace) {
		auto monitor = g_pCompositor->getMonitorFromID(node->workspace->m_iMonitorID);
		executeResizeOperation(delta, corner, node, monitor);
	}
}

void Hy3Layout::resizeActiveWindow(const Vector2D& delta, eRectCorner corner, CWindow* pWindow) {
	auto window = pWindow ? pWindow : g_pCompositor->m_pLastWindow;
	if (window == nullptr || !g_pCompositor->windowValidMapped(window)) return;

	if (window->m_bIsFloating) {
		// Use the same logic as the `main` layout for floating windows
		const auto required_size = Vector2D(
		    std::max((window->m_vRealSize.goal() + delta).x, 20.0),
		    std::max((window->m_vRealSize.goal() + delta).y, 20.0)
		);
		window->m_vRealSize = required_size;
		g_pXWaylandManager->setWindowSize(window, required_size);
	} else if (auto* node = this->getNodeFromWindow(window); node != nullptr) {
		executeResizeOperation(
		    delta,
		    corner,
		    &node->getExpandActor(),
		    g_pCompositor->getMonitorFromID(window->m_iMonitorID)
		);
	}
}

void Hy3Layout::fullscreenRequestForWindow(
    CWindow* window,
    eFullscreenMode fullscreen_mode,
    bool on
) {
	if (!g_pCompositor->windowValidMapped(window)) return;
	if (on == window->m_bIsFullscreen || window->m_pWorkspace->m_bIsSpecialWorkspace) return;

	const auto monitor = g_pCompositor->getMonitorFromID(window->m_iMonitorID);
	if (window->m_pWorkspace->m_bHasFullscreenWindow && on) return;

	window->m_bIsFullscreen = on;
	window->m_pWorkspace->m_bHasFullscreenWindow = !window->m_pWorkspace->m_bHasFullscreenWindow;

	if (!window->m_bIsFullscreen) {
		auto* node = this->getNodeFromWindow(window);

		if (node) {
			// restore node positioning if tiled
			this->applyNodeDataToWindow(node);
		} else {
			// restore floating position if not
			window->m_vRealPosition = window->m_vLastFloatingPosition;
			window->m_vRealSize = window->m_vLastFloatingSize;

			window->m_sSpecialRenderData.rounding = true;
			window->m_sSpecialRenderData.border = true;
			window->m_sSpecialRenderData.decorate = true;
		}
	} else {
		window->m_pWorkspace->m_efFullscreenMode = fullscreen_mode;

		// save position and size if floating
		if (window->m_bIsFloating) {
			window->m_vLastFloatingPosition = window->m_vRealPosition.goal();
			window->m_vPosition = window->m_vRealPosition.goal();
			window->m_vLastFloatingSize = window->m_vRealSize.goal();
			window->m_vSize = window->m_vRealSize.goal();
		}

		if (fullscreen_mode == FULLSCREEN_FULL) {
			window->m_vRealPosition = monitor->vecPosition;
			window->m_vRealSize = monitor->vecSize;
		} else {
			// Copy of vaxry's massive hack

			// clang-format off
			static const auto gaps_in = ConfigValue<Hyprlang::CUSTOMTYPE, CCssGapData>("general:gaps_in");
			static const auto gaps_out = ConfigValue<Hyprlang::CUSTOMTYPE, CCssGapData>("general:gaps_out");
			// clang-format on

			// clang-format off
			auto gap_pos_offset = Vector2D(
			    -(gaps_in->left - gaps_out->left),
			    -(gaps_in->top - gaps_out->top)
			);
			// clang-format on

			auto gap_size_offset = Vector2D(
			    -(gaps_in->left - gaps_out->left) + -(gaps_in->right - gaps_out->right),
			    -(gaps_in->top - gaps_out->top) + -(gaps_in->bottom - gaps_out->bottom)
			);

			Hy3Node fakeNode = {
			    .data = window,
			    .position = monitor->vecPosition + monitor->vecReservedTopLeft,
			    .size = monitor->vecSize - monitor->vecReservedTopLeft - monitor->vecReservedBottomRight,
			    .gap_topleft_offset = gap_pos_offset,
			    .gap_bottomright_offset = gap_size_offset,
			    .workspace = window->m_pWorkspace,
			};

			this->applyNodeDataToWindow(&fakeNode);
		}
	}

	g_pCompositor->updateWindowAnimatedDecorationValues(window);
	g_pXWaylandManager->setWindowSize(window, window->m_vRealSize.goal());
	g_pCompositor->changeWindowZOrder(window, true);
	this->recalculateMonitor(monitor->ID);
}

std::any Hy3Layout::layoutMessage(SLayoutMessageHeader header, std::string content) {
	if (content == "togglesplit") {
		auto* node = this->getNodeFromWindow(header.pWindow);
		if (node != nullptr && node->parent != nullptr) {
			auto& layout = node->parent->data.as_group.layout;

			switch (layout) {
			case Hy3GroupLayout::SplitH:
				layout = Hy3GroupLayout::SplitV;
				node->parent->recalcSizePosRecursive();
				break;
			case Hy3GroupLayout::SplitV:
				layout = Hy3GroupLayout::SplitH;
				node->parent->recalcSizePosRecursive();
				break;
			case Hy3GroupLayout::Tabbed: break;
			}
		}
	}

	return "";
}

SWindowRenderLayoutHints Hy3Layout::requestRenderHints(CWindow* window) { return {}; }

void Hy3Layout::switchWindows(CWindow* pWindowA, CWindow* pWindowB) {
	// todo
}

void Hy3Layout::moveWindowTo(CWindow* window, const std::string& direction) {
	auto* node = this->getNodeFromWindow(window);
	if (node == nullptr) {
		const auto neighbor = g_pCompositor->getWindowInDirection(window, direction[0]);

		if (window->workspaceID() != neighbor->workspaceID()) {
			// if different monitors, send to monitor
			onWindowRemovedTiling(window);
			window->moveToWorkspace(neighbor->m_pWorkspace);
			window->m_iMonitorID = neighbor->m_iMonitorID;
			onWindowCreatedTiling(window);
		}
	} else {
		ShiftDirection shift;
		if (direction == "l") shift = ShiftDirection::Left;
		else if (direction == "r") shift = ShiftDirection::Right;
		else if (direction == "u") shift = ShiftDirection::Up;
		else if (direction == "d") shift = ShiftDirection::Down;
		else return;

		this->shiftNode(*node, shift, false, false);
	}
}

void Hy3Layout::alterSplitRatio(CWindow* pWindow, float delta, bool exact) {
	// todo
}

std::string Hy3Layout::getLayoutName() { return "hy3"; }

CWindow* Hy3Layout::getNextWindowCandidate(CWindow* window) {
	if (window->m_pWorkspace->m_bHasFullscreenWindow) {
		return g_pCompositor->getFullscreenWindowOnWorkspace(window->m_pWorkspace->m_iID);
	}

	// return the first floating window on the same workspace that has not asked not to be focused
	if (window->m_bIsFloating) {
		for (auto& w: g_pCompositor->m_vWindows | std::views::reverse) {
			if (w->m_bIsMapped && !w->isHidden() && w->m_bIsFloating && w->m_iX11Type != 2
			    && w->m_pWorkspace == window->m_pWorkspace && !w->m_bX11ShouldntFocus
			    && !w->m_sAdditionalConfigData.noFocus && w.get() != window)
			{
				return w.get();
			}
		}
	}

	auto* node = this->getWorkspaceFocusedNode(window->m_pWorkspace, true);
	if (node == nullptr) return nullptr;

	switch (node->data.type) {
	case Hy3NodeType::Window: return node->data.as_window;
	case Hy3NodeType::Group: return nullptr;
	default: return nullptr;
	}
}

void Hy3Layout::replaceWindowDataWith(CWindow* from, CWindow* to) {
	auto* node = this->getNodeFromWindow(from);
	if (node == nullptr) return;

	node->data.as_window = to;
	this->applyNodeDataToWindow(node);
}

bool Hy3Layout::isWindowReachable(CWindow* window) {
	return this->getNodeFromWindow(window) != nullptr || IHyprLayout::isWindowReachable(window);
}

void Hy3Layout::bringWindowToTop(CWindow* window) {
	auto node = this->getNodeFromWindow(window);
	if (node == nullptr) return;
	node->bringToTop();
}

void Hy3Layout::onEnable() {
	for (auto& window: g_pCompositor->m_vWindows) {
		if (window->isHidden() || !window->m_bIsMapped || window->m_bFadingOut || window->m_bIsFloating)
			continue;

		this->onWindowCreatedTiling(window.get());
	}

	HyprlandAPI::registerCallbackStatic(PHANDLE, "render", renderHookPtr.get());
	HyprlandAPI::registerCallbackStatic(PHANDLE, "windowTitle", windowTitleHookPtr.get());
	HyprlandAPI::registerCallbackStatic(PHANDLE, "urgent", urgentHookPtr.get());
	HyprlandAPI::registerCallbackStatic(PHANDLE, "tick", tickHookPtr.get());
	selection_hook::enable();
}

void Hy3Layout::onDisable() {
	HyprlandAPI::unregisterCallback(PHANDLE, renderHookPtr.get());
	HyprlandAPI::unregisterCallback(PHANDLE, windowTitleHookPtr.get());
	HyprlandAPI::unregisterCallback(PHANDLE, urgentHookPtr.get());
	HyprlandAPI::unregisterCallback(PHANDLE, tickHookPtr.get());
	selection_hook::disable();

	for (auto& node: this->nodes) {
		if (node.data.type == Hy3NodeType::Window) {
			node.data.as_window->setHidden(false);
		}
	}

	this->nodes.clear();
}

void Hy3Layout::makeGroupOnWorkspace(
    const PHLWORKSPACE& workspace,
    Hy3GroupLayout layout,
    GroupEphemeralityOption ephemeral
) {
	auto* node = this->getWorkspaceFocusedNode(workspace);
	this->makeGroupOn(node, layout, ephemeral);
}

void Hy3Layout::makeOppositeGroupOnWorkspace(
    const PHLWORKSPACE& workspace,
    GroupEphemeralityOption ephemeral
) {
	auto* node = this->getWorkspaceFocusedNode(workspace);
	this->makeOppositeGroupOn(node, ephemeral);
}

void Hy3Layout::changeGroupOnWorkspace(const PHLWORKSPACE& workspace, Hy3GroupLayout layout) {
	auto* node = this->getWorkspaceFocusedNode(workspace);
	if (node == nullptr) return;

	this->changeGroupOn(*node, layout);
}

void Hy3Layout::untabGroupOnWorkspace(const PHLWORKSPACE& workspace) {
	auto* node = this->getWorkspaceFocusedNode(workspace);
	if (node == nullptr) return;

	this->untabGroupOn(*node);
}

void Hy3Layout::toggleTabGroupOnWorkspace(const PHLWORKSPACE& workspace) {
	auto* node = this->getWorkspaceFocusedNode(workspace);
	if (node == nullptr) return;

	this->toggleTabGroupOn(*node);
}

void Hy3Layout::changeGroupToOppositeOnWorkspace(const PHLWORKSPACE& workspace) {
	auto* node = this->getWorkspaceFocusedNode(workspace);
	if (node == nullptr) return;

	this->changeGroupToOppositeOn(*node);
}

void Hy3Layout::changeGroupEphemeralityOnWorkspace(const PHLWORKSPACE& workspace, bool ephemeral) {
	auto* node = this->getWorkspaceFocusedNode(workspace);
	if (node == nullptr) return;

	this->changeGroupEphemeralityOn(*node, ephemeral);
}

void Hy3Layout::makeGroupOn(
    Hy3Node* node,
    Hy3GroupLayout layout,
    GroupEphemeralityOption ephemeral
) {
	if (node == nullptr) return;

	if (node->parent != nullptr) {
		auto& group = node->parent->data.as_group;
		if (group.children.size() == 1) {
			group.setLayout(layout);
			group.setEphemeral(ephemeral);
			node->parent->updateTabBarRecursive();
			node->parent->recalcSizePosRecursive();
			return;
		}
	}

	node->intoGroup(layout, ephemeral);
}

void Hy3Layout::makeOppositeGroupOn(Hy3Node* node, GroupEphemeralityOption ephemeral) {
	if (node == nullptr) return;

	if (node->parent == nullptr) {
		node->intoGroup(Hy3GroupLayout::SplitH, ephemeral);
		return;
	}

	auto& group = node->parent->data.as_group;
	auto layout =
	    group.layout == Hy3GroupLayout::SplitH ? Hy3GroupLayout::SplitV : Hy3GroupLayout::SplitH;

	if (group.children.size() == 1) {
		group.setLayout(layout);
		group.setEphemeral(ephemeral);
		node->parent->recalcSizePosRecursive();
		return;
	}

	node->intoGroup(layout, ephemeral);
}

void Hy3Layout::changeGroupOn(Hy3Node& node, Hy3GroupLayout layout) {
	if (node.parent == nullptr) {
		makeGroupOn(&node, layout, GroupEphemeralityOption::Ephemeral);
		return;
	}

	auto& group = node.parent->data.as_group;
	group.setLayout(layout);
	node.parent->updateTabBarRecursive();
	node.parent->recalcSizePosRecursive();
}

void Hy3Layout::untabGroupOn(Hy3Node& node) {
	if (node.parent == nullptr) return;

	auto& group = node.parent->data.as_group;
	if (group.layout != Hy3GroupLayout::Tabbed) return;

	changeGroupOn(node, group.previous_nontab_layout);
}

void Hy3Layout::toggleTabGroupOn(Hy3Node& node) {
	if (node.parent == nullptr) return;

	auto& group = node.parent->data.as_group;
	if (group.layout != Hy3GroupLayout::Tabbed) changeGroupOn(node, Hy3GroupLayout::Tabbed);
	else changeGroupOn(node, group.previous_nontab_layout);
}

void Hy3Layout::changeGroupToOppositeOn(Hy3Node& node) {
	if (node.parent == nullptr) return;

	auto& group = node.parent->data.as_group;

	if (group.layout == Hy3GroupLayout::Tabbed) {
		group.setLayout(group.previous_nontab_layout);
	} else {
		group.setLayout(
		    group.layout == Hy3GroupLayout::SplitH ? Hy3GroupLayout::SplitV : Hy3GroupLayout::SplitH
		);
	}

	node.parent->recalcSizePosRecursive();
}

void Hy3Layout::changeGroupEphemeralityOn(Hy3Node& node, bool ephemeral) {
	if (node.parent == nullptr) return;

	auto& group = node.parent->data.as_group;
	group.setEphemeral(
	    ephemeral ? GroupEphemeralityOption::ForceEphemeral : GroupEphemeralityOption::Standard
	);
}

void Hy3Layout::shiftNode(Hy3Node& node, ShiftDirection direction, bool once, bool visible) {
	if (once && node.parent != nullptr && node.parent->data.as_group.children.size() == 1) {
		if (node.parent->parent == nullptr) {
			node.parent->data.as_group.setLayout(Hy3GroupLayout::SplitH);
			node.parent->recalcSizePosRecursive();
		} else {
			auto* node2 = node.parent;
			Hy3Node::swapData(node, *node2);
			node2->layout->nodes.remove(node);
			node2->updateTabBarRecursive();
			node2->recalcSizePosRecursive();
		}
	} else {
		this->shiftOrGetFocus(node, direction, true, once, visible);
	}
}

void shiftFloatingWindow(CWindow* window, ShiftDirection direction) {
	static const auto kbd_shift_delta = ConfigValue<Hyprlang::INT>("plugin:hy3:kbd_shift_delta");

	if (!window) return;

	Vector2D bounds {0, 0};
	// BUG:  Assumes horizontal monitor layout
	// BUG:  Ignores monitor reserved space
	for (auto m: g_pCompositor->m_vMonitors) {
		bounds.x = std::max(bounds.x, m->vecPosition.x + m->vecSize.x);
		if (m->ID == window->m_iMonitorID) {
			bounds.y = m->vecPosition.y + m->vecSize.y;
		}
	}

	const int delta = getSearchDirection(direction) == SearchDirection::Forwards ? *kbd_shift_delta
	                                                                             : -*kbd_shift_delta;

	Vector2D movement_delta =
	    (getAxis(direction) == Axis::Horizontal) ? Vector2D {delta, 0} : Vector2D {0, delta};

	auto window_pos = window->m_vRealPosition.value();
	auto window_size = window->m_vRealSize.value();

	// Keep at least `delta` pixels visible
	if (window_pos.x + window_size.x + delta < 0 || window_pos.x + delta > bounds.x)
		movement_delta.x = 0;
	if (window_pos.y + window_size.y + delta < 0 || window_pos.y + delta > bounds.y)
		movement_delta.y = 0;
	if (movement_delta.x != 0 || movement_delta.y != 0) {
		auto new_pos = window_pos + movement_delta;
		// Do we need to change the workspace?
		const auto new_monitor = g_pCompositor->getMonitorFromVector(new_pos);
		if (new_monitor && new_monitor->ID != window->m_iMonitorID) {
			// Ignore the movement request if the new workspace is special
			if (!new_monitor->activeSpecialWorkspace) {
				const auto old_workspace = window->m_pWorkspace;
				const auto new_workspace = new_monitor->activeWorkspace;
				const auto previous_monitor = g_pCompositor->getMonitorFromID(window->m_iMonitorID);
				const auto original_new_pos = new_pos;

				if (new_workspace && previous_monitor) {
					switch (direction) {
					case ShiftDirection::Left: new_pos.x += new_monitor->vecSize.x; break;
					case ShiftDirection::Right: new_pos.x -= previous_monitor->vecSize.x; break;
					case ShiftDirection::Up: new_pos.y += new_monitor->vecSize.y; break;
					case ShiftDirection::Down: new_pos.y -= previous_monitor->vecSize.y; break;
					default: UNREACHABLE();
					}
				}

				window->m_vRealPosition = new_pos;
				g_pCompositor->moveWindowToWorkspaceSafe(window, new_workspace);
				g_pCompositor->setActiveMonitor(new_monitor);

				const static auto allow_workspace_cycles =
				    ConfigValue<Hyprlang::INT>("binds:allow_workspace_cycles");
				if (*allow_workspace_cycles) new_workspace->rememberPrevWorkspace(old_workspace);
			}
		} else {
			window->m_vRealPosition = new_pos;
		}
	}
}

void Hy3Layout::shiftWindow(const PHLWORKSPACE &workspace, ShiftDirection direction, bool once, bool visible) {
	auto focused_window = g_pCompositor->m_pLastWindow;
	auto* node = getWorkspaceFocusedNode(workspace);

	if (focused_window && focused_window->m_bIsFloating) {
		shiftFloatingWindow(focused_window, direction);
	} else if (node) {
		shiftNode(*node, direction, once, visible);
	}
}

void Hy3Layout::focusMonitor(CMonitor* monitor) {
	if (monitor == nullptr) return;

	g_pCompositor->setActiveMonitor(monitor);
	const auto focusedNode = this->getWorkspaceFocusedNode(monitor->activeWorkspace);
	if (focusedNode != nullptr) {
		focusedNode->focus();
	} else {
		auto workspace = monitor->activeWorkspace;
		CWindow* next_window = nullptr;
		if (workspace != nullptr) {
			workspace->setActive(true);
			if (workspace->m_bHasFullscreenWindow) {
				next_window = g_pCompositor->getFullscreenWindowOnWorkspace(workspace->m_iID);
			} else {
				next_window = workspace->getLastFocusedWindow();
			}
		} else {
			for (auto& w: g_pCompositor->m_vWindows | std::views::reverse) {
				if (w->m_bIsMapped && !w->isHidden() && w->m_bIsFloating && w->m_iX11Type != 2
				    && w->m_pWorkspace == next_window->m_pWorkspace && !w->m_bX11ShouldntFocus
				    && !w->m_sAdditionalConfigData.noFocus)
				{
					next_window = w.get();
					break;
				}
			}
		}
		g_pCompositor->focusWindow(next_window);
	}
}

CWindow* getFocusedWindow(const Hy3Node* node) {
	auto search = node;
	while (search != nullptr && search->data.type == Hy3NodeType::Group) {
		search = search->data.as_group.focused_child;
	}

	if (search == nullptr || search->data.type != Hy3NodeType::Window) {
		return nullptr;
	}

	return search->data.as_window;
}

bool shiftIsForward(ShiftDirection direction) {
	return direction == ShiftDirection::Right || direction == ShiftDirection::Down;
}

bool shiftIsVertical(ShiftDirection direction) {
	return direction == ShiftDirection::Up || direction == ShiftDirection::Down;
}

bool shiftMatchesLayout(Hy3GroupLayout layout, ShiftDirection direction) {
	return (layout == Hy3GroupLayout::SplitV && shiftIsVertical(direction))
	    || (layout != Hy3GroupLayout::SplitV && !shiftIsVertical(direction));
}

bool covers(const CBox& outer, const CBox& inner) {
	return outer.x <= inner.x && outer.y <= inner.y && outer.x + outer.w >= inner.x + inner.w
	    && outer.y + outer.h >= inner.y + inner.h;
}

bool isObscured(CWindow* window) {
	if (!window) return false;

	const auto inner_box = window->getWindowMainSurfaceBox();

	bool is_obscured = false;
	for (auto& w: g_pCompositor->m_vWindows | std::views::reverse) {
		if (w.get() == window) {
			// Don't go any further if this is a floating window, because m_vWindows is sorted bottom->top
			// per Compositor.cpp
			if (window->m_bIsFloating) break;
			else continue;
		}

		if (!w->m_bIsFloating) continue;

		const auto outer_box = w->getWindowMainSurfaceBox();
		is_obscured = covers(outer_box, inner_box);

		if (is_obscured) break;
	};

	return is_obscured;
}

bool isObscured(Hy3Node* node) {
	return node && node->data.type == Hy3NodeType::Window && isObscured(node->data.as_window);
}

bool isNotObscured(CWindow* window) { return !isObscured(window); }
bool isNotObscured(Hy3Node* node) { return !isObscured(node); }

CWindow* getWindowInDirection(
    CWindow* source,
    ShiftDirection direction,
    BitFlag<Layer> layers_same_monitor,
    BitFlag<Layer> layers_other_monitors
) {
	if (!source) return nullptr;
	if (layers_other_monitors == Layer::None && layers_same_monitor == Layer::None) return nullptr;

	CWindow* target_window = nullptr;
	const auto source_middle = source->middle();
	std::optional<Distance> target_distance;

	const auto static focus_policy =
	    ConfigValue<Hyprlang::INT>("plugin:hy3:focus_obscured_windows_policy");
	bool permit_obscured_windows =
	    *focus_policy == 0
	    || (*focus_policy == 2 && layers_same_monitor.HasNot(Layer::Floating | Layer::Tiled));

	const auto source_monitor = g_pCompositor->getMonitorFromID(source->m_iMonitorID);
	const auto next_monitor =
	    layers_other_monitors.HasAny(Layer::Floating | Layer::Tiled)
	        ? g_pCompositor->getMonitorInDirection(source_monitor, directionToChar(direction))
	        : nullptr;

	const auto next_workspace = next_monitor ? next_monitor->activeSpecialWorkspace
	                                             ? next_monitor->activeSpecialWorkspace
	                                             : next_monitor->activeWorkspace
	                                         : nullptr;

	auto isCandidate = [=, mon = source->m_iMonitorID](CWindow* w) {
		const auto window_layer = w->m_bIsFloating ? Layer::Floating : Layer::Tiled;
		const auto monitor_flags = w->m_iMonitorID == mon ? layers_same_monitor : layers_other_monitors;

		return (monitor_flags.Has(window_layer)) && w->m_bIsMapped && w->m_iX11Type != 2
		    && !w->m_sAdditionalConfigData.noFocus && !w->isHidden() && !w->m_bX11ShouldntFocus
		    && (w->m_bPinned || w->m_pWorkspace == source->m_pWorkspace
		        || w->m_pWorkspace == next_workspace);
	};

	for (auto& pw: g_pCompositor->m_vWindows) {
		auto w = pw.get();
		if (w != source && isCandidate(w)) {
			auto dist = Distance {direction, source_middle, w->middle()};
			if ((target_distance.has_value() ? dist < target_distance.value()
			                                 : dist.isInDirection(direction))
			    && (permit_obscured_windows || isNotObscured(w)))
			{
				target_window = w;
				target_distance = dist;
			}
		}
	}

	hy3_log(LOG, "getWindowInDirection: closest window to {} is {}", source, target_window);

	// If the closest window is on a different monitor and the nearest edge has the same position
	// as the last focused window on that monitor's workspace then choose the last focused window
	// instead; this allows seamless back-and-forth by direction keys
	if (target_window && target_window->m_iMonitorID != source->m_iMonitorID) {
		if (next_workspace) {
			if (auto last_focused = next_workspace->getLastFocusedWindow()) {
				auto target_bounds =
				    CBox(target_window->m_vRealPosition.value(), target_window->m_vRealSize.value());
				auto last_focused_bounds =
				    CBox(last_focused->m_vRealPosition.value(), last_focused->m_vRealSize.value());

				if ((direction == ShiftDirection::Left
				     && STICKS(
				         target_bounds.x + target_bounds.w,
				         last_focused_bounds.x + last_focused_bounds.w
				     ))
				    || (direction == ShiftDirection::Right && STICKS(target_bounds.x, last_focused_bounds.x)
				    )
				    || (direction == ShiftDirection::Up
				        && STICKS(
				            target_bounds.y + target_bounds.h,
				            last_focused_bounds.y + last_focused_bounds.h
				        ))
				    || (direction == ShiftDirection::Down && STICKS(target_bounds.y, last_focused_bounds.y)
				    ))
				{
					target_window = last_focused;
				}
			}
		}
	}

	return target_window;
}

void Hy3Layout::shiftFocusToMonitor(ShiftDirection direction) {
	auto target_monitor = g_pCompositor->getMonitorInDirection(directionToChar(direction));
	if (target_monitor) this->focusMonitor(target_monitor);
}

void Hy3Layout::shiftFocus(const PHLWORKSPACE& source_workspace, ShiftDirection direction, bool visible, BitFlag<Layer> eligible_layers) {
	Hy3Node    *candidate_node   = nullptr;
	CWindow    *closest_window   = nullptr;
	Hy3Node    *source_node      = nullptr;
	CWindow    *source_window    = g_pCompositor->m_pLastWindow;

	if (source_workspace) {
		source_window = source_workspace->m_pLastFocusedWindow;
	} else {
		source_window = g_pCompositor->m_pLastWindow;
	}

	if (source_window == nullptr || (source_workspace && source_workspace->m_bHasFullscreenWindow)) {
		shiftFocusToMonitor(direction);
		return;
	}

	hy3_log(
	    LOG,
	    "shiftFocus: Source: {} ({}), workspace: {:x}, direction: {}, visible: {}",
	    source_window,
	    source_window->m_bIsFloating ? "floating" : "tiled",
	    (uintptr_t) &source_workspace,
	    (int) direction,
	    visible
	);

	// If no eligible_layers specified then choose the same layer as the source window
	if (eligible_layers == Layer::None)
		eligible_layers = source_window->m_bIsFloating ? Layer::Floating : Layer::Tiled;

	const auto static focus_policy =
	    ConfigValue<Hyprlang::INT>("plugin:hy3:focus_obscured_windows_policy");
	bool skip_obscured = *focus_policy == 1
	                  || (*focus_policy == 2 && eligible_layers.Has(Layer::Floating | Layer::Tiled));

	// Determine the starting point for looking for a tiled node - it's either the
	// workspace's focused node or the floating window's focus entry point (which may be null)
	if (eligible_layers.Has(Layer::Tiled)) {
		source_node = source_window->m_bIsFloating ? getFocusOverride(source_window, direction)
		                                           : getWorkspaceFocusedNode(source_workspace);

		if (source_node) {
			candidate_node = this->shiftOrGetFocus(*source_node, direction, false, false, visible);
			while (candidate_node && skip_obscured && isObscured(candidate_node)) {
				candidate_node = this->shiftOrGetFocus(*candidate_node, direction, false, false, visible);
			}
		}
	}

	BitFlag<Layer> this_monitor = eligible_layers & Layer::Floating;
	if (source_window->m_bIsFloating && !candidate_node)
		this_monitor |= (eligible_layers & Layer::Tiled);

	BitFlag<Layer> other_monitors;
	if (!candidate_node) other_monitors |= eligible_layers;

	// Find the closest window in the right direction.  Consider other monitors
	// if we don't have a tiled candidate
	closest_window = getWindowInDirection(source_window, direction, this_monitor, other_monitors);

	// If there's a window in the right direction then choose between that window and the tiled
	// candidate.
	bool focus_closest_window = false;
	if (closest_window) {
		if (candidate_node) {
			// If the closest window is tiled then focus the tiled node which was obtained from
			// `shiftOrGetFocus`, otherwise focus whichever is closer
			if (closest_window->m_bIsFloating) {
				Distance distanceToClosestWindow(
				    direction,
				    source_window->middle(),
				    closest_window->middle()
				);
				Distance distanceToTiledNode(direction, source_window->middle(), candidate_node->middle());

				if (distanceToClosestWindow < distanceToTiledNode) {
					focus_closest_window = true;
				}
			}
		} else {
			focus_closest_window = true;
		}
	}

	std::optional<uint64_t> new_monitor_id;
	if (focus_closest_window) {
		new_monitor_id = closest_window->m_iMonitorID;
		setFocusOverride(closest_window, direction, source_node);
		g_pCompositor->focusWindow(closest_window);
	} else if (candidate_node) {
		if (candidate_node->data.type == Hy3NodeType::Window) {
			new_monitor_id = candidate_node->data.as_window->m_iMonitorID;
		} else if (auto workspace = candidate_node->getRoot()->workspace) {
			new_monitor_id = workspace->m_iMonitorID;
		}
		candidate_node->focusWindow();
		candidate_node->getRoot()->recalcSizePosRecursive();
	} else {
		shiftFocusToMonitor(direction);
	}

	if (new_monitor_id && new_monitor_id.value() != source_window->m_iMonitorID) {
		if (auto* monitor = g_pCompositor->getMonitorFromID(new_monitor_id.value())) {
			g_pCompositor->setActiveMonitor(monitor);
		}
	}
}

Hy3Node* Hy3Layout::getFocusOverride(CWindow* src, ShiftDirection direction) {
	if (auto intercept = this->m_focusIntercepts.find(src);
	    intercept != this->m_focusIntercepts.end())
	{
		Hy3Node** accessor = intercept->second.forDirection(direction);

		if (auto override = *accessor) {
			// If the root isn't valid or is on a different workspsace then update the intercept data
			if (override->workspace != src->m_pWorkspace
			    || !std::ranges::contains(this->nodes, *override))
			{
				*accessor = nullptr;
				// If there are no remaining overrides then discard the intercept
				if (intercept->second.isEmpty()) {
					this->m_focusIntercepts.erase(intercept);
				}
			}

			return override;
		}
	}

	return nullptr;
}

void Hy3Layout::setFocusOverride(CWindow* src, ShiftDirection direction, Hy3Node* dest) {
	if (auto intercept = this->m_focusIntercepts.find(src);
	    intercept != this->m_focusIntercepts.end())
	{
		*intercept->second.forDirection(direction) = dest;
	} else {
		FocusOverride override;
		*override.forDirection(direction) = dest;
		this->m_focusIntercepts.insert({src, override});
	}
}


void changeNodeWorkspaceRecursive(Hy3Node& node, const PHLWORKSPACE& workspace) {
	node.workspace = workspace;

	if (node.data.type == Hy3NodeType::Window) {
		auto* window = node.data.as_window;
		window->moveToWorkspace(workspace);
		window->updateToplevel();
		window->updateDynamicRules();
	} else {
		for (auto* child: node.data.as_group.children) {
			changeNodeWorkspaceRecursive(*child, workspace);
		}
	}
}

void Hy3Layout::moveNodeToWorkspace(const PHLWORKSPACE& origin, std::string wsname, bool follow) {
	std::string target_name;
	auto target_id = getWorkspaceIDFromString(wsname, target_name);

	if (target_id == WORKSPACE_INVALID) {
		hy3_log(ERR, "moveNodeToWorkspace called with invalid workspace {}", wsname);
		return;
	}

	auto workspace = g_pCompositor->getWorkspaceByID(target_id);

	if (origin == workspace) return;

	auto* node = this->getWorkspaceFocusedNode(origin);
	auto* focused_window = g_pCompositor->m_pLastWindow;
	auto* focused_window_node = this->getNodeFromWindow(focused_window);

	auto origin_ws = node != nullptr           ? node->workspace
	               : focused_window != nullptr ? focused_window->m_pWorkspace
	                                           : nullptr;

	if (!valid(origin_ws)) return;

	if (workspace == nullptr) {
		hy3_log(LOG, "creating target workspace {} for node move", target_id);

		workspace = g_pCompositor->createNewWorkspace(target_id, origin_ws->m_iMonitorID, target_name);
	}

	// floating or fullscreen
	if (focused_window != nullptr
	    && (focused_window_node == nullptr || focused_window->m_bIsFullscreen))
	{
		g_pCompositor->moveWindowToWorkspaceSafe(focused_window, workspace);
	} else {
		if (node == nullptr) return;

		hy3_log(
		    LOG,
		    "moving node {:x} from workspace {} to workspace {} (follow: {})",
		    (uintptr_t) node,
		    origin->m_iID,
		    workspace->m_iID,
		    follow
		);

		Hy3Node* expand_actor = nullptr;
		node->removeFromParentRecursive(&expand_actor);
		if (expand_actor != nullptr) expand_actor->recalcSizePosRecursive();

		changeNodeWorkspaceRecursive(*node, workspace);
		this->insertNode(*node);
	}

	if (follow) {
		auto* monitor = g_pCompositor->getMonitorFromID(workspace->m_iMonitorID);

		if (workspace->m_bIsSpecialWorkspace) {
			monitor->setSpecialWorkspace(workspace);
		} else if (origin_ws->m_bIsSpecialWorkspace) {
			g_pCompositor->getMonitorFromID(origin_ws->m_iMonitorID)->setSpecialWorkspace(nullptr);
		}

		monitor->changeWorkspace(workspace);

		static const auto allow_workspace_cycles =
		    ConfigValue<Hyprlang::INT>("binds:allow_workspace_cycles");
		if (*allow_workspace_cycles) workspace->rememberPrevWorkspace(origin_ws);
	}
}

void Hy3Layout::changeFocus(const PHLWORKSPACE& workspace, FocusShift shift) {
	auto* node = this->getWorkspaceFocusedNode(workspace);
	if (node == nullptr) return;

	switch (shift) {
	case FocusShift::Bottom: goto bottom;
	case FocusShift::Top:
		while (node->parent != nullptr) {
			node = node->parent;
		}

		node->focus();
		return;
	case FocusShift::Raise:
		if (node->parent == nullptr) goto bottom;
		else {
			node->parent->focus();
		}
		return;
	case FocusShift::Lower:
		if (node->data.type == Hy3NodeType::Group && node->data.as_group.focused_child != nullptr)
			node->data.as_group.focused_child->focus();
		return;
	case FocusShift::Tab:
		// make sure we go up at least one level
		if (node->parent != nullptr) node = node->parent;
		while (node->parent != nullptr) {
			if (node->data.as_group.layout == Hy3GroupLayout::Tabbed) {
				node->focus();
				return;
			}

			node = node->parent;
		}
		return;
	case FocusShift::TabNode:
		// make sure we go up at least one level
		if (node->parent != nullptr) node = node->parent;
		while (node->parent != nullptr) {
			if (node->parent->data.as_group.layout == Hy3GroupLayout::Tabbed) {
				node->focus();
				return;
			}

			node = node->parent;
		}
		return;
	}

bottom:
	while (node->data.type == Hy3NodeType::Group && node->data.as_group.focused_child != nullptr) {
		node = node->data.as_group.focused_child;
	}

	node->focus();
	return;
}

Hy3Node* findTabBarAt(Hy3Node& node, Vector2D pos, Hy3Node** focused_node) {
	// clang-format off
	static const auto gaps_in = ConfigValue<Hyprlang::CUSTOMTYPE, CCssGapData>("general:gaps_in");
	static const auto gaps_out = ConfigValue<Hyprlang::CUSTOMTYPE, CCssGapData>("general:gaps_out");
	static const auto tab_bar_height = ConfigValue<Hyprlang::INT>("plugin:hy3:tabs:height");
	static const auto tab_bar_padding = ConfigValue<Hyprlang::INT>("plugin:hy3:tabs:padding");
	// clang-format on

	auto inset = *tab_bar_height + *tab_bar_padding;

	if (node.parent == nullptr) {
		inset += gaps_out->left;
	} else {
		inset += gaps_in->left;
	}

	if (node.data.type == Hy3NodeType::Group) {
		if (node.hidden) return nullptr;
		// note: tab bar clicks ignore animations
		if (node.position.x > pos.x || node.position.y > pos.y || node.position.x + node.size.x < pos.x
		    || node.position.y + node.size.y < pos.y)
			return nullptr;

		if (node.data.as_group.layout == Hy3GroupLayout::Tabbed
		    && node.data.as_group.tab_bar != nullptr)
		{
			if (pos.y < node.position.y + node.gap_topleft_offset.y + inset) {
				auto& children = node.data.as_group.children;
				auto& tab_bar = *node.data.as_group.tab_bar;

				auto size = tab_bar.size.value();
				auto x = pos.x - tab_bar.pos.value().x;
				auto child_iter = children.begin();

				for (auto& tab: tab_bar.bar.entries) {
					if (child_iter == children.end()) break;

					if (x > tab.offset.value() * size.x
					    && x < (tab.offset.value() + tab.width.value()) * size.x)
					{
						*focused_node = *child_iter;
						return &node;
					}

					child_iter = std::next(child_iter);
				}
			}

			if (node.data.as_group.focused_child != nullptr) {
				return findTabBarAt(*node.data.as_group.focused_child, pos, focused_node);
			}
		} else {
			for (auto child: node.data.as_group.children) {
				if (findTabBarAt(*child, pos, focused_node)) return child;
			}
		}
	}

	return nullptr;
}

void Hy3Layout::focusTab(
    const PHLWORKSPACE& workspace,
    TabFocus target,
    TabFocusMousePriority mouse,
    bool wrap_scroll,
    int index
) {
	auto* node = this->getWorkspaceRootGroup(workspace);
	if (node == nullptr) return;

	Hy3Node* tab_node = nullptr;
	Hy3Node* tab_focused_node;

	if (target == TabFocus::MouseLocation || mouse != TabFocusMousePriority::Ignore) {
		auto mouse_pos = g_pInputManager->getMouseCoordsInternal();
		if (g_pCompositor->vectorToWindowUnified(
		        mouse_pos,
		        RESERVED_EXTENTS | INPUT_EXTENTS | ALLOW_FLOATING | FLOATING_ONLY
		    )
		    == nullptr)
		{
			tab_node = findTabBarAt(*node, mouse_pos, &tab_focused_node);
			if (tab_node != nullptr) goto hastab;
		}

		if (target == TabFocus::MouseLocation || mouse == TabFocusMousePriority::Require) return;
	}

	if (tab_node == nullptr) {
		tab_node = this->getWorkspaceFocusedNode(workspace);
		if (tab_node == nullptr) return;

		while (tab_node != nullptr && tab_node->data.as_group.layout != Hy3GroupLayout::Tabbed
		       && tab_node->parent != nullptr)
			tab_node = tab_node->parent;

		if (tab_node == nullptr || tab_node->data.type != Hy3NodeType::Group
		    || tab_node->data.as_group.layout != Hy3GroupLayout::Tabbed)
			return;
	}

hastab:
	if (target != TabFocus::MouseLocation) {
		if (tab_node->data.as_group.focused_child == nullptr
		    || tab_node->data.as_group.children.size() < 2)
			return;

		auto& children = tab_node->data.as_group.children;
		if (target == TabFocus::Index) {
			int i = 1;

			for (auto* node: children) {
				if (i == index) {
					tab_focused_node = node;
					goto cont;
				}

				i++;
			}

			return;
		cont:;
		} else {
			auto node_iter =
			    std::find(children.begin(), children.end(), tab_node->data.as_group.focused_child);
			if (node_iter == children.end()) return;
			if (target == TabFocus::Left) {
				if (node_iter == children.begin()) {
					if (wrap_scroll) node_iter = std::prev(children.end());
					else return;
				} else node_iter = std::prev(node_iter);

				tab_focused_node = *node_iter;
			} else {
				if (node_iter == std::prev(children.end())) {
					if (wrap_scroll) node_iter = children.begin();
					else return;
				} else node_iter = std::next(node_iter);

				tab_focused_node = *node_iter;
			}
		}
	}

	auto* focus = tab_focused_node;
	while (focus->data.type == Hy3NodeType::Group && !focus->data.as_group.group_focused
	       && focus->data.as_group.focused_child != nullptr)
		focus = focus->data.as_group.focused_child;

	focus->focus();
	tab_node->recalcSizePosRecursive();
}

void Hy3Layout::setNodeSwallow(const PHLWORKSPACE& workspace, SetSwallowOption option) {
	auto* node = this->getWorkspaceFocusedNode(workspace);
	if (node == nullptr || node->parent == nullptr) return;

	auto* containment = &node->parent->data.as_group.containment;
	switch (option) {
	case SetSwallowOption::NoSwallow: *containment = false; break;
	case SetSwallowOption::Swallow: *containment = true; break;
	case SetSwallowOption::Toggle: *containment = !*containment; break;
	}
}

void Hy3Layout::killFocusedNode(const PHLWORKSPACE& workspace) {
	if (g_pCompositor->m_pLastWindow != nullptr && g_pCompositor->m_pLastWindow->m_bIsFloating) {
		g_pCompositor->closeWindow(g_pCompositor->m_pLastWindow);
	} else {
		auto* node = this->getWorkspaceFocusedNode(workspace);
		if (node == nullptr) return;

		std::vector<CWindow*> windows;
		node->appendAllWindows(windows);

		for (auto* window: windows) {
			window->setHidden(false);
			g_pCompositor->closeWindow(window);
		}
	}
}

void Hy3Layout::expand(
    const PHLWORKSPACE& workspace,
    ExpandOption option,
    ExpandFullscreenOption fs_option
) {
	auto* node = this->getWorkspaceFocusedNode(workspace, false, true);
	if (node == nullptr) return;

	const auto monitor = g_pCompositor->getMonitorFromID(workspace->m_iMonitorID);

	switch (option) {
	case ExpandOption::Expand: {
		if (node->parent == nullptr) {
			switch (fs_option) {
			case ExpandFullscreenOption::MaximizeAsFullscreen:
			case ExpandFullscreenOption::MaximizeIntermediate: goto fullscreen;
			case ExpandFullscreenOption::MaximizeOnly: return;
			}
		}

		if (node->data.type == Hy3NodeType::Group && !node->data.as_group.group_focused)
			node->data.as_group.expand_focused = ExpandFocusType::Stack;

		auto& group = node->parent->data.as_group;
		group.focused_child = node;
		group.expand_focused = ExpandFocusType::Latch;

		node->parent->recalcSizePosRecursive();

		if (node->parent->parent == nullptr) {
			switch (fs_option) {
			case ExpandFullscreenOption::MaximizeAsFullscreen: goto fullscreen;
			case ExpandFullscreenOption::MaximizeIntermediate:
			case ExpandFullscreenOption::MaximizeOnly: return;
			}
		}
	} break;
	case ExpandOption::Shrink:
		if (node->data.type == Hy3NodeType::Group) {
			auto& group = node->data.as_group;

			group.expand_focused = ExpandFocusType::NotExpanded;
			if (group.focused_child->data.type == Hy3NodeType::Group)
				group.focused_child->data.as_group.expand_focused = ExpandFocusType::Latch;

			node->recalcSizePosRecursive();
		}
		break;
	case ExpandOption::Base: {
		if (node->data.type == Hy3NodeType::Group) {
			node->data.as_group.collapseExpansions();
			node->recalcSizePosRecursive();
		}
		break;
	}
	case ExpandOption::Maximize: break;
	case ExpandOption::Fullscreen: break;
	}

	return;

	CWindow* window;
fullscreen:
	if (node->data.type != Hy3NodeType::Window) return;
	window = node->data.as_window;
	if (!window->m_bIsFullscreen || window->m_pWorkspace->m_bIsSpecialWorkspace) return;

	if (workspace->m_bHasFullscreenWindow) return;

	window->m_bIsFullscreen = true;
	workspace->m_bHasFullscreenWindow = true;
	workspace->m_efFullscreenMode = FULLSCREEN_FULL;
	window->m_vRealPosition = monitor->vecPosition;
	window->m_vRealSize = monitor->vecSize;
	goto fsupdate;
// unfullscreen:
// 	if (node->data.type != Hy3NodeType::Window) return;
// 	window = node->data.as_window;
// 	window->m_bIsFullscreen = false;
// 	workspace->m_bHasFullscreenWindow = false;
// 	goto fsupdate;
fsupdate:
	g_pCompositor->updateWindowAnimatedDecorationValues(window);
	g_pXWaylandManager->setWindowSize(window, window->m_vRealSize.goal());
	g_pCompositor->changeWindowZOrder(window, true);
	this->recalculateMonitor(monitor->ID);
}

bool Hy3Layout::shouldRenderSelected(CWindow* window) {
	if (window == nullptr) return false;
	auto* root = this->getWorkspaceRootGroup(window->m_pWorkspace);
	if (root == nullptr || root->data.as_group.focused_child == nullptr) return false;
	auto* focused = root->getFocusedNode();
	if (focused == nullptr
	    || (focused->data.type == Hy3NodeType::Window
	        && focused->data.as_window != g_pCompositor->m_pLastWindow))
		return false;

	switch (focused->data.type) {
	case Hy3NodeType::Window: return focused->data.as_window == window;
	case Hy3NodeType::Group: {
		auto* node = this->getNodeFromWindow(window);
		if (node == nullptr) return false;
		return focused->hasChild(node);
	}
	default: return false;
	}
}

Hy3Node* Hy3Layout::getWorkspaceRootGroup(const PHLWORKSPACE& workspace) {
	for (auto& node: this->nodes) {
		if (node.workspace == workspace && node.parent == nullptr
		    && node.data.type == Hy3NodeType::Group && !node.reparenting)
		{
			return &node;
		}
	}

	return nullptr;
}

Hy3Node* Hy3Layout::getWorkspaceFocusedNode(
    const PHLWORKSPACE& workspace,
    bool ignore_group_focus,
    bool stop_at_expanded
) {
	auto* rootNode = this->getWorkspaceRootGroup(workspace);
	if (rootNode == nullptr) return nullptr;
	return rootNode->getFocusedNode(ignore_group_focus, stop_at_expanded);
}

void Hy3Layout::renderHook(void*, SCallbackInfo&, std::any data) {
	static bool rendering_normally = false;
	static std::vector<Hy3TabGroup*> rendered_groups;

	auto render_stage = std::any_cast<eRenderStage>(data);

	switch (render_stage) {
	case RENDER_PRE_WINDOWS:
		rendering_normally = true;
		rendered_groups.clear();
		break;
	case RENDER_POST_WINDOW:
		if (!rendering_normally) break;

		for (auto& entry: g_Hy3Layout->tab_groups) {
			if (!entry.hidden && entry.target_window == g_pHyprOpenGL->m_pCurrentWindow
			    && std::find(rendered_groups.begin(), rendered_groups.end(), &entry)
			           == rendered_groups.end())
			{
				entry.renderTabBar();
				rendered_groups.push_back(&entry);
			}
		}

		break;
	case RENDER_POST_WINDOWS:
		rendering_normally = false;

		for (auto& entry: g_Hy3Layout->tab_groups) {
			if (!entry.hidden
			    && entry.target_window->m_iMonitorID == g_pHyprOpenGL->m_RenderData.pMonitor->ID
			    && std::find(rendered_groups.begin(), rendered_groups.end(), &entry)
			           == rendered_groups.end())
			{
				entry.renderTabBar();
			}
		}

		break;
	default: break;
	}
}

void Hy3Layout::windowGroupUrgentHook(void* p, SCallbackInfo& callback_info, std::any data) {
	CWindow* window = std::any_cast<CWindow*>(data);
	if (window == nullptr) return;
	window->m_bIsUrgent = true;
	Hy3Layout::windowGroupUpdateRecursiveHook(p, callback_info, data);
}

void Hy3Layout::windowGroupUpdateRecursiveHook(void*, SCallbackInfo&, std::any data) {
	CWindow* window = std::any_cast<CWindow*>(data);
	if (window == nullptr) return;
	auto* node = g_Hy3Layout->getNodeFromWindow(window);

	// it is UB for `this` to be null
	if (node == nullptr) return;
	node->updateTabBarRecursive();
}

void Hy3Layout::tickHook(void*, SCallbackInfo&, std::any) {
	auto& tab_groups = g_Hy3Layout->tab_groups;
	auto entry = tab_groups.begin();
	while (entry != tab_groups.end()) {
		entry->tick();
		if (entry->bar.destroy) tab_groups.erase(entry++);
		else entry = std::next(entry);
	}
}

Hy3Node* Hy3Layout::getNodeFromWindow(CWindow* window) {
	for (auto& node: this->nodes) {
		if (node.data.type == Hy3NodeType::Window && node.data.as_window == window) {
			return &node;
		}
	}

	return nullptr;
}

void Hy3Layout::applyNodeDataToWindow(Hy3Node* node, bool no_animation) {
	if (node->data.type != Hy3NodeType::Window) return;
	auto* window = node->data.as_window;
	auto root_node = this->getWorkspaceRootGroup(window->m_pWorkspace);

	CMonitor* monitor = nullptr;

	if (node->workspace->m_bIsSpecialWorkspace) {
		for (auto& m: g_pCompositor->m_vMonitors) {
			if (m->activeSpecialWorkspace == node->workspace) {
				monitor = m.get();
				break;
			}
		}
	} else {
		monitor = g_pCompositor->getMonitorFromID(node->workspace->m_iMonitorID);
	}

	if (monitor == nullptr) {
		hy3_log(
		    ERR,
		    "node {:x}'s workspace has no associated monitor, cannot apply node data",
		    (uintptr_t) node
		);
		errorNotif();
		return;
	}

	const auto workspace_rule = g_pConfigManager->getWorkspaceRuleFor(node->workspace);

	// clang-format off
	static const auto gaps_in = ConfigValue<Hyprlang::CUSTOMTYPE, CCssGapData>("general:gaps_in");
	static const auto no_gaps_when_only = ConfigValue<Hyprlang::INT>("plugin:hy3:no_gaps_when_only");
	// clang-format on

	if (!g_pCompositor->windowExists(window) || !window->m_bIsMapped) {
		hy3_log(
		    ERR,
		    "node {:x} is an unmapped window ({:x}), cannot apply node data, removing from tiled "
		    "layout",
		    (uintptr_t) node,
		    (uintptr_t) window
		);
		errorNotif();
		this->onWindowRemovedTiling(window);
		return;
	}

	window->updateSpecialRenderData();

	auto nodeBox = CBox(node->position, node->size);
	nodeBox.round();

	window->m_vSize = nodeBox.size();
	window->m_vPosition = nodeBox.pos();

	auto only_node = root_node != nullptr && root_node->data.as_group.children.size() == 1
	              && root_node->data.as_group.children.front()->data.type == Hy3NodeType::Window;

	if (!window->m_pWorkspace->m_bIsSpecialWorkspace
	    && ((*no_gaps_when_only != 0 && (only_node || window->m_bIsFullscreen))
	        || (window->m_bIsFullscreen && window->m_pWorkspace->m_efFullscreenMode == FULLSCREEN_FULL
	        )))
	{
		window->m_sSpecialRenderData.border = workspace_rule.border.value_or(*no_gaps_when_only == 2);

		window->m_sSpecialRenderData.rounding = false;
		window->m_sSpecialRenderData.shadow = false;

		window->updateWindowDecos();

		const auto reserved = window->getFullWindowReservedArea();

		window->m_vRealPosition = window->m_vPosition + reserved.topLeft;
		window->m_vRealSize = window->m_vSize - (reserved.topLeft + reserved.bottomRight);

		g_pXWaylandManager->setWindowSize(window, window->m_vRealSize.goal());
	} else {
		auto calcPos = window->m_vPosition;
		auto calcSize = window->m_vSize;

		auto gaps_offset_topleft = Vector2D(gaps_in->left, gaps_in->top) + node->gap_topleft_offset;
		auto gaps_offset_bottomright =
		    Vector2D(gaps_in->left + gaps_in->right, gaps_in->top + gaps_in->bottom)
		    + node->gap_bottomright_offset + node->gap_topleft_offset;

		calcPos = calcPos + gaps_offset_topleft;
		calcSize = calcSize - gaps_offset_bottomright;

		const auto reserved_area = window->getFullWindowReservedArea();
		calcPos = calcPos + reserved_area.topLeft;
		calcSize = calcSize - (reserved_area.topLeft + reserved_area.bottomRight);

		CBox wb = {calcPos, calcSize};
		wb.round();

		window->m_vRealPosition = wb.pos();
		window->m_vRealSize = wb.size();

		g_pXWaylandManager->setWindowSize(window, wb.size());

		if (no_animation) {
			g_pHyprRenderer->damageWindow(window);

			window->m_vRealPosition.warp();
			window->m_vRealSize.warp();

			g_pHyprRenderer->damageWindow(window);
		}

		window->updateWindowDecos();
	}
}

Hy3Node* Hy3Layout::shiftOrGetFocus(
    Hy3Node& node,
    ShiftDirection direction,
    bool shift,
    bool once,
    bool visible
) {
	auto* break_origin = &node.getExpandActor();
	auto* break_parent = break_origin->parent;

	auto has_broken_once = false;

	// break parents until we hit a container oriented the same way as the shift
	// direction
	while (true) {
		if (break_parent == nullptr) return nullptr;

		auto& group = break_parent->data.as_group; // must be a group in order to be a parent

		if (shiftMatchesLayout(group.layout, direction)
		    && (!visible || group.layout != Hy3GroupLayout::Tabbed))
		{
			// group has the correct orientation

			if (once && shift && has_broken_once) break;
			if (break_origin != &node) has_broken_once = true;

			// if this movement would break out of the group, continue the break loop
			// (do not enter this if) otherwise break.
			if ((has_broken_once && once && shift)
			    || !(
			        (!shiftIsForward(direction) && group.children.front() == break_origin)
			        || (shiftIsForward(direction) && group.children.back() == break_origin)
			    ))
				break;
		}

		if (break_parent->parent == nullptr) {
			if (!shift) return nullptr;

			// if we haven't gone up any levels and the group is in the same direction
			// there's no reason to wrap the root group.
			if (group.layout != Hy3GroupLayout::Tabbed && shiftMatchesLayout(group.layout, direction))
				break;

			if (group.layout != Hy3GroupLayout::Tabbed && group.children.size() == 2
			    && std::find(group.children.begin(), group.children.end(), &node) != group.children.end())
			{
				group.setLayout(
				    shiftIsVertical(direction) ? Hy3GroupLayout::SplitV : Hy3GroupLayout::SplitH
				);
			} else {
				// wrap the root group in another group
				this->nodes.push_back({
				    .parent = break_parent,
				    .data = shiftIsVertical(direction) ? Hy3GroupLayout::SplitV : Hy3GroupLayout::SplitH,
				    .position = break_parent->position,
				    .size = break_parent->size,
				    .workspace = break_parent->workspace,
				    .layout = this,
				});

				auto* newChild = &this->nodes.back();
				Hy3Node::swapData(*break_parent, *newChild);
				break_parent->data.as_group.children.push_back(newChild);
				break_parent->data.as_group.group_focused = false;
				break_parent->data.as_group.focused_child = newChild;
				break_origin = newChild;
			}

			break;
		} else {
			break_origin = break_parent;
			break_parent = break_origin->parent;
		}
	}

	auto& parent_group = break_parent->data.as_group;
	Hy3Node* target_group = break_parent;
	std::list<Hy3Node*>::iterator insert;

	if (break_origin == parent_group.children.front() && !shiftIsForward(direction)) {
		if (!shift) {
			return nullptr;
		}
		insert = parent_group.children.begin();
	} else if (break_origin == parent_group.children.back() && shiftIsForward(direction)) {
		if (!shift) {
			return nullptr;
		}
		insert = parent_group.children.end();
	} else {
		auto& group_data = target_group->data.as_group;

		auto iter = std::find(group_data.children.begin(), group_data.children.end(), break_origin);
		if (shiftIsForward(direction)) iter = std::next(iter);
		else iter = std::prev(iter);

		if ((*iter)->data.type == Hy3NodeType::Window
		    || ((*iter)->data.type == Hy3NodeType::Group
		        && (*iter)->data.as_group.expand_focused != ExpandFocusType::NotExpanded)
		    || (shift && once && has_broken_once))
		{
			if (shift) {
				if (target_group == node.parent) {
					if (shiftIsForward(direction)) insert = std::next(iter);
					else insert = iter;
				} else {
					if (shiftIsForward(direction)) insert = iter;
					else insert = std::next(iter);
				}
			} else {
				return (*iter)->getFocusedNode();
			}
		} else {
			// break into neighboring groups until we hit a window
			while (true) {
				target_group = *iter;
				auto& group_data = target_group->data.as_group;

				if (group_data.children.empty()) {
					// in theory this would never happen
					return nullptr;
				}

				bool shift_after = false;

				if (!shift && group_data.layout == Hy3GroupLayout::Tabbed
				    && group_data.focused_child != nullptr)
				{
					iter = std::find(
					    group_data.children.begin(),
					    group_data.children.end(),
					    group_data.focused_child
					);
				} else if (visible && group_data.layout == Hy3GroupLayout::Tabbed && group_data.focused_child != nullptr)
				{
					// if the group is tabbed and we're going by visible nodes, jump to the current entry
					iter = std::find(
					    group_data.children.begin(),
					    group_data.children.end(),
					    group_data.focused_child
					);
					shift_after = true;
				} else if (shiftMatchesLayout(group_data.layout, direction) || (visible && group_data.layout == Hy3GroupLayout::Tabbed))
				{
					// if the group has the same orientation as movement pick the
					// last/first child based on movement direction
					if (shiftIsForward(direction)) iter = group_data.children.begin();
					else {
						iter = std::prev(group_data.children.end());
						shift_after = true;
					}
				} else {
					if (group_data.focused_child != nullptr) {
						iter = std::find(
						    group_data.children.begin(),
						    group_data.children.end(),
						    group_data.focused_child
						);
						shift_after = true;
					} else {
						iter = group_data.children.begin();
					}
				}

				if (shift && once) {
					if (shift_after) insert = std::next(iter);
					else insert = iter;
					break;
				}

				if ((*iter)->data.type == Hy3NodeType::Window
				    || ((*iter)->data.type == Hy3NodeType::Group
				        && (*iter)->data.as_group.expand_focused != ExpandFocusType::NotExpanded))
				{
					if (shift) {
						if (shift_after) insert = std::next(iter);
						else insert = iter;
						break;
					} else {
						return (*iter)->getFocusedNode();
					}
				}
			}
		}
	}

	auto& group_data = target_group->data.as_group;

	if (target_group == node.parent) {
		// nullptr is used as a signal value instead of removing it first to avoid
		// iterator invalidation.
		auto iter = std::find(group_data.children.begin(), group_data.children.end(), &node);
		*iter = nullptr;
		target_group->data.as_group.children.insert(insert, &node);
		target_group->data.as_group.children.remove(nullptr);
		target_group->recalcSizePosRecursive();
	} else {
		target_group->data.as_group.children.insert(insert, &node);

		// must happen AFTER `insert` is used
		auto* old_parent = node.removeFromParentRecursive(nullptr);
		node.parent = target_group;
		node.size_ratio = 1.0;

		if (old_parent != nullptr) {
			auto& group = old_parent->data.as_group;
			if (old_parent->parent != nullptr && group.ephemeral && group.children.size() == 1
			    && !old_parent->hasChild(&node))
			{
				Hy3Node::swallowGroups(old_parent);
			}

			old_parent->updateTabBarRecursive();
			old_parent->recalcSizePosRecursive();
		}

		target_group->recalcSizePosRecursive();

		auto* target_parent = target_group->parent;
		while (target_parent != nullptr && Hy3Node::swallowGroups(target_parent)) {
			target_parent = target_parent->parent;
		}

		node.updateTabBarRecursive();
		node.focus();

		if (target_parent != target_group && target_parent != nullptr)
			target_parent->recalcSizePosRecursive();
	}

	return nullptr;
}

void Hy3Layout::updateAutotileWorkspaces() {
	static const auto autotile_raw_workspaces =
	    ConfigValue<Hyprlang::STRING>("plugin:hy3:autotile:workspaces");

	if (*autotile_raw_workspaces == this->autotile.raw_workspaces) {
		return;
	}

	this->autotile.raw_workspaces = *autotile_raw_workspaces;
	this->autotile.workspaces.clear();

	if (this->autotile.raw_workspaces == "all") {
		return;
	}

	this->autotile.workspace_blacklist = this->autotile.raw_workspaces.rfind("not:", 0) == 0;

	const auto autotile_raw_workspaces_filtered = (this->autotile.workspace_blacklist)
	                                                ? this->autotile.raw_workspaces.substr(4)
	                                                : this->autotile.raw_workspaces;

	// split on space and comma
	const std::regex regex {R"([\s,]+)"};
	const auto begin = std::sregex_token_iterator(
	    autotile_raw_workspaces_filtered.begin(),
	    autotile_raw_workspaces_filtered.end(),
	    regex,
	    -1
	);
	const auto end = std::sregex_token_iterator();

	for (auto s = begin; s != end; ++s) {
		try {
			this->autotile.workspaces.insert(std::stoi(*s));
		} catch (...) {
			hy3_log(ERR, "autotile:workspaces: invalid workspace id: {}", (std::string) *s);
		}
	}
}

bool Hy3Layout::shouldAutotileWorkspace(const PHLWORKSPACE& workspace) {
	if (this->autotile.workspace_blacklist) {
		return !this->autotile.workspaces.contains(workspace->m_iID);
	} else {
		return this->autotile.workspaces.empty()
		    || this->autotile.workspaces.contains(workspace->m_iID);
	}
}
