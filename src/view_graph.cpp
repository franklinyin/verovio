/////////////////////////////////////////////////////////////////////////////
// Name:        view_graph.cpp
// Author:      Laurent Pugin
// Created:     2005
// Copyright (c) Authors and others. All rights reserved.
/////////////////////////////////////////////////////////////////////////////

#include "view.h"

//----------------------------------------------------------------------------

#include <cassert>
#include <sstream>

//----------------------------------------------------------------------------

#include "devicecontext.h"
#include "doc.h"
#include "graphic.h"
#include "options.h"
#include "svg.h"
#include "symboldef.h"
#include "vrv.h"

#include <algorithm>
#include <cmath>

namespace vrv {

namespace {

void CopyBezier(const Point src[4], Point dest[4])
{
    for (int i = 0; i < 4; ++i) dest[i] = src[i];
}

void SplitBezier(const Point bezier[4], double t, Point left[4], Point right[4])
{
    Point p01, p12, p23, p012, p123, p0123;
    BoundingBox::CalcLinearInterpolation(p01, bezier[0], bezier[1], t);
    BoundingBox::CalcLinearInterpolation(p12, bezier[1], bezier[2], t);
    BoundingBox::CalcLinearInterpolation(p23, bezier[2], bezier[3], t);
    BoundingBox::CalcLinearInterpolation(p012, p01, p12, t);
    BoundingBox::CalcLinearInterpolation(p123, p12, p23, t);
    BoundingBox::CalcLinearInterpolation(p0123, p012, p123, t);
    left[0] = bezier[0];
    left[1] = p01;
    left[2] = p012;
    left[3] = p0123;
    right[0] = p0123;
    right[1] = p123;
    right[2] = p23;
    right[3] = bezier[3];
}

void ExtractBezierSegment(const Point bezier[4], double t0, double t1, Point out[4])
{
    t0 = std::clamp(t0, 0.0, 1.0);
    t1 = std::clamp(t1, 0.0, 1.0);
    if (t1 <= t0) {
        CopyBezier(bezier, out);
        out[1] = out[0];
        out[2] = out[0];
        out[3] = out[0];
        return;
    }

    Point tmp[4], discard[4];
    if (t0 > 0.0) {
        SplitBezier(bezier, t0, discard, tmp);
    }
    else {
        CopyBezier(bezier, tmp);
    }

    if (t1 < 1.0) {
        const double denom = 1.0 - t0;
        const double u = (denom > 0.0) ? ((t1 - t0) / denom) : 1.0;
        SplitBezier(tmp, u, out, discard);
    }
    else {
        CopyBezier(tmp, out);
    }
}

double BezierPointDistance(const Point &a, const Point &b)
{
    const double dx = static_cast<double>(a.x - b.x);
    const double dy = static_cast<double>(a.y - b.y);
    return std::sqrt(dx * dx + dy * dy);
}

double ParamAtArcLength(const double *cum, int steps, double target)
{
    if (target <= 0.0) return 0.0;
    if (target >= cum[steps]) return 1.0;
    int lo = 0;
    int hi = steps;
    while (hi - lo > 1) {
        const int mid = (lo + hi) / 2;
        if (cum[mid] < target) {
            lo = mid;
        }
        else {
            hi = mid;
        }
    }
    const double span = cum[hi] - cum[lo];
    const double frac = (span > 0.0) ? ((target - cum[lo]) / span) : 0.0;
    return (static_cast<double>(lo) + frac) / static_cast<double>(steps);
}

} // namespace

void View::DrawVerticalLine(DeviceContext *dc, int y1, int y2, int x1, int width, int dashLength, int gapLength)
{
    assert(dc);

    dc->SetPen(std::max(1, this->ToDeviceContextX(width)), PEN_SOLID, dashLength, gapLength);

    dc->DrawLine(
        this->ToDeviceContextX(x1), this->ToDeviceContextY(y1), this->ToDeviceContextX(x1), this->ToDeviceContextY(y2));

    dc->ResetPen();
    return;
}

void View::DrawHorizontalLine(DeviceContext *dc, int x1, int x2, int y1, int width, int dashLength, int gapLength)
{
    assert(dc);

    dc->SetPen(std::max(1, this->ToDeviceContextX(width)), PEN_SOLID, dashLength, gapLength);

    dc->DrawLine(
        this->ToDeviceContextX(x1), this->ToDeviceContextY(y1), this->ToDeviceContextX(x2), this->ToDeviceContextY(y1));

    dc->ResetPen();
    return;
}

void View::DrawObliqueLine(DeviceContext *dc, int x1, int x2, int y1, int y2, int width, int dashLength, int gapLength)
{
    assert(dc);

    dc->SetPen(std::max(1, this->ToDeviceContextX(width)), PEN_SOLID, dashLength, gapLength);

    dc->DrawLine(
        this->ToDeviceContextX(x1), this->ToDeviceContextY(y1), this->ToDeviceContextX(x2), this->ToDeviceContextY(y2));

    dc->ResetPen();
    return;
}

void View::DrawVerticalSegmentedLine(
    DeviceContext *dc, int x1, SegmentedLine &line, int width, int dashLength, int gapLength)
{
    int start, end;
    for (int i = 0; i < line.GetSegmentCount(); ++i) {
        std::tie(start, end) = line.GetStartEnd(i);
        this->DrawVerticalLine(dc, start, end, x1, width, dashLength, gapLength);
    }
}

void View::DrawHorizontalSegmentedLine(
    DeviceContext *dc, int y1, SegmentedLine &line, int width, int dashLength, int gapLength)
{
    int start, end;
    for (int i = 0; i < line.GetSegmentCount(); ++i) {
        std::tie(start, end) = line.GetStartEnd(i);
        this->DrawHorizontalLine(dc, start, end, y1, width, dashLength, gapLength);
    }
}

void View::DrawNotFilledEllipse(DeviceContext *dc, int x1, int y1, int x2, int y2, int lineThickness)
{
    assert(dc); // DC cannot be NULL

    std::swap(y1, y2);

    dc->SetPen(lineThickness, PEN_SOLID);
    dc->SetBrush(0.0);

    int width = x2 - x1;
    int height = y1 - y2;

    dc->DrawEllipse(this->ToDeviceContextX(x1), this->ToDeviceContextY(y1), width, height);

    dc->ResetPen();
    dc->ResetBrush();
}

void View::DrawNotFilledRectangle(DeviceContext *dc, int x1, int y1, int x2, int y2, int lineThickness, int radius = 0)
{
    assert(dc); // DC cannot be NULL

    std::swap(y1, y2);

    const int penWidth = lineThickness;
    dc->SetPen(penWidth, PEN_SOLID);
    dc->SetBrush(0.0);

    dc->DrawRoundedRectangle(this->ToDeviceContextX(x1), this->ToDeviceContextY(y1), this->ToDeviceContextX(x2 - x1),
        this->ToDeviceContextX(y1 - y2), radius);

    dc->ResetPen();
    dc->ResetBrush();

    return;
}

/* Draw a filled rectangle with horizontal and vertical sides. */
void View::DrawFilledRectangle(DeviceContext *dc, int x1, int y1, int x2, int y2)
{
    assert(dc);

    this->DrawFilledRoundedRectangle(dc, x1, y1, x2, y2, 0);

    return;
}

void View::DrawFilledRoundedRectangle(DeviceContext *dc, int x1, int y1, int x2, int y2, int radius)
{
    assert(dc);

    std::swap(y1, y2);

    dc->SetPen(0, PEN_SOLID);

    dc->DrawRoundedRectangle(this->ToDeviceContextX(x1), this->ToDeviceContextY(y1), this->ToDeviceContextX(x2 - x1),
        this->ToDeviceContextX(y1 - y2), radius);

    dc->ResetPen();

    return;
}

/* Draw an oblique quadrilateral: specifically, a parallelogram with vertical left
    and right sides, and with opposite vertices at (x1,y1) and (x2,y2). */
void View::DrawObliquePolygon(DeviceContext *dc, int x1, int y1, int x2, int y2, int height)
{
    Point p[4];

    dc->SetPen(0, PEN_SOLID);

    height = this->ToDeviceContextX(height);
    p[0].x = this->ToDeviceContextX(x1);
    p[0].y = this->ToDeviceContextY(y1);
    p[1].x = this->ToDeviceContextX(x2);
    p[1].y = this->ToDeviceContextY(y2);
    p[2].x = p[1].x;
    p[2].y = p[1].y - height;
    p[3].x = p[0].x;
    p[3].y = p[0].y - height;

    dc->DrawPolygon(4, p);

    dc->ResetPen();
}

/* Draw an empty ("void") diamond with its top lefthand point at (x1, y1). */

void View::DrawDiamond(DeviceContext *dc, int x1, int y1, int height, int width, bool fill, int linewidth)
{
    Point p[4];

    dc->SetPen(linewidth, PEN_SOLID);
    if (fill) {
        dc->SetBrush(1.0);
    }
    else {
        dc->SetBrush(0.0);
    }

    int dHeight = this->ToDeviceContextX(height);
    int dWidth = this->ToDeviceContextX(width);
    p[0].x = this->ToDeviceContextX(x1);
    p[0].y = this->ToDeviceContextY(y1);
    p[1].x = this->ToDeviceContextX(x1 + dWidth / 2);
    p[1].y = this->ToDeviceContextY(y1 + dHeight / 2);
    p[2].x = p[0].x + dWidth;
    p[2].y = p[0].y;
    p[3].x = this->ToDeviceContextX(x1 + dWidth / 2);
    p[3].y = this->ToDeviceContextY(y1 - dHeight / 2);

    dc->DrawPolygon(4, p);

    dc->ResetPen();
    dc->ResetBrush();
}

void View::DrawDot(DeviceContext *dc, int x, int y, int staffSize, bool dimin)
{
    int r = std::max(this->ToDeviceContextX(m_doc->GetDrawingDoubleUnit(staffSize) / 5), 2);
    if (dimin) r *= m_doc->GetOptions()->m_graceFactor.GetValue();

    dc->SetPen(0, PEN_SOLID);

    dc->DrawCircle(this->ToDeviceContextX(x), this->ToDeviceContextY(y), r);

    dc->ResetPen();
}

void View::DrawVerticalDots(DeviceContext *dc, int x, const SegmentedLine &line, int barlineWidth, int interval)
{
    if (line.GetSegmentCount() > 1) return;

    const auto [top, bottom] = line.GetStartEnd(0);
    const int radius = std::max(barlineWidth, 2);
    int drawingPosition = top - interval / 2;

    dc->SetPen(0, PEN_SOLID);

    while (drawingPosition > bottom) {
        dc->DrawCircle(this->ToDeviceContextX(x), this->ToDeviceContextY(drawingPosition), radius);
        drawingPosition -= interval;
    }

    dc->ResetPen();
}

void View::DrawSquareBracket(DeviceContext *dc, bool leftBracket, int x, int y, int height, int width,
    int horizontalThickness, int verticalThickness)
{
    assert(dc);

    const int sign = leftBracket ? 1 : -1;

    this->DrawFilledRectangle(dc, x, y - horizontalThickness / 2, x + sign * verticalThickness,
        y + height + horizontalThickness / 2); // vertical
    this->DrawFilledRectangle(
        dc, x, y - horizontalThickness / 2, x + sign * width, y + horizontalThickness / 2); // horizontal bottom
    this->DrawFilledRectangle(dc, x, y + height - horizontalThickness / 2, x + sign * width,
        y + height + horizontalThickness / 2); // horizontal top
}

void View::DrawEnclosingBrackets(DeviceContext *dc, int x, int y, int height, int width, int offset, int bracketWidth,
    int horizontalThickness, int verticalThickness)
{
    assert(dc);

    this->DrawSquareBracket(
        dc, true, x - offset, y - offset, height + 2 * offset, bracketWidth, horizontalThickness, verticalThickness);
    this->DrawSquareBracket(dc, false, x + width + offset, y - offset, height + 2 * offset, bracketWidth,
        horizontalThickness, verticalThickness);
}

/*
void View::DrawSmuflCodeWithCustomFont(DeviceContext *dc, const std::string &customFont, int x, int y, char32_t code,
    int staffSize, bool dimin, bool setBBGlyph)
{
    if (customFont.empty()) {
        this->DrawSmuflCode(dc, x, y, code, staffSize, dimin, setBBGlyph);
        return;
    }

    Resources &resources = m_doc->GetResourcesForModification();
    const std::string prevFont = resources.GetCurrentFont();

    resources.SetCurrentFont(customFont);

    this->DrawSmuflCode(dc, x, y, code, staffSize, dimin, setBBGlyph);

    resources.SetCurrentFont(prevFont);
}
*/

void View::DrawSmuflCode(DeviceContext *dc, int x, int y, char32_t code, int staffSize, bool dimin, bool setBBGlyph)
{
    assert(dc);

    if (code == 0) return;

    std::u32string str;
    str.push_back(code);

    dc->SetFont(m_doc->GetDrawingSmuflFont(staffSize, dimin));

    dc->DrawMusicText(str, this->ToDeviceContextX(x), this->ToDeviceContextY(y), setBBGlyph);

    dc->ResetFont();

    return;
}

void View::DrawSmuflLine(
    DeviceContext *dc, Point orig, int length, int staffSize, bool dimin, char32_t fill, char32_t start, char32_t end)
{
    assert(dc);

    if (length <= 0) return;

    const int startWidth = (start == 0) ? 0 : m_doc->GetGlyphAdvX(start, staffSize, dimin);
    const int endWidth = (end == 0) ? 0 : m_doc->GetGlyphAdvX(end, staffSize, dimin);
    int fillWidth = m_doc->GetGlyphAdvX(fill, staffSize, dimin);

    if (fillWidth == 0) fillWidth = m_doc->GetGlyphWidth(fill, staffSize, dimin);

    // We add half a fill length for an average shorter / longer line result
    const int count = (length + fillWidth / 2 - startWidth - endWidth) / fillWidth;

    dc->SetFont(m_doc->GetDrawingSmuflFont(staffSize, dimin));

    std::u32string str;

    if (start != 0) {
        str.push_back(start);
    }

    for (int i = 0; i < count; ++i) {
        str.push_back(fill);
    }

    if (end != 0) {
        str.push_back(end);
    }

    dc->DrawMusicText(str, this->ToDeviceContextX(orig.x), this->ToDeviceContextY(orig.y), false);

    dc->ResetFont();
}

void View::DrawSmuflString(DeviceContext *dc, int x, int y, std::u32string s, data_HORIZONTALALIGNMENT alignment,
    int staffSize, bool dimin, bool setBBGlyph)
{
    assert(dc);

    int xDC = this->ToDeviceContextX(x);

    dc->SetFont(m_doc->GetDrawingSmuflFont(staffSize, dimin));

    if (alignment == HORIZONTALALIGNMENT_center) {
        TextExtend extend;
        dc->GetSmuflTextExtent(s, &extend);
        xDC -= extend.m_width / 2;
    }
    else if (alignment == HORIZONTALALIGNMENT_right) {
        TextExtend extend;
        dc->GetSmuflTextExtent(s, &extend);
        xDC -= extend.m_width;
    }

    dc->DrawMusicText(s, xDC, this->ToDeviceContextY(y), setBBGlyph);

    dc->ResetFont();
}

void View::DrawThickBezierCurve(
    DeviceContext *dc, Point bezier[4], int thickness, int staffSize, int penWidth, PenStyle penStyle)
{
    assert(dc);

    Point bez1[4], bez2[4]; // filled array with control points and end point

    BoundingBox::CalcThickBezier(bezier, thickness, bez1, bez2);

    bez1[0] = this->ToDeviceContext(bez1[0]);
    bez1[1] = this->ToDeviceContext(bez1[1]);
    bez1[2] = this->ToDeviceContext(bez1[2]);
    bez1[3] = this->ToDeviceContext(bez1[3]);

    bez2[0] = this->ToDeviceContext(bez2[0]);
    bez2[1] = this->ToDeviceContext(bez2[1]);
    bez2[2] = this->ToDeviceContext(bez2[2]);
    bez2[3] = this->ToDeviceContext(bez2[3]);

    // Same filled ribbon as a solid slur/tie (thick middle, thin ends).
    dc->SetPen(std::max(1, m_doc->GetDrawingStemWidth(staffSize) / 2), PEN_SOLID);

    if (penStyle == PEN_SOLID || dc->Is(BBOX_DEVICE_CONTEXT)) {
        dc->DrawCubicBezierPathFilled(bez1, bez2);
        dc->ResetPen();
        return;
    }

    // Dashed/dotted: keep the ribbon shape, but draw discontinuous segments
    // along the centerline (not a uniform-width stroked curve).
    constexpr int kSteps = 64;
    double cum[kSteps + 1];
    cum[0] = 0.0;
    Point prev = BoundingBox::CalcPointAtBezier(bez1, 0.0);
    for (int i = 1; i <= kSteps; ++i) {
        const Point cur = BoundingBox::CalcPointAtBezier(bez1, static_cast<double>(i) / kSteps);
        cum[i] = cum[i - 1] + BezierPointDistance(prev, cur);
        prev = cur;
    }
    const double totalLen = cum[kSteps];
    if (totalLen <= 1.0) {
        dc->DrawCubicBezierPathFilled(bez1, bez2);
        dc->ResetPen();
        return;
    }

    const double unit = static_cast<double>(std::max(1, m_doc->GetDrawingUnit(staffSize)));
    double dashLen = 0.0;
    double gapLen = 0.0;
    if (penStyle == PEN_DOT) {
        dashLen = std::max(unit * 0.35, static_cast<double>(std::max(1, thickness)) * 0.45);
        gapLen = std::max(unit * 0.7, static_cast<double>(std::max(1, thickness)) * 1.2);
    }
    else if (penStyle == PEN_LONG_DASH) {
        dashLen = std::max(unit * 2.0, static_cast<double>(std::max(1, thickness)) * 4.0);
        gapLen = std::max(unit * 1.0, static_cast<double>(std::max(1, thickness)) * 2.0);
    }
    else {
        // PEN_SHORT_DASH and other non-solid styles
        dashLen = std::max(unit * 1.2, static_cast<double>(std::max(1, thickness)) * 2.5);
        gapLen = std::max(unit * 0.8, static_cast<double>(std::max(1, thickness)) * 1.5);
    }

    double s = 0.0;
    while (s < totalLen) {
        const double s1 = std::min(totalLen, s + dashLen);
        const double t0 = ParamAtArcLength(cum, kSteps, s);
        const double t1 = ParamAtArcLength(cum, kSteps, s1);
        if (t1 > t0 + 1e-4) {
            Point seg1[4], seg2[4];
            ExtractBezierSegment(bez1, t0, t1, seg1);
            ExtractBezierSegment(bez2, t0, t1, seg2);
            dc->DrawCubicBezierRibbonSegment(seg1, seg2);
        }
        s += dashLen + gapLen;
    }

    dc->ResetPen();
}

void View::DrawSymbolDef(DeviceContext *dc, Object *parent, SymbolDef *symbolDef, int x, int y, int staffSize,
    bool dimin, data_HORIZONTALALIGNMENT alignment)
{
    assert(dc);
    assert(symbolDef);

    TextDrawingParams params;
    params.m_x = x;
    params.m_y = y;

    // Because image y coordinates are inverted we need to adjust the y position
    params.m_y += symbolDef->GetSymbolHeight(m_doc, staffSize, dimin);

    if (alignment != HORIZONTALALIGNMENT_left) {
        const int width = symbolDef->GetSymbolWidth(m_doc, staffSize, dimin);
        params.m_x -= (alignment == HORIZONTALALIGNMENT_center) ? (width / 2) : width;
    }

    // Because thg Svg is a child of symbolDef we need to temporarily change the parent for the bounding boxes
    // to be properly propagated in the device context
    symbolDef->SetTemporaryParent(parent);

    for (Object *current : symbolDef->GetChildren()) {
        if (current->Is(GRAPHIC)) {
            Graphic *graphic = vrv_cast<Graphic *>(current);
            assert(graphic);
            this->DrawGraphic(dc, graphic, params, staffSize, dimin);
        }
        if (current->Is(SVG)) {
            Svg *svg = vrv_cast<Svg *>(current);
            assert(svg);
            this->DrawSvg(dc, svg, params, staffSize, dimin);
        }
    }

    symbolDef->ResetTemporaryParent();
}

} // namespace vrv
