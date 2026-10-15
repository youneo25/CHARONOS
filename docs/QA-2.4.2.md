# CHARONOS 2.4.2 재생 시작 안정성 검증

2026-10-15 (Asia/Seoul), Windows x64, Qt 6.10.3 / FFmpeg 8.1 / Release.

## 원인과 재현

사용자 확인: 재생 직전 0프레임, In/Out 반복 꺼짐.

기존 `MainWindow::onPlayTick()`의 오디오 동기화 경로는 장치 시각을 프레임으로 변환한 뒤 `goToFrame(heard)`로 바로 이동했습니다. 타이머 재생 경로와 달리 `m_realtime`(Skip Frames to Keep Up) 설정을 검사하지 않았습니다. 따라서 오디오가 시작된 후 파형 생성 등으로 UI 처리가 지연되면, 첫 화면 갱신에서 이미 경과한 시간만큼 건너뛰었습니다.

수정 전 코드를 실제 Windows 오디오 출력 장치로 검사했습니다. 240fps, 0프레임, 반복 꺼짐, 건너뛰기 꺼짐 상태에서 첫 타이머 이벤트를 350ms 지연시켰습니다.

- 수정 전: 오디오 시각 0.323초, 화면 **77프레임**. 재현 검사 실패.
- 수정 후: 같은 지연 조건에서 오디오 시각 0.333초, 화면 **1프레임**. 재현 검사 통과.
- 수정 전 기록: `qa-startup-before-fix.txt`. 수정 후 전체 결과: `feature-results.txt`.

이는 보고된 수십 프레임 점프를 재현하는 코드 경로입니다. 사용자 개인 원본 영상의 파일·코덱과 당시 UI 지연 시간은 직접 측정하지 않았습니다.

## 수정

- 오디오 기반 재생도 건너뛰기 설정을 지킵니다. 꺼져 있으면 타이머 이벤트마다 최대 한 프레임만 전진하고, 오디오가 크게 앞서면 기존 지터 허용치 밖에서만 표시 프레임에 다시 맞춥니다. 켜져 있으면 기존 실시간 건너뛰기를 유지합니다.
- 장치 시각이 뒤로 흔들려도 영상이 역행하지 않게 했습니다. 클립 끝·반복 전환은 범위를 유지합니다.
- 오디오 디코딩 완료 시 파형/속성 갱신을 먼저 처리한 뒤, 현재 표시된 영상 프레임에서 소리를 시작합니다.
- `waveOutGetPosition`의 성공 여부와 반환 단위(샘플/바이트/밀리초)를 확인합니다. 유효하지 않은 장치 시각은 세션 경과 시간과 실제 제출한 오디오 길이를 기준으로 제한합니다.
- 정지/스크럽 소리 종료 상태에서는 장치에 무음 버퍼를 계속 제출하지 않습니다. 새 재생 시 장치 리셋과 세션 시계를 초기화합니다. 장치 리셋/버퍼 제출 실패는 연속 재생 상태로 남지 않습니다.
- Space는 현재 표시 위치에서 이어 재생합니다. 항상 0으로 되돌리는 변경은 하지 않았습니다.

## 검사

**39 passed, 0 failed, 0 skipped**. 새 기능 검사 4개와 기존 35개를 실행했습니다. 오디오 출력 장치를 실제로 사용했으며 검사 음원은 음소거했습니다.

- `playbackStartupMustNotSkipWhenDisabled`: 시작 직후 350ms 지연, 이후 연속 틱의 최대 1프레임 전진, 오디오 재동기화, 명시적 실시간 모드에서 건너뛰기 유지.
- `repeatedSpaceStartsFromDisplayedFrame`: 실제 Qt Space 키 입력으로 0/110프레임에서 번갈아 8회 재생·정지. 시작 지연에도 위치 보존. In=80인 반복을 명시적으로 켠 경우만 80에서 시작.
- `audioClockRestartsAtRequestedPosition`: 스크럽 소리→정지→대기→0부터 재생을 12회 반복해 초기 시계가 이전 세션의 위치를 이어받지 않는지 확인.
- `audioClockRejectsInvalidAndStaleReports`: 반환 단위 변환, 80프레임에 해당하는 낡은 값, 시계 역행, 지원되지 않는 단위/오류, 제출 길이 제한 및 NaN 방어.
- 기존 MP4/AAC 루프와 Home 정지, 첫 프레임 디코딩 지연, 주석 키 눈금과 색, A/B FPS 매핑, 북마크 단계 재생 검사 유지.
- 별도 `audio_test` 시계 검사: 2초에서 시작해 실제 1.573초 후 오디오 위치 3.561초, 반속 재생은 5초에서 실제 1.002초 후 5.491초였습니다(예상 약 5.5초). 결과: `qa-startup-audio-clock.txt`.

프레임 건너뛰기를 끄면 컴퓨터가 목표 FPS를 처리하지 못하는 경우 재생이 느려질 수 있습니다. 동기화 지연이 커지면 오디오를 다시 맞추므로 느린 디코딩 환경에서는 짧은 소리 끊김이 생길 수 있습니다.

## API 근거

- [Microsoft: waveOutGetPosition](https://learn.microsoft.com/en-us/windows/win32/api/mmeapi/nf-mmeapi-waveoutgetposition): 호출 후 반환 단위를 확인해야 하며, 장치 열기/리셋 시 위치가 초기화됩니다.
- [Microsoft: Retrieving the Current Playback Position](https://learn.microsoft.com/en-us/windows/win32/multimedia/retrieving-the-current-playback-position): 장치가 요청된 단위 대신 다른 시간 단위를 반환할 수 있습니다.

완성 배포물과 검사 기록은 F:의 새 출력 폴더에 저장합니다. 기존 C: 프로젝트의 소스와 빌드 작업본은 유지합니다.
