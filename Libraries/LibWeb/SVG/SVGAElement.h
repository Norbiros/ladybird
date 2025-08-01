/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/SVGGraphicsElement.h>
#include <LibWeb/SVG/SVGURIReference.h>

namespace Web::SVG {

class SVGAElement final
    : public SVGGraphicsElement
    , public SVGURIReferenceMixin<SupportsXLinkHref::Yes> {
    WEB_PLATFORM_OBJECT(SVGAElement, SVGGraphicsElement);
    GC_DECLARE_ALLOCATOR(SVGAElement);

public:
    virtual ~SVGAElement() override;

    GC::Ref<SVGAnimatedString> target();

    GC::Ref<DOM::DOMTokenList> rel_list();

    virtual GC::Ptr<Layout::Node> create_layout_node(GC::Ref<CSS::ComputedProperties>) override;

private:
    SVGAElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    virtual bool is_svg_a_element() const override { return true; }

    // ^DOM::Element
    virtual void attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_) override;
    virtual i32 default_tab_index_value() const override;

    GC::Ptr<DOM::DOMTokenList> m_rel_list;

    GC::Ptr<SVGAnimatedString> m_target;
};

}

namespace Web::DOM {

template<>
inline bool Node::fast_is<SVG::SVGAElement>() const { return is_svg_a_element(); }

}
