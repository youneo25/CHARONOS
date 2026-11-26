# Plan: CHARONOS Website V1

## Components

1. **Hero** — CHARONOS 로고/워드마크, 태그라인, 다운로드 CTA
2. **Features** — README 기반 6+ 핵심 기능 그리드
3. **Download** — 눈에 띄는 다운로드 버튼 + 버전/플랫폼 정보
4. **Footer** — 라이선스 요약, GitHub 링크

## Implementation Order

1. HTML 구조 (모든 섹션 시맨틱 마크업)
2. CSS (다크 테마, 반응형, 타이포그래피, 그리드 레이아웃)
3. 한국어 콘텐츠 (README에서 추출)
4. JS (스무스 스크롤, 없으면 생략)
5. 브라우저 테스트

## Risk

- 텍스트 워드마크: 로고 이미지가 없으므로 깔끔한 타이포그래피로 대체
- 반응형: 375px ~ 1440px 범위 커버