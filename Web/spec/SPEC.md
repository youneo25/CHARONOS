# Spec: CHARONOS Program Introduction & Download Website — V1

## Assumptions

```
ASSUMPTIONS I'M MAKING:
1. This is a single-page static website (no backend, no server needed)
2. It's a simple, self-contained site: HTML + CSS + JavaScript only (no build step, no framework)
3. The site will be deployed as-is on any static web server or opened directly in a browser
4. The download link will point to the GitHub releases page (or a placeholder until final URL is set)
5. Screenshots/demo images are placeholders until real assets are available
6. The site is written in Korean (with English program name and technical terms preserved)
7. Target: Windows 10/11 users looking for a professional animation review tool
8. No analytics, no cookie banners, no external tracking
→ Correct me now or I'll proceed with these.
```

## Objective

Create a polished single-page landing website that introduces **CHARONOS** — a frame-accurate video review and 2D animation tool for animators — and provides a prominent download section linking to the official release. The site serves as the public face of the project: what it does, why it matters, and how to get it.

**Target user:** Windows 10/11 animators and animation reviewers who need a professional, native tool for frame-accurate review.

**Success looks like:**
- A visitor understands what CHARONOS is and who it's for within 5 seconds
- A visitor can find and click the download button without scrolling more than one viewport
- The page looks professional and modern on desktop and mobile

## Tech Stack

- **HTML5** — single `index.html` file
- **CSS3** — inline `<style>` or a single `styles.css` file; no external dependencies (no CDN, no frameworks)
- **JavaScript** — minimal or none; only for smooth scroll or theme toggle if needed
- No build step, no package manager, no bundler
- **Fonts:** system fonts (no Google Fonts or external font files)

## Commands

```
Open: double-click index.html or serve with any static server
Dev:  any static file server (e.g. python -m http.server)
Build: none required
```

## Project Structure

```
F:\AnyThing\
├── index.html          # The complete website (HTML + CSS + JS)
└── spec/
    └── SPEC.md         # This spec file
```

## Code Style

Single self-contained `index.html` file. Modern, clean CSS with custom properties:

```css
:root {
  --primary: #1a1a2e;
  --accent: #e94560;
  --text: #eaeaea;
  --bg: #0f0f1a;
}
```

- Naming: BEM-like class names (`hero`, `feature-grid`, `download-btn`)
- No inline styles except dynamic ones via JS
- Semantic HTML: `<header>`, `<main>`, `<section>`, `<footer>`
- Dark theme matching CHARONOS's professional tool aesthetic
- Responsive: works on desktop (primary) and gracefully scales to tablet

## Testing Strategy

- **Manual:** Open `index.html` in Chrome and Firefox on Windows
- **Responsive check:** DevTools device toolbar at 375px, 768px, 1024px, 1440px
- **Link check:** Download button links to correct GitHub releases URL
- No automated tests needed (static site)

## Boundaries

- **Always:** Semantic HTML, keyboard-accessible links, alt text on images, responsive layout
- **Ask first:** Changing the GitHub releases URL, adding external dependencies or CDN links
- **Never:** Add tracking/analytics, use jQuery or any framework, create multiple HTML pages, use inline `style=` attributes, add build tooling

## Success Criteria

- [ ] `index.html` opens in any modern browser without errors
- [ ] Hero section states "CHARONOS" prominently with a one-line tagline
- [ ] Features section lists at least 6 key capabilities from the README
- [ ] Download section has a prominent, working link to the releases page
- [ ] Page is fully responsive (no horizontal scroll on mobile/tablet)
- [ ] No external network requests (fully offline-capable, single file)
- [ ] Keyboard accessible (Tab navigates all links, focus states visible)
- [ ] Page loads in under 1 second on 4G

## Out of Scope (Deferred)

- Multi-page site (features page, docs page, about page) — V2 if the single page feels cramped
- Light theme toggle — not essential for V1, dark theme is the primary design
- Localization beyond Korean — English is the base; Korean translation is a follow-up
- Interactive demo/video embed — needs hosted video assets, not available yet
- Screenshot gallery — needs real UI screenshots from the app, placeholder shapes for V1
- Blog/news section — not needed for a program landing page
- FAQ section — can be added once user questions surface

## Open Questions

1. **Download URL:** `https://github.com/youneo25/CHARONOS/releases` (confirmed from README)
2. **Screenshots:** Placeholder shapes for V1; real UI screenshots are a follow-up
3. **Logo:** Text-based wordmark "CHARONOS" for V1
4. **License info:** Include MIT / GPL-3.0 license summary in footer