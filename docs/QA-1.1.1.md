# CHARONOS 1.1.1 검증 기록

검증일: 2026-10-22 (Asia/Seoul). Windows / MSVC 2022 / Qt 6.10.3 / FFmpeg 8.1.

## 검사 범위

- `tools/build-quick.bat CHRONOS feature_regression`로 Release 실행 파일과 Qt 검사 도구 빌드.
- `feature_regression.exe` 전체 검사: **70 passed, 0 failed**. 자세한 기록은 `feature-results.txt`.
- 원래 1.1.0 기능 검사와 함께 아래 5개 회귀 검사를 추가.

| 검사 | 검증한 동작 |
|---|---|
| `sourceTreeDragCopiesToTimelineAndUndo` | 실제 SOURCES 트리와 하단 Sources 탭 모델의 MIME 데이터를 타임라인에 드롭. 여러 소스 순서 배치, 복사 액션, 소스 보존, 잠긴 트랙 거부, 실행 취소/다시 실행, 지정 프레임 배치. |
| `mixedFpsTimelineKeepsDurationPitchAndEdits` | 24fps 타임라인에 24·60·30·29.97fps 순서로 추가. 실제 길이, 소스 프레임 매핑, 44.1kHz→48kHz 오디오, 880·660·550Hz 음높이, 원본 프레임 사이 분할, 트림, 프로젝트 저장/불러오기, FPS 변경/실행 취소. |
| `mixedFpsExportPreservesToneAfterCut` | 24fps 영상 뒤 60fps 영상을 붙이고 원본 프레임 사이에서 영상·오디오 연결 분할. 실제 H.264/AAC MP4를 내보낸 뒤 다시 디코딩하여 440Hz·880Hz 음높이와 약 3초 길이 확인. |
| `variableFpsUsesElapsedTimeAndExactSplit` | 일정하지 않은 프레임 PTS와 0이 아닌 시작 시간. 실제 경과 시간에 맞는 영상 프레임, 분할 지점 및 길이 보존. |
| `timelineVisibilityPersistsOnlyAfterClosing` | 설정 없는 최초 실행은 숨김. 표시한 상태로 정상 종료 후 재실행은 표시. 숨긴 상태로 종료 후 재실행은 숨김. 뷰어만 보기 상태로 종료해도 원래 선택 유지. 이전 버전 저장 레이아웃 복원. |

## 첨부

- `qa-source-drag-drop.png`: 소스를 복사해 타임라인에 추가한 실제 Qt 창.
- `qa-mixed-fps-export.mp4`: 2초의 24fps/440Hz 영상 + 1초의 60fps/880Hz 영상을 24fps 타임라인에서 내보낸 파일. 영상 스트림 3.000초, AAC 포함 컨테이너 3.008초(코덱 패딩).
- `changes.patch`, `changed-files.txt`: 원본 소스 ZIP과 비교한 변경 사항.

검사 영상·프로젝트는 회귀 검사에서 생성했으며 사용자의 실제 편집 프로젝트를 변경하지 않았습니다. 기존 프로젝트의 명시적인 클립 시작 위치는 보존합니다.

## 재현

소스 ZIP을 푼 폴더에서 `tools/test-features.bat` 실행. 빌드 경로는 기존 스크립트와 같은 `C:/dev/Qt/6.10.3/msvc2022_64`, `C:/dev/ffmpeg`를 사용합니다. 생성 파일과 실행 파일은 소스 폴더의 `build-test/`에 저장합니다.
