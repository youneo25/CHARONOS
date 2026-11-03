# CHARONOS 1.1.2 검증 기록

검증일: 2026-10-22 (Asia/Seoul). Windows / Qt 6.10.3 / FFmpeg 8.1 / MSVC 2022.

전체 기능 회귀 검사 **72 passed, 0 failed**. 실행 로그는 `feature-results.txt`입니다.

## 추가 검사

- `sourceTreeDragCopiesToTimelineAndUndo`: 영상 뷰어에 포커스를 둔 상태에서 Sources 탭의 미디어를 드롭하고, 타임라인으로 포커스가 옮겨지는지 확인. 실제 포커스 위젯에 Delete 키를 보내 추가한 클립이 삭제되고 실행 취소로 복원되는지 확인.
- `timelineDeleteUsesRealFocusAndRespectsNoteLocks`: 실제 마우스 클릭으로 클립을 선택한 후 Delete, 숫자 키패드 Delete, Shift+Delete 및 창의 Delete 명령 경로 확인. 주석 블록 삭제, 주석 레인 잠금 보호, 실행 취소, 뷰어 쪽 기존 삭제 동작 확인.
- `annotationLaneMouseResizePersistsAndResets`: N1·N2 경계선 커서, 드래그로 개별 높이 변경, 다른 레인 높이 유지, 영상 트랙 위치 갱신, 최소·최대 높이 제한, 높이 기억, 더블 클릭 초기화, 세로 스크롤 범위 갱신, 타임라인 모델 불변 확인.

## 산출물

- `qa-resizable-note-lanes.png`: 직접 마우스로 높이를 키운 주석 레인.
- `qa-source-drag-drop.png`: Sources에서 추가한 타임라인.
- `qa-mixed-fps-export.mp4`: 이전 혼합 FPS 수정의 내보내기 회귀 검사 결과.
- `changes.patch`: 최초 1.1.0 소스부터의 누적 수정 사항.

1.1.1에서 통과한 혼합 FPS·오디오 음높이·분할·내보내기 및 타임라인 표시 상태 검사도 함께 실행했습니다.
