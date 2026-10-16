#pragma once

#include <string>

// 에디터 UI 글자와 언어 (docs/CODING_MODEL.md "에디터 UI 문자열과 언어").
// 코드에는 영어 원문을 쓰고 그것이 키다. 다른 언어는 engine/res/strings/<언어>.txt의 "원문 = 번역" 줄이고, 없으면 원문이 보인다.
//
//   ImGui::TextUnformatted(tr("Recent projects"));
//   ImGui::Begin(trl("Scene Tree"));              // "씬 트리###Scene Tree": 언어가 바뀌어도 창 ID(도킹)가 같다
//   message = trf("Saved %s", path.c_str());
//
// 원문에 "##id"가 붙어 있으면 그 앞부분만 번역하고 뒤는 그대로 둔다.
namespace mealkit::editor {

struct Language {
    const char* code; // 파일 이름 (engine/res/strings/<code>.txt). 영어는 원문이라 파일이 없다
    const char* name; // 언어 고르기 목록에 그 언어로 쓴 이름
};
inline constexpr Language kLanguages[] = {{"ko", "한국어"}, {"en", "English"}}; // i18n-ignore (언어 이름은 그 언어로)
inline constexpr const char* kDefaultLanguage = "ko";

// 엔진 폴더의 engine/res/strings/에서 표를 읽고, 사용자 설정(%LOCALAPPDATA%\MealKit\editor_settings.txt)의 언어를 고른다.
void initStrings(const std::string& engineDirectory);
const char* currentLanguage();
// 언어를 바꾸고 사용자 설정에 저장한다. 이미 돌려준 tr() 포인터는 계속 유효하다(언어마다 따로 보관).
void setLanguage(const char* code);

const char* tr(const char* text);            // 화면에 보일 글자
const char* trl(const char* label);          // ImGui 이름: "번역###원문"
std::string trf(const char* format, ...);     // 번역한 printf 형식
// 선언된 데이터 글자 (필드 설명, 씬 종류 이름): 엔진이 선언한 것은 표에 있고, 게임이 선언한 것은 게임의 언어 그대로 둔다(경고 없음)
const char* trData(const char* text);

} // namespace mealkit::editor
