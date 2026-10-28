#pragma once

#include <string>
#include <vector>

// 로그 줄의 "파일:줄"을 찾아 코드 편집기로 연다 (빌드 로그·[로그] 창·인스펙터 "코드 열기").
namespace mealkit::editor {

struct FileLine {
    std::string file; // 로그에 적힌 그대로 (절대 경로나 작업 폴더 기준)
    int line = 0;
};

// MSVC "D:\x\a.cpp(27): error", clang·일반 "src/a.cpp:27:5" 모양. 소스 확장자(.cpp .h .hpp .c .hlsl .hlsli .inl)만
bool parseFileLine(const std::string& text, FileLine* out);

// 환경설정의 코드 편집기로 연다. VS Code 방식 "<편집기> -g 파일:줄"(.exe가 아니면 cmd로 부른다)
bool openInCodeEditor(const std::string& editor, const std::string& file, int line);

// folders 아래(하위 폴더 포함) .h에서 needle이 있는 첫 줄
bool findInHeaders(const std::vector<std::string>& folders, const std::string& needle, FileLine* out);

} // namespace mealkit::editor
