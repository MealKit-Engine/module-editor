#pragma once

#include "mealkit/core/Reflect.h"
#include "mealkit/data/Tables.h"

// 필드 선언(reflect::TypeInfo)을 ImGui 위젯으로 그린다. 인스펙터의 오브젝트 필드, 튜닝 패널, [표] 패널, 실시간 상태 보기가 같이 쓴다.
namespace mealkit::editor {

// 위젯이 참고하는 에디터 데이터: 행 참조(RowRef)를 고를 표 목록 (없으면 id를 글자로 고친다)
struct FieldContext {
    const data::TableIndexList* tables = nullptr;
    // 기본값 인스턴스 (같은 타입). 있으면 기본값과 다른 필드에 점을 찍고, 오른쪽 클릭 "기본값으로"를 켠다
    const void* defaults = nullptr;
};

// 필드 표시 이름 "이름|설명": 설명은 마우스를 올리면 보인다 (단위·범위). MEALKIT_FIELD의 표시 이름에 쓴다
struct FieldLabel {
    std::string name;
    std::string tooltip;
};
FieldLabel splitFieldLabel(const char* label);

// 편집 가능한 필드 위젯. 값이 바뀌었으면 true. 컨테이너(Pool)는 읽기 전용으로 보여 준다.
bool drawFields(const reflect::TypeInfo& type, void* data, const FieldContext* context = nullptr);

// 값 하나의 위젯 (표의 칸). label이 "##"로 시작하면 이름 없이. 바뀌었으면 true.
bool drawValueWidget(const char* label, const char* name, const reflect::ValueInfo& value, void* data,
                     const FieldContext* context = nullptr);

// 읽기 전용 표 (이름 | 값). 구조체·배열·Pool은 펼쳐 볼 수 있다. 큰 Pool은 보이는 줄만 그린다.
void drawFieldsReadOnly(const reflect::TypeInfo& type, const void* data);

} // namespace mealkit::editor
