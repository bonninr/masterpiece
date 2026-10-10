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
    case Icon::Open:  // folder-open
      return R"(<path d="m6 14 1.5-2.9A2 2 0 0 1 9.24 10H20a2 2 0 0 1 1.94 2.5l-1.54 6a2 2 0 0 1-1.95 1.5H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h3.9a2 2 0 0 1 1.69.9l.81 1.2a2 2 0 0 0 1.67.9H18a2 2 0 0 1 2 2v2"/>)";
    case Icon::Settings:  // settings
      return R"(<path d="M12.22 2h-.44a2 2 0 0 0-2 2v.18a2 2 0 0 1-1 1.73l-.43.25a2 2 0 0 1-2 0l-.15-.08a2 2 0 0 0-2.73.73l-.22.38a2 2 0 0 0 .73 2.73l.15.1a2 2 0 0 1 1 1.72v.51a2 2 0 0 1-1 1.74l-.15.09a2 2 0 0 0-.73 2.73l.22.38a2 2 0 0 0 2.73.73l.15-.08a2 2 0 0 1 2 0l.43.25a2 2 0 0 1 1 1.73V20a2 2 0 0 0 2 2h.44a2 2 0 0 0 2-2v-.18a2 2 0 0 1 1-1.73l.43-.25a2 2 0 0 1 2 0l.15.08a2 2 0 0 0 2.73-.73l.22-.39a2 2 0 0 0-.73-2.73l-.15-.08a2 2 0 0 1-1-1.74v-.5a2 2 0 0 1 1-1.74l.15-.09a2 2 0 0 0 .73-2.73l-.22-.38a2 2 0 0 0-2.73-.73l-.15.08a2 2 0 0 1-2 0l-.43-.25a2 2 0 0 1-1-1.73V4a2 2 0 0 0-2-2z"/><circle cx="12" cy="12" r="3"/>)";
    case Icon::Keys:  // keyboard-music
      return R"(<rect width="20" height="16" x="2" y="4" rx="2"/><path d="M6 8h4"/><path d="M14 8h.01"/><path d="M18 8h.01"/><path d="M2 12h20"/><path d="M6 12v4"/><path d="M10 12v4"/><path d="M14 12v4"/><path d="M18 12v4"/>)";
    case Icon::Panic:  // octagon-x
      return R"(<path d="m15 9-6 6"/><path d="M2.586 16.726A2 2 0 0 1 2 15.312V8.688a2 2 0 0 1 .586-1.414l4.688-4.688A2 2 0 0 1 8.688 2h6.624a2 2 0 0 1 1.414.586l4.688 4.688A2 2 0 0 1 22 8.688v6.624a2 2 0 0 1-.586 1.414l-4.688 4.688a2 2 0 0 1-1.414.586H8.688a2 2 0 0 1-1.414-.586z"/><path d="m9 9 6 6"/>)";
    case Icon::Combinations:  // layers
      return R"(<path d="m12.83 2.18a2 2 0 0 0-1.66 0L2.6 6.08a1 1 0 0 0 0 1.83l8.58 3.91a2 2 0 0 0 1.66 0l8.58-3.9a1 1 0 0 0 0-1.83Z"/><path d="m22 17.65-9.17 4.16a2 2 0 0 1-1.66 0L2 17.65"/><path d="m22 12.65-9.17 4.16a2 2 0 0 1-1.66 0L2 12.65"/>)";
    case Icon::Panels:  // layout-grid
      return R"(<rect width="7" height="7" x="3" y="3" rx="1"/><rect width="7" height="7" x="14" y="3" rx="1"/><rect width="7" height="7" x="14" y="14" rx="1"/><rect width="7" height="7" x="3" y="14" rx="1"/>)";
    case Icon::Swell:  // sliders-vertical
      return R"(<line x1="4" x2="4" y1="21" y2="14"/><line x1="4" x2="4" y1="10" y2="3"/><line x1="12" x2="12" y1="21" y2="12"/><line x1="12" x2="12" y1="8" y2="3"/><line x1="20" x2="20" y1="21" y2="16"/><line x1="20" x2="20" y1="12" y2="3"/><line x1="2" x2="6" y1="14" y2="14"/><line x1="10" x2="14" y1="8" y2="8"/><line x1="18" x2="22" y1="16" y2="16"/>)";
    case Icon::Previous:  // chevron-left
      return R"(<path d="m15 18-6-6 6-6"/>)";
    case Icon::Next:  // chevron-right
      return R"(<path d="m9 18 6-6-6-6"/>)";
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
