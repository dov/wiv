// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
#pragma once
// CSS in SVG files. The rules of the document's <style> elements are applied
// to the DOM so that nanosvg (which only knows simple class selectors) sees
// them as plain `style` attributes, and CSS animations (`@keyframes` plus the
// `animation` properties) are turned into the equivalent SMIL <animate>
// elements, which SvgAnimation then plays.
//
// Selectors: `*`, element, `.class`, `#id`, compounds of those, and the
// descendant and child combinators. Rules with pseudo-classes or attribute
// selectors are ignored. Animated properties: any presentation attribute or
// geometry property (opacity, fill, stroke*, r, cx, width, d, ...) and
// `transform` (translate/scale/rotate/skew/matrix, honoring `transform-origin`
// and `transform-box: fill-box`). Timing: duration, delay (also negative),
// iteration-count, direction, fill-mode (`both`/`backwards` hold the first
// keyframe during the delay), and timing functions (as per-keyframe splines;
// `steps()` is treated as linear).

#include "SvgDom.h"

namespace giv
{

// Returns true if the DOM was changed.
bool applySvgCss(SvgNode& root);

} // namespace giv
