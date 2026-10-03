#pragma once

// The control kit every page is built from. The layout is a column of cards;
// inside a card, rows put a label (and an optional hint under it) on the left
// and the control on the right. Controls take an id that is only used as an
// ImGui id, never shown, so labels can change without losing state.
//
//   kit::BeginCard("Playback", "What happens when a macro plays");
//   kit::SwitchRow("Ignore my inputs", "Only the macro presses buttons", &gb->ignoreInputs);
//   kit::RowBegin("Speed", nullptr);
//   kit::SliderFloat("speed", &speed, 0.1f, 2.f, "%.2fx");
//   kit::RowEnd();
//   kit::EndCard();

#include "ui/look.hpp"

#include <imgui.h>

#include <string>
#include <vector>

namespace gucci::ui::kit {

    // ---- layout

    void BeginCard(const char* title, const char* caption = nullptr);
    void EndCard();

    // Width left on the current line, inside the current card or row column.
    float Avail();

    void Gap(float lines = 0.5f);
    void Divider();
    // A small heading between groups of rows inside a card.
    void Section(const char* text);

    // Text. Label is one line; Paragraph and Hint wrap to the available width.
    void Label(const char* text, Tone tone = Tone::Plain);
    void Paragraph(const char* text, Tone tone = Tone::Plain);
    void Hint(const char* text);
    // A callout box with a coloured edge (warnings, explanations).
    void Note(const char* text, Tone tone = Tone::Muted);
    // A small rounded status label; stays on the current line, so it can be
    // followed by ImGui::SameLine() and another chip.
    void Chip(const char* text, Tone tone = Tone::Accent);
    // The theme's quote for a page.
    void Quote(const char* text, const char* by);
    // A progress bar with optional text on it.
    void Progress(float fraction, const char* overlay = nullptr);

    // Rows: label (+hint) on the left, whatever is drawn between the two calls
    // on the right. Rows can hold several controls (use ImGui::SameLine()).
    void RowBegin(const char* label, const char* hint = nullptr);
    void RowEnd();

    // ---- controls

    bool Switch(const char* id, bool* value);
    // A segmented control: all options visible, one selected.
    bool Choice(const char* id, int* index, const char* const* options, int count);
    bool Dropdown(const char* id, int* index, const char* const* options, int count);
    bool Dropdown(const char* id, int* index, const std::vector<std::string>& options);
    bool SliderFloat(const char* id, float* v, float lo, float hi, const char* fmt = "%.2f");
    bool SliderInt(const char* id, int* v, int lo, int hi, const char* fmt = "%d");
    bool InputFloat(const char* id, float* v, float step = 0.f, const char* fmt = "%.3f");
    bool InputDouble(const char* id, double* v, double step = 0.0, const char* fmt = "%.3f");
    bool InputInt(const char* id, int* v, int step = 1);
    bool InputText(const char* id, char* buf, size_t size, const char* placeholder = nullptr,
                   ImGuiInputTextFlags flags = 0);
    bool InputText(const char* id, std::string* s, const char* placeholder = nullptr, ImGuiInputTextFlags flags = 0);
    // width: 0 = fit the label, < 0 = fill the rest of the line, > 0 = exact.
    bool Button(const char* label, Tone tone = Tone::Plain, float width = 0.f);
    bool Color(const char* id, ImVec4* c, bool alpha = true);
    // Shows the bound key; click to capture the next key (Escape unbinds),
    // right-click to unbind.
    bool Key(const char* id, int* keyCode);

    // ---- one-control rows (the common case)

    bool SwitchRow(const char* label, const char* hint, bool* value);
    bool ChoiceRow(const char* label, const char* hint, int* index, const char* const* options, int count);
    bool DropdownRow(const char* label, const char* hint, int* index, const char* const* options, int count);
    bool SliderRow(const char* label, const char* hint, float* v, float lo, float hi, const char* fmt = "%.2f");
    bool SliderRow(const char* label, const char* hint, int* v, int lo, int hi, const char* fmt = "%d");
    bool KeyRow(const char* label, const char* hint, int* keyCode);
    bool ColorRow(const char* label, const char* hint, ImVec4* c, bool alpha = true);
    bool TextRow(const char* label, const char* hint, char* buf, size_t size, const char* placeholder = nullptr);

    // A feature with options: a switch row (and optionally its key), and while
    // it is on, the rows drawn until FeatureEnd() appear indented beneath it.
    // Returns true when the body should be drawn - call FeatureEnd() only then.
    bool FeatureBegin(const char* label, const char* hint, bool* enabled, int* keyCode = nullptr,
                      bool* changed = nullptr);
    void FeatureEnd();

    // A yes/no dialog. Call Confirm() every frame from the page; OpenConfirm()
    // to show it. Returns 1 when confirmed, -1 when cancelled, 0 otherwise.
    void OpenConfirm(const char* id);
    int Confirm(const char* id, const char* title, const char* message, const char* confirmLabel,
                Tone tone = Tone::Bad);

} // namespace gucci::ui::kit
