# CHARONOS 2.4.0 검증 기록

2026-10-15 (Asia/Seoul), Windows x64, Qt 6.10.3 / FFmpeg 8.1 / Release.

## 결과

`tools\test-features.bat`: **35 passed, 0 failed, 0 skipped**. 기존 28개 검사와 신규 7개 검사를 실행했습니다. 실제 Windows 오디오 출력 장치를 사용한 검사도 실행되었습니다(테스트 출력은 음소거).

- `reviewRangeSemantics`: 주석 유지 구간, 빈 삭제 키 이후 빈 구간, 트래킹, 프레임 범위 자르기.
- `reviewBarsRenderedColorsAndBoundaries`: 상단 6px 빨간 선, 하단 2px 선, 구간 안/밖 글자색 픽셀 확인, 마지막 프레임 및 한 프레임 클립, 빈 소스 초기화.
- `reviewNavigationLoopUndoAndClipChanges`: 편집 키 탐색, 구간 반복, 한 프레임 반복, 삭제/실행 취소/다시 실행 시 표시 갱신, 여러 클립의 전체 프레임과 로컬 프레임 변환.
- `compareReviewRangesRespectDifferentFps`: A 24fps / B 48fps의 구간 투영, 짧은 A 끝 이후 상단 탐색, 비교 해제 시 표시 제거.
- `steppedPlaybackHoldsTimingSeeksAndLoops`: 북마크 없는 재생 방지, 원래 키 간격 유지, 탐색 시 홀드 프레임 선택, 끝에서 반복, In/Out 안의 북마크만 사용, 북마크 삭제 후 정지, 일반 재생 복귀.
- `audioWaitsForLoopPictureAndStopsAfterPause`: 루프 첫 프레임 디코딩을 180ms 지연시켜 화면 대기 중 오디오 정지 및 디코딩 완료 후 재개 확인. 정지 후 늦은 frameReady와 syncAudio가 소리를 재시작하지 않는지, 중복 stop도 오디오를 끄는지 검사.
- `realVideoAudioWrapAndHome`: 실제 H.264 MP4와 AAC 오디오를 디코딩해 2초 루프를 재생. 마지막 구간에서 시작 구간으로 복귀 확인 후 실제 Home QAction을 실행하여 화면 0프레임·재생 상태·오디오가 모두 정지하는지 확인.

## 오디오 수정 범위

루프 전환 때 `goToFrame()`이 디코딩을 요청해도 첫 이미지가 아직 준비되지 않으면 이전 이미지가 남습니다. 기존 `syncAudio(true)`는 이때도 즉시 오디오를 시작했습니다. 새 버전은 요청된 프레임이 실제 뷰어 프레임 키와 일치한 후 오디오를 시작합니다. 재생 도중 필요한 프레임이 캐시에 없으면 오디오를 멈추고 그 프레임을 기다립니다.

처음/끝 버튼과 Home/End는 영상·오디오를 함께 정지한 후 이동합니다. `stopPlayback()`은 이미 일시정지 상태여도 남아 있는 오디오와 보류된 스크럽 소리를 취소합니다.

검사는 생성한 미디어와 지연 디코더를 사용했습니다. 사용자 개인 원본 영상의 모든 코덱/오디오 장치 조합에 대한 검증은 포함하지 않습니다.

## 결과 파일

- `qa-review-ranges.png`: 상단 굵은 빨간 구간, 하단 얇은 빨간 구간, 연한 초록색 현재 프레임 숫자.
- `qa-review-single-frame.png`: 단일 프레임 편집 표시.
- `qa-review-demo.frvm`: 두 클립의 주석/삭제 키를 포함한 재현 프로젝트.
- `qa-audio-loop.mp4`: 테스트용 패턴 영상과 440Hz AAC 음원.
- `feature-results.txt`: 전체 검사 로그.

소스와 빌드/검사 작업본은 기존 C: 프로젝트 경로에 유지합니다. 완성 배포물과 검증 자료는 별도의 고유한 F: 출력 폴더로 복사하고 실행 파일 SHA-256을 확인합니다.
