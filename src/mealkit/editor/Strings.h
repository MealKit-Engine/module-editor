#pragma once

#include <string>

// Editor UI text and languages (docs/CODING_MODEL.md "Editor UI strings and languages").
// Code uses the English source text and that is the key. Other languages are "source = translation" lines in engine/res/strings/<language>.txt; if missing, the source shows.
//
//   ImGui::TextUnformatted(tr("Recent projects"));
//   ImGui::Begin(trl("Scene Tree"));              // "<translation>###Scene Tree": the window ID (docking) stays the same when the language changes
//   message = trf("Saved %s", path.c_str());
//
// If the source has "##id" attached, only the part before it is translated and the rest is left as is.
namespace mealkit::editor {

struct Language {
    const char* code; // file name (engine/res/strings/<code>.txt). English is the source, so it has no file
    const char* name; // the language's name written in that language for the language picker
};
inline constexpr Language kLanguages[] = {{"ko", "한국어"}, {"en", "English"}}; // i18n-ignore (language names are in their own language)
inline constexpr const char* kDefaultLanguage = "ko";

// Reads tables from engine/res/strings/ in the engine folder and picks the language from user settings (%LOCALAPPDATA%\MealKit\editor_settings.txt).
// If language is given (--lang, for docs screenshots), uses that language without changing user settings.
void initStrings(const std::string& engineDirectory, const std::string& language = {});
const char* currentLanguage();
// Changes the language and saves it to user settings. tr() pointers already returned stay valid (kept separately per language).
void setLanguage(const char* code);

const char* tr(const char* text);            // text shown on screen
const char* trl(const char* label);          // ImGui name: "translation###source"
std::string trf(const char* format, ...);     // translated printf format
// Declared data text (field descriptions, scene kind names): engine declarations are in the table; game declarations stay in the game's language (no warning)
const char* trData(const char* text);

} // namespace mealkit::editor
