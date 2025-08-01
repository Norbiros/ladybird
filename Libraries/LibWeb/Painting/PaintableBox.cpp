/*
 * Copyright (c) 2022-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2024-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericShorthands.h>
#include <AK/TemporaryChange.h>
#include <LibGfx/Font/Font.h>
#include <LibUnicode/CharacterTypes.h>
#include <LibWeb/CSS/SystemColor.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Position.h>
#include <LibWeb/DOM/Range.h>
#include <LibWeb/HTML/HTMLHtmlElement.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Layout/BlockContainer.h>
#include <LibWeb/Layout/InlineNode.h>
#include <LibWeb/Painting/BackgroundPainting.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/Painting/SVGPaintable.h>
#include <LibWeb/Painting/SVGSVGPaintable.h>
#include <LibWeb/Painting/ShadowPainting.h>
#include <LibWeb/Painting/StackingContext.h>
#include <LibWeb/Painting/TableBordersPainting.h>
#include <LibWeb/Painting/TextPaintable.h>
#include <LibWeb/Painting/ViewportPaintable.h>
#include <LibWeb/Platform/FontPlugin.h>
#include <LibWeb/Selection/Selection.h>

namespace Web::Painting {

GC_DEFINE_ALLOCATOR(PaintableWithLines);

bool g_paint_viewport_scrollbars = true;

GC::Ref<PaintableWithLines> PaintableWithLines::create(Layout::BlockContainer const& block_container)
{
    return block_container.heap().allocate<PaintableWithLines>(block_container);
}

GC::Ref<PaintableWithLines> PaintableWithLines::create(Layout::InlineNode const& inline_node, size_t line_index)
{
    return inline_node.heap().allocate<PaintableWithLines>(inline_node, line_index);
}

GC::Ref<PaintableBox> PaintableBox::create(Layout::Box const& layout_box)
{
    return layout_box.heap().allocate<PaintableBox>(layout_box);
}

GC::Ref<PaintableBox> PaintableBox::create(Layout::InlineNode const& layout_box)
{
    return layout_box.heap().allocate<PaintableBox>(layout_box);
}

PaintableBox::PaintableBox(Layout::Box const& layout_box)
    : Paintable(layout_box)
{
}

PaintableBox::PaintableBox(Layout::InlineNode const& layout_box)
    : Paintable(layout_box)
{
}

PaintableBox::~PaintableBox()
{
}

PaintableWithLines::PaintableWithLines(Layout::BlockContainer const& layout_box)
    : PaintableBox(layout_box)
{
}

PaintableWithLines::PaintableWithLines(Layout::InlineNode const& inline_node, size_t line_index)
    : PaintableBox(inline_node)
    , m_line_index(line_index)
{
}

PaintableWithLines::~PaintableWithLines()
{
}

CSSPixelPoint PaintableBox::scroll_offset() const
{
    if (is_viewport()) {
        auto navigable = document().navigable();
        VERIFY(navigable);
        return navigable->viewport_scroll_offset();
    }

    auto const& node = layout_node();
    if (auto pseudo_element = node.generated_for_pseudo_element(); pseudo_element.has_value())
        return node.pseudo_element_generator()->scroll_offset(*pseudo_element);

    if (!(dom_node() && is<DOM::Element>(*dom_node())))
        return {};

    return static_cast<DOM::Element const*>(dom_node())->scroll_offset({});
}

void PaintableBox::set_scroll_offset(CSSPixelPoint offset)
{
    auto scrollable_overflow_rect = this->scrollable_overflow_rect();
    if (!scrollable_overflow_rect.has_value())
        return;

    document().set_needs_to_refresh_scroll_state(true);

    auto padding_rect = absolute_padding_box_rect();
    auto max_x_offset = max(scrollable_overflow_rect->width() - padding_rect.width(), 0);
    auto max_y_offset = max(scrollable_overflow_rect->height() - padding_rect.height(), 0);

    offset.set_x(clamp(offset.x(), 0, max_x_offset));
    offset.set_y(clamp(offset.y(), 0, max_y_offset));

    // FIXME: If there is horizontal and vertical scroll ignore only part of the new offset
    if (offset.y() < 0 || scroll_offset() == offset)
        return;

    auto& node = layout_node();
    if (auto pseudo_element = node.generated_for_pseudo_element(); pseudo_element.has_value()) {
        node.pseudo_element_generator()->set_scroll_offset(*pseudo_element, offset);
    } else if (is<DOM::Element>(*dom_node())) {
        static_cast<DOM::Element*>(dom_node())->set_scroll_offset({}, offset);
    } else {
        return;
    }

    // https://drafts.csswg.org/cssom-view-1/#scrolling-events
    // Whenever an element gets scrolled (whether in response to user interaction or by an API),
    // the user agent must run these steps:

    // 1. Let doc be the element’s node document.
    auto& document = layout_node().document();

    // FIXME: 2. If the element is a snap container, run the steps to update snapchanging targets for the element with
    //           the element’s eventual snap target in the block axis as newBlockTarget and the element’s eventual snap
    //           target in the inline axis as newInlineTarget.

    GC::Ref<DOM::EventTarget> const event_target = *dom_node();

    // 3. If the element is already in doc’s pending scroll event targets, abort these steps.
    if (document.pending_scroll_event_targets().contains_slow(event_target))
        return;

    // 4. Append the element to doc’s pending scroll event targets.
    document.pending_scroll_event_targets().append(*layout_node_with_style_and_box_metrics().dom_node());

    set_needs_display(InvalidateDisplayList::No);
}

void PaintableBox::scroll_by(int delta_x, int delta_y)
{
    set_scroll_offset(scroll_offset().translated(delta_x, delta_y));
}

void PaintableBox::set_offset(CSSPixelPoint offset)
{
    m_offset = offset;
}

void PaintableBox::set_content_size(CSSPixelSize size)
{
    m_content_size = size;
    if (is<Layout::Box>(Paintable::layout_node())) {
        static_cast<Layout::Box&>(layout_node_with_style_and_box_metrics()).did_set_content_size();
    }
}

CSSPixelPoint PaintableBox::offset() const
{
    return m_offset;
}

CSSPixelRect PaintableBox::compute_absolute_rect() const
{
    CSSPixelRect rect { offset(), content_size() };
    for (auto const* block = containing_block(); block; block = block->containing_block())
        rect.translate_by(block->offset());
    return rect;
}

CSSPixelRect PaintableBox::absolute_rect() const
{
    if (!m_absolute_rect.has_value())
        m_absolute_rect = compute_absolute_rect();
    return *m_absolute_rect;
}

CSSPixelRect PaintableBox::compute_absolute_paint_rect() const
{
    // FIXME: This likely incomplete:
    auto rect = absolute_border_box_rect();
    if (has_scrollable_overflow()) {
        auto scrollable_overflow_rect = this->scrollable_overflow_rect().value();
        if (computed_values().overflow_x() == CSS::Overflow::Visible)
            rect.unite_horizontally(scrollable_overflow_rect);
        if (computed_values().overflow_y() == CSS::Overflow::Visible)
            rect.unite_vertically(scrollable_overflow_rect);
    }
    for (auto const& shadow : box_shadow_data()) {
        if (shadow.placement == ShadowPlacement::Inner)
            continue;
        auto inflate = shadow.spread_distance + shadow.blur_radius;
        auto shadow_rect = rect.inflated(inflate, inflate, inflate, inflate).translated(shadow.offset_x, shadow.offset_y);
        rect.unite(shadow_rect);
    }
    return rect;
}

CSSPixelRect PaintableBox::absolute_padding_box_rect() const
{
    auto absolute_rect = this->absolute_rect();
    CSSPixelRect rect;
    rect.set_x(absolute_rect.x() - box_model().padding.left);
    rect.set_width(content_width() + box_model().padding.left + box_model().padding.right);
    rect.set_y(absolute_rect.y() - box_model().padding.top);
    rect.set_height(content_height() + box_model().padding.top + box_model().padding.bottom);
    return rect;
}

CSSPixelRect PaintableBox::absolute_border_box_rect() const
{
    auto padded_rect = this->absolute_padding_box_rect();
    CSSPixelRect rect;
    auto use_collapsing_borders_model = override_borders_data().has_value();
    // Implement the collapsing border model https://www.w3.org/TR/CSS22/tables.html#collapsing-borders.
    auto border_top = use_collapsing_borders_model ? round(box_model().border.top / 2) : box_model().border.top;
    auto border_bottom = use_collapsing_borders_model ? round(box_model().border.bottom / 2) : box_model().border.bottom;
    auto border_left = use_collapsing_borders_model ? round(box_model().border.left / 2) : box_model().border.left;
    auto border_right = use_collapsing_borders_model ? round(box_model().border.right / 2) : box_model().border.right;
    rect.set_x(padded_rect.x() - border_left);
    rect.set_width(padded_rect.width() + border_left + border_right);
    rect.set_y(padded_rect.y() - border_top);
    rect.set_height(padded_rect.height() + border_top + border_bottom);
    return rect;
}

// https://drafts.csswg.org/css-overflow-4/#overflow-clip-edge
CSSPixelRect PaintableBox::overflow_clip_edge_rect() const
{
    // FIXME: Apply overflow-clip-margin-* properties
    return absolute_padding_box_rect();
}

CSSPixelRect PaintableBox::absolute_paint_rect() const
{
    if (!m_absolute_paint_rect.has_value())
        m_absolute_paint_rect = compute_absolute_paint_rect();
    return *m_absolute_paint_rect;
}

template<typename Callable>
static CSSPixelRect united_rect_for_continuation_chain(PaintableBox const& start, Callable get_rect)
{
    // Combine the absolute rects of all paintable boxes of all nodes in the continuation chain. Without this, we
    // calculate the wrong rect for inline nodes that were split because of block elements.
    Optional<CSSPixelRect> result;

    // FIXME: instead of walking the continuation chain in the layout tree, also keep track of this chain in the
    //        painting tree so we can skip visiting the layout nodes altogether.
    for (auto const* node = &start.layout_node_with_style_and_box_metrics(); node; node = node->continuation_of_node()) {
        for (auto const& paintable : node->paintables()) {
            if (!is<PaintableBox>(paintable))
                continue;
            auto const& paintable_box = static_cast<PaintableBox const&>(paintable);
            auto paintable_border_box_rect = get_rect(paintable_box);
            if (!result.has_value())
                result = paintable_border_box_rect;
            else if (!paintable_border_box_rect.is_empty())
                result->unite(paintable_border_box_rect);
        }
    }
    return result.value_or({});
}

CSSPixelRect PaintableBox::absolute_united_border_box_rect() const
{
    return united_rect_for_continuation_chain(*this, [](auto const& paintable_box) {
        return paintable_box.absolute_border_box_rect();
    });
}

CSSPixelRect PaintableBox::absolute_united_content_rect() const
{
    return united_rect_for_continuation_chain(*this, [](auto const& paintable_box) {
        return paintable_box.absolute_rect();
    });
}

CSSPixelRect PaintableBox::absolute_united_padding_box_rect() const
{
    return united_rect_for_continuation_chain(*this, [](auto const& paintable_box) {
        return paintable_box.absolute_padding_box_rect();
    });
}

Optional<CSSPixelRect> PaintableBox::get_clip_rect() const
{
    auto clip = computed_values().clip();
    if (clip.is_rect() && layout_node_with_style_and_box_metrics().is_absolutely_positioned()) {
        auto border_box = absolute_border_box_rect();
        return clip.to_rect().resolved(layout_node(), border_box);
    }
    return {};
}

bool PaintableBox::wants_mouse_events() const
{
    if (compute_scrollbar_data(ScrollDirection::Vertical).has_value())
        return true;
    if (compute_scrollbar_data(ScrollDirection::Horizontal).has_value())
        return true;
    return false;
}

void PaintableBox::before_paint(PaintContext& context, PaintPhase phase) const
{
    if (!is_visible())
        return;

    if (first_is_one_of(phase, PaintPhase::Background, PaintPhase::Foreground) && own_clip_frame()) {
        context.display_list_recorder().push_clip_frame(own_clip_frame());
    } else if (!has_css_transform()) {
        apply_clip_overflow_rect(context, phase);
    }
    apply_scroll_offset(context);
}

void PaintableBox::after_paint(PaintContext& context, PaintPhase phase) const
{
    if (!is_visible())
        return;

    reset_scroll_offset(context);
    if (first_is_one_of(phase, PaintPhase::Background, PaintPhase::Foreground) && own_clip_frame()) {
        context.display_list_recorder().pop_clip_frame();
    } else if (!has_css_transform()) {
        clear_clip_overflow_rect(context, phase);
    }
}

bool PaintableBox::could_be_scrolled_by_wheel_event(ScrollDirection direction) const
{
    auto overflow = direction == ScrollDirection::Horizontal ? computed_values().overflow_x() : computed_values().overflow_y();
    auto scrollable_overflow_rect = this->scrollable_overflow_rect();
    if (!scrollable_overflow_rect.has_value())
        return false;
    auto scrollable_overflow_size = direction == ScrollDirection::Horizontal ? scrollable_overflow_rect->width() : scrollable_overflow_rect->height();
    auto scrollport_size = direction == ScrollDirection::Horizontal ? absolute_padding_box_rect().width() : absolute_padding_box_rect().height();
    auto overflow_value_allows_scrolling = overflow == CSS::Overflow::Auto || overflow == CSS::Overflow::Scroll;
    if ((is_viewport() && overflow != CSS::Overflow::Hidden) || overflow_value_allows_scrolling)
        return scrollable_overflow_size > scrollport_size;
    return false;
}

bool PaintableBox::could_be_scrolled_by_wheel_event() const
{
    return could_be_scrolled_by_wheel_event(ScrollDirection::Horizontal) || could_be_scrolled_by_wheel_event(ScrollDirection::Vertical);
}

static constexpr CSSPixels SCROLLBAR_THUMB_NORMAL_THICKNESS = 5;
static constexpr CSSPixels SCROLLBAR_THUMB_WIDENED_THICKNESS = 10;

Optional<PaintableBox::ScrollbarData> PaintableBox::compute_scrollbar_data(ScrollDirection direction, AdjustThumbRectForScrollOffset adjust_thumb_rect_for_scroll_offset) const
{
    bool is_horizontal = direction == ScrollDirection::Horizontal;
    bool display_scrollbar = could_be_scrolled_by_wheel_event(direction);
    if (is_horizontal) {
        display_scrollbar |= computed_values().overflow_x() == CSS::Overflow::Scroll;
    } else {
        display_scrollbar |= computed_values().overflow_y() == CSS::Overflow::Scroll;
    }
    if (!display_scrollbar) {
        return {};
    }

    if (!own_scroll_frame_id().has_value()) {
        return {};
    }

    auto padding_rect = absolute_padding_box_rect();
    auto scrollable_overflow_rect = this->scrollable_overflow_rect().value();
    auto scroll_overflow_size = is_horizontal ? scrollable_overflow_rect.width() : scrollable_overflow_rect.height();
    auto scrollport_size = is_horizontal ? padding_rect.width() : padding_rect.height();
    if (scroll_overflow_size == 0)
        return {};

    auto thickness = [&]() {
        if (is_horizontal)
            return m_draw_enlarged_horizontal_scrollbar ? SCROLLBAR_THUMB_WIDENED_THICKNESS : SCROLLBAR_THUMB_NORMAL_THICKNESS;
        return m_draw_enlarged_vertical_scrollbar ? SCROLLBAR_THUMB_WIDENED_THICKNESS : SCROLLBAR_THUMB_NORMAL_THICKNESS;
    }();

    auto scrollbar_rect_length = is_horizontal ? scrollport_size - thickness : scrollport_size;

    auto min_thumb_length = min(scrollbar_rect_length, 24);
    auto thumb_length = max(scrollbar_rect_length * (scrollport_size / scroll_overflow_size), min_thumb_length);

    ScrollbarData scrollbar_data;

    if (scroll_overflow_size > scrollport_size)
        scrollbar_data.scroll_length = (scrollbar_rect_length - thumb_length) / (scroll_overflow_size - scrollport_size);

    if (is_horizontal) {
        if (m_draw_enlarged_horizontal_scrollbar)
            scrollbar_data.gutter_rect = { padding_rect.left(), padding_rect.bottom() - thickness, padding_rect.width(), thickness };
        scrollbar_data.thumb_rect = { padding_rect.left(), padding_rect.bottom() - thickness, thumb_length, thickness };
    } else {
        if (m_draw_enlarged_vertical_scrollbar)
            scrollbar_data.gutter_rect = { padding_rect.right() - thickness, padding_rect.top(), thickness, padding_rect.height() };
        scrollbar_data.thumb_rect = { padding_rect.right() - thickness, padding_rect.top(), thickness, thumb_length };
    }

    if (adjust_thumb_rect_for_scroll_offset == AdjustThumbRectForScrollOffset::Yes) {
        auto scroll_offset = is_horizontal ? -own_scroll_frame_offset().x() : -own_scroll_frame_offset().y();
        auto thumb_offset = scroll_offset * scrollbar_data.scroll_length;

        if (is_horizontal)
            scrollbar_data.thumb_rect.translate_by(thumb_offset, 0);
        else
            scrollbar_data.thumb_rect.translate_by(0, thumb_offset);
    }

    return scrollbar_data;
}

void PaintableBox::paint(PaintContext& context, PaintPhase phase) const
{
    if (!is_visible())
        return;

    auto empty_cells_property_applies = [this]() {
        return display().is_internal_table() && computed_values().empty_cells() == CSS::EmptyCells::Hide && !has_children();
    };

    if (phase == PaintPhase::Background && !empty_cells_property_applies()) {
        paint_backdrop_filter(context);
        paint_background(context);
        paint_box_shadow(context);
    }

    auto const is_table_with_collapsed_borders = display().is_table_inside() && computed_values().border_collapse() == CSS::BorderCollapse::Collapse;
    if (!display().is_table_cell() && !is_table_with_collapsed_borders && phase == PaintPhase::Border) {
        paint_border(context);
    }

    if ((display().is_table_inside() || computed_values().border_collapse() == CSS::BorderCollapse::Collapse) && phase == PaintPhase::TableCollapsedBorder) {
        paint_table_borders(context, *this);
    }

    if (phase == PaintPhase::Outline) {
        auto const& outline_data = this->outline_data();
        if (outline_data.has_value()) {
            auto outline_offset = this->outline_offset();
            auto border_radius_data = normalized_border_radii_data(ShrinkRadiiForBorders::No);
            auto borders_rect = absolute_border_box_rect();

            auto outline_offset_x = outline_offset;
            auto outline_offset_y = outline_offset;
            // "Both the height and the width of the outside of the shape drawn by the outline should not
            // become smaller than twice the computed value of the outline-width property to make sure
            // that an outline can be rendered even with large negative values."
            // https://www.w3.org/TR/css-ui-4/#outline-offset
            // So, if the horizontal outline offset is > half the borders_rect's width then we set it to that.
            // (And the same for y)
            if ((borders_rect.width() / 2) + outline_offset_x < 0)
                outline_offset_x = -borders_rect.width() / 2;
            if ((borders_rect.height() / 2) + outline_offset_y < 0)
                outline_offset_y = -borders_rect.height() / 2;

            border_radius_data.inflate(outline_data->top.width + outline_offset_y, outline_data->right.width + outline_offset_x, outline_data->bottom.width + outline_offset_y, outline_data->left.width + outline_offset_x);
            borders_rect.inflate(outline_data->top.width + outline_offset_y, outline_data->right.width + outline_offset_x, outline_data->bottom.width + outline_offset_y, outline_data->left.width + outline_offset_x);

            paint_all_borders(context.display_list_recorder(), context.rounded_device_rect(borders_rect), border_radius_data.as_corners(context.device_pixel_converter()), outline_data->to_device_pixels(context));
        }
    }

    if (phase == PaintPhase::Overlay && (g_paint_viewport_scrollbars || !is_viewport()) && computed_values().scrollbar_width() != CSS::ScrollbarWidth::None) {
        auto scrollbar_colors = computed_values().scrollbar_color();
        if (auto scrollbar_data = compute_scrollbar_data(ScrollDirection::Vertical); scrollbar_data.has_value()) {
            auto gutter_rect = context.rounded_device_rect(scrollbar_data->gutter_rect).to_type<int>();
            auto thumb_rect = context.rounded_device_rect(scrollbar_data->thumb_rect).to_type<int>();
            context.display_list_recorder().paint_scrollbar(own_scroll_frame_id().value(), gutter_rect, thumb_rect, scrollbar_data->scroll_length, scrollbar_colors.thumb_color, scrollbar_colors.track_color, true);
        }
        if (auto scrollbar_data = compute_scrollbar_data(ScrollDirection::Horizontal); scrollbar_data.has_value()) {
            auto gutter_rect = context.rounded_device_rect(scrollbar_data->gutter_rect).to_type<int>();
            auto thumb_rect = context.rounded_device_rect(scrollbar_data->thumb_rect).to_type<int>();
            context.display_list_recorder().paint_scrollbar(own_scroll_frame_id().value(), gutter_rect, thumb_rect, scrollbar_data->scroll_length, scrollbar_colors.thumb_color, scrollbar_colors.track_color, false);
        }
    }

    if (phase == PaintPhase::Overlay && layout_node().document().highlighted_layout_node() == &layout_node_with_style_and_box_metrics()) {
        auto content_rect = absolute_united_content_rect();
        auto margin_rect = united_rect_for_continuation_chain(*this, [](PaintableBox const& box) {
            auto margin_box = box.box_model().margin_box();
            return CSSPixelRect {
                box.absolute_x() - margin_box.left,
                box.absolute_y() - margin_box.top,
                box.content_width() + margin_box.left + margin_box.right,
                box.content_height() + margin_box.top + margin_box.bottom,
            };
        });
        auto border_rect = absolute_united_border_box_rect();
        auto padding_rect = absolute_united_padding_box_rect();

        auto paint_inspector_rect = [&](CSSPixelRect const& rect, Color color) {
            auto device_rect = context.enclosing_device_rect(rect).to_type<int>();
            context.display_list_recorder().fill_rect(device_rect, Color(color).with_alpha(100));
            context.display_list_recorder().draw_rect(device_rect, Color(color));
        };

        paint_inspector_rect(margin_rect, Color::Yellow);
        paint_inspector_rect(padding_rect, Color::Cyan);
        paint_inspector_rect(border_rect, Color::Green);
        paint_inspector_rect(content_rect, Color::Magenta);

        auto font = Platform::FontPlugin::the().default_font(12);

        StringBuilder builder;
        if (layout_node_with_style_and_box_metrics().dom_node())
            builder.append(layout_node_with_style_and_box_metrics().dom_node()->debug_description());
        else
            builder.append(layout_node_with_style_and_box_metrics().debug_description());
        builder.appendff(" {}x{} @ {},{}", border_rect.width(), border_rect.height(), border_rect.x(), border_rect.y());
        auto size_text = MUST(builder.to_string());
        auto size_text_rect = border_rect;
        size_text_rect.set_y(border_rect.y() + border_rect.height());
        size_text_rect.set_top(size_text_rect.top());
        size_text_rect.set_width(CSSPixels::nearest_value_for(font->width(size_text)) + 4);
        size_text_rect.set_height(CSSPixels::nearest_value_for(font->pixel_size()) + 4);
        auto size_text_device_rect = context.enclosing_device_rect(size_text_rect).to_type<int>();
        context.display_list_recorder().fill_rect(size_text_device_rect, context.palette().color(Gfx::ColorRole::Tooltip));
        context.display_list_recorder().draw_rect(size_text_device_rect, context.palette().threed_shadow1());
        context.display_list_recorder().draw_text(size_text_device_rect, size_text, font->with_size(font->point_size() * context.device_pixels_per_css_pixel()), Gfx::TextAlignment::Center, context.palette().color(Gfx::ColorRole::TooltipText));
    }
}

void PaintableBox::set_stacking_context(NonnullOwnPtr<StackingContext> stacking_context)
{
    m_stacking_context = move(stacking_context);
}

void PaintableBox::invalidate_stacking_context()
{
    m_stacking_context = nullptr;
}

BordersData PaintableBox::remove_element_kind_from_borders_data(PaintableBox::BordersDataWithElementKind borders_data)
{
    return {
        .top = borders_data.top.border_data,
        .right = borders_data.right.border_data,
        .bottom = borders_data.bottom.border_data,
        .left = borders_data.left.border_data,
    };
}

void PaintableBox::paint_border(PaintContext& context) const
{
    auto borders_data = m_override_borders_data.has_value() ? remove_element_kind_from_borders_data(m_override_borders_data.value()) : BordersData {
        .top = box_model().border.top == 0 ? CSS::BorderData() : computed_values().border_top(),
        .right = box_model().border.right == 0 ? CSS::BorderData() : computed_values().border_right(),
        .bottom = box_model().border.bottom == 0 ? CSS::BorderData() : computed_values().border_bottom(),
        .left = box_model().border.left == 0 ? CSS::BorderData() : computed_values().border_left(),
    };
    paint_all_borders(context.display_list_recorder(), context.rounded_device_rect(absolute_border_box_rect()), normalized_border_radii_data().as_corners(context.device_pixel_converter()), borders_data.to_device_pixels(context));
}

void PaintableBox::paint_backdrop_filter(PaintContext& context) const
{
    auto const& backdrop_filter = computed_values().backdrop_filter();
    if (!backdrop_filter.has_value()) {
        return;
    }

    auto backdrop_region = context.rounded_device_rect(absolute_border_box_rect());
    auto border_radii_data = normalized_border_radii_data();
    ScopedCornerRadiusClip corner_clipper { context, backdrop_region, border_radii_data };
    context.display_list_recorder().apply_backdrop_filter(backdrop_region.to_type<int>(), border_radii_data, backdrop_filter.value());
}

void PaintableBox::paint_background(PaintContext& context) const
{
    // If the body's background properties were propagated to the root element, do no re-paint the body's background.
    if (layout_node_with_style_and_box_metrics().is_body() && document().html_element()->should_use_body_background_properties())
        return;

    Painting::paint_background(context, *this, computed_values().image_rendering(), m_resolved_background, normalized_border_radii_data());
}

void PaintableBox::paint_box_shadow(PaintContext& context) const
{
    auto const& resolved_box_shadow_data = box_shadow_data();
    if (resolved_box_shadow_data.is_empty())
        return;
    auto borders_data = BordersData {
        .top = computed_values().border_top(),
        .right = computed_values().border_right(),
        .bottom = computed_values().border_bottom(),
        .left = computed_values().border_left(),
    };
    Painting::paint_box_shadow(context, absolute_border_box_rect(), absolute_padding_box_rect(),
        borders_data, normalized_border_radii_data(), resolved_box_shadow_data);
}

BorderRadiiData PaintableBox::normalized_border_radii_data(ShrinkRadiiForBorders shrink) const
{
    auto border_radii_data = this->border_radii_data();
    if (shrink == ShrinkRadiiForBorders::Yes)
        border_radii_data.shrink(computed_values().border_top().width, computed_values().border_right().width, computed_values().border_bottom().width, computed_values().border_left().width);
    return border_radii_data;
}

Optional<int> PaintableBox::own_scroll_frame_id() const
{
    if (m_own_scroll_frame)
        return m_own_scroll_frame->id();
    return {};
}

Optional<int> PaintableBox::scroll_frame_id() const
{
    if (m_enclosing_scroll_frame)
        return m_enclosing_scroll_frame->id();
    return {};
}

CSSPixelPoint PaintableBox::cumulative_offset_of_enclosing_scroll_frame() const
{
    if (m_enclosing_scroll_frame)
        return m_enclosing_scroll_frame->cumulative_offset();
    return {};
}

Optional<CSSPixelRect> PaintableBox::clip_rect_for_hit_testing() const
{
    if (m_enclosing_clip_frame)
        return m_enclosing_clip_frame->clip_rect_for_hit_testing();
    return {};
}

void PaintableBox::apply_scroll_offset(PaintContext& context) const
{
    if (scroll_frame_id().has_value()) {
        context.display_list_recorder().push_scroll_frame_id(scroll_frame_id().value());
    }
}

void PaintableBox::reset_scroll_offset(PaintContext& context) const
{
    if (scroll_frame_id().has_value()) {
        context.display_list_recorder().pop_scroll_frame_id();
    }
}

void PaintableBox::apply_clip_overflow_rect(PaintContext& context, PaintPhase phase) const
{
    if (!enclosing_clip_frame())
        return;

    if (!AK::first_is_one_of(phase, PaintPhase::Background, PaintPhase::Border, PaintPhase::TableCollapsedBorder, PaintPhase::Foreground, PaintPhase::Outline))
        return;

    context.display_list_recorder().push_clip_frame(enclosing_clip_frame());
}

void PaintableBox::clear_clip_overflow_rect(PaintContext& context, PaintPhase phase) const
{
    if (!enclosing_clip_frame())
        return;

    if (!AK::first_is_one_of(phase, PaintPhase::Background, PaintPhase::Border, PaintPhase::TableCollapsedBorder, PaintPhase::Foreground, PaintPhase::Outline))
        return;

    context.display_list_recorder().pop_clip_frame();
}

void paint_cursor_if_needed(PaintContext& context, TextPaintable const& paintable, PaintableFragment const& fragment)
{
    auto const& navigable = *paintable.navigable();
    auto const& document = paintable.document();

    if (!navigable.is_focused())
        return;

    if (!document.cursor_blink_state())
        return;

    auto cursor_position = document.cursor_position();
    if (!cursor_position || !cursor_position->node())
        return;

    if (cursor_position->node() != paintable.dom_node())
        return;

    // NOTE: This checks if the cursor is before the start or after the end of the fragment. If it is at the end, after all text, it should still be painted.
    if (cursor_position->offset() < (unsigned)fragment.start_offset() || cursor_position->offset() > (unsigned)(fragment.start_offset() + fragment.length_in_code_units()))
        return;

    auto active_element = document.active_element();
    auto active_element_is_editable = is<HTML::FormAssociatedTextControlElement>(active_element)
        && dynamic_cast<HTML::FormAssociatedTextControlElement const&>(*active_element).is_mutable();

    auto dom_node = fragment.layout_node().dom_node();
    if (!dom_node || (!dom_node->is_editable() && !active_element_is_editable))
        return;

    auto caret_color = paintable.computed_values().caret_color();
    if (caret_color.alpha() == 0)
        return;

    auto fragment_rect = fragment.absolute_rect();
    auto text = fragment.text();

    auto const& font = fragment.glyph_run() ? fragment.glyph_run()->font() : fragment.layout_node().first_available_font();
    auto cursor_offset = font.width(text.substring_view(0, cursor_position->offset() - fragment.start_offset()));

    CSSPixelRect cursor_rect {
        fragment_rect.x() + CSSPixels::nearest_value_for(cursor_offset),
        fragment_rect.top(),
        1,
        fragment_rect.height()
    };

    auto cursor_device_rect = context.rounded_device_rect(cursor_rect).to_type<int>();

    context.display_list_recorder().draw_rect(cursor_device_rect, caret_color);
}

void paint_text_decoration(PaintContext& context, TextPaintable const& paintable, PaintableFragment const& fragment)
{
    auto& painter = context.display_list_recorder();
    auto& font = fragment.layout_node().first_available_font();
    auto fragment_box = fragment.absolute_rect();
    CSSPixels glyph_height = CSSPixels::nearest_value_for(font.pixel_size());
    auto baseline = fragment.baseline();

    auto line_color = paintable.computed_values().text_decoration_color();
    auto line_style = paintable.computed_values().text_decoration_style();
    auto device_line_thickness = context.rounded_device_pixels(fragment.text_decoration_thickness());
    auto text_decoration_lines = paintable.computed_values().text_decoration_line();
    for (auto line : text_decoration_lines) {
        DevicePixelPoint line_start_point {};
        DevicePixelPoint line_end_point {};

        if (line == CSS::TextDecorationLine::SpellingError) {
            // https://drafts.csswg.org/css-text-decor-4/#valdef-text-decoration-line-spelling-error
            // This value indicates the type of text decoration used by the user agent to highlight spelling mistakes.
            // Its appearance is UA-defined, and may be platform-dependent. It is often rendered as a red wavy underline.
            line_color = Color::Red;
            device_line_thickness = context.rounded_device_pixels(1);
            line_style = CSS::TextDecorationStyle::Wavy;
            line = CSS::TextDecorationLine::Underline;
        } else if (line == CSS::TextDecorationLine::GrammarError) {
            // https://drafts.csswg.org/css-text-decor-4/#valdef-text-decoration-line-grammar-error
            // This value indicates the type of text decoration used by the user agent to highlight grammar mistakes.
            // Its appearance is UA defined, and may be platform-dependent. It is often rendered as a green wavy underline.
            line_color = Color::DarkGreen;
            device_line_thickness = context.rounded_device_pixels(1);
            line_style = CSS::TextDecorationStyle::Wavy;
            line = CSS::TextDecorationLine::Underline;
        }

        switch (line) {
        case CSS::TextDecorationLine::None:
            return;
        case CSS::TextDecorationLine::Underline:
            line_start_point = context.rounded_device_point(fragment_box.top_left().translated(0, baseline + 2));
            line_end_point = context.rounded_device_point(fragment_box.top_right().translated(-1, baseline + 2));
            break;
        case CSS::TextDecorationLine::Overline:
            line_start_point = context.rounded_device_point(fragment_box.top_left().translated(0, baseline - glyph_height));
            line_end_point = context.rounded_device_point(fragment_box.top_right().translated(-1, baseline - glyph_height));
            break;
        case CSS::TextDecorationLine::LineThrough: {
            auto x_height = font.x_height();
            line_start_point = context.rounded_device_point(fragment_box.top_left().translated(0, baseline - x_height * CSSPixels(0.5f)));
            line_end_point = context.rounded_device_point(fragment_box.top_right().translated(-1, baseline - x_height * CSSPixels(0.5f)));
            break;
        }
        case CSS::TextDecorationLine::Blink:
            // Conforming user agents may simply not blink the text
            return;
        case CSS::TextDecorationLine::SpellingError:
        case CSS::TextDecorationLine::GrammarError:
            // Handled above.
            VERIFY_NOT_REACHED();
        }

        switch (line_style) {
        case CSS::TextDecorationStyle::Solid:
            painter.draw_line(line_start_point.to_type<int>(), line_end_point.to_type<int>(), line_color, device_line_thickness.value(), Gfx::LineStyle::Solid);
            break;
        case CSS::TextDecorationStyle::Double:
            switch (line) {
            case CSS::TextDecorationLine::Underline:
                break;
            case CSS::TextDecorationLine::Overline:
                line_start_point.translate_by(0, -device_line_thickness - context.rounded_device_pixels(1));
                line_end_point.translate_by(0, -device_line_thickness - context.rounded_device_pixels(1));
                break;
            case CSS::TextDecorationLine::LineThrough:
                line_start_point.translate_by(0, -device_line_thickness / 2);
                line_end_point.translate_by(0, -device_line_thickness / 2);
                break;
            default:
                VERIFY_NOT_REACHED();
            }

            painter.draw_line(line_start_point.to_type<int>(), line_end_point.to_type<int>(), line_color, device_line_thickness.value());
            painter.draw_line(line_start_point.translated(0, device_line_thickness + 1).to_type<int>(), line_end_point.translated(0, device_line_thickness + 1).to_type<int>(), line_color, device_line_thickness.value());
            break;
        case CSS::TextDecorationStyle::Dashed:
            painter.draw_line(line_start_point.to_type<int>(), line_end_point.to_type<int>(), line_color, device_line_thickness.value(), Gfx::LineStyle::Dashed);
            break;
        case CSS::TextDecorationStyle::Dotted:
            painter.draw_line(line_start_point.to_type<int>(), line_end_point.to_type<int>(), line_color, device_line_thickness.value(), Gfx::LineStyle::Dotted);
            break;
        case CSS::TextDecorationStyle::Wavy:
            auto amplitude = device_line_thickness.value() * 3;
            switch (line) {
            case CSS::TextDecorationLine::Underline:
                line_start_point.translate_by(0, device_line_thickness + context.rounded_device_pixels(1));
                line_end_point.translate_by(0, device_line_thickness + context.rounded_device_pixels(1));
                break;
            case CSS::TextDecorationLine::Overline:
                line_start_point.translate_by(0, -device_line_thickness - context.rounded_device_pixels(1));
                line_end_point.translate_by(0, -device_line_thickness - context.rounded_device_pixels(1));
                break;
            case CSS::TextDecorationLine::LineThrough:
                line_start_point.translate_by(0, -device_line_thickness / 2);
                line_end_point.translate_by(0, -device_line_thickness / 2);
                break;
            default:
                VERIFY_NOT_REACHED();
            }
            painter.draw_triangle_wave(line_start_point.to_type<int>(), line_end_point.to_type<int>(), line_color, amplitude, device_line_thickness.value());
            break;
        }
    }
}

void paint_text_fragment(PaintContext& context, TextPaintable const& paintable, PaintableFragment const& fragment, PaintPhase phase)
{
    if (!paintable.is_visible())
        return;

    auto& painter = context.display_list_recorder();

    if (phase == PaintPhase::Foreground) {
        auto fragment_absolute_rect = fragment.absolute_rect();
        auto fragment_absolute_device_rect = context.enclosing_device_rect(fragment_absolute_rect);

        if (paintable.document().highlighted_layout_node() == &paintable.layout_node())
            context.display_list_recorder().draw_rect(fragment_absolute_device_rect.to_type<int>(), Color::Magenta);

        auto glyph_run = fragment.glyph_run();
        if (!glyph_run)
            return;

        auto scale = context.device_pixels_per_css_pixel();
        auto baseline_start = Gfx::FloatPoint {
            fragment_absolute_rect.x().to_float(),
            fragment_absolute_rect.y().to_float() + fragment.baseline().to_float(),
        } * scale;
        painter.draw_glyph_run(baseline_start, *glyph_run, paintable.computed_values().webkit_text_fill_color(), fragment_absolute_device_rect.to_type<int>(), scale, fragment.orientation());

        auto selection_rect = context.enclosing_device_rect(fragment.selection_rect()).to_type<int>();
        if (!selection_rect.is_empty()) {
            painter.fill_rect(selection_rect, CSS::SystemColor::highlight(paintable.computed_values().color_scheme()));
            DisplayListRecorderStateSaver saver(painter);
            painter.add_clip_rect(selection_rect);
            painter.draw_glyph_run(baseline_start, *glyph_run, CSS::SystemColor::highlight_text(paintable.computed_values().color_scheme()), fragment_absolute_device_rect.to_type<int>(), scale, fragment.orientation());
        }

        paint_text_decoration(context, paintable, fragment);
        paint_cursor_if_needed(context, paintable, fragment);
    }
}

void PaintableWithLines::paint(PaintContext& context, PaintPhase phase) const
{
    if (!is_visible())
        return;

    PaintableBox::paint(context, phase);

    // Text shadows
    // This is yet another loop, but done here because all shadows should appear under all text.
    // So, we paint the shadows before painting any text.
    // FIXME: Find a smarter way to do this?
    if (phase == PaintPhase::Foreground) {
        for (auto& fragment : fragments())
            paint_text_shadow(context, fragment, fragment.shadows());
    }

    for (auto const& fragment : m_fragments) {
        auto fragment_absolute_rect = fragment.absolute_rect();
        if (context.should_show_line_box_borders()) {
            auto fragment_absolute_device_rect = context.enclosing_device_rect(fragment_absolute_rect);
            context.display_list_recorder().draw_rect(fragment_absolute_device_rect.to_type<int>(), Color::Green);
            context.display_list_recorder().draw_line(
                context.rounded_device_point(fragment_absolute_rect.top_left().translated(0, fragment.baseline())).to_type<int>(),
                context.rounded_device_point(fragment_absolute_rect.top_right().translated(-1, fragment.baseline())).to_type<int>(), Color::Red);
        }
        if (is<TextPaintable>(fragment.paintable()))
            paint_text_fragment(context, static_cast<TextPaintable const&>(fragment.paintable()), fragment, phase);
    }
}

Paintable::DispatchEventOfSameName PaintableBox::handle_mousedown(Badge<EventHandler>, CSSPixelPoint position, unsigned, unsigned)
{
    position = adjust_position_for_cumulative_scroll_offset(position);

    auto handle_scrollbar = [&](auto direction) {
        auto scrollbar_data = compute_scrollbar_data(direction);
        if (!scrollbar_data.has_value())
            return false;

        if (scrollbar_data->gutter_rect.contains(position)) {
            m_scroll_thumb_dragging_direction = direction;

            navigable()->event_handler().set_mouse_event_tracking_paintable(this);
            scroll_to_mouse_position(position);
            return true;
        }

        return false;
    };

    if (handle_scrollbar(ScrollDirection::Vertical))
        return Paintable::DispatchEventOfSameName::No;
    if (handle_scrollbar(ScrollDirection::Horizontal))
        return Paintable::DispatchEventOfSameName::No;

    return Paintable::DispatchEventOfSameName::Yes;
}

Paintable::DispatchEventOfSameName PaintableBox::handle_mouseup(Badge<EventHandler>, CSSPixelPoint, unsigned, unsigned)
{
    if (m_scroll_thumb_grab_position.has_value()) {
        m_scroll_thumb_grab_position.clear();
        m_scroll_thumb_dragging_direction.clear();
        navigable()->event_handler().set_mouse_event_tracking_paintable(nullptr);
    }
    return Paintable::DispatchEventOfSameName::Yes;
}

Paintable::DispatchEventOfSameName PaintableBox::handle_mousemove(Badge<EventHandler>, CSSPixelPoint position, unsigned, unsigned)
{
    position = adjust_position_for_cumulative_scroll_offset(position);

    if (m_scroll_thumb_grab_position.has_value()) {
        scroll_to_mouse_position(position);
        return Paintable::DispatchEventOfSameName::No;
    }

    auto previous_draw_enlarged_horizontal_scrollbar = m_draw_enlarged_horizontal_scrollbar;
    m_draw_enlarged_horizontal_scrollbar = scrollbar_contains_mouse_position(ScrollDirection::Horizontal, position);
    if (previous_draw_enlarged_horizontal_scrollbar != m_draw_enlarged_horizontal_scrollbar)
        set_needs_display();

    auto previous_draw_enlarged_vertical_scrollbar = m_draw_enlarged_vertical_scrollbar;
    m_draw_enlarged_vertical_scrollbar = scrollbar_contains_mouse_position(ScrollDirection::Vertical, position);
    if (previous_draw_enlarged_vertical_scrollbar != m_draw_enlarged_vertical_scrollbar)
        set_needs_display();

    if (m_draw_enlarged_horizontal_scrollbar || m_draw_enlarged_vertical_scrollbar)
        return Paintable::DispatchEventOfSameName::No;

    return Paintable::DispatchEventOfSameName::Yes;
}

void PaintableBox::handle_mouseleave(Badge<EventHandler>)
{
    auto previous_draw_enlarged_horizontal_scrollbar = m_draw_enlarged_horizontal_scrollbar;
    m_draw_enlarged_horizontal_scrollbar = false;
    if (previous_draw_enlarged_horizontal_scrollbar != m_draw_enlarged_horizontal_scrollbar)
        set_needs_display();

    auto previous_draw_enlarged_vertical_scrollbar = m_draw_enlarged_vertical_scrollbar;
    m_draw_enlarged_vertical_scrollbar = false;
    if (previous_draw_enlarged_vertical_scrollbar != m_draw_enlarged_vertical_scrollbar)
        set_needs_display();
}

bool PaintableBox::scrollbar_contains_mouse_position(ScrollDirection direction, CSSPixelPoint position)
{
    TemporaryChange force_enlarged_horizontal_scrollbar { m_draw_enlarged_horizontal_scrollbar, true };
    TemporaryChange force_enlarged_vertical_scrollbar { m_draw_enlarged_vertical_scrollbar, true };

    auto scrollbar_data = compute_scrollbar_data(direction);
    if (!scrollbar_data.has_value())
        return false;

    return scrollbar_data->gutter_rect.contains(position);
}

void PaintableBox::scroll_to_mouse_position(CSSPixelPoint position)
{
    VERIFY(m_scroll_thumb_dragging_direction.has_value());

    auto scrollbar_data = compute_scrollbar_data(m_scroll_thumb_dragging_direction.value(), AdjustThumbRectForScrollOffset::Yes);
    VERIFY(scrollbar_data.has_value());

    auto orientation = m_scroll_thumb_dragging_direction == ScrollDirection::Horizontal ? Orientation::Horizontal : Orientation::Vertical;
    auto offset_relative_to_gutter = (position - scrollbar_data->gutter_rect.location()).primary_offset_for_orientation(orientation);
    auto gutter_size = scrollbar_data->gutter_rect.primary_size_for_orientation(orientation);
    auto thumb_size = scrollbar_data->thumb_rect.primary_size_for_orientation(orientation);

    // Set the thumb grab position, if we haven't got one already.
    if (!m_scroll_thumb_grab_position.has_value()) {
        m_scroll_thumb_grab_position = scrollbar_data->thumb_rect.contains(position)
            ? (position - scrollbar_data->thumb_rect.location()).primary_offset_for_orientation(orientation)
            : max(min(offset_relative_to_gutter, thumb_size / 2), offset_relative_to_gutter - gutter_size + thumb_size);
    }

    // Calculate the relative scroll position (0..1) based on the position of the mouse cursor. We only move the thumb
    // if we are interacting with the grab point on the thumb. E.g. if the thumb is all the way to its minimum position
    // and the position is beyond the grab point, we should do nothing.
    auto constrained_offset = AK::clamp(offset_relative_to_gutter - m_scroll_thumb_grab_position.value(), 0, gutter_size - thumb_size);
    auto scroll_position = constrained_offset.to_double() / (gutter_size - thumb_size).to_double();

    // Calculate the scroll offset we need to apply to the viewport or element.
    auto scrollable_overflow_size = scrollable_overflow_rect()->primary_size_for_orientation(orientation);
    auto padding_size = absolute_padding_box_rect().primary_size_for_orientation(orientation);
    auto scroll_position_in_pixels = CSSPixels::nearest_value_for(scroll_position * (scrollable_overflow_size - padding_size));

    // Set the new scroll offset.
    auto new_scroll_offset = is_viewport() ? document().navigable()->viewport_scroll_offset() : scroll_offset();
    new_scroll_offset.set_primary_offset_for_orientation(orientation, scroll_position_in_pixels);

    if (is_viewport())
        document().navigable()->perform_scroll_of_viewport(new_scroll_offset);
    else
        set_scroll_offset(new_scroll_offset);
}

bool PaintableBox::handle_mousewheel(Badge<EventHandler>, CSSPixelPoint, unsigned, unsigned, int wheel_delta_x, int wheel_delta_y)
{
    // if none of the axes we scrolled with can be accepted by this element, don't handle scroll.
    if ((!wheel_delta_x || !could_be_scrolled_by_wheel_event(ScrollDirection::Horizontal)) && (!wheel_delta_y || !could_be_scrolled_by_wheel_event(ScrollDirection::Vertical))) {
        return false;
    }

    scroll_by(wheel_delta_x, wheel_delta_y);
    return true;
}

Layout::NodeWithStyleAndBoxModelMetrics const& PaintableWithLines::layout_node_with_style_and_box_metrics() const
{
    return static_cast<Layout::NodeWithStyleAndBoxModelMetrics const&>(PaintableBox::layout_node_with_style_and_box_metrics());
}

Layout::NodeWithStyleAndBoxModelMetrics& PaintableWithLines::layout_node_with_style_and_box_metrics()
{
    return static_cast<Layout::NodeWithStyleAndBoxModelMetrics&>(PaintableBox::layout_node_with_style_and_box_metrics());
}

TraversalDecision PaintableBox::hit_test_scrollbars(CSSPixelPoint position, Function<TraversalDecision(HitTestResult)> const& callback) const
{
    // FIXME: This const_cast is not great, but this method is invoked from overrides of virtual const methods.
    auto& self = const_cast<PaintableBox&>(*this);

    if (self.scrollbar_contains_mouse_position(ScrollDirection::Horizontal, position))
        return callback(HitTestResult { const_cast<PaintableBox&>(*this) });

    if (m_draw_enlarged_horizontal_scrollbar) {
        self.m_draw_enlarged_horizontal_scrollbar = false;
        self.set_needs_display();
    }

    if (self.scrollbar_contains_mouse_position(ScrollDirection::Vertical, position))
        return callback(HitTestResult { const_cast<PaintableBox&>(*this) });

    if (m_draw_enlarged_vertical_scrollbar) {
        self.m_draw_enlarged_vertical_scrollbar = false;
        self.set_needs_display();
    }

    return TraversalDecision::Continue;
}

CSSPixelPoint PaintableBox::adjust_position_for_cumulative_scroll_offset(CSSPixelPoint position) const
{
    return position.translated(-cumulative_offset_of_enclosing_scroll_frame());
}

TraversalDecision PaintableBox::hit_test(CSSPixelPoint position, HitTestType type, Function<TraversalDecision(HitTestResult)> const& callback) const
{
    if (clip_rect_for_hit_testing().has_value() && !clip_rect_for_hit_testing()->contains(position))
        return TraversalDecision::Continue;

    auto position_adjusted_by_scroll_offset = adjust_position_for_cumulative_scroll_offset(position);

    if (computed_values().visibility() != CSS::Visibility::Visible)
        return TraversalDecision::Continue;

    if (hit_test_scrollbars(position_adjusted_by_scroll_offset, callback) == TraversalDecision::Break)
        return TraversalDecision::Break;

    if (is_viewport()) {
        auto& viewport_paintable = const_cast<ViewportPaintable&>(static_cast<ViewportPaintable const&>(*this));
        viewport_paintable.build_stacking_context_tree_if_needed();
        viewport_paintable.document().update_paint_and_hit_testing_properties_if_needed();
        viewport_paintable.refresh_scroll_state();
        return stacking_context()->hit_test(position, type, callback);
    }

    if (hit_test_children(position, type, callback) == TraversalDecision::Break)
        return TraversalDecision::Break;

    if (!visible_for_hit_testing())
        return TraversalDecision::Continue;

    if (!absolute_border_box_rect().contains(position_adjusted_by_scroll_offset))
        return TraversalDecision::Continue;

    if (hit_test_continuation(callback) == TraversalDecision::Break)
        return TraversalDecision::Break;

    return callback(HitTestResult { const_cast<PaintableBox&>(*this) });
}

TraversalDecision PaintableBox::hit_test_continuation(Function<TraversalDecision(HitTestResult)> const& callback) const
{
    // If we're hit testing the "middle" part of a continuation chain, we are dealing with an anonymous box that is
    // linked to a parent inline node. Since our block element children did not match the hit test, but we did, we
    // should walk the continuation chain up to the inline parent and return a hit on that instead.
    auto continuation_node = layout_node_with_style_and_box_metrics().continuation_of_node();
    if (!continuation_node || !layout_node().is_anonymous())
        return TraversalDecision::Continue;

    while (continuation_node->continuation_of_node())
        continuation_node = continuation_node->continuation_of_node();
    auto& paintable = *continuation_node->first_paintable();
    if (!paintable.visible_for_hit_testing())
        return TraversalDecision::Continue;

    return callback(HitTestResult { paintable });
}

Optional<HitTestResult> PaintableBox::hit_test(CSSPixelPoint position, HitTestType type) const
{
    Optional<HitTestResult> result;
    (void)PaintableBox::hit_test(position, type, [&](HitTestResult candidate) {
        if (!result.has_value()
            || candidate.vertical_distance.value_or(CSSPixels::max_integer_value) < result->vertical_distance.value_or(CSSPixels::max_integer_value)
            || candidate.horizontal_distance.value_or(CSSPixels::max_integer_value) < result->horizontal_distance.value_or(CSSPixels::max_integer_value)) {
            result = move(candidate);
        }

        if (result.has_value() && (type == HitTestType::Exact || (result->vertical_distance == 0 && result->horizontal_distance == 0)))
            return TraversalDecision::Break;
        return TraversalDecision::Continue;
    });
    return result;
}

TraversalDecision PaintableBox::hit_test_children(CSSPixelPoint position, HitTestType type, Function<TraversalDecision(HitTestResult)> const& callback) const
{
    for (auto const* child = last_child(); child; child = child->previous_sibling()) {
        if (child->layout_node().is_positioned() && child->computed_values().z_index().value_or(0) == 0)
            continue;
        if (child->hit_test(position, type, callback) == TraversalDecision::Break)
            return TraversalDecision::Break;
    }
    return TraversalDecision::Continue;
}

TraversalDecision PaintableWithLines::hit_test(CSSPixelPoint position, HitTestType type, Function<TraversalDecision(HitTestResult)> const& callback) const
{
    if (clip_rect_for_hit_testing().has_value() && !clip_rect_for_hit_testing()->contains(position))
        return TraversalDecision::Continue;

    auto position_adjusted_by_scroll_offset = adjust_position_for_cumulative_scroll_offset(position);

    // TextCursor hit testing mode should be able to place cursor in contenteditable elements even if they are empty
    if (m_fragments.is_empty()
        && !has_children()
        && type == HitTestType::TextCursor
        && layout_node().dom_node()
        && layout_node().dom_node()->is_editable()) {
        HitTestResult const hit_test_result {
            .paintable = const_cast<PaintableWithLines&>(*this),
            .index_in_node = 0,
            .vertical_distance = 0,
            .horizontal_distance = 0,
        };
        if (callback(hit_test_result) == TraversalDecision::Break)
            return TraversalDecision::Break;
    }

    if (!layout_node().children_are_inline())
        return PaintableBox::hit_test(position, type, callback);

    // NOTE: This CSSPixels -> Float -> CSSPixels conversion is because we can't AffineTransform::map() a CSSPixelPoint.
    auto offset_position = position_adjusted_by_scroll_offset.translated(-transform_origin()).to_type<float>();
    auto transformed_position_adjusted_by_scroll_offset = combined_css_transform().inverse().value_or({}).map(offset_position).to_type<CSSPixels>() + transform_origin();

    if (hit_test_scrollbars(transformed_position_adjusted_by_scroll_offset, callback) == TraversalDecision::Break)
        return TraversalDecision::Break;

    if (hit_test_children(position, type, callback) == TraversalDecision::Break)
        return TraversalDecision::Break;

    if (!visible_for_hit_testing())
        return TraversalDecision::Continue;

    for (auto const& fragment : fragments()) {
        if (fragment.paintable().has_stacking_context() || !fragment.paintable().visible_for_hit_testing())
            continue;
        auto fragment_absolute_rect = fragment.absolute_rect();
        if (fragment_absolute_rect.contains(transformed_position_adjusted_by_scroll_offset)) {
            if (fragment.paintable().hit_test(transformed_position_adjusted_by_scroll_offset, type, callback) == TraversalDecision::Break)
                return TraversalDecision::Break;
            HitTestResult hit_test_result { const_cast<Paintable&>(fragment.paintable()), fragment.index_in_node_for_point(transformed_position_adjusted_by_scroll_offset), 0, 0 };
            if (callback(hit_test_result) == TraversalDecision::Break)
                return TraversalDecision::Break;
        } else if (type == HitTestType::TextCursor) {
            auto const* common_ancestor_parent = [&]() -> DOM::Node const* {
                auto selection = document().get_selection();
                if (!selection)
                    return nullptr;
                auto range = selection->range();
                if (!range)
                    return nullptr;
                auto common_ancestor = range->common_ancestor_container();
                if (common_ancestor->parent())
                    return common_ancestor->parent();
                return common_ancestor;
            }();

            auto const* fragment_dom_node = fragment.layout_node().dom_node();
            if (common_ancestor_parent && fragment_dom_node && common_ancestor_parent->is_ancestor_of(*fragment_dom_node)) {
                // If we reached this point, the position is not within the fragment. However, the fragment start or end might be
                // the place to place the cursor. To determine the best place, we first find the closest fragment horizontally to
                // the cursor. If we could not find one, then find for the closest vertically above the cursor.
                // If we knew the direction of selection, we would look above if selecting upward.
                if (fragment_absolute_rect.bottom() - 1 <= transformed_position_adjusted_by_scroll_offset.y()) { // fully below the fragment
                    HitTestResult hit_test_result {
                        .paintable = const_cast<Paintable&>(fragment.paintable()),
                        .index_in_node = fragment.start_offset() + fragment.length_in_code_units(),
                        .vertical_distance = transformed_position_adjusted_by_scroll_offset.y() - fragment_absolute_rect.bottom(),
                    };
                    if (callback(hit_test_result) == TraversalDecision::Break)
                        return TraversalDecision::Break;
                } else if (fragment_absolute_rect.top() <= transformed_position_adjusted_by_scroll_offset.y()) { // vertically within the fragment
                    if (transformed_position_adjusted_by_scroll_offset.x() < fragment_absolute_rect.left()) {
                        HitTestResult hit_test_result {
                            .paintable = const_cast<Paintable&>(fragment.paintable()),
                            .index_in_node = fragment.start_offset(),
                            .vertical_distance = 0,
                            .horizontal_distance = fragment_absolute_rect.left() - transformed_position_adjusted_by_scroll_offset.x(),
                        };
                        if (callback(hit_test_result) == TraversalDecision::Break)
                            return TraversalDecision::Break;
                    } else if (transformed_position_adjusted_by_scroll_offset.x() > fragment_absolute_rect.right()) {
                        HitTestResult hit_test_result {
                            .paintable = const_cast<Paintable&>(fragment.paintable()),
                            .index_in_node = fragment.start_offset() + fragment.length_in_code_units(),
                            .vertical_distance = 0,
                            .horizontal_distance = transformed_position_adjusted_by_scroll_offset.x() - fragment_absolute_rect.right(),
                        };
                        if (callback(hit_test_result) == TraversalDecision::Break)
                            return TraversalDecision::Break;
                    }
                }
            }
        }
    }

    if (!stacking_context() && is_visible() && (!layout_node().is_anonymous() || layout_node().is_positioned())
        && absolute_border_box_rect().contains(position_adjusted_by_scroll_offset)) {
        if (callback(HitTestResult { const_cast<PaintableWithLines&>(*this) }) == TraversalDecision::Break)
            return TraversalDecision::Break;
    }

    return TraversalDecision::Continue;
}

void PaintableBox::set_needs_display(InvalidateDisplayList should_invalidate_display_list)
{
    document().set_needs_display(absolute_rect(), should_invalidate_display_list);
}

Optional<CSSPixelRect> PaintableBox::get_masking_area() const
{
    auto clip_path = computed_values().clip_path();
    // FIXME: Support other clip sources.
    if (!clip_path.has_value() || !clip_path->is_basic_shape())
        return {};
    // FIXME: Support other geometry boxes. See: https://drafts.fxtf.org/css-masking/#typedef-geometry-box
    return absolute_border_box_rect();
}

// https://www.w3.org/TR/css-transforms-1/#transform-box
CSSPixelRect PaintableBox::transform_box_rect() const
{
    auto transform_box = computed_values().transform_box();
    // For SVG elements without associated CSS layout box, the used value for content-box is fill-box and for
    // border-box is stroke-box.
    // FIXME: This currently detects any SVG element except the <svg> one. Is that correct?
    //        And is it correct to use `else` below?
    if (is<Painting::SVGPaintable>(*this)) {
        switch (transform_box) {
        case CSS::TransformBox::ContentBox:
            transform_box = CSS::TransformBox::FillBox;
            break;
        case CSS::TransformBox::BorderBox:
            transform_box = CSS::TransformBox::StrokeBox;
            break;
        default:
            break;
        }
    }
    // For elements with associated CSS layout box, the used value for fill-box is content-box and for
    // stroke-box and view-box is border-box.
    else {
        switch (transform_box) {
        case CSS::TransformBox::FillBox:
            transform_box = CSS::TransformBox::ContentBox;
            break;
        case CSS::TransformBox::StrokeBox:
        case CSS::TransformBox::ViewBox:
            transform_box = CSS::TransformBox::BorderBox;
            break;
        default:
            break;
        }
    }

    switch (transform_box) {
    case CSS::TransformBox::ContentBox:
        // Uses the content box as reference box.
        // FIXME: The reference box of a table is the border box of its table wrapper box, not its table box.
        return absolute_rect();
    case CSS::TransformBox::BorderBox:
        // Uses the border box as reference box.
        // FIXME: The reference box of a table is the border box of its table wrapper box, not its table box.
        return absolute_border_box_rect();
    case CSS::TransformBox::FillBox:
        // Uses the object bounding box as reference box.
        // FIXME: For now we're using the content rect as an approximation.
        return absolute_rect();
    case CSS::TransformBox::StrokeBox:
        // Uses the stroke bounding box as reference box.
        // FIXME: For now we're using the border rect as an approximation.
        return absolute_border_box_rect();
    case CSS::TransformBox::ViewBox:
        // Uses the nearest SVG viewport as reference box.
        // FIXME: If a viewBox attribute is specified for the SVG viewport creating element:
        //  - The reference box is positioned at the origin of the coordinate system established by the viewBox attribute.
        //  - The dimension of the reference box is set to the width and height values of the viewBox attribute.
        auto* svg_paintable = first_ancestor_of_type<Painting::SVGSVGPaintable>();
        if (!svg_paintable)
            return absolute_border_box_rect();
        return svg_paintable->absolute_rect();
    }
    VERIFY_NOT_REACHED();
}

void PaintableBox::resolve_paint_properties()
{
    Base::resolve_paint_properties();

    auto const& computed_values = this->computed_values();
    auto const& layout_node = this->layout_node();

    // Border radii
    BorderRadiiData radii_data {};
    if (computed_values.has_noninitial_border_radii()) {
        CSSPixelRect const border_rect { 0, 0, border_box_width(), border_box_height() };

        auto const& border_top_left_radius = computed_values.border_top_left_radius();
        auto const& border_top_right_radius = computed_values.border_top_right_radius();
        auto const& border_bottom_right_radius = computed_values.border_bottom_right_radius();
        auto const& border_bottom_left_radius = computed_values.border_bottom_left_radius();

        radii_data = normalize_border_radii_data(layout_node, border_rect, border_top_left_radius,
            border_top_right_radius, border_bottom_right_radius,
            border_bottom_left_radius);
    }
    set_border_radii_data(radii_data);

    // Box shadows
    auto const& box_shadow_data = computed_values.box_shadow();
    Vector<Painting::ShadowData> resolved_box_shadow_data;
    resolved_box_shadow_data.ensure_capacity(box_shadow_data.size());
    for (auto const& layer : box_shadow_data) {
        resolved_box_shadow_data.empend(
            layer.color,
            layer.offset_x.to_px(layout_node),
            layer.offset_y.to_px(layout_node),
            layer.blur_radius.to_px(layout_node),
            layer.spread_distance.to_px(layout_node),
            layer.placement == CSS::ShadowPlacement::Outer ? Painting::ShadowPlacement::Outer
                                                           : Painting::ShadowPlacement::Inner);
    }
    set_box_shadow_data(move(resolved_box_shadow_data));

    auto const& transformations = computed_values.transformations();
    auto const& translate = computed_values.translate();
    auto const& rotate = computed_values.rotate();
    auto const& scale = computed_values.scale();
    auto matrix = Gfx::FloatMatrix4x4::identity();
    if (translate.has_value())
        matrix = matrix * translate->to_matrix(*this).release_value();
    if (rotate.has_value())
        matrix = matrix * rotate->to_matrix(*this).release_value();
    if (scale.has_value())
        matrix = matrix * scale->to_matrix(*this).release_value();
    for (auto const& transform : transformations)
        matrix = matrix * transform.to_matrix(*this).release_value();
    set_transform(matrix);

    auto const& transform_origin = computed_values.transform_origin();
    auto reference_box = transform_box_rect();
    auto x = reference_box.left() + transform_origin.x.to_px(layout_node, reference_box.width());
    auto y = reference_box.top() + transform_origin.y.to_px(layout_node, reference_box.height());
    set_transform_origin({ x, y });
    set_transform_origin({ x, y });

    // Outlines
    auto outline_width = computed_values.outline_width().to_px(layout_node);
    auto outline_data = borders_data_for_outline(layout_node, computed_values.outline_color(), computed_values.outline_style(), outline_width);
    auto outline_offset = computed_values.outline_offset().to_px(layout_node);
    set_outline_data(outline_data);
    set_outline_offset(outline_offset);

    auto combined_transform = compute_combined_css_transform();
    set_combined_css_transform(combined_transform);

    CSSPixelRect background_rect;
    Color background_color = computed_values.background_color();
    auto const* background_layers = &computed_values.background_layers();
    if (layout_node_with_style_and_box_metrics().is_root_element()) {
        background_rect = navigable()->viewport_rect();

        // Section 2.11.2: If the computed value of background-image on the root element is none and its background-color is transparent,
        // user agents must instead propagate the computed values of the background properties from that element’s first HTML BODY child element.
        if (document().html_element()->should_use_body_background_properties()) {
            background_layers = document().background_layers();
            background_color = document().background_color();
        }
    } else {
        background_rect = absolute_padding_box_rect();
    }

    // HACK: If the Box has a border, use the bordered_rect to paint the background.
    //       This way if we have a border-radius there will be no gap between the filling and actual border.
    if (computed_values.border_top().width != 0 || computed_values.border_right().width != 0 || computed_values.border_bottom().width != 0 || computed_values.border_left().width != 0)
        background_rect = absolute_border_box_rect();

    m_resolved_background.layers.clear();
    if (background_layers) {
        m_resolved_background = resolve_background_layers(*background_layers, *this, background_color, background_rect, normalized_border_radii_data());
    };

    if (auto mask_image = computed_values.mask_image()) {
        mask_image->resolve_for_size(layout_node_with_style_and_box_metrics(), absolute_padding_box_rect().size());
    }
}

void PaintableWithLines::resolve_paint_properties()
{
    Base::resolve_paint_properties();

    auto const& layout_node = this->layout_node();
    for (auto& fragment : fragments()) {
        if (!fragment.m_layout_node->is_text_node())
            continue;
        auto const& text_node = static_cast<Layout::TextNode const&>(*fragment.m_layout_node);

        auto const& font = fragment.m_layout_node->first_available_font();
        auto const glyph_height = CSSPixels::nearest_value_for(font.pixel_size());
        auto const css_line_thickness = [&] {
            auto computed_thickness = text_node.computed_values().text_decoration_thickness().resolved(text_node, CSS::Length(1, CSS::Length::Type::Em).to_px(text_node));
            if (computed_thickness.is_auto())
                return max(glyph_height.scaled(0.1), 1);
            return computed_thickness.to_px(*fragment.m_layout_node);
        }();
        fragment.set_text_decoration_thickness(css_line_thickness);

        auto const& text_shadow = text_node.computed_values().text_shadow();
        if (!text_shadow.is_empty()) {
            Vector<ShadowData> resolved_shadow_data;
            resolved_shadow_data.ensure_capacity(text_shadow.size());
            for (auto const& layer : text_shadow) {
                resolved_shadow_data.empend(
                    layer.color,
                    layer.offset_x.to_px(layout_node),
                    layer.offset_y.to_px(layout_node),
                    layer.blur_radius.to_px(layout_node),
                    layer.spread_distance.to_px(layout_node),
                    ShadowPlacement::Outer);
            }
            fragment.set_shadows(move(resolved_shadow_data));
        }
    }
}

RefPtr<ScrollFrame const> PaintableBox::nearest_scroll_frame() const
{
    if (is_fixed_position())
        return nullptr;
    auto const* paintable = this->containing_block();
    while (paintable) {
        if (paintable->own_scroll_frame())
            return paintable->own_scroll_frame();
        if (paintable->is_fixed_position())
            return nullptr;
        paintable = paintable->containing_block();
    }
    return nullptr;
}

CSSPixelRect PaintableBox::border_box_rect_relative_to_nearest_scrollable_ancestor() const
{
    auto result = absolute_border_box_rect();
    auto const* nearest_scrollable_ancestor = this->nearest_scrollable_ancestor();
    if (nearest_scrollable_ancestor) {
        result.set_location(result.location() - nearest_scrollable_ancestor->absolute_rect().top_left());
    }
    return result;
}

PaintableBox const* PaintableBox::nearest_scrollable_ancestor() const
{
    auto const* paintable = this->containing_block();
    while (paintable) {
        if (paintable->could_be_scrolled_by_wheel_event())
            return paintable;
        if (paintable->is_fixed_position())
            return nullptr;
        paintable = paintable->containing_block();
    }
    return nullptr;
}

}
