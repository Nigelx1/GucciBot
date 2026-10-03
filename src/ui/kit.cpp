#include "ui/kit.hpp"
#include "ui/state.hpp"
#include "ui/ui.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

namespace gucci::ui::kit {

    namespace {

        // Eased per-control values (a switch's knob position), by ImGui id.
        float ease(ImGuiID id, float target, float speed = 16.f) {
            static std::unordered_map<ImGuiID, float> s_values;
            auto [it, fresh] = s_values.try_emplace(id, target);
            float& v = it->second;
            if (fresh || !look().effects) {
                v = target;
                return v;
            }
            float const k = std::min(1.f, ImGui::GetIO().DeltaTime * speed);
            v += (target - v) * k;
            if (std::fabs(target - v) < 0.002f)
                v = target;
            return v;
        }

        struct RowFrame {
            ImVec2 start;       // screen position where the row began
            float labelBottom;  // screen y the label column reached
        };
        std::vector<RowFrame>& rowStack() {
            static std::vector<RowFrame> s;
            return s;
        }

        std::vector<ImVec2>& featureStack() {
            static std::vector<ImVec2> s;
            return s;
        }

        int growString(ImGuiInputTextCallbackData* data) {
            if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
                auto* s = static_cast<std::string*>(data->UserData);
                s->resize(static_cast<size_t>(data->BufTextLen));
                data->Buf = s->data();
            }
            return 0;
        }

