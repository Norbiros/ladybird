/*
 * Copyright (c) 2023, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2022, Martin Falisse <mfalisse@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibWeb/CSS/CalculatedOr.h>

namespace Web::CSS {

class GridTrackPlacement {
public:
    static GridTrackPlacement make_auto()
    {
        return GridTrackPlacement();
    }

    static GridTrackPlacement make_line(Optional<IntegerOrCalculated> line_number, Optional<String> name)
    {
        return GridTrackPlacement(AreaOrLine { .line_number = line_number, .name = name });
    }

    static GridTrackPlacement make_span(IntegerOrCalculated value, Optional<String> name)
    {
        return GridTrackPlacement(Span { .value = value, .name = name });
    }

    bool is_auto() const { return m_value.has<Auto>(); }
    bool is_span() const { return m_value.has<Span>(); }
    bool is_area_or_line() const { return m_value.has<AreaOrLine>(); }

    bool is_auto_positioned() const { return is_auto() || is_span(); }
    bool is_positioned() const { return !is_auto_positioned(); }

    bool is_custom_ident() const { return is_area_or_line() && !m_value.get<AreaOrLine>().line_number.has_value(); }

    bool has_identifier() const
    {
        return is_area_or_line() && m_value.get<AreaOrLine>().name.has_value();
    }

    bool has_line_number() const
    {
        return is_area_or_line() && m_value.get<AreaOrLine>().line_number.has_value();
    }

    String identifier() const { return *m_value.get<AreaOrLine>().name; }

    IntegerOrCalculated line_number() const { return *m_value.get<AreaOrLine>().line_number; }
    IntegerOrCalculated span() const { return m_value.get<Span>().value; }

    String to_string() const;

    bool operator==(GridTrackPlacement const& other) const = default;

private:
    struct Auto {
        bool operator==(Auto const&) const = default;
    };

    struct AreaOrLine {
        Optional<IntegerOrCalculated> line_number;
        Optional<String> name;
        bool operator==(AreaOrLine const& other) const = default;
    };

    struct Span {
        IntegerOrCalculated value;
        Optional<String> name;
        bool operator==(Span const& other) const = default;
    };

    GridTrackPlacement()
        : m_value(Auto {})
    {
    }
    GridTrackPlacement(AreaOrLine value)
        : m_value(value)
    {
    }
    GridTrackPlacement(Span value)
        : m_value(value)
    {
    }

    Variant<Auto, AreaOrLine, Span> m_value;
};

}
