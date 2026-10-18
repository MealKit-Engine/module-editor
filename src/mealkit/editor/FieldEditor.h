#pragma once

#include "mealkit/core/Reflect.h"
#include "mealkit/game/Tables.h"

// 필드 선언(reflect::TypeInfo)을 ImGui 위젯으로 그린다. 인스펙터의 오브젝트 필드, 튜닝 패널, [표] 패널, 실시간 상태 보기가 같이 쓴다.
namespace mealkit::editor {

// 위젯이 참고하는 에디터 데이터: 행 참조(RowRef)를 고를 표 목록 (없으면 id를 글자로 고친다)
struct FieldContext {
    const game::TableIndexList* tables = nullptr;
};

// 편집 가능한 필드 위젯. 값이 바뀌었으면 true. 컨테이너(Pool)는 읽기 전용으로 보여 준다.
bool drawFields(const reflect::TypeInfo& type, void* data, const FieldContext* context = nullptr);

// 값 하나의 위젯 (표의 칸). label이 "##"로 시작하면 이름 없이. 바뀌었으면 true.
bool drawValueWidget(const char* label, const char* name, const reflect::ValueInfo& value, void* data,
                     const FieldContext* context = nullptr);

// 읽기 전용 표 (이름 | 값). 구조체·배열·Pool은 펼쳐 볼 수 있다. 큰 Pool은 보이는 줄만 그린다.
void drawFieldsReadOnly(const reflect::TypeInfo& type, const void* data);

} // namespace mealkit::editor
