// The drawings below are from Lucide (https://lucide.dev), version 0.460.0,
// under the ISC licence:
//
//   ISC License
//
//   Copyright (c) for portions of Lucide are held by Cole Bemis 2013-2022 as
//   part of Feather (MIT). All other copyright (c) for Lucide are held by
//   Lucide Contributors 2022.
//
//   Permission to use, copy, modify, and/or distribute this software for any
//   purpose with or without fee is hereby granted, provided that the above
//   copyright notice and this permission notice appear in all copies.
//
//   THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
//   WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
//   MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
//   SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
//   WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
//   ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR
//   IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
//
// The Feather portions are under the MIT licence:
//
//   Copyright (c) 2013-2022 Cole Bemis
//
//   Permission is hereby granted, free of charge, to any person obtaining a
//   copy of this software and associated documentation files (the
//   "Software"), to deal in the Software without restriction, including
//   without limitation the rights to use, copy, modify, merge, publish,
//   distribute, sublicense, and/or sell copies of the Software, and to permit
//   persons to whom the Software is furnished to do so, subject to the
//   following conditions:
//
//   The above copyright notice and this permission notice shall be included
//   in all copies or substantial portions of the Software.
//
//   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
//   THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
//   FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
//   DEALINGS IN THE SOFTWARE.
#include "Icons.h"

namespace mp::ui {

namespace {

// Each icon's elements, from the Lucide SVG of the name in the comment.
const char* body(Icon icon) {
  switch (icon) {
    case Icon::Save:  // save
      return R"(<path d="M15.2 3a2 2 0 0 1 1.4.6l3.8 3.8a2 2 0 0 1 .6 1.4V19a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2z"/><path d="M17 21v-7a1 1 0 0 0-1-1H8a1 1 0 0 0-1 1v7"/><path d="M7 3v4a1 1 0 0 0 1 1h7"/>)";
    case Icon::Panic:  // octagon-x
      return R"(<path d="m15 9-6 6"/><path d="M2.586 16.726A2 2 0 0 1 2 15.312V8.688a2 2 0 0 1 .586-1.414l4.688-4.688A2 2 0 0 1 8.688 2h6.624a2 2 0 0 1 1.414.586l4.688 4.688A2 2 0 0 1 22 8.688v6.624a2 2 0 0 1-.586 1.414l-4.688 4.688a2 2 0 0 1-1.414.586H8.688a2 2 0 0 1-1.414-.586z"/><path d="m9 9 6 6"/>)";
  }
  return "";
}

}  // namespace

std::unique_ptr<juce::Drawable> makeIcon(Icon icon, juce::Colour colour) {
  const juce::String svg =
      "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"24\" height=\"24\" viewBox=\"0 0 24 24\" "
      "fill=\"none\" stroke=\"#" + colour.toDisplayString(false) + "\" stroke-width=\"2\" "
      "stroke-linecap=\"round\" stroke-linejoin=\"round\">" + juce::String(body(icon)) + "</svg>";
  return juce::Drawable::createFromSVGString(svg);
}

IconButton::IconButton(Icon icon, const juce::String& name, const juce::String& tooltip,
                       juce::Colour colour)
    : juce::TextButton(name),
      normal_(makeIcon(icon, colour)),
      dimmed_(makeIcon(icon, colour.withMultipliedAlpha(0.35f))) {
  setTooltip(tooltip);
  setTitle(name);
}

void IconButton::paintButton(juce::Graphics& g, bool highlighted, bool down) {
  auto& lf = getLookAndFeel();
  const auto bg = findColour(getToggleState() ? buttonOnColourId : buttonColourId);
  lf.drawButtonBackground(g, *this, bg, highlighted, down);
  auto* d = isEnabled() ? normal_.get() : dimmed_.get();
  if (d == nullptr) return;
  const float side = juce::jmin(static_cast<float>(getHeight()) - 12.0f, 22.0f);
  const auto box = getLocalBounds().toFloat().withSizeKeepingCentre(side, side)
                       .translated(0.0f, down ? 1.0f : 0.0f);
  d->drawWithin(g, box, juce::RectanglePlacement::centred, 1.0f);
}

}  // namespace mp::ui