        ImVec4 lift(const ImVec4& c, float amount) {
            return ImVec4(std::min(1.f, c.x + amount), std::min(1.f, c.y + amount), std::min(1.f, c.z + amount), c.w);
        }

    } // namespace

    // ---- layout

    float Avail() {
        return std::max(1.f, ImGui::GetContentRegionAvail().x);
    }

    void BeginCard(const char* title, const char* caption) {
        auto const& pal = look().pal;
        ImGui::PushID(title ? title : "card");
        ImGui::PushStyleColor(ImGuiCol_ChildBg, pal.surface);
        ImGui::PushStyleColor(ImGuiCol_Border, pal.line);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.f, 11.f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, pal.radius + 3.f);
        ImGui::BeginChild("##card", ImVec2(0.f, 0.f),
                          ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
        if (title && *title) {
            {
                UseFont f(Font::Strong);
                ImGui::TextUnformatted(title);
            }
            if (caption && *caption) {
                UseFont f(Font::Small);
                ImGui::PushStyleColor(ImGuiCol_Text, pal.muted);
                ImGui::PushTextWrapPos(0.f);
                ImGui::TextUnformatted(caption);
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
            }
            ImGui::Dummy(ImVec2(0.f, 3.f));
        }
    }

    void EndCard() {
        ImGui::EndChild();
        ImGui::PopID();
        ImGui::Dummy(ImVec2(0.f, 3.f));
    }

    void Gap(float lines) {
        ImGui::Dummy(ImVec2(0.f, ImGui::GetTextLineHeight() * std::max(0.f, lines)));
    }

    void Divider() {
        ImVec2 const p = ImGui::GetCursorScreenPos();
        float const w = Avail();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + 4.f), ImVec2(p.x + w, p.y + 4.f), u32(look().pal.line));
        ImGui::Dummy(ImVec2(w, 8.f));
    }

    void Section(const char* text) {
        ImGui::Dummy(ImVec2(0.f, 2.f));
        UseFont f(Font::Small);
        ImGui::PushStyleColor(ImGuiCol_Text, look().pal.accent);
        ImGui::TextUnformatted(text);
        ImGui::PopStyleColor();
    }

    void Label(const char* text, Tone tone) {
        ImGui::PushStyleColor(ImGuiCol_Text, look().tone(tone));
        ImGui::TextUnformatted(text);
        ImGui::PopStyleColor();
    }

    void Paragraph(const char* text, Tone tone) {
        ImGui::PushStyleColor(ImGuiCol_Text, look().tone(tone));
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }

    void Hint(const char* text) {
        UseFont f(Font::Small);
        Paragraph(text, Tone::Muted);
    }

    void Note(const char* text, Tone tone) {
        if (!text || !*text)
            return;
        auto const& pal = look().pal;
        ImVec4 const edge = tone == Tone::Plain ? pal.line : look().tone(tone);
        UseFont f(Font::Small);
        float const width = Avail();
        ImVec2 const pad(10.f, 7.f);
        float const wrap = std::max(10.f, width - pad.x * 2.f - 3.f);
        ImVec2 const textSize = ImGui::CalcTextSize(text, nullptr, false, wrap);
        ImVec2 const a = ImGui::GetCursorScreenPos();
        ImVec2 const b(a.x + width, a.y + textSize.y + pad.y * 2.f);
        auto* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(a, b, u32(withAlpha(edge, 0.11f)), pal.radius);
        dl->AddRectFilled(a, ImVec2(a.x + 3.f, b.y), u32(edge), pal.radius, ImDrawFlags_RoundCornersLeft);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(a.x + 3.f + pad.x, a.y + pad.y),
                    u32(tone == Tone::Muted ? pal.muted : pal.ink), text, nullptr, wrap);
        ImGui::Dummy(ImVec2(width, b.y - a.y));
    }

    void Chip(const char* text, Tone tone) {
        UseFont f(Font::Small);
        ImVec4 const c = look().tone(tone);
        ImVec2 const pad(7.f, 2.f);
        ImVec2 const textSize = ImGui::CalcTextSize(text);
        ImVec2 const size(textSize.x + pad.x * 2.f, textSize.y + pad.y * 2.f);
        ImVec2 const a = ImGui::GetCursorScreenPos();
        auto* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(a, ImVec2(a.x + size.x, a.y + size.y), u32(withAlpha(c, 0.17f)), size.y * 0.5f);
        dl->AddText(ImVec2(a.x + pad.x, a.y + pad.y), u32(c), text);
        ImGui::Dummy(size);
    }

    void Quote(const char* text, const char* by) {
        if (!text || !*text)
            return;
        ImVec2 const top = ImGui::GetCursorScreenPos();
        ImGui::Indent(12.f);
        Paragraph(text, Tone::Plain);
        if (by && *by) {
            UseFont f(Font::Small);
            Label(by, Tone::Muted);
        }
        ImGui::Unindent(12.f);
        float const bottom = ImGui::GetItemRectMax().y;
        ImGui::GetWindowDrawList()->AddRectFilled(top, ImVec2(top.x + 3.f, bottom), u32(look().pal.accent), 1.5f);
    }

    void Progress(float fraction, const char* overlay) {
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, look().pal.accent);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, look().pal.raised);
        ImGui::ProgressBar(std::clamp(fraction, 0.f, 1.f), ImVec2(Avail(), 0.f), overlay);
        ImGui::PopStyleColor(2);
    }

    void RowBegin(const char* label, const char* hint) {
        ImGui::PushID(label);
        float const avail = Avail();
        float const labelWidth = std::clamp(avail * 0.44f, 110.f, 300.f);
        ImVec2 const start = ImGui::GetCursorScreenPos();
        float const startLocalX = ImGui::GetCursorPosX();

        ImGui::BeginGroup();
        ImGui::PushTextWrapPos(startLocalX + labelWidth - 12.f);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        if (hint && *hint) {
            UseFont f(Font::Small);
            ImGui::PushStyleColor(ImGuiCol_Text, look().pal.muted);
            ImGui::TextUnformatted(hint);
            ImGui::PopStyleColor();
        }
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();
        float const labelBottom = ImGui::GetItemRectMax().y;

        ImGui::SetCursorScreenPos(ImVec2(start.x + labelWidth, start.y));
        ImGui::BeginGroup();
        rowStack().push_back({start, labelBottom});
    }

    void RowEnd() {
        ImGui::EndGroup();
        float const controlBottom = ImGui::GetItemRectMax().y;
        if (rowStack().empty()) {
            ImGui::PopID();
            return;
        }
        RowFrame const row = rowStack().back();
        rowStack().pop_back();
        ImGui::SetCursorScreenPos(ImVec2(row.start.x, std::max(row.labelBottom, controlBottom)));
        ImGui::Dummy(ImVec2(0.f, 0.f));
        ImGui::PopID();
    }

    // ---- controls

    bool Switch(const char* id, bool* value) {
        float const frame = ImGui::GetFrameHeight();
        float const h = std::round(frame * 0.78f);
        float const w = std::round(h * 1.85f);
        ImVec2 const p = ImGui::GetCursorScreenPos();
        bool const clicked = ImGui::InvisibleButton(id, ImVec2(w, frame));
        if (clicked)
            *value = !*value;
        bool const hovered = ImGui::IsItemHovered();
        float const t = ease(ImGui::GetItemID(), *value ? 1.f : 0.f);

        auto const& pal = look().pal;
        ImVec2 const a(p.x, p.y + (frame - h) * 0.5f);
        ImVec2 const b(a.x + w, a.y + h);
        ImVec4 track = mix(mix(pal.raised, pal.line, 0.7f), pal.accent, t);
        if (hovered)
            track = lift(track, 0.05f);
        auto* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(a, b, u32(track), h * 0.5f);
        float const r = h * 0.5f - 2.5f;
        ImVec2 const knob(a.x + h * 0.5f + t * (w - h), a.y + h * 0.5f);
        dl->AddCircleFilled(ImVec2(knob.x, knob.y + 0.8f), r, u32(ImVec4(0.f, 0.f, 0.f, 0.25f)));
        dl->AddCircleFilled(knob, r, u32(ImVec4(0.97f, 0.97f, 0.96f, 1.f)));
        return clicked;
    }

    bool Choice(const char* id, int* index, const char* const* options, int count) {
        if (count <= 0)
            return false;
        ImGui::PushID(id);
        auto const& pal = look().pal;
        float const frame = ImGui::GetFrameHeight();
        float const width = Avail();
        float const segment = width / static_cast<float>(count);
        ImVec2 const p = ImGui::GetCursorScreenPos();
        auto* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, ImVec2(p.x + width, p.y + frame), u32(pal.raised), pal.radius);

        bool changed = false;
        for (int i = 0; i < count; ++i) {
            ImGui::PushID(i);
            ImVec2 const a(p.x + segment * static_cast<float>(i), p.y);
            ImGui::SetCursorScreenPos(a);
            if (ImGui::InvisibleButton("##part", ImVec2(segment, frame)) && *index != i) {
                *index = i;
                changed = true;
            }
            bool const hovered = ImGui::IsItemHovered();
            bool const selected = *index == i;
            ImVec2 const in0(a.x + 2.f, a.y + 2.f);
            ImVec2 const in1(a.x + segment - 2.f, a.y + frame - 2.f);
            float const rounding = std::max(0.f, pal.radius - 1.f);
            if (selected)
                dl->AddRectFilled(in0, in1, u32(pal.accent), rounding);
            else if (hovered)
                dl->AddRectFilled(in0, in1, u32(lift(pal.raised, 0.06f)), rounding);
            ImVec2 const textSize = ImGui::CalcTextSize(options[i]);
            ImVec2 const at(a.x + std::max(4.f, (segment - textSize.x) * 0.5f), a.y + (frame - textSize.y) * 0.5f);
            dl->PushClipRect(in0, in1, true);
            dl->AddText(at, u32(selected ? pal.onAccent : (hovered ? pal.ink : pal.muted)), options[i]);
            dl->PopClipRect();
            ImGui::PopID();
        }
        ImGui::SetCursorScreenPos(p);
        ImGui::Dummy(ImVec2(width, frame));
        ImGui::PopID();
        return changed;
    }

    bool Dropdown(const char* id, int* index, const char* const* options, int count) {
        ImGui::PushID(id);
        ImGui::SetNextItemWidth(Avail());
        bool const changed = ImGui::Combo("##pick", index, options, count);
        ImGui::PopID();
        return changed;
    }

    bool Dropdown(const char* id, int* index, const std::vector<std::string>& options) {
        ImGui::PushID(id);
        ImGui::SetNextItemWidth(Avail());
        bool const valid = *index >= 0 && *index < static_cast<int>(options.size());
        bool changed = false;
        if (ImGui::BeginCombo("##pick", valid ? options[static_cast<size_t>(*index)].c_str() : "")) {
            for (int i = 0; i < static_cast<int>(options.size()); ++i) {
                bool const selected = i == *index;
                if (ImGui::Selectable(options[static_cast<size_t>(i)].c_str(), selected) && !selected) {
                    *index = i;
                    changed = true;
                }
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::PopID();
        return changed;
    }

    bool SliderFloat(const char* id, float* v, float lo, float hi, const char* fmt) {
        ImGui::PushID(id);
        ImGui::SetNextItemWidth(Avail());
        bool const changed = ImGui::SliderFloat("##value", v, lo, hi, fmt);
        ImGui::PopID();
        return changed;
    }

    bool SliderInt(const char* id, int* v, int lo, int hi, const char* fmt) {
        ImGui::PushID(id);
        ImGui::SetNextItemWidth(Avail());
        bool const changed = ImGui::SliderInt("##value", v, lo, hi, fmt);
        ImGui::PopID();
        return changed;
    }

    bool InputFloat(const char* id, float* v, float step, const char* fmt) {
        ImGui::PushID(id);
        ImGui::SetNextItemWidth(Avail());
        bool const changed = ImGui::InputFloat("##value", v, step, step * 10.f, fmt);
        ImGui::PopID();
        return changed;
    }

    bool InputDouble(const char* id, double* v, double step, const char* fmt) {
        ImGui::PushID(id);
        ImGui::SetNextItemWidth(Avail());
        bool const changed = ImGui::InputDouble("##value", v, step, step * 10.0, fmt);
        ImGui::PopID();
        return changed;
    }

    bool InputInt(const char* id, int* v, int step) {
        ImGui::PushID(id);
        ImGui::SetNextItemWidth(Avail());
        bool const changed = ImGui::InputInt("##value", v, step, step * 10);
        ImGui::PopID();
        return changed;
    }

    bool InputText(const char* id, char* buf, size_t size, const char* placeholder, ImGuiInputTextFlags flags) {
        ImGui::PushID(id);
        ImGui::SetNextItemWidth(Avail());
        bool const changed = placeholder ? ImGui::InputTextWithHint("##text", placeholder, buf, size, flags)
                                         : ImGui::InputText("##text", buf, size, flags);
        ImGui::PopID();
        return changed;
    }

    bool InputText(const char* id, std::string* s, const char* placeholder, ImGuiInputTextFlags flags) {
        ImGui::PushID(id);
        ImGui::SetNextItemWidth(Avail());
        flags |= ImGuiInputTextFlags_CallbackResize;
        bool const changed = placeholder
                                 ? ImGui::InputTextWithHint("##text", placeholder, s->data(), s->capacity() + 1, flags,
                                                            growString, s)
                                 : ImGui::InputText("##text", s->data(), s->capacity() + 1, flags, growString, s);
        ImGui::PopID();
        return changed;
    }

    bool Button(const char* label, Tone tone, float width) {
        auto const& pal = look().pal;
        ImVec4 base = pal.raised;
        ImVec4 text = pal.ink;
        switch (tone) {
            case Tone::Accent:
                base = pal.accent;
                text = pal.onAccent;
                break;
            case Tone::Good:
            case Tone::Warn:
            case Tone::Bad:
                base = mix(pal.raised, look().tone(tone), 0.38f);
                break;
            case Tone::Muted:
                text = pal.muted;
                break;
            case Tone::Plain:
                break;
        }
        ImGui::PushStyleColor(ImGuiCol_Button, base);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, lift(base, 0.06f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, lift(base, 0.12f));
        ImGui::PushStyleColor(ImGuiCol_Text, text);
        bool const pressed = ImGui::Button(label, ImVec2(width < 0.f ? Avail() : width, 0.f));
        ImGui::PopStyleColor(4);
        return pressed;
    }

    bool Color(const char* id, ImVec4* c, bool alpha) {
        ImGui::PushID(id);
        ImGuiColorEditFlags flags = ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel;
        flags |= alpha ? (ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf) : ImGuiColorEditFlags_NoAlpha;
        bool const changed = ImGui::ColorEdit4("##colour", &c->x, flags);
        ImGui::PopID();
        return changed;
    }

    bool Key(const char* id, int* keyCode) {
        ImGui::PushID(id);
        bool changed = detail::takeCaptured(keyCode);
        bool const waiting = detail::captureTarget() == keyCode;
        std::string const text = (waiting ? std::string("Press a key...") : keyName(*keyCode)) + "##key";
        if (Button(text.c_str(), waiting ? Tone::Accent : Tone::Plain, std::min(Avail(), 140.f)))
            detail::setCaptureTarget(waiting ? nullptr : keyCode);
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            if (waiting)
                detail::setCaptureTarget(nullptr);
            if (*keyCode != 0) {
                *keyCode = 0;
                changed = true;
            }
        }
        if (ImGui::IsItemHovered() && !waiting)
            ImGui::SetTooltip("Click, then press a key. Right-click to unbind.");
        ImGui::PopID();
        return changed;
    }

    // ---- one-control rows

    bool SwitchRow(const char* label, const char* hint, bool* value) {
        RowBegin(label, hint);
        bool const changed = Switch("##on", value);
        RowEnd();
        return changed;
    }

    bool ChoiceRow(const char* label, const char* hint, int* index, const char* const* options, int count) {
        RowBegin(label, hint);
        bool const changed = Choice("##choice", index, options, count);
        RowEnd();
        return changed;
    }

    bool DropdownRow(const char* label, const char* hint, int* index, const char* const* options, int count) {
        RowBegin(label, hint);
        bool const changed = Dropdown("##dropdown", index, options, count);
        RowEnd();
        return changed;
    }

    bool SliderRow(const char* label, const char* hint, float* v, float lo, float hi, const char* fmt) {
        RowBegin(label, hint);
        bool const changed = SliderFloat("##slider", v, lo, hi, fmt);
        RowEnd();
        return changed;
    }

    bool SliderRow(const char* label, const char* hint, int* v, int lo, int hi, const char* fmt) {
        RowBegin(label, hint);
        bool const changed = SliderInt("##slider", v, lo, hi, fmt);
        RowEnd();
        return changed;
    }

    bool KeyRow(const char* label, const char* hint, int* keyCode) {
        RowBegin(label, hint);
        bool const changed = Key("##key", keyCode);
        RowEnd();
        return changed;
    }

    bool ColorRow(const char* label, const char* hint, ImVec4* c, bool alpha) {
        RowBegin(label, hint);
        bool const changed = Color("##colour", c, alpha);
        RowEnd();
        return changed;
    }

    bool TextRow(const char* label, const char* hint, char* buf, size_t size, const char* placeholder) {
        RowBegin(label, hint);
        bool const changed = InputText("##text", buf, size, placeholder);
        RowEnd();
        return changed;
    }

    bool FeatureBegin(const char* label, const char* hint, bool* enabled, int* keyCode, bool* changed) {
        RowBegin(label, hint);
        bool const flipped = Switch("##on", enabled);
        if (keyCode) {
            ImGui::SameLine(0.f, 12.f);
            Key("##key", keyCode);
        }
        RowEnd();
        if (changed)
            *changed = flipped;
        if (!*enabled)
            return false;
        featureStack().push_back(ImGui::GetCursorScreenPos());
        ImGui::Indent(16.f);
        return true;
    }

    void FeatureEnd() {
        ImGui::Unindent(16.f);
        if (featureStack().empty())
            return;
        ImVec2 const top = featureStack().back();
        featureStack().pop_back();
        float const bottom = ImGui::GetCursorScreenPos().y - ImGui::GetStyle().ItemSpacing.y;
        if (bottom > top.y)
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(top.x + 4.f, top.y), ImVec2(top.x + 6.f, bottom),
                                                      u32(withAlpha(look().pal.accent, 0.45f)), 1.f);
    }

    void OpenConfirm(const char* id) {
        ImGui::OpenPopup(id);
    }

    int Confirm(const char* id, const char* title, const char* message, const char* confirmLabel, Tone tone) {
        int result = 0;
        ImGui::SetNextWindowSize(ImVec2(360.f, 0.f), ImGuiCond_Always);
        if (ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize)) {
            {
                UseFont f(Font::Strong);
                ImGui::TextUnformatted(title);
            }
            Gap(0.3f);
            Paragraph(message);
            Gap(0.5f);
            float const half = (Avail() - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
            if (Button("Cancel", Tone::Plain, half)) {
                result = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (Button(confirmLabel, tone, half)) {
                result = 1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        return result;
    }

} // namespace gucci::ui::kit
