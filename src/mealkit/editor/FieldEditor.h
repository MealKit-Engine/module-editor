#pragma once

#include "mealkit/core/Reflect.h"

// 필드 선언(reflect::TypeInfo)을 ImGui 위젯으로 그린다. 인스펙터의 오브젝트 필드, 튜닝 패널, 실시간 상태 보기가 같이 쓴다.
namespace mealkit::editor {

// 편집 가능한 필드 위젯. 값이 바뀌었으면 true. 컨테이너(Pool)는 읽기 전용으로 보여 준다.
bool drawFields(const reflect::TypeInfo& type, void* data);

// 읽기 전용 표 (이름 | 값). 구조체·배열·Pool은 펼쳐 볼 수 있다. 큰 Pool은 보이는 줄만 그린다.
void drawFieldsReadOnly(const reflect::TypeInfo& type, const void* data);

} // namespace mealkit::editor
